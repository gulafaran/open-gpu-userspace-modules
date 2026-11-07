/*
 * SPDX-License-Identifier: MIT
 *
 * Opens the first nvidia-drm render node, creates a GBM device on it and
 * reports which backend libgbm loaded. Run against the build tree with:
 *
 *   GBM_BACKENDS_PATH=build/nvidia-gbm GBM_BACKEND=nvidia-open build/nvidia-gbm/gbm-probe
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>

static int open_nvidia_render_node(void)
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
        if (v && strcmp(v->name, "nvidia-drm") == 0) {
            printf("node: %s\n", devs[i]->nodes[DRM_NODE_RENDER]);
            fd = f;
        } else {
            close(f);
        }
        drmFreeVersion(v);
    }
    if (n > 0)
        drmFreeDevices(devs, n);
    return fd;
}

int main(void)
{
    int fd = open_nvidia_render_node();
    if (fd < 0) {
        fprintf(stderr, "no nvidia-drm render node\n");
        return 1;
    }

    struct gbm_device *gbm = gbm_create_device(fd);
    if (gbm == NULL) {
        fprintf(stderr, "gbm_create_device failed\n");
        return 1;
    }
    printf("backend: %s\n", gbm_device_get_backend_name(gbm));
    printf("XRGB8888 scanout supported: %d\n",
           gbm_device_is_format_supported(gbm, GBM_FORMAT_XRGB8888,
                                          GBM_BO_USE_SCANOUT));

    gbm_device_destroy(gbm);
    close(fd);
    return 0;
}
