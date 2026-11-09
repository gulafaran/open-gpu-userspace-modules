/*
 * SPDX-License-Identifier: MIT
 *
 * Temperature thresholds.
 *
 * The controls used here are GSP thermal controls whose parameters are not
 * in the open headers; the layouts below were read off the closed library's
 * calls (620.70.06) and only the fields it uses are touched:
 *
 * HWFS_EVENT_REPORTING_SETTINGS_GET (24 bytes): hardware failsafe event
 *   {u32 event id; s32 temperature in 1/256 C; ...}. Shutdown is event
 *   0x15. Slowdown is the lowest plausible temperature (0..150 C) of the
 *   events 0x09..0x14 the board supports.
 *
 * THERMAL_POLICY_GET_INFO (1784 bytes): u32 policy mask at 8; at 16 the
 *   policy indexes {GPS, acoustic, memory, GPU slowdown} (0xff: none);
 *   per policy (0x6c bytes each) s32 limits {min, rated, max} at 0x40.
 *
 * THERMAL_POLICY_GET_STATUS (3324 bytes, policy mask at 8 in): s32 margin
 *   to the nearest thermal limit in 1/256 C at 0x10.
 *
 * T.Limit values express thresholds relative to the GPU's maximum
 * operating temperature (the slowdown policy's rated limit): the HWFS
 * events report them on the T.Limit sensor when the u8 at 8 is set, and
 * the field is limit - threshold (shutdown -5 C, slowdown -2 C here).
 *
 * THERMAL_POLICY_GET_CONTROL (1440 bytes, policy mask at 8 in): policies
 *   from 0x20, 0x58 bytes each, s32 current limit in 1/256 C at +4. GPU max
 *   is the slowdown policy's limit, GPS current the GPS policy's.
 *
 * Memory max and the acoustic thresholds stay unsupported: the closed
 * library decides memory max from thermal device/channel info whose layout
 * isn't known, and no board here has an acoustic policy to read.
 */

#include <string.h>

#include "nvml-priv.h"

#include "nvstatus.h"

#define NV2080_CTRL_CMD_THERMAL_POLICY_GET_INFO                 0x2080852aU
#define NV2080_CTRL_CMD_THERMAL_POLICY_GET_STATUS               0x2080852bU
#define NV2080_CTRL_CMD_THERMAL_POLICY_GET_CONTROL              0x2080852cU
#define NV2080_CTRL_CMD_THERMAL_HWFS_EVENT_REPORTING_SETTINGS_GET 0x20808546U

#define POLICY_INFO_SIZE        1784
#define POLICY_CONTROL_SIZE     1440
#define POLICY_STATUS_SIZE      3324
#define POLICY_INFO_LIMITS      0x40
#define POLICY_INFO_STRIDE      0x6c
#define POLICY_STATUS_MARGIN    0x10
#define POLICY_MASK_OFFSET      8
#define POLICY_INDEX_OFFSET     16
#define POLICY_CONTROL_FIRST    0x20
#define POLICY_CONTROL_STRIDE   0x58
#define POLICY_LIMIT_OFFSET     4
#define POLICY_INDEX_NONE       0xff
#define POLICY_MAX              ((POLICY_CONTROL_SIZE - POLICY_CONTROL_FIRST) / POLICY_CONTROL_STRIDE)

#define HWFS_EVENT_SHUTDOWN         0x15
#define HWFS_EVENT_SLOWDOWN_FIRST   0x09
#define HWFS_EVENT_SLOWDOWN_LAST    0x14

struct hwfs_event_settings {
    NvU32 event;
    NvS32 temperature;      /* 1/256 C */
    NvU8 tlimit;            /* report on the T.Limit sensor */
    NvU8 pad[3];
    NvU32 data[3];
};

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

static NvU32 hwfs_event_temp(struct nvmlDevice_st *dev, NvU32 event, bool tlimit, NvS32 *temp)
{
    struct hwfs_event_settings p = { .event = event, .tlimit = tlimit };
    NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_THERMAL_HWFS_EVENT_REPORTING_SETTINGS_GET,
                                 &p, sizeof(p));
    if (status == NV_OK)
        *temp = p.temperature;
    return status;
}

