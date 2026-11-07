/*
 * SPDX-License-Identifier: MIT
 *
 * Imports GBM buffers the way a PCIe GPU driver (i915, amdgpu) imports a
 * PRIME buffer from another device: dma_buf_attach() + dma_buf_map_
 * attachment(), which needs the exporter's scatterlist (issue 1037). The
 * dmabuf_probe test module (~/.cache/nvfix/dmabuf-probe) does that import
 * and reads bytes through the scatterlist's pages; for linear buffers they
 * are compared with a pattern written through gbm_bo_map.
 *
 * vgem can't stand in: it imports without mapping the attachment, like
 * udl and gud, so it never asks for the scatterlist.
 *
 *   pkexec insmod ~/.cache/nvfix/dmabuf-probe/dmabuf_probe.ko
 *   GBM_BACKENDS_PATH=build/nvidia-gbm GBM_BACKEND=nvidia-open build/nvidia-gbm/gbm-prime-import
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/ioctl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <i915_drm.h>

struct dmabuf_probe_req {
    int32_t fd;
    int32_t status;
    uint64_t offset;
    uint64_t total;
    uint32_t nents;
    uint32_t have_pages;
    uint8_t data[64];
};

#define DMABUF_PROBE_IOC _IOWR('D', 0x42, struct dmabuf_probe_req)

static int failures;

static int open_render(const char *driver)
{
    drmDevicePtr devs[16];
    int n = drmGetDevices2(0, devs, 16);
    int fd = -1;

    for (int i = 0; i < n && fd < 0; i++) {
        if (!(devs[i]->available_nodes & (1 << DRM_NODE_RENDER)))
            continue;
        int f = open(devs[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
        if (f < 0)
            continue;
        drmVersionPtr v = drmGetVersion(f);
        if (v && strcmp(v->name, driver) == 0)
            fd = f;
        else
            close(f);
        drmFreeVersion(v);
    }
    if (n > 0)
        drmFreeDevices(devs, n);
    return fd;
}

static uint32_t pattern(uint32_t x, uint32_t y)
{
    return 0xff000000u | (y & 0xff) << 16 | (x & 0xff) << 8 | ((x ^ y) & 0xff);
}

/* The importer's view of 16 pixels at the start of row y. */
static bool row_matches(int probe, int dmabuf, uint32_t stride, uint32_t y, int *status)
{
    struct dmabuf_probe_req req = { .fd = dmabuf, .offset = (uint64_t)y * stride };

    if (ioctl(probe, DMABUF_PROBE_IOC, &req) != 0) {
        *status = -errno;
        return false;
    }
    *status = req.status;
    if (req.status != 0 || !req.have_pages)
        return false;
    for (uint32_t x = 0; x < 16; x++) {
        uint32_t v;
        memcpy(&v, req.data + x * 4, 4);
        if (v != pattern(x, y))
            return false;
    }
    return true;
}

/*
 * The real thing in hybrid mode: import into i915, make it fetch the pages
 * (SET_DOMAIN to CPU maps the attachment) and read 64 bytes of rows 0 and
 * h-1 back through i915 where it supports pread.
 */
static void try_i915(int i915, int dmabuf, bool linear, uint32_t stride, uint32_t h,
                     bool expect_import)
{
    uint32_t handle;

    if (i915 < 0)
        return;
    printf("               i915: ");
    if (drmPrimeFDToHandle(i915, dmabuf, &handle) != 0) {
        printf("import FAILED (%s)\n", strerror(errno));
        failures += expect_import;
        return;
    }
    struct drm_i915_gem_set_domain sd = {
        .handle = handle, .read_domains = I915_GEM_DOMAIN_CPU,
    };
    if (drmIoctl(i915, DRM_IOCTL_I915_GEM_SET_DOMAIN, &sd) != 0) {
        printf("pages FAILED (%s)%s\n", strerror(errno),
               expect_import ? "" : " [video memory: expected]");
        failures += expect_import;
    } else if (!linear) {
        printf("pages ok\n");
    } else {
        uint32_t row0[16], rowl[16];
        struct drm_i915_gem_pread p0 = { .handle = handle, .size = sizeof(row0),
                                         .data_ptr = (uintptr_t)row0 };
        struct drm_i915_gem_pread pl = { .handle = handle, .size = sizeof(rowl),
                                         .offset = (uint64_t)(h - 1) * stride,
                                         .data_ptr = (uintptr_t)rowl };
        if (drmIoctl(i915, DRM_IOCTL_I915_GEM_PREAD, &p0) != 0 ||
            drmIoctl(i915, DRM_IOCTL_I915_GEM_PREAD, &pl) != 0) {
            printf("pages ok (pread: %s)\n", strerror(errno));
        } else {
            bool ok = true;
            for (uint32_t x = 0; x < 16; x++)
                ok = ok && row0[x] == pattern(x, 0) && rowl[x] == pattern(x, h - 1);
            printf("pages ok, contents %s\n", ok ? "match" : "WRONG");
            failures += !ok;
        }
    }
    drmCloseBufferHandle(i915, handle);
}

