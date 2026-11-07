/*
 * SPDX-License-Identifier: MIT
 *
 * Device bring-up and the nvidia-push host side, modelled on NVKMS
 * (nvRmAllocDeviceEvo, AllocGpuVASpace, nvAllocNvPushDevice in
 * src/nvidia-modeset/src/nvkms-rm.c and nvkms-push.c), with the RM calls
 * going through the user-space ioctls in rm.c.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <sys/ioctl.h>

#include "nvchan-priv.h"

#include "nvidia-push-init.h"
#include "nvidia-push-utils.h"
#include "nvstatus.h"
#include "nv-ioctl.h"
#include "nv-ioctl-numbers.h"

#include "class/cl0080.h"               /* NV01_DEVICE_0 */
#include "class/cl2080.h"               /* NV20_SUBDEVICE_0 */
#include "class/cl0070.h"               /* NV01_MEMORY_VIRTUAL */
#include "class/cl2080_notification.h"  /* NV2080_ENGINE_TYPE_* */
#include "class/clb0b5sw.h"             /* NVB0B5_ALLOCATION_PARAMETERS */
#include "class/clc5b5.h"               /* TURING_DMA_COPY_A */
#include "class/clc6b5.h"               /* AMPERE_DMA_COPY_A */
#include "class/clc7b5.h"               /* AMPERE_DMA_COPY_B */
#include "class/clc8b5.h"               /* HOPPER_DMA_COPY_A */
#include "class/clc9b5.h"               /* BLACKWELL_DMA_COPY_A */
#include "class/clcab5.h"               /* BLACKWELL_DMA_COPY_B */
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "alloc/alloc_channel.h"
#include "nvos.h"

void nvc_set_err(char *err, size_t size, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || size == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, size, fmt, ap);
    va_end(ap);
}

/* ---- nvidia-push imports -------------------------------------------------- */

static struct nvchan_device *push_dev(NvPushDevicePtr pDevice)
{
    return pDevice->hostDevice;
}

static NvU32 imp_control(NvPushDevicePtr d, NvU32 hObject, NvU32 cmd,
                         void *params, NvU32 size)
{
    return nvc_rm_control(&push_dev(d)->rm, hObject, cmd, params, size);
}

static NvU32 imp_alloc(NvPushDevicePtr d, NvU32 hParent, NvU32 hObject,
                       NvU32 hClass, void *params)
{
    return nvc_rm_alloc(&push_dev(d)->rm, hParent, hObject, hClass, params);
}

static NvU32 imp_free(NvPushDevicePtr d, NvU32 hParent, NvU32 hObject)
{
    return nvc_rm_free(&push_dev(d)->rm, hParent, hObject);
}

static NvU32 imp_map_dma(NvPushDevicePtr d, NvU32 hDevice, NvU32 hDma,
                         NvU32 hMemory, NvU64 offset, NvU64 length,
                         NvU32 flags, NvU64 *pDmaOffset)
{
    return nvc_rm_map_dma(&push_dev(d)->rm, hDevice, hDma, hMemory, offset,
                          length, flags, pDmaOffset);
}

static NvU32 imp_unmap_dma(NvPushDevicePtr d, NvU32 hDevice, NvU32 hDma,
                           NvU32 hMemory, NvU32 flags, NvU64 dmaOffset)
{
    return nvc_rm_unmap_dma(&push_dev(d)->rm, hDevice, hDma, hMemory, flags,
                            dmaOffset);
}

static NvU32 imp_alloc_memory64(NvPushDevicePtr d, NvU32 hParent,
                                NvU32 hMemory, NvU32 hClass, NvU32 flags,
                                void **ppAddress, NvU64 *pLimit)
{
    return nvc_rm_alloc_memory(&push_dev(d)->rm, hParent, hMemory, hClass,
                               flags, ppAddress, pLimit);
}

static NvU32 imp_vid_heap_control(NvPushDevicePtr d, void *params)
{
    return nvc_rm_vid_heap_control(&push_dev(d)->rm, params);
}

static NvU32 imp_map(NvPushDevicePtr d, NvU32 hDevice, NvU32 hMemory,
                     NvU64 offset, NvU64 length, void **ppLinearAddress,
                     NvU32 flags)
{
    return nvc_rm_map(&push_dev(d)->rm, hDevice, hMemory, offset, length,
                      ppLinearAddress, flags);
}

static NvU32 imp_unmap(NvPushDevicePtr d, NvU32 hDevice, NvU32 hMemory,
                       void *pLinearAddress, NvU32 flags)
{
    return nvc_rm_unmap(&push_dev(d)->rm, hDevice, hMemory, pLinearAddress,
                        flags);
}