static nvmlReturn_t shutdown_temp(struct nvmlDevice_st *dev, bool tlimit, NvS32 *temp)
{
    NvU32 status = hwfs_event_temp(dev, HWFS_EVENT_SHUTDOWN, tlimit, temp);
    return status == NV_OK ? NVML_SUCCESS : nvml_from_rm(status);
}

static nvmlReturn_t slowdown_temp(struct nvmlDevice_st *dev, bool tlimit, NvS32 *temp)
{
    bool found = false;
    NvS32 min = 0;
    for (NvU32 e = HWFS_EVENT_SLOWDOWN_FIRST; e <= HWFS_EVENT_SLOWDOWN_LAST; e++) {
        NvS32 t;
        NvU32 status = hwfs_event_temp(dev, e, tlimit, &t);
        if (status == NV_ERR_NOT_SUPPORTED)
            continue;
        if (status != NV_OK)
            return nvml_from_rm(status);
        if (t <= 0 || t >= 150 * 256)     /* unused events report junk */
            continue;
        if (!found || t < min)
            min = t;
        found = true;
    }
    if (!found)
        return NVML_ERROR_NOT_SUPPORTED;
    *temp = min;
    return NVML_SUCCESS;
}

static nvmlReturn_t policy_info(struct nvmlDevice_st *dev)
{
    nvmlReturn_t ret = NVML_SUCCESS;

    pthread_mutex_lock(&dev->lock);
    if (!dev->policies_known) {
        NvU8 p[POLICY_INFO_SIZE];
        memset(p, 0, sizeof(p));
        NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_THERMAL_POLICY_GET_INFO, p, sizeof(p));
        if (status == NV_OK) {
            memcpy(&dev->policy_mask, p + POLICY_MASK_OFFSET, sizeof(dev->policy_mask));
            dev->policy_gps = p[POLICY_INDEX_OFFSET + 0];
            dev->policy_acoustic = p[POLICY_INDEX_OFFSET + 1];
            dev->policy_mem = p[POLICY_INDEX_OFFSET + 2];
            dev->policy_slowdown = p[POLICY_INDEX_OFFSET + 3];
            dev->tlimit_ref = 0;
            if (dev->policy_slowdown < POLICY_MAX &&
                (dev->policy_mask & (1U << dev->policy_slowdown)))
                memcpy(&dev->tlimit_ref, p + POLICY_INFO_LIMITS +
                       dev->policy_slowdown * POLICY_INFO_STRIDE + 4, sizeof(dev->tlimit_ref));
            dev->policies_known = true;
        } else if (status == NV_ERR_NOT_SUPPORTED) {
            dev->policy_mask = 0;
            dev->policy_gps = dev->policy_acoustic = POLICY_INDEX_NONE;
            dev->policy_mem = dev->policy_slowdown = POLICY_INDEX_NONE;
            dev->tlimit_ref = 0;
            dev->policies_known = true;
        } else {
            ret = nvml_from_rm(status);
        }
    }
    pthread_mutex_unlock(&dev->lock);
    return ret;
}

