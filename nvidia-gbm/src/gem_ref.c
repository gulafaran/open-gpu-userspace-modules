/*
 * SPDX-License-Identifier: MIT
 *
 * GEM handles are per DRM file and not reference counted by the kernel:
 * importing a dma-buf whose object already has a handle on that file returns
 * the same handle, and closing it once invalidates it for every user. So
 * everyone in the process who gets handles on a file counts them here, keyed
 * by the file's nvidia-drm unique id (fds dup'ed from one open share a file)
 * and the handle, and only the last reference closes it.
 *
 * Behaviour matches the closed libnvidia-allocator.so.1 (620.70.06):
 * failing to get the unique id makes GEMAddReference return 0 without
 * calling import and GEMRemoveReference do nothing; removing an unknown
 * handle doesn't close it.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include <xf86drm.h>

#include "nvidia-drm/nv_drm_common_ioctl.h"
#include "gem_ref.h"

struct gem_ref {
    uint64_t file_id;
    uint32_t handle;
    uint32_t refs;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct gem_ref *refs;
static size_t num_refs, max_refs;

static bool get_file_id(int fd, uint64_t *id)
{
    struct drm_nvidia_get_drm_file_unique_id_params p = { 0 };

    if (drmIoctl(fd, DRM_IOCTL_NVIDIA_GET_DRM_FILE_UNIQUE_ID, &p) != 0)
        return false;
    *id = p.id;
    return true;
}

static struct gem_ref *find(uint64_t file_id, uint32_t handle)
{
    for (size_t i = 0; i < num_refs; i++) {
        if (refs[i].handle == handle && refs[i].file_id == file_id)
            return &refs[i];
    }
    return NULL;
}

uint32_t GEMAddReference(int drm_fd, uint32_t (*import)(void *arg), void *arg)
{
    uint64_t file_id;

    if (!get_file_id(drm_fd, &file_id))
        return 0;

    pthread_mutex_lock(&lock);

    const uint32_t handle = import(arg);
    if (handle == 0)
        goto out;

    struct gem_ref *ref = find(file_id, handle);
    if (ref != NULL) {
        ref->refs++;
        goto out;
    }

    if (num_refs == max_refs) {
        size_t max = max_refs ? max_refs * 2 : 64;
        struct gem_ref *r = realloc(refs, max * sizeof(*r));
        if (r == NULL) {
            /* Nobody else holds it (not in the table), so it's ours to drop. */
            const int err = errno;
            drmCloseBufferHandle(drm_fd, handle);
            pthread_mutex_unlock(&lock);
            errno = err;
            return 0;
        }
        refs = r;
        max_refs = max;
    }
    refs[num_refs++] = (struct gem_ref){ file_id, handle, 1 };

out:
    pthread_mutex_unlock(&lock);
    return handle;
}

void GEMRemoveReference(int drm_fd, uint32_t handle)
{
    uint64_t file_id;

    if (!get_file_id(drm_fd, &file_id))
        return;

    pthread_mutex_lock(&lock);
    struct gem_ref *ref = find(file_id, handle);
    if (ref != NULL && --ref->refs == 0) {
        drmCloseBufferHandle(drm_fd, handle);
        *ref = refs[--num_refs];
    }
    pthread_mutex_unlock(&lock);
}