static NvU64 imp_get_ms(NvPushDevicePtr d)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (NvU64)ts.tv_sec * 1000 + (NvU64)ts.tv_nsec / 1000000;
}

static void imp_yield(NvPushDevicePtr d)
{
    sched_yield();
}

/* No RM event wait yet; nvidia-push then polls. */
static NvBool imp_wait_for_event(NvPushDevicePtr d, NvPushImportEvent *e,
                                 NvU64 timeout)
{
    return NV_FALSE;
}

static void imp_empty_event_fifo(NvPushDevicePtr d, NvPushImportEvent *e)
{
}

static void imp_channel_error(NvPushChannelPtr ch, NvU32 code)
{
    fprintf(stderr, "nvchan: channel error 0x%x\n", code);
}

static void imp_pushbuffer_wrapped(NvPushChannelPtr ch)
{
}

static void imp_log_error(NvPushDevicePtr d, const char *fmt, ...)
{
    va_list ap;

    fputs("nvchan: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const NvPushImports nvchan_push_imports = {
    .rmApiControl = imp_control,
    .rmApiAlloc = imp_alloc,
    .rmApiFree = imp_free,
    .rmApiMapMemoryDma = imp_map_dma,
    .rmApiUnmapMemoryDma = imp_unmap_dma,
    .rmApiAllocMemory64 = imp_alloc_memory64,
    .rmApiVidHeapControl = imp_vid_heap_control,
    .rmApiMapMemory = imp_map,
    .rmApiUnmapMemory = imp_unmap,
    .getMilliSeconds = imp_get_ms,
    .yield = imp_yield,
    .waitForEvent = imp_wait_for_event,
    .emptyEventFifo = imp_empty_event_fifo,
    .channelErrorOccurred = imp_channel_error,
    .pushbufferWrapped = imp_pushbuffer_wrapped,
    .logError = imp_log_error,
};

/* ---- device ---------------------------------------------------------------- */

/* Looks up a card by minor (minor >= 0) or by RM GPU id, from the card
 * list of the control device. */
static int find_card(int ctl_fd, int *minor, NvU32 *gpu_id)
{
    nv_ioctl_card_info_t cards[32];
    unsigned long cmd = _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC,
                             NV_ESC_CARD_INFO, sizeof(cards));

    memset(cards, 0, sizeof(cards));
    if (ioctl(ctl_fd, cmd, cards) < 0)
        return -errno;
    for (unsigned i = 0; i < 32; i++) {
        if (!cards[i].valid)
            continue;
        if ((*minor >= 0 && (int)cards[i].minor_number == *minor) ||
            (*minor < 0 && cards[i].gpu_id == *gpu_id)) {
            *minor = (int)cards[i].minor_number;
            *gpu_id = cards[i].gpu_id;
            return 0;
        }
    }
    return -ENODEV;
}

static NvU32 choose_copy_class(const struct nvchan_device *dev)
{
    static const NvU32 classes[] = {
        BLACKWELL_DMA_COPY_B, BLACKWELL_DMA_COPY_A, HOPPER_DMA_COPY_A,
        AMPERE_DMA_COPY_B, AMPERE_DMA_COPY_A, TURING_DMA_COPY_A,
    };

    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
        if (nvchan_device_has_class(dev, classes[i]))
            return classes[i];
    return 0;
}

