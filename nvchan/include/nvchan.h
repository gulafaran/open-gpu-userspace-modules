/*
 * SPDX-License-Identifier: MIT
 *
 * nvchan: GPU command submission from user space on the open NVIDIA kernel
 * modules, built on NVIDIA's nvidia-push library (src/common/unix/
 * nvidia-push in the kernel tree) with an RM API over the nvidia.ko
 * ioctls.
 */

#ifndef NVCHAN_H
#define NVCHAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nvchan_device;
struct nvchan_channel;

/* Opens /dev/nvidia<minor>. On failure returns NULL and, if err is given,
 * a description. */
struct nvchan_device *nvchan_device_open(int minor, char *err, size_t err_size);
void nvchan_device_close(struct nvchan_device *dev);

const char *nvchan_device_name(const struct nvchan_device *dev);
const char *nvchan_device_driver_version(const struct nvchan_device *dev);
bool nvchan_device_has_class(const struct nvchan_device *dev, uint32_t cls);
unsigned nvchan_device_num_classes(const struct nvchan_device *dev);
uint32_t nvchan_device_class(const struct nvchan_device *dev, unsigned index);

/* The GPFIFO and copy engine classes in use. */
uint32_t nvchan_device_gpfifo_class(const struct nvchan_device *dev);
uint32_t nvchan_device_copy_class(const struct nvchan_device *dev);

/*
 * A channel on the first copy engine, with a copy engine object bound.
 * Creating it already submits a test push buffer and waits for it.
 */
struct nvchan_channel *nvchan_channel_create_copy(struct nvchan_device *dev,
                                                  char *err, size_t err_size);
void nvchan_channel_destroy(struct nvchan_channel *ch);

/* ---- memory ---------------------------------------------------------------- */

enum nvchan_location {
    NVCHAN_SYSMEM,   /* coherent system memory, CPU cached */
    NVCHAN_VIDMEM,   /* video memory */
};

enum nvchan_layout {
    NVCHAN_PITCH,
    NVCHAN_BLOCK_LINEAR,   /* generic (uncompressed) block-linear kind */
};

struct nvchan_buffer;

/* Memory mapped into the device's GPU address space. */
struct nvchan_buffer *nvchan_buffer_create(struct nvchan_device *dev, uint64_t size,
                                           enum nvchan_location location,
                                           enum nvchan_layout layout,
                                           char *err, size_t err_size);
void nvchan_buffer_destroy(struct nvchan_buffer *buf);
uint64_t nvchan_buffer_size(const struct nvchan_buffer *buf);
uint64_t nvchan_buffer_gpu_address(const struct nvchan_buffer *buf);

/* CPU mapping, kept until the buffer is destroyed. Video memory is mapped
 * through BAR1 (uncached, write-combined). NULL on failure. */
void *nvchan_buffer_map(struct nvchan_buffer *buf);

/* ---- copy engine work --------------------------------------------------------
 *
 * Operations are queued on the channel and run in order;
 * nvchan_channel_finish() submits them and waits until they are done and
 * their writes are visible to the CPU.
 *
 * Arguments are checked against the buffers before anything is queued
 * (-EINVAL): the copy engine faults on out-of-bounds accesses, and a
 * fault kills the channel.
 */

/* Fills size bytes (a multiple of 4) at offset with a 32-bit value. */
int nvchan_fill32(struct nvchan_channel *ch, struct nvchan_buffer *buf,
                   uint64_t offset, uint64_t size, uint32_t value);

int nvchan_copy(struct nvchan_channel *ch,
                 struct nvchan_buffer *dst, uint64_t dst_offset,
                 struct nvchan_buffer *src, uint64_t src_offset, uint64_t size);

/* A 2D surface in a buffer. Block-linear surfaces use one-GOB-wide blocks
 * of 2^log2_gobs GOBs (DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D's h). */
struct nvchan_surface {
    struct nvchan_buffer *buf;
    uint64_t offset;
    uint32_t pitch;           /* bytes per row; block-linear: width in bytes,
                               * a multiple of 64 */
    uint32_t height;          /* rows */
    bool block_linear;
    uint32_t log2_gobs;
};

/* Copies a width_bytes x height rectangle between surfaces, converting
 * between pitch and block-linear layouts as needed. Block-linear origins
 * are limited to 65535 (bytes for X). */
int nvchan_copy_2d(struct nvchan_channel *ch,
                    const struct nvchan_surface *dst, uint32_t dst_x_bytes, uint32_t dst_y,
                    const struct nvchan_surface *src, uint32_t src_x_bytes, uint32_t src_y,
                    uint32_t width_bytes, uint32_t height);

/* Submits the queued work and waits up to timeout_ms. Returns 0, -ETIMEDOUT,
 * or -EIO if the channel reported an error; the channel is unusable then
 * (RM has torn it down) and must be destroyed. */
int nvchan_channel_finish(struct nvchan_channel *ch, unsigned timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
