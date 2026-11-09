/*
 * SPDX-License-Identifier: MIT
 *
 * GBM backend for nvidia-drm.
 *
 * Buffers are allocated with DRM_IOCTL_NVIDIA_GEM_ALLOC_NVKMS_MEMORY on the
 * GBM device's own fd, so GEM handles are valid there (drmModeAddFB2 etc.).
 *
 * The layouts match what the closed libnvidia-allocator produces (measured
 * with tests/gbm-layout), because importers derive the layout from the
 * modifier and the size checks must agree:
 *
 *   block-linear: stride = align(width * cpp, 64)        (one GOB is 64 B wide)
 *                 rows   = align(height, 8 << h)          (GOB is 8 rows tall)
 *   linear:       stride = align(width * cpp, 256)
 *                 rows   = height
 *   planes start and the BO ends on a 64 KiB boundary.
 *
 * GEM handles are per DRM file and not reference counted, so every plane of
 * a BO holds a reference in the process-wide table of gem_ref.c (shared
 * with NVIDIA's EGL/GL/Vulkan when this is built as libnvidia-allocator).
 *
 * Every gbm_device_v0 entry point must be non-NULL because the loader calls
 * them without checking.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <drm_fourcc.h>
#include <gbm_backend_abi.h>

#include "nvidia-drm/nv_drm_common_ioctl.h"
#include "nvchan.h"
#include "gem_ref.h"

#define NV_GBM_EXPORT __attribute__((visibility("default")))

/* libnvidia-egl-gbm only drives devices whose gbm_device_get_backend_name()
 * returns this. */
#define NV_GBM_BACKEND_NAME "nvidia"

#define NV_GBM_MAX_PLANES 3

#define NV_GOB_WIDTH_BYTES   64u
#define NV_GOB_HEIGHT_ROWS   8u
#define NV_LINEAR_PITCH_ALIGN 256u
#define NV_BO_ALIGN          (64u * 1024u)
/* Largest h the closed allocator picks on its own; up to 5 is accepted. */
#define NV_PREFERRED_MAX_LOG2_GOBS 4u
#define NV_MAX_LOG2_GOBS           5u
#define NV_MAX_DIMENSION     32768u

/* Turing+ (page_kind_generation 2) compressible generic kind, and the
 * modifier compression type the closed allocator reports with it. */
#define NV_KIND_GENERIC_COMPRESSIBLE_GEN2 0x08u
#define NV_COMPRESSION_TYPE_ROP_3D        1u

#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))
#define DIV_ROUND_UP(v, d) (((v) + (d) - 1) / (d))

struct nv_gbm_format {
    uint32_t fourcc;
    uint8_t num_planes;
    uint8_t cpp[NV_GBM_MAX_PLANES];
    uint8_t hsub, vsub;   /* chroma subsampling of planes > 0 */
    bool scanout;         /* reported for GBM_BO_USE_SCANOUT, as the closed
                           * allocator does */
};

/*
 * The formats the closed allocator accepts. RM only allocates
 * comptags for color surfaces >= 32bpp, so compression is limited to
 * single-plane formats with cpp >= 4.
 */
static const struct nv_gbm_format nv_gbm_formats[] = {
    { DRM_FORMAT_XRGB8888,      1, { 4 }, 1, 1, true },
    { DRM_FORMAT_ARGB8888,      1, { 4 }, 1, 1, true },
    { DRM_FORMAT_XBGR8888,      1, { 4 }, 1, 1, true },
    { DRM_FORMAT_ABGR8888,      1, { 4 }, 1, 1, true },
    /* 10 bpc; the closed allocator has no XRGB2101010. */
    { DRM_FORMAT_XBGR2101010,   1, { 4 }, 1, 1, true },
    { DRM_FORMAT_ABGR2101010,   1, { 4 }, 1, 1, true },
    { DRM_FORMAT_ARGB2101010,   1, { 4 }, 1, 1, true },
    { DRM_FORMAT_RGBA1010102,   1, { 4 }, 1, 1, true },
    { DRM_FORMAT_BGRA1010102,   1, { 4 }, 1, 1, true },
    { DRM_FORMAT_ABGR16161616F, 1, { 8 }, 1, 1, true },
    { DRM_FORMAT_XBGR16161616F, 1, { 8 }, 1, 1, true },
    { DRM_FORMAT_RGB565,        1, { 2 }, 1, 1, true },
    { DRM_FORMAT_BGR565,        1, { 2 }, 1, 1, true },
    { DRM_FORMAT_R8,            1, { 1 }, 1, 1, true },
    { DRM_FORMAT_GR88,          1, { 2 }, 1, 1, true },
    { DRM_FORMAT_NV12,          2, { 1, 2 }, 2, 2, true },
    { DRM_FORMAT_NV21,          2, { 1, 2 }, 2, 2, true },
    { DRM_FORMAT_NV16,          2, { 1, 2 }, 2, 1, true },
    { DRM_FORMAT_NV61,          2, { 1, 2 }, 2, 1, true },
    { DRM_FORMAT_YUV410,        3, { 1, 1, 1 }, 4, 4, true },
    { DRM_FORMAT_YVU410,        3, { 1, 1, 1 }, 4, 4, true },
    { DRM_FORMAT_YUV411,        3, { 1, 1, 1 }, 4, 1, true },
    { DRM_FORMAT_YVU411,        3, { 1, 1, 1 }, 4, 1, true },
    { DRM_FORMAT_YUV420,        3, { 1, 1, 1 }, 2, 2, true },
    { DRM_FORMAT_YVU420,        3, { 1, 1, 1 }, 2, 2, true },
    { DRM_FORMAT_YUV422,        3, { 1, 1, 1 }, 2, 1, true },
    { DRM_FORMAT_YVU422,        3, { 1, 1, 1 }, 2, 1, true },
    { DRM_FORMAT_YUV444,        3, { 1, 1, 1 }, 1, 1, true },
    { DRM_FORMAT_YVU444,        3, { 1, 1, 1 }, 1, 1, true },
};

struct nv_gbm_device {
    struct gbm_device base;
    struct drm_nvidia_get_dev_info_params info;
    bool compression;   /* we know the compressed kind for this generation */
    bool linear_render; /* the renderer can render to linear, see linear_allowed() */

    /* Protects BO CPU mappings. */
    pthread_mutex_t lock;

    /* Copy engine channel for block-linear CPU access (nvchan), created on
     * first use. ce_lock serializes all work on it and BO imports into it. */
    pthread_mutex_t ce_lock;
    struct nvchan_device *ce_dev;
    struct nvchan_channel *ce;
    bool ce_unavailable;
};

