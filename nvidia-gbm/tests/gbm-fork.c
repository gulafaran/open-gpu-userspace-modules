/*
 * SPDX-License-Identifier: MIT
 *
 * gbm_bo_map of a block-linear BO across fork(): the parent maps once (that
 * creates the backend's copy engine channel), forks, the child maps the
 * same BO (the backend must use a channel of its own, not the parent's),
 * and the parent maps again afterwards.
 *
 *   GBM_BACKENDS_PATH=build/nvidia-gbm GBM_BACKEND=nvidia-open build/nvidia-gbm/gbm-fork
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>

static int open_nvidia(void)
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
        if (v && strcmp(v->name, "nvidia-drm") == 0)
            fd = f;
        else
            close(f);
        drmFreeVersion(v);
    }
    if (n > 0)
        drmFreeDevices(devs, n);
    return fd;
}

/* Writes value everywhere through a write map, then checks it through a
 * read map. */
static bool write_read(struct gbm_bo *bo, uint32_t value)
{
    const uint32_t w = gbm_bo_get_width(bo), h = gbm_bo_get_height(bo);
    uint32_t stride;
    void *md = NULL;
    uint8_t *p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &stride, &md);

    if (p == NULL)
        return false;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            ((uint32_t *)(p + (size_t)y * stride))[x] = value ^ (y << 12 | x);
    gbm_bo_unmap(bo, md);

    p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &stride, &md);
    if (p == NULL)
        return false;
    bool ok = true;
    for (uint32_t y = 0; y < h && ok; y++)
        for (uint32_t x = 0; x < w && ok; x++)
            ok = ((uint32_t *)(p + (size_t)y * stride))[x] == (value ^ (y << 12 | x));
    gbm_bo_unmap(bo, md);
    return ok;
}

int main(void)
{
    int fd = open_nvidia();
    struct gbm_device *gbm = fd >= 0 ? gbm_create_device(fd) : NULL;
    if (gbm == NULL) {
        fprintf(stderr, "no GBM device\n");
        return 1;
    }
    struct gbm_bo *bo = gbm_bo_create(gbm, 640, 480, GBM_FORMAT_XRGB8888,
                                      GBM_BO_USE_RENDERING);
    if (bo == NULL || gbm_bo_get_modifier(bo) == DRM_FORMAT_MOD_LINEAR) {
        fprintf(stderr, "need a block-linear BO\n");
        return 1;
    }
    printf("backend: %s\n", gbm_device_get_backend_name(gbm));

    bool parent_before = write_read(bo, 0x11000000);
    printf("  %s  parent maps before fork\n", parent_before ? "ok  " : "FAIL");
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0)
        _exit(write_read(bo, 0x22000000) ? 0 : 1);
    int status = 0;
    waitpid(pid, &status, 0);
    bool child = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("  %s  child maps the inherited BO\n", child ? "ok  " : "FAIL");

    bool parent_after = write_read(bo, 0x33000000);
    printf("  %s  parent maps after the child exited\n", parent_after ? "ok  " : "FAIL");

    int failures = !parent_before + !child + !parent_after;
    printf("\n%d failure(s)\n", failures);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    close(fd);
    return failures != 0;
}
