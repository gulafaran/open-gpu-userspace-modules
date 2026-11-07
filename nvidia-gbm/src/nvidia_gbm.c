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
 * GEM handles are per DRM file and not reference counted: importing a
 * dma-buf whose object already has a handle on this fd (one of our own BOs,
 * or the same client buffer twice) returns that same handle, and closing it
 * once invalidates it for everyone. The device therefore counts references
 * to each handle and only closes it when the last BO using it goes away.
 *
 * Every gbm_device_v0 entry point must be non-NULL because the loader calls
 * them without checking.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <drm_fourcc.h>
#include <gbm_backend_abi.h>

#include "nvidia-drm/nv_drm_common_ioctl.h"

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
    { DRM_FORMAT_ABGR16161616F, 1, { 8 }, 1, 1, true },
    { DRM_FORMAT_XBGR16161616F, 1, { 8 }, 1, 1, true },
    { DRM_FORMAT_RGB565,        1, { 2 }, 1, 1, true },
    { DRM_FORMAT_R8,            1, { 1 }, 1, 1, true },
    { DRM_FORMAT_GR88,          1, { 2 }, 1, 1, false },
    { DRM_FORMAT_NV12,          2, { 1, 2 }, 2, 2, true },
};

struct nv_gbm_handle_ref {
    uint32_t handle;
    uint32_t refs;
};

struct nv_gbm_device {
    struct gbm_device base;
    struct drm_nvidia_get_dev_info_params info;
    bool compression;   /* we know the compressed kind for this generation */
    bool linear_render; /* the renderer can render to linear, see linear_allowed() */

    /* Protects the handle table, and spans getting a handle from the kernel
     * (import) or closing it, so a handle number can't be reused in between. */
    pthread_mutex_t lock;
    struct nv_gbm_handle_ref *refs;
    unsigned num_refs, max_refs;
};

struct nv_gbm_bo {
    struct gbm_bo base;
    uint64_t modifier;
    uint64_t size;
    const struct nv_gbm_format *fmt;  /* NULL for imports of unknown formats */
    unsigned num_planes;
    /* One reference in the device's handle table per plane. */
    uint32_t handle[GBM_MAX_PLANES];
    uint32_t stride[GBM_MAX_PLANES];
    uint32_t offset[GBM_MAX_PLANES];

    /* CPU mapping of the whole BO, made on the first gbm_bo_map and kept
     * until the BO is destroyed (dev->lock). */
    uint8_t *cpu;
    size_t cpu_size;
};

