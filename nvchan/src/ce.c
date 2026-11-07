/*
 * SPDX-License-Identifier: MIT
 *
 * Buffers and copy engine work. The method sequences follow UVM's copy
 * engine HAL (kernel-open/nvidia-uvm/uvm_turing_ce.c); the methods used here
 * have the same encoding from TURING_DMA_COPY_A to AMPERE_DMA_COPY_B.
 */

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvchan-priv.h"

#include "nvidia-push-methods.h"
#include "nvidia-push-utils.h"
#include "nvstatus.h"
#include "nvos.h"
#include "class/cl003e.h"          /* NV01_MEMORY_SYSTEM */
#include "class/cl0040.h"          /* NV01_MEMORY_LOCAL_USER */
#include "class/cla06fsubch.h"     /* NVA06F_SUBCHANNEL_COPY_ENGINE */
#include "class/clc7b5.h"          /* AMPERE_DMA_COPY_B methods */
#include "ctrl/ctrl0000/ctrl0000unix.h"  /* IMPORT_OBJECT_FROM_FD */

#define SUBCH NVA06F_SUBCHANNEL_COPY_ENGINE

/* Heap owner tag for our video memory, like NVKMS_RM_HEAP_ID. */
#define NVCHAN_HEAP_OWNER 0x4e564348u   /* 'NVCH' */

/* ---- buffers ---------------------------------------------------------------- */

struct nvchan_buffer *nvchan_buffer_create(struct nvchan_device *dev, uint64_t size,
                                           enum nvchan_location location,
                                           enum nvchan_layout layout,
                                           char *err, size_t err_size)
{
    struct nvchan_buffer *buf;
    NvU32 st;

    if (size == 0) {
        nvc_set_err(err, err_size, "zero size");
        return NULL;
    }
    size = (size + 0xffff) & ~(uint64_t)0xffff;   /* big page */

    buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        nvc_set_err(err, err_size, "out of memory");
        return NULL;
    }
    buf->dev = dev;
    buf->size = size;
    buf->location = location;
    buf->layout = layout;
    buf->memory = nvc_rm_handle(&dev->rm);

    if (location == NVCHAN_SYSMEM) {
        NvU64 limit = size - 1;
        NvU32 flags = DRF_DEF(OS02, _FLAGS, _PHYSICALITY, _NONCONTIGUOUS) |
                      DRF_DEF(OS02, _FLAGS, _COHERENCY, _CACHED);

        if (layout != NVCHAN_PITCH) {
            nvc_set_err(err, err_size, "block-linear system memory not supported");
            goto fail_free;
        }
        st = nvc_rm_alloc_memory(&dev->rm, dev->device, buf->memory,
                                 NV01_MEMORY_SYSTEM, flags, &buf->cpu, &limit);
        buf->cpu_from_alloc = true;
    } else {
        NV_MEMORY_ALLOCATION_PARAMS p = {
            .owner = NVCHAN_HEAP_OWNER,
            .type = NVOS32_TYPE_IMAGE,
            .flags = NVOS32_ALLOC_FLAGS_NO_SCANOUT,
            .size = size,
        };

        p.attr = DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM) |
                 DRF_DEF(OS32, _ATTR, _PHYSICALITY, _NONCONTIGUOUS) |
                 DRF_DEF(OS32, _ATTR, _DEPTH, _UNKNOWN) |
                 (layout == NVCHAN_BLOCK_LINEAR ?
                     DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR) :
                     DRF_DEF(OS32, _ATTR, _FORMAT, _PITCH));
        p.attr2 = DRF_DEF(OS32, _ATTR2, _GPU_CACHEABLE, _DEFAULT);
        st = nvc_rm_alloc(&dev->rm, dev->device, buf->memory,
                          NV01_MEMORY_LOCAL_USER, &p);
    }
    if (st != NV_OK) {
        nvc_set_err(err, err_size, "allocating %llu bytes of %s: %s",
                    (unsigned long long)size,
                    location == NVCHAN_SYSMEM ? "system memory" : "video memory",
                    nvstatusToString(st));
        goto fail_free;
    }

    NvU32 map_flags = DRF_DEF(OS46, _FLAGS, _ACCESS, _READ_WRITE) |
                      (location == NVCHAN_SYSMEM ?
                          DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE) : 0);
    st = nvc_rm_map_dma(&dev->rm, dev->device, dev->va_space, buf->memory, 0,
                        size, map_flags, &buf->gpu_va);
    if (st != NV_OK) {
        nvc_set_err(err, err_size, "mapping into the GPU address space: %s",
                    nvstatusToString(st));
        nvc_rm_free(&dev->rm, dev->device, buf->memory);
        goto fail_free;
    }
    return buf;

