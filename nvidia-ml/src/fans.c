/*
 * SPDX-License-Identifier: MIT
 *
 * Fans.
 *
 * NVML numbers the board's fan coolers 0..n-1. The set of coolers comes
 * from FAN_COOLER_GET_INFO, a GSP control whose parameters are not in the
 * open headers; only its leading u32 cooler mask is used here (the closed
 * library passes 1432 bytes and counts the mask's bits). Current speeds
 * are in the RUSD page (rpmCurr[], indexed by cooler), polled by GSP.
 *
 * Fan speed as a percentage, target speed, the min/max range and the
 * control policy need FAN_COOLER_GET_STATUS / GET_CONTROL, whose 620
 * layouts are unknown: GSP refuses them on boards whose fans the EC
 * drives (where they were reverse engineered), so they stay unsupported.
 */

#include <string.h>
#include <unistd.h>

#include "nvml-priv.h"

#include "nvstatus.h"

#define NV2080_CTRL_CMD_FAN_COOLER_GET_INFO  0x2080852eU
#define FAN_COOLER_GET_INFO_SIZE             1432

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

static nvmlReturn_t cooler_mask(struct nvmlDevice_st *dev, NvU32 *mask)
{
    nvmlReturn_t ret = NVML_SUCCESS;

    pthread_mutex_lock(&dev->lock);
    if (!dev->coolers_known) {
        NvU32 params[FAN_COOLER_GET_INFO_SIZE / sizeof(NvU32)];
        memset(params, 0, sizeof(params));
        NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FAN_COOLER_GET_INFO,
                                     params, sizeof(params));
        if (status == NV_OK || status == NV_ERR_NOT_SUPPORTED) {
            dev->cooler_mask = status == NV_OK ? params[0] : 0;
            dev->coolers_known = true;
        } else {
            ret = nvml_from_rm(status);
        }
    }
    *mask = dev->cooler_mask;
    pthread_mutex_unlock(&dev->lock);
    return ret;
}

/* NVML fan index -> cooler index (the fan'th set bit of the mask). */
static nvmlReturn_t cooler_index(struct nvmlDevice_st *dev, unsigned fan, unsigned *index)
{
    NvU32 mask;
    nvmlReturn_t ret = cooler_mask(dev, &mask);
    if (ret != NVML_SUCCESS)
        return ret;
    for (unsigned i = 0; i < 32; i++) {
        if (!(mask & (1U << i)))
            continue;
        if (fan-- == 0) {
            *index = i;
            return NVML_SUCCESS;
        }
    }
    return NVML_ERROR_INVALID_ARGUMENT;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetNumFans(nvmlDevice_t device, unsigned int *numFans)
{
    NvU32 mask;
    DEVICE(device, dev);
    CHECK_PTR(numFans);
    nvmlReturn_t ret = cooler_mask(dev, &mask);
    if (ret != NVML_SUCCESS)
        return ret;
    *numFans = (unsigned)__builtin_popcount(mask);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetFanSpeedRPM(nvmlDevice_t device, nvmlFanSpeedInfo_t *fanSpeed)
{
    RUSD_FAN_COOLER_INFO info;
    unsigned index;
    DEVICE(device, dev);
    CHECK_PTR(fanSpeed);
    if (fanSpeed->version != nvmlFanSpeedInfo_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    nvmlReturn_t ret = cooler_index(dev, fanSpeed->fan, &index);
    if (ret != NVML_SUCCESS)
        return ret;
    if (index >= RUSD_FAN_COOLER_MAX_COOLERS ||
        !NVML_RUSD_READ(dev, fanCoolerStatus, &info))
        return NVML_ERROR_NOT_SUPPORTED;
    fanSpeed->speed = info.rpmCurr[index];
    return NVML_SUCCESS;
}

/* The percentage-based calls: argument checks as the closed library does
 * them, then unsupported (see the top of the file). */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetFanSpeed(nvmlDevice_t device, unsigned int *speed)
{
    DEVICE(device, dev);
    CHECK_PTR(speed);
    return NVML_ERROR_NOT_SUPPORTED;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetFanSpeed_v2(nvmlDevice_t device, unsigned int fan,
                                                  unsigned int *speed)
{
    unsigned index;
    DEVICE(device, dev);
    CHECK_PTR(speed);
    nvmlReturn_t ret = cooler_index(dev, fan, &index);
    return ret != NVML_SUCCESS ? ret : NVML_ERROR_NOT_SUPPORTED;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetTargetFanSpeed(nvmlDevice_t device, unsigned int fan,
                                                     unsigned int *targetSpeed)
{
    unsigned index;
    DEVICE(device, dev);
    CHECK_PTR(targetSpeed);
    nvmlReturn_t ret = cooler_index(dev, fan, &index);
    return ret != NVML_SUCCESS ? ret : NVML_ERROR_NOT_SUPPORTED;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMinMaxFanSpeed(nvmlDevice_t device, unsigned int *minSpeed,
                                                     unsigned int *maxSpeed)
{
    DEVICE(device, dev);
    CHECK_PTR(minSpeed);
    CHECK_PTR(maxSpeed);
    unsigned index;   /* the range of fan 0 */
    nvmlReturn_t ret = cooler_index(dev, 0, &index);
    return ret != NVML_SUCCESS ? ret : NVML_ERROR_NOT_SUPPORTED;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetFanControlPolicy_v2(nvmlDevice_t device, unsigned int fan,
                                                          nvmlFanControlPolicy_t *policy)
{
    unsigned index;
    DEVICE(device, dev);
    CHECK_PTR(policy);
    nvmlReturn_t ret = cooler_index(dev, fan, &index);
    return ret != NVML_SUCCESS ? ret : NVML_ERROR_NOT_SUPPORTED;
}

/* Setting fans needs FAN_COOLER_SET_CONTROL; only root may, as with the
 * closed library. */
static nvmlReturn_t set_fan(nvmlDevice_t device, unsigned fan)
{
    unsigned index;
    DEVICE(device, dev);
    if (geteuid() != 0)
        return NVML_ERROR_NO_PERMISSION;
    nvmlReturn_t ret = cooler_index(dev, fan, &index);
    return ret != NVML_SUCCESS ? ret : NVML_ERROR_NOT_SUPPORTED;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceSetFanSpeed_v2(nvmlDevice_t device, unsigned int fan,
                                                  unsigned int speed)
{
    (void)speed;
    return set_fan(device, fan);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceSetDefaultFanSpeed_v2(nvmlDevice_t device, unsigned int fan)
{
    return set_fan(device, fan);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceSetFanControlPolicy(nvmlDevice_t device, unsigned int fan,
                                                       nvmlFanControlPolicy_t policy)
{
    (void)policy;
    return set_fan(device, fan);
}