/* Current limit of the policy picked by which (offset of its index). */
static nvmlReturn_t policy_limit(struct nvmlDevice_st *dev, const NvU8 *index, unsigned *temp)
{
    nvmlReturn_t ret = policy_info(dev);
    if (ret != NVML_SUCCESS)
        return ret;
    NvU8 i = *index;
    if (i >= POLICY_MAX || !(dev->policy_mask & (1U << i)))
        return NVML_ERROR_NOT_SUPPORTED;

    NvU8 p[POLICY_CONTROL_SIZE];
    memset(p, 0, sizeof(p));
    memcpy(p + POLICY_MASK_OFFSET, &dev->policy_mask, sizeof(dev->policy_mask));
    NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_THERMAL_POLICY_GET_CONTROL, p, sizeof(p));
    if (status != NV_OK)
        return nvml_from_rm(status);
    NvS32 limit;
    memcpy(&limit, p + POLICY_CONTROL_FIRST + i * POLICY_CONTROL_STRIDE + POLICY_LIMIT_OFFSET,
           sizeof(limit));
    if (limit <= 0)
        return NVML_ERROR_NOT_SUPPORTED;
    *temp = (unsigned)(limit >> 8);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetTemperatureThreshold(nvmlDevice_t device,
                                                           nvmlTemperatureThresholds_t thresholdType,
                                                           unsigned int *temp)
{
    DEVICE(device, dev);
    if (temp == NULL || (unsigned)thresholdType >= NVML_TEMPERATURE_THRESHOLD_COUNT)
        return NVML_ERROR_INVALID_ARGUMENT;
    NvS32 t;
    nvmlReturn_t ret;
    switch (thresholdType) {
    case NVML_TEMPERATURE_THRESHOLD_SHUTDOWN:
        ret = shutdown_temp(dev, false, &t);
        if (ret == NVML_SUCCESS)
            *temp = (unsigned)(t >> 8);
        return ret;
    case NVML_TEMPERATURE_THRESHOLD_SLOWDOWN:
        ret = slowdown_temp(dev, false, &t);
        if (ret == NVML_SUCCESS)
            *temp = (unsigned)(t >> 8);
        return ret;
    case NVML_TEMPERATURE_THRESHOLD_GPU_MAX:
        return policy_limit(dev, &dev->policy_slowdown, temp);
    case NVML_TEMPERATURE_THRESHOLD_GPS_CURR:
        return policy_limit(dev, &dev->policy_gps, temp);
    default:
        return NVML_ERROR_NOT_SUPPORTED;
    }
}

nvmlReturn_t nvml_tlimit_field(struct nvmlDevice_st *dev, unsigned field, int *value)
{
    nvmlReturn_t ret = policy_info(dev);
    if (ret != NVML_SUCCESS)
        return ret;
    if (dev->tlimit_ref <= 0)
        return NVML_ERROR_NOT_SUPPORTED;

    NvS32 t;
    switch (field) {
    case NVML_FI_DEV_TEMPERATURE_SHUTDOWN_TLIMIT:
        ret = shutdown_temp(dev, true, &t);
        break;
    case NVML_FI_DEV_TEMPERATURE_SLOWDOWN_TLIMIT:
        ret = slowdown_temp(dev, true, &t);
        break;
    case NVML_FI_DEV_TEMPERATURE_GPU_MAX_TLIMIT:
        t = dev->tlimit_ref;    /* T.Limit is this temperature */
        break;
    default:
        return NVML_ERROR_NOT_SUPPORTED;
    }
    if (ret == NVML_SUCCESS)
        *value = (dev->tlimit_ref >> 8) - (t >> 8);
    return ret;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMarginTemperature(nvmlDevice_t device,
                                                       nvmlMarginTemperature_t *marginTempInfo)
{
    DEVICE(device, dev);
    if (marginTempInfo == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (marginTempInfo->version != nvmlMarginTemperature_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    nvmlReturn_t ret = policy_info(dev);
    if (ret != NVML_SUCCESS)
        return ret;
    if (dev->policy_mask == 0)
        return NVML_ERROR_NOT_SUPPORTED;

    NvU8 p[POLICY_STATUS_SIZE];
    memset(p, 0, sizeof(p));
    memcpy(p + POLICY_MASK_OFFSET, &dev->policy_mask, sizeof(dev->policy_mask));
    NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_THERMAL_POLICY_GET_STATUS, p, sizeof(p));
    if (status != NV_OK)
        return nvml_from_rm(status);
    NvS32 margin;
    memcpy(&margin, p + POLICY_STATUS_MARGIN, sizeof(margin));
    /* 1/256 C, rounded to the nearest degree */
    marginTempInfo->marginTemperature = margin >= 0 ? (margin + 128) >> 8 : -((-margin + 128) >> 8);
    return NVML_SUCCESS;
}