fail_free:
    free(buf);
    return NULL;
}

struct nvchan_buffer *nvchan_buffer_import_rm_fd(struct nvchan_device *dev, int fd,
                                                 uint64_t size, char *err,
                                                 size_t err_size)
{
    struct nvchan_buffer *buf = calloc(1, sizeof(*buf));
    NvU32 st;

    if (buf == NULL) {
        nvc_set_err(err, err_size, "out of memory");
        return NULL;
    }
    buf->dev = dev;
    buf->size = size;
    buf->location = NVCHAN_VIDMEM;      /* only used for CPU mappings */
    buf->layout = NVCHAN_PITCH;         /* the caller knows the layout */
    buf->memory = nvc_rm_handle(&dev->rm);

    NV0000_CTRL_OS_UNIX_IMPORT_OBJECT_FROM_FD_PARAMS p = {
        .fd = fd,
        .object = {
            .type = NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TYPE_RM,
            .data.rmObject = {
                .hDevice = dev->device,
                .hParent = dev->device,
                .hObject = buf->memory,
            },
        },
    };
    st = nvc_rm_control(&dev->rm, dev->rm.client,
                        NV0000_CTRL_CMD_OS_UNIX_IMPORT_OBJECT_FROM_FD, &p, sizeof(p));
    if (st != NV_OK) {
        nvc_set_err(err, err_size, "importing RM memory fd: %s", nvstatusToString(st));
        free(buf);
        return NULL;
    }

    /* Default flags: the memory's page kind (compression included). */
    st = nvc_rm_map_dma(&dev->rm, dev->device, dev->va_space, buf->memory, 0,
                        size, DRF_DEF(OS46, _FLAGS, _ACCESS, _READ_WRITE),
                        &buf->gpu_va);
    if (st != NV_OK) {
        nvc_set_err(err, err_size, "mapping imported memory: %s", nvstatusToString(st));
        nvc_rm_free(&dev->rm, dev->device, buf->memory);
        free(buf);
        return NULL;
    }
    return buf;
}

void nvchan_buffer_destroy(struct nvchan_buffer *buf)
{
    struct nvchan_device *dev;

    if (buf == NULL)
        return;
    dev = buf->dev;
    if (buf->cpu != NULL && !buf->cpu_from_alloc)
        nvc_rm_unmap(&dev->rm, dev->subdevice, buf->memory, buf->cpu, 0);
    nvc_rm_unmap_dma(&dev->rm, dev->device, dev->va_space, buf->memory, 0,
                     buf->gpu_va);
    /* Also drops a mapping made at allocation. */
    nvc_rm_free(&dev->rm, dev->device, buf->memory);
    free(buf);
}

uint64_t nvchan_buffer_size(const struct nvchan_buffer *buf)
{
    return buf->size;
}

uint64_t nvchan_buffer_gpu_address(const struct nvchan_buffer *buf)
{
    return buf->gpu_va;
}

void *nvchan_buffer_map(struct nvchan_buffer *buf)
{
    if (buf->cpu == NULL &&
        nvc_rm_map(&buf->dev->rm, buf->dev->subdevice, buf->memory, 0,
                   buf->size, &buf->cpu, 0) != NV_OK)
        buf->cpu = NULL;
    return buf->cpu;
}

/* ---- channel setup ----------------------------------------------------------- */

bool nvc_channel_init_copy(struct nvchan_channel *ch, char *err, size_t err_size)
{
    ch->sema = nvchan_buffer_create(ch->dev, 4096, NVCHAN_SYSMEM, NVCHAN_PITCH,
                                    err, err_size);
    if (ch->sema == NULL)
        return false;
    ch->sema_cpu = ch->sema->cpu;
    *ch->sema_cpu = 0;
    ch->sema_payload = 0;

    nvPushSetObject(&ch->push, SUBCH, &ch->copy_object);
    return true;
}

void nvc_channel_fini_copy(struct nvchan_channel *ch)
{
    nvchan_buffer_destroy(ch->sema);
    ch->sema = NULL;
    ch->sema_cpu = NULL;
}

