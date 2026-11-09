/*
 * SPDX-License-Identifier: MIT
 *
 * Power limits.
 *
 * The range and default are the total GPU power (TGP) policy's limits,
 * static and looked up once. The controls are GSP power-management
 * controls without open parameter definitions; layouts read off the
 * closed library's calls (620.70.06), only the fields it uses:
 *
 * PMGR_PWR_MONITOR_GET_INFO (54352 bytes): NvBool supported at 0.
 *
 * PMGR_PWR_POLICY_GET_INFO (20512 bytes): u32 policy mask at 4, u8 TGP
 *   policy index at 0x2d; per policy (0x104 bytes from 0xcc) the limits
 *   {min, rated, max} in mW at +8. Default = rated.
 *
 * The current limit and power management mode come from another control
 * (PMGR 0x1d) in the closed library, which GSP refuses on this laptop, so
 * its reply layout is unknown and those stay unsupported, as they are
 * there.
 */

#include <stdlib.h>
#include <string.h>

#include "nvml-priv.h"

#include "nvstatus.h"

#define NV2080_CTRL_CMD_PMGR_PWR_MONITOR_GET_INFO   0x2080a612U
#define NV2080_CTRL_CMD_PMGR_PWR_POLICY_GET_INFO    0x2080a618U

#define PWR_MONITOR_INFO_SIZE   54352
#define PWR_POLICY_INFO_SIZE    20512
#define PWR_POLICY_MASK         4
#define PWR_POLICY_TGP_INDEX    0x2d
#define PWR_POLICY_FIRST        0xcc
#define PWR_POLICY_STRIDE       0x104
#define PWR_POLICY_LIMITS       8
#define PWR_POLICY_MAX          32

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

static NvU32 lookup_limits(struct nvmlDevice_st *dev)
{
    NvU8 *p = calloc(1, PWR_MONITOR_INFO_SIZE);
    if (p == NULL)
        return NV_ERR_NO_MEMORY;
    dev->power_min = dev->power_default = dev->power_max = 0;

    NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_PMGR_PWR_MONITOR_GET_INFO, p, PWR_MONITOR_INFO_SIZE);
    if (status != NV_OK || !p[0])
        goto out;

    memset(p, 0, PWR_POLICY_INFO_SIZE);
    status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_PMGR_PWR_POLICY_GET_INFO, p, PWR_POLICY_INFO_SIZE);
    if (status != NV_OK)
        goto out;
    NvU32 mask, lim[3];
    memcpy(&mask, p + PWR_POLICY_MASK, sizeof(mask));
    unsigned tgp = p[PWR_POLICY_TGP_INDEX];
    if (tgp >= PWR_POLICY_MAX || !(mask & (1U << tgp)) ||
        PWR_POLICY_FIRST + (tgp + 1) * PWR_POLICY_STRIDE > PWR_POLICY_INFO_SIZE)
        goto out;
    memcpy(lim, p + PWR_POLICY_FIRST + tgp * PWR_POLICY_STRIDE + PWR_POLICY_LIMITS, sizeof(lim));
    if (lim[0] <= lim[1] && lim[1] <= lim[2] && lim[1] != 0) {
        dev->power_min = lim[0];
        dev->power_default = lim[1];
        dev->power_max = lim[2];
    }
out:
    free(p);
    return status;
}

static nvmlReturn_t limits(struct nvmlDevice_st *dev)
{
    NvU32 status = NV_OK;
    pthread_mutex_lock(&dev->lock);
    if (!dev->power_known) {
        status = lookup_limits(dev);
        if (status == NV_OK || status == NV_ERR_NOT_SUPPORTED) {
            dev->power_known = true;
            status = NV_OK;
        }
    }
    bool ok = dev->power_default != 0;
    pthread_mutex_unlock(&dev->lock);
    if (status != NV_OK)
        return nvml_from_rm(status);
    return ok ? NVML_SUCCESS : NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvml_power_limits(struct nvmlDevice_st *dev, unsigned *min, unsigned *def, unsigned *max)
{
    nvmlReturn_t ret = limits(dev);
    if (ret == NVML_SUCCESS) {
        *min = dev->power_min;
        *def = dev->power_default;
        *max = dev->power_max;
    }
    return ret;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPowerManagementDefaultLimit(nvmlDevice_t device,
                                                                  unsigned int *defaultLimit)
{
    DEVICE(device, dev);
    CHECK_PTR(defaultLimit);
    nvmlReturn_t ret = limits(dev);
    if (ret == NVML_SUCCESS)
        *defaultLimit = dev->power_default;
    return ret;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPowerManagementLimitConstraints(nvmlDevice_t device,
                                                                      unsigned int *minLimit,
                                                                      unsigned int *maxLimit)
{
    DEVICE(device, dev);
    CHECK_PTR(minLimit);
    CHECK_PTR(maxLimit);
    nvmlReturn_t ret = limits(dev);
    if (ret == NVML_SUCCESS) {
        *minLimit = dev->power_min;
        *maxLimit = dev->power_max;
    }
    return ret;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPowerManagementLimit(nvmlDevice_t device, unsigned int *limit)
{
    DEVICE(device, dev);
    CHECK_PTR(limit);
    return NVML_ERROR_NOT_SUPPORTED;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPowerManagementMode(nvmlDevice_t device, nvmlEnableState_t *mode)
{
    DEVICE(device, dev);
    CHECK_PTR(mode);
    return NVML_ERROR_NOT_SUPPORTED;
}
