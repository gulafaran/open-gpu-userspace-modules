/*
 * SPDX-License-Identifier: MIT
 */

#ifndef NVCHAN_PRIV_H
#define NVCHAN_PRIV_H

#include "nvchan.h"
#include "rm.h"

#include "nvidia-push-init.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"

struct nvchan_device {
    struct nvc_rm rm;

    NvU32 gpu_id;
    NvU32 device_instance;
    NvHandle device;
    NvHandle subdevice;
    NvHandle va_space;      /* NV01_MEMORY_VIRTUAL, ctxDma for MapMemoryDma */
    NvU32 push_handles[NV_PUSH_DEVICE_HANDLE_POOL_NUM];
    bool push_device_allocated;

    NvU32 classes[NV0080_CTRL_GPU_CLASSLIST_MAX_SIZE];
    unsigned num_classes;
    NvU32 engines[NV2080_GPU_MAX_ENGINES_LIST_SIZE];
    unsigned num_engines;
    NvU32 copy_class;

    char name[NV2080_GPU_MAX_NAME_STRING_LENGTH];

    NvPushDeviceRec push;
};

struct nvchan_channel {
    struct nvchan_device *dev;
    NvPushChannelRec push;
    NvU32 engine_type;
    NvHandle copy_object;

    /* Completion semaphore the copy engine releases (system memory). */
    struct nvchan_buffer *sema;
    volatile NvU32 *sema_cpu;
    NvU32 sema_payload;

    /* Non-stall interrupt of our copy engine, delivered to event_fd
     * (poll() for POLLIN); -1 if unavailable, then finish() polls the
     * semaphore with sleeps. */
    int event_fd;
    NvHandle event;
};

struct nvchan_buffer {
    struct nvchan_device *dev;
    NvHandle memory;
    NvU64 size;
    NvU64 gpu_va;
    enum nvchan_location location;
    enum nvchan_layout layout;
    void *cpu;              /* CPU mapping, made on first nvchan_buffer_map */
    bool cpu_from_alloc;    /* system memory: mapped at allocation */
};

void nvc_set_err(char *err, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Copy engine object binding and the completion semaphore (ce.c). */
bool nvc_channel_init_copy(struct nvchan_channel *ch, char *err, size_t err_size);
void nvc_channel_fini_copy(struct nvchan_channel *ch);

#endif