/* ---- methods ---------------------------------------------------------------- */

static void push_offsets(NvPushChannelPtr p, NvU64 in, NvU64 out)
{
    nvPushMethod(p, SUBCH, NVC7B5_OFFSET_IN_UPPER, 4);
    nvPushSetMethodData(p, NvU64_HI32(in));
    nvPushSetMethodData(p, NvU64_LO32(in));
    nvPushSetMethodData(p, NvU64_HI32(out));
    nvPushSetMethodData(p, NvU64_LO32(out));
}

/* Each transfer waits for the previous one (NON_PIPELINED), so later
 * operations see earlier results; the flush comes with the completion
 * semaphore in nvchan_channel_finish(). */
static NvU32 launch_common(void)
{
    return DRF_DEF(C7B5, _LAUNCH_DMA, _DATA_TRANSFER_TYPE, _NON_PIPELINED) |
           DRF_DEF(C7B5, _LAUNCH_DMA, _FLUSH_ENABLE, _FALSE) |
           DRF_DEF(C7B5, _LAUNCH_DMA, _SEMAPHORE_TYPE, _NONE) |
           DRF_DEF(C7B5, _LAUNCH_DMA, _INTERRUPT_TYPE, _NONE) |
           DRF_DEF(C7B5, _LAUNCH_DMA, _SRC_TYPE, _VIRTUAL) |
           DRF_DEF(C7B5, _LAUNCH_DMA, _DST_TYPE, _VIRTUAL);
}

static bool in_buffer(const struct nvchan_buffer *buf, uint64_t offset, uint64_t size)
{
    return offset <= buf->size && size <= buf->size - offset;
}

int nvchan_fill32(struct nvchan_channel *ch, struct nvchan_buffer *buf,
                  uint64_t offset, uint64_t size, uint32_t value)
{
    NvPushChannelPtr p = &ch->push;
    NvU64 dst = buf->gpu_va + offset;
    NvU64 count = size / 4;

    if (size % 4 != 0 || !in_buffer(buf, offset, size))
        return -EINVAL;

    nvPushMethod(p, SUBCH, NVC7B5_SET_REMAP_CONST_B, 2);
    nvPushSetMethodData(p, value);
    nvPushSetMethodData(p,
        DRF_DEF(C7B5, _SET_REMAP_COMPONENTS, _DST_X, _CONST_B) |
        DRF_DEF(C7B5, _SET_REMAP_COMPONENTS, _COMPONENT_SIZE, _FOUR) |
        DRF_DEF(C7B5, _SET_REMAP_COMPONENTS, _NUM_DST_COMPONENTS, _ONE));

    while (count > 0) {
        NvU32 n = count > 0xffffffffu ? 0xffffffffu : (NvU32)count;

        push_offsets(p, 0, dst);
        nvPushMethod(p, SUBCH, NVC7B5_LINE_LENGTH_IN, 1);
        nvPushSetMethodData(p, n);
        nvPushMethod(p, SUBCH, NVC7B5_LAUNCH_DMA, 1);
        nvPushSetMethodData(p, launch_common() |
            DRF_DEF(C7B5, _LAUNCH_DMA, _SRC_MEMORY_LAYOUT, _PITCH) |
            DRF_DEF(C7B5, _LAUNCH_DMA, _DST_MEMORY_LAYOUT, _PITCH) |
            DRF_DEF(C7B5, _LAUNCH_DMA, _MULTI_LINE_ENABLE, _FALSE) |
            DRF_DEF(C7B5, _LAUNCH_DMA, _REMAP_ENABLE, _TRUE));
        dst += (NvU64)n * 4;
        count -= n;
    }
    return 0;
}

int nvchan_copy(struct nvchan_channel *ch,
                struct nvchan_buffer *dst, uint64_t dst_offset,
                struct nvchan_buffer *src, uint64_t src_offset, uint64_t size)
{
    NvPushChannelPtr p = &ch->push;
    NvU64 in = src->gpu_va + src_offset, out = dst->gpu_va + dst_offset;

    if (!in_buffer(src, src_offset, size) || !in_buffer(dst, dst_offset, size))
        return -EINVAL;

