/*
 * SPDX-License-Identifier: MIT
 *
 * GBM backend for nvidia-drm.
 *
 * Skeleton: device creation and the nvidia-drm capability query are real,
 * buffer allocation is not implemented yet. Every gbm_device_v0 entry point
 * must be non-NULL because the loader calls them without checking.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include <drm_fourcc.h>
#include <gbm_backend_abi.h>

#include "nvidia-drm/nv_drm_common_ioctl.h"

#define NV_GBM_EXPORT __attribute__((visibility("default")))

/* libnvidia-egl-gbm only drives devices whose gbm_device_get_backend_name()
 * returns this. */
#define NV_GBM_BACKEND_NAME "nvidia"

struct nv_gbm_device {
    struct gbm_device base;
    struct drm_nvidia_get_dev_info_params info;
};

static const struct gbm_core *core;

static inline struct nv_gbm_device *nv_gbm_device(struct gbm_device *gbm)
{
    return (struct nv_gbm_device *)gbm;
}

static void nv_gbm_destroy(struct gbm_device *gbm)
{
    free(nv_gbm_device(gbm));
}

static int nv_gbm_is_format_supported(struct gbm_device *gbm,
                                      uint32_t format, uint32_t usage)
{
    return 0;
}

static int nv_gbm_get_format_modifier_plane_count(struct gbm_device *gbm,
                                                  uint32_t format,
                                                  uint64_t modifier)
{
    return -1;
}

static struct gbm_bo *nv_gbm_bo_create(struct gbm_device *gbm,
                                       uint32_t width, uint32_t height,
                                       uint32_t format, uint32_t usage,
                                       const uint64_t *modifiers,
                                       const unsigned int count)
{
    errno = ENOSYS;
    return NULL;
}

static struct gbm_bo *nv_gbm_bo_import(struct gbm_device *gbm, uint32_t type,
                                       void *buffer, uint32_t usage)
{
    errno = ENOSYS;
    return NULL;
}

static void *nv_gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height, uint32_t flags,
                           uint32_t *stride, void **map_data)
{
    errno = ENOSYS;
    return NULL;
}

static void nv_gbm_bo_unmap(struct gbm_bo *bo, void *map_data)
{
}

static int nv_gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t data)
{
    errno = ENOSYS;
    return -1;
}

static int nv_gbm_bo_get_fd(struct gbm_bo *bo)
{
    return -1;
}

static int nv_gbm_bo_get_planes(struct gbm_bo *bo)
{
    return 0;
}

static union gbm_bo_handle nv_gbm_bo_get_handle(struct gbm_bo *bo, int plane)
{
    union gbm_bo_handle h = { .s64 = -1 };
    return h;
}

static int nv_gbm_bo_get_plane_fd(struct gbm_bo *bo, int plane)
{
    return -1;
}

static uint32_t nv_gbm_bo_get_stride(struct gbm_bo *bo, int plane)
{
    return 0;
}

static uint32_t nv_gbm_bo_get_offset(struct gbm_bo *bo, int plane)
{
    return 0;
}

static uint64_t nv_gbm_bo_get_modifier(struct gbm_bo *bo)
{
    return DRM_FORMAT_MOD_INVALID;
}

static void nv_gbm_bo_destroy(struct gbm_bo *bo)
{
}

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
