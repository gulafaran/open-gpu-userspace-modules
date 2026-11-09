/*
 * SPDX-License-Identifier: MIT
 *
 * Process-wide GEM handle reference counts, with the ABI of the closed
 * libnvidia-allocator.so.1. NVIDIA's EGL, GL and Vulkan libraries dlopen
 * that library by name and look these two functions up; the GBM backend
 * uses them too, so everyone in the process shares one table.
 */

#ifndef NV_GEM_REF_H
#define NV_GEM_REF_H

#include <stdint.h>

#define NV_GEM_REF_EXPORT __attribute__((visibility("default")))

/*
 * Takes a reference on the handle import(arg) returns on drm_fd.
 * import runs with the table locked, so a concurrent last
 * GEMRemoveReference can't close the handle between the kernel handing it
 * out and it being counted. Returns the handle, or 0 if import returned 0,
 * drm_fd isn't an nvidia-drm file, or the table couldn't grow (then a
 * handle that nobody else holds is closed again).
 */
NV_GEM_REF_EXPORT uint32_t GEMAddReference(int drm_fd,
                                           uint32_t (*import)(void *arg),
                                           void *arg);

/* Drops a reference; the last one closes the handle. Unknown handles are
 * ignored. */
NV_GEM_REF_EXPORT void GEMRemoveReference(int drm_fd, uint32_t handle);

#endif