    while (size > 0) {
        NvU32 n = size > 0xffffffffu ? 0xffffffffu : (NvU32)size;

        push_offsets(p, in, out);
        nvPushMethod(p, SUBCH, NVC7B5_LINE_LENGTH_IN, 1);
        nvPushSetMethodData(p, n);
        nvPushMethod(p, SUBCH, NVC7B5_LAUNCH_DMA, 1);
        nvPushSetMethodData(p, launch_common() |
            DRF_DEF(C7B5, _LAUNCH_DMA, _SRC_MEMORY_LAYOUT, _PITCH) |
            DRF_DEF(C7B5, _LAUNCH_DMA, _DST_MEMORY_LAYOUT, _PITCH) |
            DRF_DEF(C7B5, _LAUNCH_DMA, _MULTI_LINE_ENABLE, _FALSE) |
            DRF_DEF(C7B5, _LAUNCH_DMA, _REMAP_ENABLE, _FALSE));
        in += n;
        out += n;
        size -= n;
    }
    return 0;
}

static NvU32 block_size(uint32_t log2_gobs)
{
    return DRF_DEF(C7B5, _SET_DST_BLOCK_SIZE, _WIDTH, _ONE_GOB) |
           DRF_NUM(C7B5, _SET_DST_BLOCK_SIZE, _HEIGHT, log2_gobs) |
           DRF_DEF(C7B5, _SET_DST_BLOCK_SIZE, _DEPTH, _ONE_GOB) |
           DRF_DEF(C7B5, _SET_DST_BLOCK_SIZE, _GOB_HEIGHT, _GOB_HEIGHT_FERMI_8);
}

/*
 * A block-linear side is described by its base address, block size,
 * dimensions and an origin; a pitch side by the address of the first byte
 * and the pitch. The copy engine works in bytes (remap disabled), so widths
 * and X are bytes. ORIGIN_X/Y are 16-bit.
 */
/* The surface fits its buffer and the rectangle fits the surface. */
static bool surface_ok(const struct nvchan_surface *s, uint32_t x, uint32_t y,
                       uint32_t width_bytes, uint32_t height)
{
    uint64_t bytes;

    if (s->pitch == 0 || s->height == 0 ||
        (uint64_t)x + width_bytes > s->pitch || (uint64_t)y + height > s->height)
        return false;
    if (s->block_linear) {
        uint32_t rows = 8u << s->log2_gobs;

        if (s->log2_gobs > 5 || s->pitch % 64 != 0 || x > 0xffff || y > 0xffff)
            return false;
        bytes = (uint64_t)s->pitch * ((s->height + rows - 1) / rows * rows);
    } else {
        bytes = (uint64_t)s->pitch * s->height;
    }
    return in_buffer(s->buf, s->offset, bytes);
}