static void run(struct gbm_device *gbm, int probe, int i915, const char *tag,
                uint32_t usage, bool expect_import)
{
    const uint32_t w = 640, h = 480;
    struct gbm_bo *bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_XRGB8888, usage);

    printf("%-14s ", tag);
    if (bo == NULL) {
        printf("create failed (%s)\n", strerror(errno));
        return;
    }
    uint64_t mod = gbm_bo_get_modifier(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    bool linear = mod == DRM_FORMAT_MOD_LINEAR;
    printf("mod=0x%016" PRIx64 ": ", mod);

    if (linear) {
        uint32_t mstride;
        void *md = NULL;
        uint8_t *p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &mstride, &md);
        if (p == NULL) {
            printf("map failed (%s)\n", strerror(errno));
            failures++;
            gbm_bo_destroy(bo);
            return;
        }
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                ((uint32_t *)(p + (size_t)y * mstride))[x] = pattern(x, y);
        gbm_bo_unmap(bo, md);
    }

    int fd = gbm_bo_get_fd(bo);
    struct dmabuf_probe_req req = { .fd = fd };
    int ret = probe < 0 ? 0 : ioctl(probe, DMABUF_PROBE_IOC, &req) ? -errno : req.status;

    if (probe < 0) {
        printf("(no dmabuf_probe)\n");
    } else if (ret != 0 && ret != -ENXIO) {
        printf("import FAILED (%s)%s\n", strerror(-ret),
               expect_import ? "" : " [video memory: expected]");
        failures += expect_import;
    } else if (!linear) {
        printf("import ok (%u entries, %s)\n", req.nents,
               req.have_pages ? "pages" : "no pages");
    } else {
        int st0, st1;
        bool ok = row_matches(probe, fd, stride, 0, &st0) &&
                  row_matches(probe, fd, stride, h - 1, &st1);
        printf("import ok (%u entries), contents %s\n", req.nents,
               ok ? "match" : "WRONG");
        failures += !ok;
    }
    try_i915(i915, fd, linear, stride, h, expect_import);
    close(fd);
    gbm_bo_destroy(bo);
}

int main(void)
{
    int fd = open_render("nvidia-drm");
    int probe = open("/dev/dmabuf_probe", O_RDWR | O_CLOEXEC);
    int i915 = open_render("i915");

    if (fd < 0) {
        fprintf(stderr, "no nvidia-drm node\n");
        return 1;
    }
    if (probe < 0 && i915 < 0) {
        fprintf(stderr, "no importer: load ~/.cache/nvfix/dmabuf-probe/dmabuf_probe.ko "
                        "or run in hybrid mode (i915)\n");
        return 1;
    }
    printf("importers: %s%s\n", probe >= 0 ? "dmabuf_probe " : "", i915 >= 0 ? "i915" : "");
    struct gbm_device *gbm = gbm_create_device(fd);
    if (gbm == NULL) {
        fprintf(stderr, "gbm_create_device failed\n");
        return 1;
    }
    printf("backend: %s\n", gbm_device_get_backend_name(gbm));

    /* Linear without scanout: what goes to another GPU (issue 1037). */
    run(gbm, probe, i915, "linear", GBM_BO_USE_LINEAR, true);
    /* Scanout and block-linear memory stay in video memory. */
    run(gbm, probe, i915, "linear-scanout", GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT, false);
    run(gbm, probe, i915, "render (BL)", GBM_BO_USE_RENDERING, false);

    printf("\n%d failure(s)\n", failures);
    gbm_device_destroy(gbm);
    if (probe >= 0)
        close(probe);
    if (i915 >= 0)
        close(i915);
    close(fd);
    return failures != 0;
}