static struct nvchan_device *device_open(int minor, NvU32 gpu_id,
                                         char *err, size_t err_size)
{
    struct nvchan_device *dev = calloc(1, sizeof(*dev));
    NvU32 st;
    int ret;

    if (dev == NULL) {
        nvc_set_err(err, err_size, "out of memory");
        return NULL;
    }

    ret = nvc_rm_open(&dev->rm);
    if (ret == -EPROTO) {
        nvc_set_err(err, err_size, "nvidia.ko is %s, nvchan was built for another version",
                dev->rm.version);
        goto fail_free;
    } else if (ret != 0) {
        nvc_set_err(err, err_size, "opening the RM client failed: %s", strerror(-ret));
        goto fail_free;
    }

    dev->gpu_id = gpu_id;
    ret = find_card(dev->rm.ctl_fd, &minor, &dev->gpu_id);
    if (ret == 0)
        ret = nvc_rm_open_gpu(&dev->rm, minor);
    if (ret != 0) {
        if (minor >= 0)
            nvc_set_err(err, err_size, "opening /dev/nvidia%d failed: %s", minor, strerror(-ret));
        else
            nvc_set_err(err, err_size, "opening GPU id 0x%x failed: %s", gpu_id, strerror(-ret));
        goto fail;
    }

    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = dev->gpu_id };
    st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2,
                        &id, sizeof(id));
    if (st != NV_OK) {
        nvc_set_err(err, err_size, "GPU id 0x%x: %s", dev->gpu_id, nvstatusToString(st));
        goto fail;
    }
    dev->device_instance = id.deviceInstance;

    /* Our own GPU virtual address space, as NVKMS does. */
    NV0080_ALLOC_PARAMETERS dev_params = {
        .deviceId = dev->device_instance,
        .hClientShare = dev->rm.client,
    };
    dev->device = nvc_rm_handle(&dev->rm);
    st = nvc_rm_alloc(&dev->rm, dev->rm.client, dev->device, NV01_DEVICE_0, &dev_params);
    if (st != NV_OK) {
        dev->device = 0;
        nvc_set_err(err, err_size, "NV01_DEVICE_0: %s", nvstatusToString(st));
        goto fail;
    }

    NV2080_ALLOC_PARAMETERS sub_params = { .subDeviceId = 0 };
    dev->subdevice = nvc_rm_handle(&dev->rm);
    st = nvc_rm_alloc(&dev->rm, dev->device, dev->subdevice, NV20_SUBDEVICE_0, &sub_params);
    if (st != NV_OK) {
        dev->subdevice = 0;
        nvc_set_err(err, err_size, "NV20_SUBDEVICE_0: %s", nvstatusToString(st));
        goto fail;
    }

    NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS name = {
        .gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII,
    };
    if (nvc_rm_control(&dev->rm, dev->subdevice, NV2080_CTRL_CMD_GPU_GET_NAME_STRING,
                       &name, sizeof(name)) == NV_OK)
        snprintf(dev->name, sizeof(dev->name), "%s", (const char *)name.gpuNameString.ascii);

    NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS classes = { 0 };
    st = nvc_rm_control(&dev->rm, dev->device, NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2,
                        &classes, sizeof(classes));
    if (st != NV_OK) {
        nvc_set_err(err, err_size, "class list: %s", nvstatusToString(st));
        goto fail;
    }
    dev->num_classes = classes.numClasses;
    memcpy(dev->classes, classes.classList, dev->num_classes * sizeof(NvU32));

    NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS engines = { 0 };
    st = nvc_rm_control(&dev->rm, dev->subdevice, NV2080_CTRL_CMD_GPU_GET_ENGINES_V2,
                        &engines, sizeof(engines));
    if (st != NV_OK) {
        nvc_set_err(err, err_size, "engine list: %s", nvstatusToString(st));
        goto fail;
    }
    dev->num_engines = engines.engineCount;
    memcpy(dev->engines, engines.engineList, dev->num_engines * sizeof(NvU32));
    dev->copy_class = choose_copy_class(dev);

    NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS va = { 0 };   /* default VA space */
    dev->va_space = nvc_rm_handle(&dev->rm);
    st = nvc_rm_alloc(&dev->rm, dev->device, dev->va_space, NV01_MEMORY_VIRTUAL, &va);
    if (st != NV_OK) {
        dev->va_space = 0;
        nvc_set_err(err, err_size, "NV01_MEMORY_VIRTUAL: %s", nvstatusToString(st));
        goto fail;
    }

    NvPushAllocDeviceParams pp = {
        .hostDevice = dev,
        .pImports = &nvchan_push_imports,
        .clientHandle = dev->rm.client,
        .numSubDevices = 1,
        .subDevice[0] = {
            .deviceHandle = dev->device,
            .handle = dev->subdevice,
            .gpuVASpaceCtxDma = dev->va_space,
        },
        .amodel.config = NV_AMODEL_NONE,
        .numClasses = dev->num_classes,
        .supportedClasses = dev->classes,
    };
    for (unsigned i = 0; i < NV_PUSH_DEVICE_HANDLE_POOL_NUM; i++)
        pp.handlePool[i] = dev->push_handles[i] = nvc_rm_handle(&dev->rm);
    if (!nvPushAllocDevice(&pp, &dev->push)) {
        nvc_set_err(err, err_size, "nvPushAllocDevice failed (see stderr)");
        goto fail;
    }
    dev->push_device_allocated = true;
    return dev;

fail:
    nvchan_device_close(dev);
    return NULL;
fail_free:
    if (dev->rm.ctl_fd >= 0 && dev->rm.client)
        nvc_rm_close(&dev->rm);
    free(dev);
    return NULL;
}

struct nvchan_device *nvchan_device_open(int minor, char *err, size_t err_size)
{
    return device_open(minor < 0 ? 0 : minor, 0, err, err_size);
}