int nvchan_copy_2d(struct nvchan_channel *ch,
                   const struct nvchan_surface *dst, uint32_t dst_x, uint32_t dst_y,
                   const struct nvchan_surface *src, uint32_t src_x, uint32_t src_y,
                   uint32_t width_bytes, uint32_t height)
{
    NvPushChannelPtr p = &ch->push;
    NvU64 in, out;

    if (width_bytes == 0 || height == 0 ||
        !surface_ok(src, src_x, src_y, width_bytes, height) ||
        !surface_ok(dst, dst_x, dst_y, width_bytes, height))
        return -EINVAL;

    if (src->block_linear) {
        in = src->buf->gpu_va + src->offset;
        nvPushMethod(p, SUBCH, NVC7B5_SET_SRC_BLOCK_SIZE, 6);
        nvPushSetMethodData(p, block_size(src->log2_gobs));
        nvPushSetMethodData(p, src->pitch);    /* SET_SRC_WIDTH */
        nvPushSetMethodData(p, src->height);   /* SET_SRC_HEIGHT */
        nvPushSetMethodData(p, 1);             /* SET_SRC_DEPTH */
        nvPushSetMethodData(p, 0);             /* SET_SRC_LAYER */
        nvPushSetMethodData(p,                 /* SET_SRC_ORIGIN */
            DRF_NUM(C7B5, _SET_SRC_ORIGIN, _X, src_x) |
            DRF_NUM(C7B5, _SET_SRC_ORIGIN, _Y, src_y));
    } else {
        in = src->buf->gpu_va + src->offset + (NvU64)src_y * src->pitch + src_x;
    }

    if (dst->block_linear) {
        out = dst->buf->gpu_va + dst->offset;
        nvPushMethod(p, SUBCH, NVC7B5_SET_DST_BLOCK_SIZE, 6);
        nvPushSetMethodData(p, block_size(dst->log2_gobs));
        nvPushSetMethodData(p, dst->pitch);    /* SET_DST_WIDTH */
        nvPushSetMethodData(p, dst->height);   /* SET_DST_HEIGHT */
        nvPushSetMethodData(p, 1);             /* SET_DST_DEPTH */
        nvPushSetMethodData(p, 0);             /* SET_DST_LAYER */
        nvPushSetMethodData(p,                 /* SET_DST_ORIGIN */
            DRF_NUM(C7B5, _SET_DST_ORIGIN, _X, dst_x) |
            DRF_NUM(C7B5, _SET_DST_ORIGIN, _Y, dst_y));
    } else {
        out = dst->buf->gpu_va + dst->offset + (NvU64)dst_y * dst->pitch + dst_x;
    }

    push_offsets(p, in, out);
    nvPushMethod(p, SUBCH, NVC7B5_PITCH_IN, 4);
    nvPushSetMethodData(p, src->pitch);    /* PITCH_IN */
    nvPushSetMethodData(p, dst->pitch);    /* PITCH_OUT */
    nvPushSetMethodData(p, width_bytes);   /* LINE_LENGTH_IN */
    nvPushSetMethodData(p, height);        /* LINE_COUNT */

    nvPushMethod(p, SUBCH, NVC7B5_LAUNCH_DMA, 1);
    nvPushSetMethodData(p, launch_common() |
        (src->block_linear ?
            DRF_DEF(C7B5, _LAUNCH_DMA, _SRC_MEMORY_LAYOUT, _BLOCKLINEAR) :
            DRF_DEF(C7B5, _LAUNCH_DMA, _SRC_MEMORY_LAYOUT, _PITCH)) |
        (dst->block_linear ?
            DRF_DEF(C7B5, _LAUNCH_DMA, _DST_MEMORY_LAYOUT, _BLOCKLINEAR) :
            DRF_DEF(C7B5, _LAUNCH_DMA, _DST_MEMORY_LAYOUT, _PITCH)) |
        DRF_DEF(C7B5, _LAUNCH_DMA, _MULTI_LINE_ENABLE, _TRUE) |
        DRF_DEF(C7B5, _LAUNCH_DMA, _REMAP_ENABLE, _FALSE));
    return 0;
}

/* ---- completion -------------------------------------------------------------- */

static NvU64 now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (NvU64)ts.tv_sec * 1000 + (NvU64)ts.tv_nsec / 1000000;
}

int nvchan_channel_finish(struct nvchan_channel *ch, unsigned timeout_ms)
{
    NvPushChannelPtr p = &ch->push;
    const NvU32 payload = ++ch->sema_payload;
    const NvU64 va = ch->sema->gpu_va;
    const NvU64 deadline = now_ms() + timeout_ms;

    /* After all queued transfers, with a system-wide flush so the CPU
     * sees their writes. */
    nvPushMethod(p, SUBCH, NVC7B5_SET_SEMAPHORE_A, 3);
    nvPushSetMethodData(p, NvU64_HI32(va));
    nvPushSetMethodData(p, NvU64_LO32(va));
    nvPushSetMethodData(p, payload);
    nvPushMethod(p, SUBCH, NVC7B5_LAUNCH_DMA, 1);
    nvPushSetMethodData(p,
        DRF_DEF(C7B5, _LAUNCH_DMA, _DATA_TRANSFER_TYPE, _NONE) |
        DRF_DEF(C7B5, _LAUNCH_DMA, _FLUSH_ENABLE, _TRUE) |
        DRF_DEF(C7B5, _LAUNCH_DMA, _FLUSH_TYPE, _SYS) |
        DRF_DEF(C7B5, _LAUNCH_DMA, _SEMAPHORE_TYPE, _RELEASE_ONE_WORD_SEMAPHORE));

    nvPushKickoff(p);

    for (unsigned spins = 0;; spins++) {
        if ((NvS32)(*ch->sema_cpu - payload) >= 0) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            return 0;
        }
        if ((spins & 1023) == 1023) {
            if (nvPushCheckChannelError(p))
                return -EIO;
            if (now_ms() > deadline)
                return -ETIMEDOUT;
            sched_yield();
        }
    }
}