/* What gbm_bo_map returns as map_data. */
struct nv_gbm_map {
    struct nv_gbm_bo *bo;
    uint8_t *staging;   /* detiled copy of a block-linear region, or NULL */
    uint32_t x, y, width, height;
    uint32_t flags;
    uint32_t stride;    /* of staging */
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

/* ---- GEM handle references (dev->lock held) -------------------------------- */

static int handle_ref(struct nv_gbm_device *dev, uint32_t handle)
{
    for (unsigned i = 0; i < dev->num_refs; i++) {
        if (dev->refs[i].handle == handle) {
            dev->refs[i].refs++;
            return 0;
        }
    }

    if (dev->num_refs == dev->max_refs) {
        unsigned max = dev->max_refs ? dev->max_refs * 2 : 64;
        struct nv_gbm_handle_ref *refs = realloc(dev->refs, max * sizeof(*refs));
        if (refs == NULL)
            return -ENOMEM;
        dev->refs = refs;
        dev->max_refs = max;
    }
    dev->refs[dev->num_refs++] = (struct nv_gbm_handle_ref){ handle, 1 };
    return 0;
}

static void handle_unref(struct nv_gbm_device *dev, uint32_t handle)
{
    for (unsigned i = 0; i < dev->num_refs; i++) {
        if (dev->refs[i].handle != handle)
            continue;
        if (--dev->refs[i].refs == 0) {
            dev->refs[i] = dev->refs[--dev->num_refs];
            drmCloseBufferHandle(dev->base.v0.fd, handle);
        }
        return;
    }
}

static void bo_unref_handles(struct nv_gbm_device *dev, struct nv_gbm_bo *bo,
                             unsigned num)
{
    for (unsigned p = 0; p < num; p++)
        handle_unref(dev, bo->handle[p]);
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
 * taller than the surface, at most 16 GOBs (128 rows). */
static uint32_t preferred_log2_gobs(uint32_t height)
{
    uint32_t h = floor_log2(DIV_ROUND_UP(height, NV_GOB_HEIGHT_ROWS));
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

    /* Handles still referenced belong to leaked BOs; the fd owns them. */
    pthread_mutex_destroy(&dev->lock);
    free(dev->refs);
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

/* Allocates the memory and takes the plane references on its handle. */
static int alloc_gem(struct nv_gbm_device *dev, struct nv_gbm_bo *bo,
                     bool scanout, bool *compressed)
{
    struct drm_nvidia_gem_alloc_nvkms_memory_params p = {
        .block_linear = bo->modifier != DRM_FORMAT_MOD_LINEAR,
        .compressible = *compressed,
        .memory_size = bo->size,
        .flags = scanout ? 0 : NV_GEM_ALLOC_NO_SCANOUT,
    };

    if (drmIoctl(dev->base.v0.fd, DRM_IOCTL_NVIDIA_GEM_ALLOC_NVKMS_MEMORY, &p) != 0)
        return -errno;

    *compressed = p.compressible != 0;

    pthread_mutex_lock(&dev->lock);
    for (unsigned i = 0; i < bo->num_planes; i++) {
        int ret = handle_ref(dev, p.handle);
        if (ret != 0) {
            if (i == 0)
                drmCloseBufferHandle(dev->base.v0.fd, p.handle);
            else
                bo_unref_handles(dev, bo, i);
            pthread_mutex_unlock(&dev->lock);
            return ret;
        }
        bo->handle[i] = p.handle;
    }
    pthread_mutex_unlock(&dev->lock);
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
            pthread_mutex_lock(&dev->lock);
            bo_unref_handles(dev, bo, bo->num_planes);
            pthread_mutex_unlock(&dev->lock);
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
        /* The importer has to know the layout some other way, as with
         * Mesa's backend. */
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

    pthread_mutex_lock(&dev->lock);
    for (unsigned p = 0; p < num_fds; p++) {
        uint32_t handle = 0;
        const bool got = drmPrimeFDToHandle(gbm->v0.fd, fds[p], &handle) == 0;
        int ret = got ? handle_ref(dev, handle) : -errno;

        if (ret != 0) {
            /* A handle we got but couldn't count is ours to drop only if
             * nobody else holds it. */
            if (got) {
                bool known = false;
                for (unsigned i = 0; i < dev->num_refs && !known; i++)
                    known = dev->refs[i].handle == handle;
                if (!known)
                    drmCloseBufferHandle(gbm->v0.fd, handle);
            }
            bo_unref_handles(dev, bo, p);
            pthread_mutex_unlock(&dev->lock);
            free(bo);
            errno = -ret;
            return NULL;
        }
        bo->handle[p] = handle;
        bo->stride[p] = (uint32_t)strides[p];
        bo->offset[p] = (uint32_t)offsets[p];
    }
    pthread_mutex_unlock(&dev->lock);

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
 * The CPU sees the memory as laid out by the GPU: linear BOs are mapped
 * directly, block-linear ones are detiled into a staging copy on map and
 * tiled back on unmap.
 *
 * nvidia-drm can only map physically contiguous video memory while all of
 * it is statically mapped through BAR1 (resizable BAR): scanout BOs, which
 * includes cursors, and BOs of a single page. Other maps fail with ENOMEM
 * until nvidia-drm maps each BAR1 range separately.
 */

/*
 * Byte offset of byte xb of row y in a block-linear plane, for Turing+ color
 * GOBs (64 B x 8 rows, 512 B): two 32 B halves of 256 B, each four rows of
 * two 16 B sectors, then the other four rows. A block is 2^log2_gobs GOBs
 * stacked vertically; blocks are laid out row by row. Matches Mesa's
 * CopyGOBTuring2D and was verified against GL rendering (tests/gbm-map).
 *
 * Blackwell keeps this layout for >= 32 bpp but uses others for 8 and 16
 * bpp, which we can't tell apart yet, so only cpp >= 4 is detiled.
 */
static size_t bl_offset(uint32_t xb, uint32_t y, uint32_t stride, uint32_t log2_gobs)
{
    const uint32_t gob_x = xb / NV_GOB_WIDTH_BYTES, gob_y = y / NV_GOB_HEIGHT_ROWS;
    const uint32_t bx = xb % NV_GOB_WIDTH_BYTES, by = y % NV_GOB_HEIGHT_ROWS;
    const size_t gob_bytes = NV_GOB_WIDTH_BYTES * NV_GOB_HEIGHT_ROWS;
    const size_t block = (size_t)(gob_y >> log2_gobs) * (stride / NV_GOB_WIDTH_BYTES) + gob_x;
    const size_t gob = (block << log2_gobs) + (gob_y & ((1u << log2_gobs) - 1));

    return gob * gob_bytes + (bx >> 5) * 256 + (by >> 2) * 128 +
           ((bx >> 4) & 1) * 64 + (by & 3) * 16 + (bx & 15);
}

/* Copies a rectangle between block-linear memory and a linear buffer, in
 * runs of at most one 16 B sector, which are contiguous in both. */
static void bl_copy(uint8_t *tiled, uint32_t tiled_stride, uint32_t log2_gobs,
                    uint8_t *lin, uint32_t lin_stride,
                    uint32_t xb, uint32_t y, uint32_t wb, uint32_t h, bool to_tiled)
{
    for (uint32_t row = 0; row < h; row++) {
        uint8_t *l = lin + (size_t)row * lin_stride;

        for (uint32_t b = 0; b < wb;) {
            const uint32_t x = xb + b;
            const uint32_t run = 16 - (x & 15) < wb - b ? 16 - (x & 15) : wb - b;
            uint8_t *t = tiled + bl_offset(x, y + row, tiled_stride, log2_gobs);

            if (to_tiled)
                memcpy(t, l + b, run);
            else
                memcpy(l + b, t, run);
            b += run;
        }
    }
}

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

static bool bo_cpu_detilable(const struct nv_gbm_device *dev, const struct nv_gbm_bo *bo)
{
    bool compressed;

    return is_nv_block_linear(bo->modifier) &&
           modifier_supported(dev, bo->fmt, bo->modifier, &compressed) &&
           !compressed && bo->fmt->cpp[0] >= 4 &&
           bo->stride[0] % NV_GOB_WIDTH_BYTES == 0;
}

static void *nv_gbm_bo_map(struct gbm_bo *_bo, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height, uint32_t flags,
                           uint32_t *stride, void **map_data)
{
    struct nv_gbm_bo *bo = nv_gbm_bo(_bo);
    struct nv_gbm_device *dev = nv_gbm_device(_bo->gbm);
    const bool linear = bo->modifier == DRM_FORMAT_MOD_LINEAR;

    /* Plane 0 only, like the other backends. Compressed memory, layouts we
     * can't detile and imports of unknown format are refused. */
    if (bo->fmt == NULL || !(flags & GBM_BO_TRANSFER_READ_WRITE) ||
        width == 0 || height == 0 ||
        (uint64_t)x + width > _bo->v0.width || (uint64_t)y + height > _bo->v0.height ||
        (!linear && !bo_cpu_detilable(dev, bo))) {
        errno = EINVAL;
        return NULL;
    }

    struct nv_gbm_map *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    *m = (struct nv_gbm_map){ bo, NULL, x, y, width, height, flags, 0 };

    pthread_mutex_lock(&dev->lock);
    int ret = bo_cpu_map(dev, bo);
    pthread_mutex_unlock(&dev->lock);
    if (ret != 0) {
        free(m);
        errno = -ret;
        return NULL;
    }

    const uint32_t cpp = bo->fmt->cpp[0];
    uint8_t *plane = bo->cpu + bo->offset[0];

    if (linear) {
        *stride = bo->stride[0];
        *map_data = m;
        return plane + (size_t)y * bo->stride[0] + (size_t)x * cpp;
    }

    m->stride = ALIGN(width * cpp, 64u);
    m->staging = malloc((size_t)m->stride * height);
    if (m->staging == NULL) {
        free(m);
        errno = ENOMEM;
        return NULL;
    }
    if (flags & GBM_BO_TRANSFER_READ)
        bl_copy(plane, bo->stride[0], NV_MOD_H(bo->modifier), m->staging, m->stride,
                x * cpp, y, width * cpp, height, false);

    *stride = m->stride;
    *map_data = m;
    return m->staging;
}

static void nv_gbm_bo_unmap(struct gbm_bo *_bo, void *map_data)
{
    struct nv_gbm_map *m = map_data;

    if (m == NULL)
        return;
    if (m->staging != NULL) {
        struct nv_gbm_bo *bo = m->bo;
        const uint32_t cpp = bo->fmt->cpp[0];

        if (m->flags & GBM_BO_TRANSFER_WRITE)
            bl_copy(bo->cpu + bo->offset[0], bo->stride[0], NV_MOD_H(bo->modifier),
                    m->staging, m->stride, m->x * cpp, m->y, m->width * cpp,
                    m->height, true);
        free(m->staging);
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
    pthread_mutex_lock(&dev->lock);
    bo_unref_handles(dev, bo, bo->num_planes);
    pthread_mutex_unlock(&dev->lock);
    free(bo);
}

/* ---- surfaces ------------------------------------------------------------ */

static struct gbm_surface *nv_gbm_surface_create(struct gbm_device *gbm,
                                                 uint32_t width, uint32_t height,
                                                 uint32_t format, uint32_t flags,
                                                 const uint64_t *modifiers,
                                                 const unsigned count)
{
    errno = ENOSYS;
    return NULL;
}

static struct gbm_bo *nv_gbm_surface_lock_front_buffer(struct gbm_surface *surface)
{
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