struct nvchan_device *nvchan_device_open_gpu_id(uint32_t gpu_id, char *err,
                                                size_t err_size)
{
    return device_open(-1, gpu_id, err, err_size);
}

void nvchan_device_close(struct nvchan_device *dev)
{
    if (dev == NULL)
        return;
    if (dev->push_device_allocated)
        nvPushFreeDevice(&dev->push);
    if (dev->va_space)
        nvc_rm_free(&dev->rm, dev->device, dev->va_space);
    if (dev->subdevice)
        nvc_rm_free(&dev->rm, dev->device, dev->subdevice);
    if (dev->device)
        nvc_rm_free(&dev->rm, dev->rm.client, dev->device);
    nvc_rm_close(&dev->rm);   /* frees the client and everything under it */
    free(dev);
}

const char *nvchan_device_name(const struct nvchan_device *dev)
{
    return dev->name;
}

const char *nvchan_device_driver_version(const struct nvchan_device *dev)
{
    return dev->rm.version;
}

bool nvchan_device_has_class(const struct nvchan_device *dev, uint32_t cls)
{
    for (unsigned i = 0; i < dev->num_classes; i++)
        if (dev->classes[i] == cls)
            return true;
    return false;
}

unsigned nvchan_device_num_classes(const struct nvchan_device *dev)
{
    return dev->num_classes;
}

uint32_t nvchan_device_class(const struct nvchan_device *dev, unsigned index)
{
    return index < dev->num_classes ? dev->classes[index] : 0;
}

uint32_t nvchan_device_gpfifo_class(const struct nvchan_device *dev)
{
    return dev->push.gpfifoClass;
}

uint32_t nvchan_device_copy_class(const struct nvchan_device *dev)
{
    return dev->copy_class;
}

/* ---- channels -------------------------------------------------------------- */

struct nvchan_channel *nvchan_channel_create_copy(struct nvchan_device *dev,
                                                  char *err, size_t err_size)
{
    struct nvchan_channel *ch;
    NvU32 engine = 0;
    NvU32 st;

    if (dev->copy_class == 0) {
        nvc_set_err(err, err_size, "no supported copy engine class");
        return NULL;
    }
    for (unsigned i = 0; i < dev->num_engines && engine == 0; i++)
        if (NV2080_ENGINE_TYPE_IS_COPY(dev->engines[i]))
            engine = dev->engines[i];
    if (engine == 0) {
        nvc_set_err(err, err_size, "no copy engine");
        return NULL;
    }

    ch = calloc(1, sizeof(*ch));
    if (ch == NULL) {
        nvc_set_err(err, err_size, "out of memory");
        return NULL;
    }
    ch->dev = dev;
    ch->engine_type = engine;
    ch->event_fd = -1;

    NvPushAllocChannelParams cp = {
        .engineType = engine,
        .numNotifiers = 1,
        .pushBufferSizeInBytes = 64 * 1024,
        .pDevice = &dev->push,
    };
    for (unsigned i = 0; i < NV_PUSH_CHANNEL_HANDLE_POOL_NUM; i++)
        cp.handlePool[i] = nvc_rm_handle(&dev->rm);

    /* Also submits a test push buffer and waits for it. */
    if (!nvPushAllocChannel(&cp, &ch->push)) {
        nvc_set_err(err, err_size, "nvPushAllocChannel failed (see stderr)");
        free(ch);
        return NULL;
    }

    NVB0B5_ALLOCATION_PARAMETERS ce = {
        .version = NVB0B5_ALLOCATION_PARAMETERS_VERSION_1,
        .engineType = engine,
    };
    ch->copy_object = nvc_rm_handle(&dev->rm);
    st = nvc_rm_alloc(&dev->rm, ch->push.channelHandle[0], ch->copy_object,
                      dev->copy_class, &ce);
    if (st != NV_OK) {
        nvc_set_err(err, err_size, "copy engine object 0x%04x: %s", dev->copy_class,
                nvstatusToString(st));
        ch->copy_object = 0;
        nvchan_channel_destroy(ch);
        return NULL;
    }
    if (!nvc_channel_init_copy(ch, err, err_size)) {
        nvchan_channel_destroy(ch);
        return NULL;
    }
    return ch;
}

void nvchan_channel_destroy(struct nvchan_channel *ch)
{
    if (ch == NULL)
        return;
    nvc_channel_fini_copy(ch);
    if (ch->copy_object)
        nvc_rm_free(&ch->dev->rm, ch->push.channelHandle[0], ch->copy_object);
    nvPushFreeChannel(&ch->push);
    free(ch);
}