struct nv_gbm_bo {
    struct gbm_bo base;
    uint64_t modifier;
    uint64_t size;
    const struct nv_gbm_format *fmt;  /* NULL for imports of unknown formats */
    unsigned num_planes;
    /* One GEMAddReference per plane. */
    uint32_t handle[GBM_MAX_PLANES];
    uint32_t stride[GBM_MAX_PLANES];
    uint32_t offset[GBM_MAX_PLANES];

    /* CPU mapping of the whole BO, made on the first gbm_bo_map and kept
     * until the BO is destroyed (dev->lock). */
    uint8_t *cpu;
    size_t cpu_size;

    /* The BO's memory imported into the copy engine's address space, and
     * the nvchan device it was imported into (dev->ce_lock). */
    struct nvchan_buffer *ce_buf;
    struct nvchan_device *ce_buf_dev;
};

/* What gbm_bo_map returns as map_data. */
struct nv_gbm_map {
    struct nv_gbm_bo *bo;
    struct nvchan_buffer *staging;  /* linear copy of a block-linear region */
    struct nvchan_device *ce_dev;   /* the nvchan device staging belongs to */
    uint32_t x, y, width, height;
    uint32_t flags;
    uint32_t stride;                /* of staging */
};

static const struct gbm_core *core;

static inline struct nv_gbm_device *nv_gbm_device(struct gbm_device *gbm)
{
    return (struct nv_gbm_device *)gbm;
}

static inline struct nv_gbm_bo *nv_gbm_bo(struct gbm_bo *bo)
{
    return (struct nv_gbm_bo *)bo;
}

/* ---- GEM handle references ---------------------------------------------- */

static uint32_t import_existing(void *arg)
{
    return *(const uint32_t *)arg;
}

/* Takes one more reference on a handle the caller already holds one on. */
static bool handle_ref_again(struct nv_gbm_device *dev, uint32_t handle)
{
    return GEMAddReference(dev->base.v0.fd, import_existing, &handle) == handle;
}

static void bo_unref_handles(struct nv_gbm_device *dev, struct nv_gbm_bo *bo,
                             unsigned num)
{
    for (unsigned p = 0; p < num; p++)
        GEMRemoveReference(dev->base.v0.fd, bo->handle[p]);
}

struct prime_import_args {
    int fd;
    int dmabuf;
    int err;
};

static uint32_t prime_import(void *arg)
{
    struct prime_import_args *a = arg;
    uint32_t handle = 0;

    if (drmPrimeFDToHandle(a->fd, a->dmabuf, &handle) != 0) {
        a->err = errno;
        return 0;
    }
    return handle;
}

static uint32_t canonicalize_format(uint32_t format)
{
    /* Maps the legacy GBM_BO_FORMAT_* enums to fourccs. */
    return core ? core->v0.format_canonicalize(format) : format;
}

static const struct nv_gbm_format *find_format(uint32_t fourcc)
{
    for (size_t i = 0; i < sizeof(nv_gbm_formats) / sizeof(nv_gbm_formats[0]); i++)
        if (nv_gbm_formats[i].fourcc == fourcc)
            return &nv_gbm_formats[i];
    return NULL;
}

static bool format_compressible(const struct nv_gbm_format *fmt)
{
    return fmt->num_planes == 1 && fmt->cpp[0] >= 4;
}

/* ---- modifiers ---------------------------------------------------------- */

#define NV_MOD_H(m) ((uint32_t)((m) & 0xf))
#define NV_MOD_K(m) ((uint32_t)(((m) >> 12) & 0xff))
#define NV_MOD_G(m) ((uint32_t)(((m) >> 20) & 0x3))
#define NV_MOD_S(m) ((uint32_t)((((m) >> 22) & 0x1) | (((m) >> 25) & 0x6)))
#define NV_MOD_C(m) ((uint32_t)(((m) >> 23) & 0x7))

static bool is_nv_block_linear(uint64_t mod)
{
    /* BLOCK_LINEAR_2D sets bit 4. The legacy 16BX2 modifiers are
     * BLOCK_LINEAR_2D with s = g = k = 0 and fail the s/g/k checks. */
    return (mod >> 56) == DRM_FORMAT_MOD_VENDOR_NVIDIA && (mod & 0x10) != 0;
}

static uint64_t nv_bl_modifier(const struct nv_gbm_device *dev, bool compressed,
                               uint32_t log2_gobs)
{
    if (compressed)
        return DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(
            NV_COMPRESSION_TYPE_ROP_3D, dev->info.sector_layout,
            dev->info.page_kind_generation, NV_KIND_GENERIC_COMPRESSIBLE_GEN2,
            log2_gobs);
    return DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(
        0, dev->info.sector_layout, dev->info.page_kind_generation,
        dev->info.generic_page_kind, log2_gobs);
}

/* Whether we can allocate mod for fmt; *compressed tells which kind it is. */
static bool modifier_supported(const struct nv_gbm_device *dev,
                               const struct nv_gbm_format *fmt, uint64_t mod,
                               bool *compressed)
{
    *compressed = false;

    if (mod == DRM_FORMAT_MOD_LINEAR)
        return true;
    if (!is_nv_block_linear(mod))
        return false;
    if (NV_MOD_H(mod) > NV_MAX_LOG2_GOBS ||
        NV_MOD_S(mod) != dev->info.sector_layout ||
        NV_MOD_G(mod) != dev->info.page_kind_generation)
        return false;

    if (NV_MOD_K(mod) == dev->info.generic_page_kind && NV_MOD_C(mod) == 0)
        return true;

    if (dev->compression && format_compressible(fmt) &&
        NV_MOD_K(mod) == NV_KIND_GENERIC_COMPRESSIBLE_GEN2 &&
        NV_MOD_C(mod) == NV_COMPRESSION_TYPE_ROP_3D) {
        *compressed = true;
        return true;
    }
    return false;
}

static uint32_t floor_log2(uint32_t v)
{
    return v ? 31u - (uint32_t)__builtin_clz(v) : 0;
}

/* The GOB height the closed allocator picks: the tallest block that is not
 * taller than the surface (one GOB for surfaces under 8 rows), at most 16
 * GOBs (128 rows). */
static uint32_t preferred_log2_gobs(uint32_t height)
{
    uint32_t h = floor_log2(height / NV_GOB_HEIGHT_ROWS);
    return h > NV_PREFERRED_MAX_LOG2_GOBS ? NV_PREFERRED_MAX_LOG2_GOBS : h;
}

