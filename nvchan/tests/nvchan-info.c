/*
 * SPDX-License-Identifier: MIT
 *
 * Brings up an nvchan device and a copy engine channel. Creating the
 * channel submits nvidia-push's test push buffer and waits for the GPU to
 * process it, so success means the whole submission path works.
 *
 *   build/nvchan/nvchan-info [minor]
 */

#include <stdio.h>
#include <stdlib.h>

#include "nvchan.h"

int main(int argc, char **argv)
{
    int minor = argc > 1 ? atoi(argv[1]) : 0;
    char err[256] = "";

    struct nvchan_device *dev = nvchan_device_open(minor, err, sizeof(err));
    if (dev == NULL) {
        fprintf(stderr, "device: %s\n", err);
        return 1;
    }
    printf("GPU: %s (driver %s)\n", nvchan_device_name(dev),
           nvchan_device_driver_version(dev));
    printf("GPFIFO class 0x%04x, copy class 0x%04x, %u classes:",
           nvchan_device_gpfifo_class(dev), nvchan_device_copy_class(dev),
           nvchan_device_num_classes(dev));
    for (unsigned i = 0; i < nvchan_device_num_classes(dev); i++)
        printf("%s%04x", i % 16 ? " " : "\n  ", nvchan_device_class(dev, i));
    printf("\n");

    struct nvchan_channel *ch = nvchan_channel_create_copy(dev, err, sizeof(err));
    if (ch == NULL) {
        fprintf(stderr, "copy channel: %s\n", err);
        nvchan_device_close(dev);
        return 1;
    }
    printf("copy channel: created, test push buffer completed\n");

    nvchan_channel_destroy(ch);
    nvchan_device_close(dev);
    printf("closed\n");
    return 0;
}
