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

#ifdef __cplusplus
}
#endif

#endif