/*
 * Whether a linear BO may be handed out for GBM_BO_USE_RENDERING. This is a
 * property of the GL/Vulkan driver that renders into the BO, not of the
 * memory: NVIDIA's closed GL imports pitch-linear images but can't render to
 * them (GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT, see tests/gbm-egl), and the
 * closed allocator refuses the combination for that reason. Drivers that can
 * render to linear set dev->linear_render.
 */
static bool linear_allowed(const struct nv_gbm_device *dev, uint32_t usage)
{
    return dev->linear_render || !(usage & GBM_BO_USE_RENDERING);
}

/* Cursors are written by the CPU and scanned out without tiling support. */
static bool linear_required(uint32_t usage)
{
    return usage & (GBM_BO_USE_LINEAR | GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
}

/*
 * Picks a modifier from the caller's list. Order: block-linear over linear;
 * among block-linear, compressed unless the BO is for scanout (the display
 * can't read compressed surfaces), then the GOB height closest to the
 * preferred one, lower first.
 */
static bool choose_modifier(const struct nv_gbm_device *dev,
                            const struct nv_gbm_format *fmt,
                            uint32_t height, uint32_t usage,
                            const uint64_t *mods, unsigned count,
                            uint64_t *out)
{
    const uint32_t want_h = preferred_log2_gobs(height);
    const bool want_compressed = !(usage & GBM_BO_USE_SCANOUT);
    bool have = false, have_linear = false;
    int best_score = 0;

    for (unsigned i = 0; i < count; i++) {
        bool compressed;

        if (!modifier_supported(dev, fmt, mods[i], &compressed))
            continue;
        if (mods[i] == DRM_FORMAT_MOD_LINEAR) {
            have_linear = linear_allowed(dev, usage);
            continue;
        }
        if (linear_required(usage))
            continue;

        uint32_t h = NV_MOD_H(mods[i]);
        int dist = h <= want_h ? (int)(want_h - h) * 2 : (int)(h - want_h) * 2 + 1;
        int score = 100 - dist + (compressed == want_compressed ? 1000 : 0);

        if (!have || score > best_score) {
            *out = mods[i];
            best_score = score;
            have = true;
        }
    }

    if (!have && have_linear) {
        *out = DRM_FORMAT_MOD_LINEAR;
        have = true;
    }
    return have;
}

/* ---- device ------------------------------------------------------------- */

static void nv_gbm_destroy(struct gbm_device *gbm)
{
    struct nv_gbm_device *dev = nv_gbm_device(gbm);

    nvchan_channel_destroy(dev->ce);
    nvchan_device_close(dev->ce_dev);
    pthread_mutex_destroy(&dev->ce_lock);

    /* Handles still referenced belong to leaked BOs; the fd owns them. */
    pthread_mutex_destroy(&dev->lock);
    free(dev);
}

static int nv_gbm_is_format_supported(struct gbm_device *gbm,
                                      uint32_t format, uint32_t usage)
{
    const struct nv_gbm_format *fmt = find_format(canonicalize_format(format));

    if (fmt == NULL)
        return 0;
    if ((usage & GBM_BO_USE_CURSOR) && fmt->fourcc != DRM_FORMAT_ARGB8888)
        return 0;
    if ((usage & GBM_BO_USE_SCANOUT) && !fmt->scanout)
        return 0;
    return 1;
}

static int nv_gbm_get_format_modifier_plane_count(struct gbm_device *gbm,
                                                  uint32_t format,
                                                  uint64_t modifier)
{
    const struct nv_gbm_format *fmt = find_format(canonicalize_format(format));
    bool compressed;

    if (fmt == NULL ||
        !modifier_supported(nv_gbm_device(gbm), fmt, modifier, &compressed))
        return -1;
    return fmt->num_planes;
}

/* ---- buffer objects ----------------------------------------------------- */

/* Fills in stride/offset/size for bo->modifier. */
static bool compute_layout(struct nv_gbm_bo *bo, uint32_t width, uint32_t height)
{
    const struct nv_gbm_format *fmt = bo->fmt;
    const bool linear = bo->modifier == DRM_FORMAT_MOD_LINEAR;
    const uint32_t row_align = linear ? 1 : NV_GOB_HEIGHT_ROWS << NV_MOD_H(bo->modifier);
    uint64_t end = 0;

    bo->num_planes = fmt->num_planes;
    for (unsigned p = 0; p < fmt->num_planes; p++) {
        uint32_t pw = p ? DIV_ROUND_UP(width, fmt->hsub) : width;
        uint32_t ph = p ? DIV_ROUND_UP(height, fmt->vsub) : height;
        uint64_t stride = (uint64_t)pw * fmt->cpp[p];

        stride = ALIGN(stride, linear ? NV_LINEAR_PITCH_ALIGN : NV_GOB_WIDTH_BYTES);
        if (stride > UINT32_MAX)
            return false;

        end = ALIGN(end, NV_BO_ALIGN);
        if (end > UINT32_MAX)
            return false;
        bo->offset[p] = (uint32_t)end;
        bo->stride[p] = (uint32_t)stride;
        end += stride * ALIGN((uint64_t)ph, row_align);
    }

    bo->size = ALIGN(end, NV_BO_ALIGN);
    return true;
}

struct alloc_gem_args {
    int fd;
    struct drm_nvidia_gem_alloc_nvkms_memory_params p;
    int err;
};

static uint32_t alloc_gem_import(void *arg)
{
    struct alloc_gem_args *a = arg;

    if (drmIoctl(a->fd, DRM_IOCTL_NVIDIA_GEM_ALLOC_NVKMS_MEMORY, &a->p) != 0) {
        a->err = errno;
        return 0;
    }
    return a->p.handle;
}

/* Allocates the memory and takes the plane references on its handle. */
static int alloc_gem(struct nv_gbm_device *dev, struct nv_gbm_bo *bo,
                     bool scanout, bool *compressed)
{
    struct alloc_gem_args a = {
        .fd = dev->base.v0.fd,
        .p = {
            .block_linear = bo->modifier != DRM_FORMAT_MOD_LINEAR,
            .compressible = *compressed,
            .memory_size = bo->size,
            .flags = scanout ? 0 : NV_GEM_ALLOC_NO_SCANOUT,
        },
        .err = ENOMEM,
    };

    const uint32_t handle = GEMAddReference(a.fd, alloc_gem_import, &a);
    if (handle == 0)
        return -a.err;
    bo->handle[0] = handle;

    for (unsigned i = 1; i < bo->num_planes; i++) {
        if (!handle_ref_again(dev, handle)) {
            bo_unref_handles(dev, bo, i);
            return -ENOMEM;
        }
        bo->handle[i] = handle;
    }

    *compressed = a.p.compressible != 0;
    return 0;
}

static struct gbm_bo *nv_gbm_bo_create(struct gbm_device *gbm,
                                       uint32_t width, uint32_t height,
                                       uint32_t format, uint32_t usage,
                                       const uint64_t *modifiers,
                                       const unsigned int count)
{
    struct nv_gbm_device *dev = nv_gbm_device(gbm);
    const struct nv_gbm_format *fmt;
    struct nv_gbm_bo *bo;
    uint64_t modifier;
    bool compressed;
    int ret;

    format = canonicalize_format(format);
    fmt = find_format(format);
    if (fmt == NULL || width == 0 || height == 0 ||
        width > NV_MAX_DIMENSION || height > NV_MAX_DIMENSION) {
        errno = EINVAL;
        return NULL;
    }
    if ((usage & GBM_BO_USE_CURSOR) && fmt->fourcc != DRM_FORMAT_ARGB8888 &&
        fmt->fourcc != DRM_FORMAT_XRGB8888) {
        errno = EINVAL;
        return NULL;
    }
    if ((usage & GBM_BO_USE_SCANOUT) && !fmt->scanout) {
        errno = EINVAL;
        return NULL;
    }

    if (linear_required(usage) && !linear_allowed(dev, usage)) {
        errno = EINVAL;
        return NULL;
    }

    if (count > 0) {
        if (usage & GBM_BO_USE_LINEAR) {
            /* Same rule as the closed allocator and Mesa's dri backend. */
            errno = EINVAL;
            return NULL;
        }
        if (!choose_modifier(dev, fmt, height, usage, modifiers, count, &modifier)) {
            errno = EINVAL;
            return NULL;
        }
    } else if (linear_required(usage)) {
        modifier = DRM_FORMAT_MOD_LINEAR;
    } else {
        modifier = nv_bl_modifier(dev,
                                  dev->compression && format_compressible(fmt) &&
                                  !(usage & GBM_BO_USE_SCANOUT),
                                  preferred_log2_gobs(height));
    }

    modifier_supported(dev, fmt, modifier, &compressed);

    bo = calloc(1, sizeof(*bo));
    if (bo == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    bo->fmt = fmt;
    bo->modifier = modifier;
    if (!compute_layout(bo, width, height)) {
        free(bo);
        errno = EINVAL;
        return NULL;
    }

    /* Cursors are scanned out too. */
    const bool scanout = usage & (GBM_BO_USE_SCANOUT | GBM_BO_USE_CURSOR);
    const bool wanted_compressed = compressed;

    ret = alloc_gem(dev, bo, scanout, &compressed);
    if (ret != 0) {
        free(bo);
        errno = -ret;
        return NULL;
    }

    if (wanted_compressed && !compressed) {
        /*
         * No comptags (the kernel retries without them when video memory is
         * short). The same layout without compression is only valid if the
         * caller accepts that modifier.
         */
        uint64_t plain = nv_bl_modifier(dev, false, NV_MOD_H(modifier));
        bool ok = count == 0;

        for (unsigned i = 0; i < count && !ok; i++)
            ok = modifiers[i] == plain;
        if (!ok) {
            bo_unref_handles(dev, bo, bo->num_planes);
            free(bo);
            errno = ENOMEM;
            return NULL;
        }
        bo->modifier = plain;
    }

    bo->base.gbm = gbm;
    bo->base.v0.width = width;
    bo->base.v0.height = height;
    bo->base.v0.stride = bo->stride[0];
    bo->base.v0.format = format;
    bo->base.v0.handle.u32 = bo->handle[0];
    return &bo->base;
}

/*
 * Rejects plane layouts that don't fit in their dma-buf, so a bad client
 * buffer fails here instead of faulting the GPU or the display later. Only
 * checked for formats and modifiers whose layout we know.
 */
static bool import_fits(const struct nv_gbm_format *fmt, uint64_t modifier,
                        uint32_t width, uint32_t height,
                        const int *fds, const int *strides, const int *offsets)
{
    uint32_t row_align;

    if (fmt == NULL)
        return true;
    if (modifier == DRM_FORMAT_MOD_LINEAR)
        row_align = 1;
    else if (is_nv_block_linear(modifier) && NV_MOD_H(modifier) <= NV_MAX_LOG2_GOBS)
        row_align = NV_GOB_HEIGHT_ROWS << NV_MOD_H(modifier);
    else
        return true;

    for (unsigned p = 0; p < fmt->num_planes; p++) {
        uint32_t pw = p ? DIV_ROUND_UP(width, fmt->hsub) : width;
        uint32_t ph = p ? DIV_ROUND_UP(height, fmt->vsub) : height;
        off_t size = lseek(fds[p], 0, SEEK_END);

        if ((uint64_t)strides[p] < (uint64_t)pw * fmt->cpp[p])
            return false;
        if (row_align > 1 && strides[p] % NV_GOB_WIDTH_BYTES != 0)
            return false;
        if (size < 0)
            continue;   /* not seekable: can't tell */

        /* The last row of a linear plane only needs its pixels. */
        uint64_t need = row_align > 1 ?
            (uint64_t)strides[p] * ALIGN((uint64_t)ph, row_align) :
            (uint64_t)strides[p] * (ph - 1) + (uint64_t)pw * fmt->cpp[p];
        if ((uint64_t)offsets[p] + need > (uint64_t)size)
            return false;
    }
    return true;
}

static struct gbm_bo *nv_gbm_bo_import(struct gbm_device *gbm, uint32_t type,
                                       void *buffer, uint32_t usage)
{
    struct nv_gbm_device *dev = nv_gbm_device(gbm);
    uint32_t width, height, format, num_fds;
    int fds[GBM_MAX_PLANES], strides[GBM_MAX_PLANES], offsets[GBM_MAX_PLANES];
    uint64_t modifier;

    switch (type) {
    case GBM_BO_IMPORT_FD: {
        const struct gbm_import_fd_data *d = buffer;

        width = d->width;
        height = d->height;
        format = d->format;
        num_fds = 1;
        fds[0] = d->fd;
        strides[0] = (int)d->stride;
        offsets[0] = 0;
        /* Resolved below once the format is known. */
        modifier = DRM_FORMAT_MOD_INVALID;
        break;
    }
    case GBM_BO_IMPORT_FD_MODIFIER: {
        const struct gbm_import_fd_modifier_data *d = buffer;

        width = d->width;
        height = d->height;
        format = d->format;
        num_fds = d->num_fds;
        if (num_fds == 0 || num_fds > GBM_MAX_PLANES) {
            errno = EINVAL;
            return NULL;
        }
        memcpy(fds, d->fds, sizeof(fds));
        memcpy(strides, d->strides, sizeof(strides));
        memcpy(offsets, d->offsets, sizeof(offsets));
        modifier = d->modifier;
        break;
    }
    default:
        /* No wl_drm on NVIDIA; EGLImages would need the EGL display. */
        errno = ENOSYS;
        return NULL;
    }

    format = canonicalize_format(format);
    const struct nv_gbm_format *fmt = find_format(format);

    /* One fd per plane, as GBM callers pass them (repeated for a single
     * dma-buf). */
    if (width == 0 || height == 0 ||
        width > NV_MAX_DIMENSION || height > NV_MAX_DIMENSION ||
        (fmt != NULL && num_fds != fmt->num_planes)) {
        errno = EINVAL;
        return NULL;
    }
    for (unsigned p = 0; p < num_fds; p++) {
        if (fds[p] < 0 || strides[p] <= 0 || offsets[p] < 0) {
            errno = EINVAL;
            return NULL;
        }
    }
    /*
     * A buffer imported without a modifier has the implicit layout, which
     * between NVIDIA's drivers is the default render layout: what bo_create
     * picks for GBM_BO_USE_RENDERING (compressed generic kind for >= 32 bpp,
     * the GOB height for the buffer height). NVIDIA's GL and Vulkan allocate
     * modifier-less buffers that way, Xwayland's implicit DRI3 path imports
     * them with GBM_BO_IMPORT_FD and builds the EGLImage from the modifier
     * we report, and eglcore rejects DRM_FORMAT_MOD_INVALID. The closed
     * allocator reports the same. If that layout doesn't fit the dma-buf,
     * the buffer is something else and stays DRM_FORMAT_MOD_INVALID.
     */
    if (type == GBM_BO_IMPORT_FD && fmt != NULL) {
        uint64_t implicit = nv_bl_modifier(dev,
                                           dev->compression && format_compressible(fmt),
                                           preferred_log2_gobs(height));
        if (import_fits(fmt, implicit, width, height, fds, strides, offsets))
            modifier = implicit;
    }

    if (!import_fits(fmt, modifier, width, height, fds, strides, offsets)) {
        errno = EINVAL;
        return NULL;
    }

    struct nv_gbm_bo *bo = calloc(1, sizeof(*bo));
    if (bo == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    bo->fmt = fmt;
    bo->modifier = modifier;
    bo->num_planes = num_fds;
    off_t size = lseek(fds[0], 0, SEEK_END);
    bo->size = size > 0 ? (uint64_t)size : 0;

    for (unsigned p = 0; p < num_fds; p++) {
        struct prime_import_args a = { gbm->v0.fd, fds[p], ENOMEM };
        const uint32_t handle = GEMAddReference(gbm->v0.fd, prime_import, &a);

        if (handle == 0) {
            bo_unref_handles(dev, bo, p);
            free(bo);
            errno = a.err;
            return NULL;
        }
        bo->handle[p] = handle;
        bo->stride[p] = (uint32_t)strides[p];
        bo->offset[p] = (uint32_t)offsets[p];
    }

    bo->base.gbm = gbm;
    bo->base.v0.width = width;
    bo->base.v0.height = height;
    bo->base.v0.stride = bo->stride[0];
    bo->base.v0.format = format;
    bo->base.v0.handle.u32 = bo->handle[0];
    return &bo->base;
}

/* ---- CPU access ------------------------------------------------------------
 *
 * Linear BOs are mapped directly. Block-linear BOs are copied by the copy
 * engine (nvchan) into a linear staging buffer in cached system memory on
 * map, and back on unmap after a write map. The copy engine handles every
 * block-linear layout (Blackwell's 8 and 16 bpp GOBs included) and reads
 * compressed memory through its page kind, and it is far faster than CPU
 * reads through BAR1.
 *
 * nvidia-drm maps video memory made of several BAR1 ranges (static BAR1,
 * resizable BAR) only with the multi-range GEM mmap support in our kernel
 * patch stack.
 */

/* Maps the whole BO for the CPU once. Caller holds dev->lock. */
static int bo_cpu_map(struct nv_gbm_device *dev, struct nv_gbm_bo *bo)
{
    struct drm_nvidia_gem_map_offset_params p = { .handle = bo->handle[0] };

    if (bo->cpu != NULL)
        return 0;
    if (bo->size == 0)
        return -EINVAL;
    if (drmIoctl(dev->base.v0.fd, DRM_IOCTL_NVIDIA_GEM_MAP_OFFSET, &p) != 0)
        return -errno;

    void *ptr = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     dev->base.v0.fd, (off_t)p.offset);
    if (ptr == MAP_FAILED)
        return -errno;
    bo->cpu = ptr;
    bo->cpu_size = bo->size;
    return 0;
}

/*
 * Waits for the implicit fences of the BO's dma-buf: writers before the CPU
 * reads (POLLIN), all users before it writes (POLLOUT).
 */
static void wait_implicit_fences(struct nv_gbm_device *dev, struct nv_gbm_bo *bo,
                                 bool write)
{
    int fd;

    if (drmPrimeHandleToFD(dev->base.v0.fd, bo->handle[0], DRM_CLOEXEC, &fd) != 0)
        return;
    struct pollfd pfd = { .fd = fd, .events = write ? POLLOUT : POLLIN };
    while (poll(&pfd, 1, -1) < 0 && errno == EINTR)
        ;
    close(fd);
}

/* The copy engine channel, created on first use. dev->ce_lock held. */
static struct nvchan_channel *ce_get(struct nv_gbm_device *dev)
{
    char err[256] = "";

    /*
     * In a child after fork() the channel is the parent's. Leave it alone:
     * its GPU objects belong to the parent's RM client, and BOs may still
     * reference the old device (destroying their buffers only drops local
     * copies). Start over with a device of our own.
     */
    if (dev->ce_dev != NULL && !nvchan_device_is_usable(dev->ce_dev)) {
        dev->ce = NULL;
        dev->ce_dev = NULL;
        dev->ce_unavailable = false;
    }

    if (dev->ce != NULL || dev->ce_unavailable)
        return dev->ce;

    if (dev->ce_dev == NULL)
        dev->ce_dev = nvchan_device_open_gpu_id(dev->info.gpu_id, err, sizeof(err));
    if (dev->ce_dev != NULL)
        dev->ce = nvchan_channel_create_copy(dev->ce_dev, err, sizeof(err));
    if (dev->ce == NULL) {
        fprintf(stderr, "nvidia-gbm: no copy engine for block-linear maps: %s\n", err);
        dev->ce_unavailable = true;
    }
    return dev->ce;
}

/* After a channel error RM has torn the channel down; the next map makes a
 * new one. dev->ce_lock held. */
static void ce_reset(struct nv_gbm_device *dev)
{
    nvchan_channel_destroy(dev->ce);
    dev->ce = NULL;
}

/* The BO's memory in the copy engine's address space, imported through an
 * RM memory fd from nvidia-drm. dev->ce_lock held. */
static struct nvchan_buffer *ce_bo_buffer(struct nv_gbm_device *dev, struct nv_gbm_bo *bo)
{
    struct NvKmsKapiPrivExportMemoryParams_ {
        int memFd;    /* struct NvKmsKapiPrivExportMemoryParams */
    } nvkms = { -1 };
    char err[256] = "";

    if (bo->ce_buf != NULL && bo->ce_buf_dev == dev->ce_dev)
        return bo->ce_buf;
    if (bo->ce_buf != NULL) {
        /* Imported into a device of the parent before fork(). */
        nvchan_buffer_destroy(bo->ce_buf);
        bo->ce_buf = NULL;
    }

    /* An empty RM file for the export, as the RM API library makes one. */
    nvkms.memFd = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
    if (nvkms.memFd < 0)
        return NULL;

    struct drm_nvidia_gem_export_nvkms_memory_params p = {
        .handle = bo->handle[0],
        .nvkms_params_ptr = (uint64_t)(uintptr_t)&nvkms,
        .nvkms_params_size = sizeof(nvkms),
    };
    if (drmIoctl(dev->base.v0.fd, DRM_IOCTL_NVIDIA_GEM_EXPORT_NVKMS_MEMORY, &p) == 0)
        bo->ce_buf = nvchan_buffer_import_rm_fd(dev->ce_dev, nvkms.memFd, bo->size,
                                                err, sizeof(err));
    else
        snprintf(err, sizeof(err), "GEM_EXPORT_NVKMS_MEMORY: %s", strerror(errno));
    close(nvkms.memFd);
    if (bo->ce_buf == NULL)
        fprintf(stderr, "nvidia-gbm: importing BO for the copy engine: %s\n", err);
    bo->ce_buf_dev = dev->ce_dev;
    return bo->ce_buf;
}

/* Copies the map's rectangle between the BO and the staging buffer.
 * dev->ce_lock held. */
static int ce_transfer(struct nv_gbm_device *dev, struct nvchan_channel *ch,
                       struct nvchan_buffer *bo_buf, struct nv_gbm_map *m, bool to_bo)
{
    struct nv_gbm_bo *bo = m->bo;
    const uint32_t cpp = bo->fmt->cpp[0];
    const struct nvchan_surface bl = {
        .buf = bo_buf,
        .offset = bo->offset[0],
        .pitch = bo->stride[0],
        .height = bo->base.v0.height,
        .block_linear = true,
        .log2_gobs = NV_MOD_H(bo->modifier),
    };
    const struct nvchan_surface lin = {
        .buf = m->staging,
        .pitch = m->stride,
        .height = m->height,
    };
    int ret = to_bo ?
        nvchan_copy_2d(ch, &bl, m->x * cpp, m->y, &lin, 0, 0, m->width * cpp, m->height) :
        nvchan_copy_2d(ch, &lin, 0, 0, &bl, m->x * cpp, m->y, m->width * cpp, m->height);

    if (ret == 0)
        ret = nvchan_channel_finish(ch, 5000);
    if (ret == -EIO || ret == -ETIMEDOUT)
        ce_reset(dev);
    return ret;
}

static void *map_block_linear(struct nv_gbm_device *dev, struct nv_gbm_map *m,
                              uint32_t *stride)
{
    struct nv_gbm_bo *bo = m->bo;
    const uint32_t cpp = bo->fmt->cpp[0];
    struct nvchan_buffer *bo_buf;
    struct nvchan_channel *ch;
    void *cpu = NULL;
    int ret = 0;

    m->stride = ALIGN(m->width * cpp, 64u);

    pthread_mutex_lock(&dev->ce_lock);
    ch = ce_get(dev);
    bo_buf = ch ? ce_bo_buffer(dev, bo) : NULL;
    if (bo_buf != NULL) {
        m->ce_dev = dev->ce_dev;
        m->staging = nvchan_buffer_create(dev->ce_dev, (uint64_t)m->stride * m->height,
                                          NVCHAN_SYSMEM, NVCHAN_PITCH, NULL, 0);
    }
    if (m->staging != NULL)
        cpu = nvchan_buffer_map(m->staging);
    if (cpu != NULL && (m->flags & GBM_BO_TRANSFER_READ))
        ret = ce_transfer(dev, ch, bo_buf, m, false);
    pthread_mutex_unlock(&dev->ce_lock);

    if (cpu == NULL || ret != 0) {
        if (m->staging != NULL) {
            pthread_mutex_lock(&dev->ce_lock);
            nvchan_buffer_destroy(m->staging);
            pthread_mutex_unlock(&dev->ce_lock);
        }
        errno = cpu == NULL ? (ch == NULL ? ENODEV : ENOMEM) : EIO;
        return NULL;
    }
    *stride = m->stride;
    return cpu;
}

static void *nv_gbm_bo_map(struct gbm_bo *_bo, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height, uint32_t flags,
                           uint32_t *stride, void **map_data)
{
    struct nv_gbm_bo *bo = nv_gbm_bo(_bo);
    struct nv_gbm_device *dev = nv_gbm_device(_bo->gbm);
    const bool linear = bo->modifier == DRM_FORMAT_MOD_LINEAR;
    bool compressed;
    void *ptr;

    /* Plane 0 only, like the other backends. Block-linear needs a layout
     * we know; imports of unknown format or modifier are refused. */
    if (bo->fmt == NULL || !(flags & GBM_BO_TRANSFER_READ_WRITE) ||
        width == 0 || height == 0 ||
        (uint64_t)x + width > _bo->v0.width || (uint64_t)y + height > _bo->v0.height ||
        (!linear && (!is_nv_block_linear(bo->modifier) ||
                     !modifier_supported(dev, bo->fmt, bo->modifier, &compressed) ||
                     bo->stride[0] % NV_GOB_WIDTH_BYTES != 0))) {
        errno = EINVAL;
        return NULL;
    }

    struct nv_gbm_map *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    *m = (struct nv_gbm_map){ .bo = bo, .x = x, .y = y, .width = width,
                              .height = height, .flags = flags };

    if (flags & GBM_BO_TRANSFER_READ)
        wait_implicit_fences(dev, bo, false);

    if (!linear) {
        ptr = map_block_linear(dev, m, stride);
        if (ptr == NULL) {
            free(m);
            return NULL;
        }
        *map_data = m;
        return ptr;
    }

    pthread_mutex_lock(&dev->lock);
    int ret = bo_cpu_map(dev, bo);
    pthread_mutex_unlock(&dev->lock);
    if (ret != 0) {
        free(m);
        errno = -ret;
        return NULL;
    }
    *stride = bo->stride[0];
    *map_data = m;
    return bo->cpu + bo->offset[0] + (size_t)y * bo->stride[0] + (size_t)x * bo->fmt->cpp[0];
}

static void nv_gbm_bo_unmap(struct gbm_bo *_bo, void *map_data)
{
    struct nv_gbm_map *m = map_data;
    struct nv_gbm_device *dev = nv_gbm_device(_bo->gbm);

    if (m == NULL)
        return;
    if (m->staging != NULL) {
        if (m->flags & GBM_BO_TRANSFER_WRITE)
            wait_implicit_fences(dev, m->bo, true);

        pthread_mutex_lock(&dev->ce_lock);
        if (m->flags & GBM_BO_TRANSFER_WRITE) {
            struct nvchan_channel *ch = ce_get(dev);
            struct nvchan_buffer *bo_buf = ch ? ce_bo_buffer(dev, m->bo) : NULL;
            /* Mapped before a fork(): staging belongs to the parent. */
            int ret = m->ce_dev != dev->ce_dev ? -EPERM :
                      bo_buf ? ce_transfer(dev, ch, bo_buf, m, true) : -ENODEV;
            if (ret != 0)
                fprintf(stderr, "nvidia-gbm: writing back a block-linear map failed: %s\n",
                        strerror(-ret));
        }
        nvchan_buffer_destroy(m->staging);
        pthread_mutex_unlock(&dev->ce_lock);
    }
    free(m);
}

/* Copies raw bytes to the start of a linear BO (cursor images), like Mesa's
 * backend does for dumb buffers. */
static int nv_gbm_bo_write(struct gbm_bo *_bo, const void *buf, size_t count)
{
    struct nv_gbm_bo *bo = nv_gbm_bo(_bo);
    struct nv_gbm_device *dev = nv_gbm_device(_bo->gbm);

    if (bo->modifier != DRM_FORMAT_MOD_LINEAR || count > bo->size - bo->offset[0]) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&dev->lock);
    int ret = bo_cpu_map(dev, bo);
    pthread_mutex_unlock(&dev->lock);
    if (ret != 0) {
        errno = -ret;
        return -1;
    }
    memcpy(bo->cpu + bo->offset[0], buf, count);
    return 0;
}

static bool valid_plane(const struct nv_gbm_bo *bo, int plane)
{
    if (plane >= 0 && (unsigned)plane < bo->num_planes)
        return true;
    errno = EINVAL;
    return false;
}

static int export_handle(struct gbm_bo *_bo, uint32_t handle)
{
    int fd;

    if (drmPrimeHandleToFD(_bo->gbm->v0.fd, handle, DRM_CLOEXEC | DRM_RDWR, &fd) != 0)
        return -1;
    return fd;
}

static int nv_gbm_bo_get_fd(struct gbm_bo *_bo)
{
    return export_handle(_bo, nv_gbm_bo(_bo)->handle[0]);
}

static int nv_gbm_bo_get_planes(struct gbm_bo *bo)
{
    return (int)nv_gbm_bo(bo)->num_planes;
}

static union gbm_bo_handle nv_gbm_bo_get_handle(struct gbm_bo *_bo, int plane)
{
    struct nv_gbm_bo *bo = nv_gbm_bo(_bo);
    union gbm_bo_handle h = { .s64 = -1 };

    if (valid_plane(bo, plane))
        h.u32 = bo->handle[plane];
    return h;
}

static int nv_gbm_bo_get_plane_fd(struct gbm_bo *_bo, int plane)
{
    struct nv_gbm_bo *bo = nv_gbm_bo(_bo);

    return valid_plane(bo, plane) ? export_handle(_bo, bo->handle[plane]) : -1;
}

static uint32_t nv_gbm_bo_get_stride(struct gbm_bo *_bo, int plane)
{
    struct nv_gbm_bo *bo = nv_gbm_bo(_bo);

    return valid_plane(bo, plane) ? bo->stride[plane] : 0;
}

static uint32_t nv_gbm_bo_get_offset(struct gbm_bo *_bo, int plane)
{
    struct nv_gbm_bo *bo = nv_gbm_bo(_bo);

    return valid_plane(bo, plane) ? bo->offset[plane] : 0;
}

static uint64_t nv_gbm_bo_get_modifier(struct gbm_bo *bo)
{
    return nv_gbm_bo(bo)->modifier;
}

static void nv_gbm_bo_destroy(struct gbm_bo *_bo)
{
    struct nv_gbm_bo *bo = nv_gbm_bo(_bo);

    struct nv_gbm_device *dev = nv_gbm_device(_bo->gbm);

    /* The loader has already called destroy_user_data. */
    if (bo->cpu != NULL)
        munmap(bo->cpu, bo->cpu_size);
    if (bo->ce_buf != NULL) {
        pthread_mutex_lock(&dev->ce_lock);
        nvchan_buffer_destroy(bo->ce_buf);
        pthread_mutex_unlock(&dev->ce_lock);
    }
    bo_unref_handles(dev, bo, bo->num_planes);
    free(bo);
}

/* ---- surfaces ------------------------------------------------------------ */

/*
 * Window surfaces are implemented by libnvidia-egl-gbm, not here. When an EGL
 * display is initialized on the device it replaces the device's
 * surface_lock_front_buffer, surface_release_buffer and
 * surface_has_free_buffers with its own. At eglCreateWindowSurface it reads
 * the parameters from struct gbm_surface v0 (the modifiers go to eglcore,
 * which allocates the images), and lock_front_buffer imports each image with
 * our bo_import(GBM_BO_IMPORT_FD_MODIFIER).
 *
 * egl-gbm keeps its surface pointer in the pointer-sized slot right before
 * the struct gbm_surface the backend returns (GetPrivPtr() in its
 * gbm-surface.c), as in NVIDIA's own backend. That layout is ABI.
 */
struct nv_gbm_surface {
    void *egl_gbm_priv;   /* owned by libnvidia-egl-gbm */
    struct gbm_surface base;
};

_Static_assert(offsetof(struct nv_gbm_surface, base) == sizeof(void *),
               "libnvidia-egl-gbm expects its pointer right before gbm_surface");

static inline struct nv_gbm_surface *nv_gbm_surface(struct gbm_surface *s)
{
    return (struct nv_gbm_surface *)((uint8_t *)s - offsetof(struct nv_gbm_surface, base));
}

static struct gbm_surface *nv_gbm_surface_create(struct gbm_device *gbm,
                                                 uint32_t width, uint32_t height,
                                                 uint32_t format, uint32_t flags,
                                                 const uint64_t *modifiers,
                                                 const unsigned count)
{
    struct nv_gbm_surface *surf;

    format = canonicalize_format(format);
    if (find_format(format) == NULL || width == 0 || height == 0 ||
        width > NV_MAX_DIMENSION || height > NV_MAX_DIMENSION ||
        (count > 0 && modifiers == NULL)) {
        errno = EINVAL;
        return NULL;
    }

    surf = calloc(1, sizeof(*surf));
    if (surf == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    /* Passed on unfiltered: eglcore allocates the images and may know
     * modifiers we don't; importing them works for any modifier. */
    if (count > 0) {
        surf->base.v0.modifiers = calloc(count, sizeof(*modifiers));
        if (surf->base.v0.modifiers == NULL) {
            free(surf);
            errno = ENOMEM;
            return NULL;
        }
        memcpy(surf->base.v0.modifiers, modifiers, count * sizeof(*modifiers));
        surf->base.v0.count = count;
    }

    surf->base.gbm = gbm;
    surf->base.v0.width = width;
    surf->base.v0.height = height;
    surf->base.v0.format = format;
    surf->base.v0.flags = flags;
    return &surf->base;
}

/* Used until an EGL display is initialized on the device. */
static struct gbm_bo *nv_gbm_surface_lock_front_buffer(struct gbm_surface *surface)
{
    errno = EINVAL;
    return NULL;
}

static void nv_gbm_surface_release_buffer(struct gbm_surface *surface,
                                          struct gbm_bo *bo)
{
}

static int nv_gbm_surface_has_free_buffers(struct gbm_surface *surface)
{
    return 0;
}

static void nv_gbm_surface_destroy(struct gbm_surface *surface)
{
    struct nv_gbm_surface *surf = nv_gbm_surface(surface);

    /* egl-gbm's own surface state goes away with the EGLSurface. */
    free(surf->base.v0.modifiers);
    free(surf);
}

/* ---- backend ------------------------------------------------------------- */

static int nv_gbm_is_nvidia_drm(int fd)
{
    drmVersionPtr ver = drmGetVersion(fd);
    int ret;

    if (ver == NULL)
        return 0;
    ret = ver->name != NULL && strcmp(ver->name, "nvidia-drm") == 0;
    drmFreeVersion(ver);
    return ret;
}

static struct gbm_device *nv_gbm_create_device(int fd, uint32_t gbm_backend_version);

static const struct gbm_backend nv_gbm_backend = {
    .v0.backend_version = GBM_BACKEND_ABI_VERSION,
    .v0.backend_name = NV_GBM_BACKEND_NAME,
    .v0.create_device = nv_gbm_create_device,
};

static struct gbm_device *nv_gbm_create_device(int fd, uint32_t gbm_backend_version)
{
    struct nv_gbm_device *dev;

    if (!nv_gbm_is_nvidia_drm(fd))
        return NULL;

    dev = calloc(1, sizeof(*dev));
    if (dev == NULL)
        return NULL;

    if (drmIoctl(fd, DRM_IOCTL_NVIDIA_GET_DEV_INFO, &dev->info) != 0 ||
        !dev->info.supports_alloc) {
        free(dev);
        return NULL;
    }
    dev->compression = dev->info.page_kind_generation == 2;
    pthread_mutex_init(&dev->lock, NULL);
    pthread_mutex_init(&dev->ce_lock, NULL);
    /* Off while NVIDIA's closed GL is the renderer. NV_GBM_LINEAR_RENDER=1
     * turns it on for a renderer that supports it. */
    const char *env = getenv("NV_GBM_LINEAR_RENDER");
    dev->linear_render = env != NULL && strcmp(env, "1") == 0;

    dev->base.v0.backend_desc = NULL; /* filled in by the loader */
    dev->base.v0.backend_version = gbm_backend_version;
    dev->base.v0.fd = fd;
    /* What gbm_device_get_backend_name() returns and egl-gbm checks. */
    dev->base.v0.name = NV_GBM_BACKEND_NAME;

    dev->base.v0.destroy = nv_gbm_destroy;
    dev->base.v0.is_format_supported = nv_gbm_is_format_supported;
    dev->base.v0.get_format_modifier_plane_count =
        nv_gbm_get_format_modifier_plane_count;
    dev->base.v0.bo_create = nv_gbm_bo_create;
    dev->base.v0.bo_import = nv_gbm_bo_import;
    dev->base.v0.bo_map = nv_gbm_bo_map;
    dev->base.v0.bo_unmap = nv_gbm_bo_unmap;
    dev->base.v0.bo_write = nv_gbm_bo_write;
    dev->base.v0.bo_get_fd = nv_gbm_bo_get_fd;
    dev->base.v0.bo_get_planes = nv_gbm_bo_get_planes;
    dev->base.v0.bo_get_handle = nv_gbm_bo_get_handle;
    dev->base.v0.bo_get_plane_fd = nv_gbm_bo_get_plane_fd;
    dev->base.v0.bo_get_stride = nv_gbm_bo_get_stride;
    dev->base.v0.bo_get_offset = nv_gbm_bo_get_offset;
    dev->base.v0.bo_get_modifier = nv_gbm_bo_get_modifier;
    dev->base.v0.bo_destroy = nv_gbm_bo_destroy;
    dev->base.v0.surface_create = nv_gbm_surface_create;
    dev->base.v0.surface_lock_front_buffer = nv_gbm_surface_lock_front_buffer;
    dev->base.v0.surface_release_buffer = nv_gbm_surface_release_buffer;
    dev->base.v0.surface_has_free_buffers = nv_gbm_surface_has_free_buffers;
    dev->base.v0.surface_destroy = nv_gbm_surface_destroy;

    return &dev->base;
}

NV_GBM_EXPORT const struct gbm_backend *gbmint_get_backend(const struct gbm_core *gbm_core)
{
    core = gbm_core;
    return &nv_gbm_backend;
}
