/*
 * SPDX-License-Identifier: MIT
 *
 * User-space RM API over the /dev/nvidiactl and /dev/nvidiaN ioctls of the
 * open kernel modules (src/nvidia/arch/nvalloc/unix/src/escape.c).
 *
 * The RM structures come from the kernel tree nvchan is built against and
 * must match the running nvidia.ko; nvc_rm_open() checks the version.
 *
 * CPU mappings: RM_ALLOC_MEMORY (system memory) and RM_MAP_MEMORY (video
 * memory, BAR0 regions) leave a mapping context on a separate, freshly
 * opened file, which is then mmap()ed at offset 0. A file holds one context,
 * so every mapping gets its own file, kept open until it is unmapped.
 * System memory is mapped through a control device file, everything else
 * through a GPU device file.
 */

#ifndef NVCHAN_RM_H
#define NVCHAN_RM_H

#include <pthread.h>
#include <stddef.h>

#include "nvtypes.h"
#include "nvos.h"

struct nvc_rm_mapping;

struct nvc_rm {
    int ctl_fd;             /* /dev/nvidiactl, owns the client */
    int dev_fd;             /* /dev/nvidiaN, registered with ctl_fd */
    int minor;
    NvHandle client;
    NvU32 next_handle;
    char version[64];       /* of the running kernel module */

    pthread_mutex_t lock;   /* mappings, sysmem handles, next_handle */
    struct nvc_rm_mapping *mappings;
    NvHandle *sysmem;       /* memory objects mapped through a ctl file */
    unsigned num_sysmem, max_sysmem;
};

/* Opens /dev/nvidiactl, checks the module version against the headers'
 * and allocates a client. Returns 0 or -errno; on a version mismatch
 * rm->version says what is running. */
int nvc_rm_open(struct nvc_rm *rm);

/* Opens /dev/nvidia<minor> and registers it with the client's file, which
 * keeps the GPU initialized. */
int nvc_rm_open_gpu(struct nvc_rm *rm, int minor);

void nvc_rm_close(struct nvc_rm *rm);

/* A new client-unique object handle. */
NvHandle nvc_rm_handle(struct nvc_rm *rm);

/* RM calls; they return an NV_STATUS. */
NvU32 nvc_rm_alloc(struct nvc_rm *rm, NvHandle parent, NvHandle object,
                   NvU32 cls, void *params);
NvU32 nvc_rm_free(struct nvc_rm *rm, NvHandle parent, NvHandle object);
NvU32 nvc_rm_control(struct nvc_rm *rm, NvHandle object, NvU32 cmd,
                     void *params, NvU32 size);
NvU32 nvc_rm_vid_heap_control(struct nvc_rm *rm, NVOS32_PARAMETERS *params);

/* NV01_ALLOC_MEMORY; for system memory *cpu returns a CPU mapping when the
 * caller passes a non-NULL cpu pointer. */
NvU32 nvc_rm_alloc_memory(struct nvc_rm *rm, NvHandle parent, NvHandle memory,
                          NvU32 cls, NvU32 flags, void **cpu, NvU64 *limit);

NvU32 nvc_rm_map(struct nvc_rm *rm, NvHandle device, NvHandle memory,
                 NvU64 offset, NvU64 length, void **cpu, NvU32 flags);
NvU32 nvc_rm_unmap(struct nvc_rm *rm, NvHandle device, NvHandle memory,
                   void *cpu, NvU32 flags);

NvU32 nvc_rm_map_dma(struct nvc_rm *rm, NvHandle device, NvHandle dma,
                     NvHandle memory, NvU64 offset, NvU64 length, NvU32 flags,
                     NvU64 *dma_offset);
NvU32 nvc_rm_unmap_dma(struct nvc_rm *rm, NvHandle device, NvHandle dma,
                       NvHandle memory, NvU32 flags, NvU64 dma_offset);

#endif
