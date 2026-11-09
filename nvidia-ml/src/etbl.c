/*
 * SPDX-License-Identifier: MIT
 *
 * nvmlInternalGetExportTable: the private interface nvidia-smi uses.
 *
 * nvidia-smi asks for one table (the id below) and refuses to run without
 * it. The table is a size followed by function pointers; it only grows, and
 * nvidia-smi checks both the size and the pointer before every call, so
 * entries we don't provide stay NULL and nvidia-smi skips them. Slot
 * meanings were worked out from nvidia-smi's calls into the closed library
 * (620.70.06): arguments, results and the labels it prints next to them.
 */

#include <stdarg.h>
#include <string.h>

#include "nvml-priv.h"

#define ETBL_SLOTS 311   /* closed 620.70.06: size 0x9b8 */

nvmlReturn_t nvml_bus_id_for_index(unsigned index, char *busId, unsigned length);
nvmlReturn_t nvml_gpu_part_number(nvmlDevice_t device, char *buf, unsigned length);
nvmlReturn_t nvml_host_max_link_gen(nvmlDevice_t device, unsigned *gen);

/* 26: nvidia-smi passes every line it prints, for NVML's debug log. */
static nvmlReturn_t trace_log_printf(const char *fmt, ...)
{
    (void)fmt;
    return NVML_SUCCESS;
}

/* 72: whether the GPU is fully supported (else nvidia-smi hides it unless
 * NVSMI_SHOW_ALL_DEVICES is set). */
static nvmlReturn_t is_fully_supported(nvmlDevice_t device, unsigned *supported)
{
    struct nvmlDevice_st *dev;
    nvmlReturn_t ret = nvml_device(device, &dev);
    if (ret != NVML_SUCCESS)
        return ret;
    if (supported == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    *supported = 1;
    return NVML_SUCCESS;
}

/* 237: FRU part number; not set on GeForce boards. */
static nvmlReturn_t fru_part_number(nvmlDevice_t device, char *buf, unsigned length)
{
    struct nvmlDevice_st *dev;
    nvmlReturn_t ret = nvml_device(device, &dev);
    if (ret != NVML_SUCCESS)
        return ret;
    return nvml_copy_string(buf, length, "");
}

/* 310: CXL device (no). */
static nvmlReturn_t is_cxl_device(nvmlDevice_t device, unsigned *cxl)
{
    struct nvmlDevice_st *dev;
    nvmlReturn_t ret = nvml_device(device, &dev);
    if (ret != NVML_SUCCESS)
        return ret;
    if (cxl == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    *cxl = 0;
    return NVML_SUCCESS;
}

static const struct {
    unsigned long long size;
    const void *slot[ETBL_SLOTS - 1];
} common_internal = {
    .size = ETBL_SLOTS * sizeof(void *),
    .slot = {
#define SLOT(n, fn) [(n) - 1] = (const void *)(fn)
        SLOT(26, trace_log_printf),
        SLOT(29, nvmlDeviceGetGpuOperationMode),
        SLOT(30, nvmlDeviceSetGpuOperationMode),
        SLOT(31, nvmlDeviceGetDisplayActive),
        SLOT(46, nvmlDeviceGetPowerManagementLimitConstraints),
        SLOT(47, nvmlDeviceGetPowerManagementDefaultLimit),
        SLOT(72, is_fully_supported),
        SLOT(81, nvml_bus_id_for_index),
        SLOT(108, nvmlDeviceGetEnforcedPowerLimit),
        SLOT(149, nvmlDeviceGetBoardPartNumber),
        SLOT(230, nvml_host_max_link_gen),
        SLOT(231, nvml_gpu_part_number),
        SLOT(237, fru_part_number),
        SLOT(310, is_cxl_device),
#undef SLOT
    },
};

/* 6c3efec4-8fc9-4e6c-a327-ee696e12f7c4, as the two little-endian quadwords
 * nvidia-smi passes. */
static const unsigned char id_common_internal[16] = {
    0xc4, 0xfe, 0x3e, 0x6c, 0xc9, 0x8f, 0x6c, 0x4e,
    0xa3, 0x27, 0xee, 0x69, 0x6e, 0x12, 0xf7, 0xc4,
};

NVML_EXPORT nvmlReturn_t nvmlInternalGetExportTable(const void **table, const unsigned char *id)
{
    if (table == NULL || id == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    *table = NULL;
    if (memcmp(id, id_common_internal, sizeof(id_common_internal)) == 0) {
        *table = &common_internal;
        return NVML_SUCCESS;
    }
    return NVML_ERROR_INVALID_ARGUMENT;
}
