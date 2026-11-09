/*
 * SPDX-License-Identifier: MIT
 *
 * Clocks event (throttle) reasons and nvmlDeviceGetFieldValues.
 */

#include <time.h>

#include "nvml-priv.h"

#include "nvfixedtypes.h"

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

/* RUSD_CLK_THROTTLE_REASON_* and nvmlClocksEventReason* use the same bits;
 * these are the ones both know. */
#define NVML_EVENT_REASONS_KNOWN 0x7ffULL

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCurrentClocksEventReasons(nvmlDevice_t device,
                                                                unsigned long long *reasons)
{
    RUSD_CLK_THROTTLE_INFO t;
    DEVICE(device, dev);
    CHECK_PTR(reasons);
    if (dev->rusd == NULL ||
        !nvml_rusd_read(dev, &dev->rusd->clkThrottleInfo.lastModifiedTimestamp,
                        (void *)&dev->rusd->clkThrottleInfo.reasonMask, &t.reasonMask,
                        sizeof(t.reasonMask)))
        return NVML_ERROR_NOT_SUPPORTED;
    *reasons = t.reasonMask & NVML_EVENT_REASONS_KNOWN;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCurrentClocksThrottleReasons(nvmlDevice_t device,
                                                                   unsigned long long *reasons)
{
    return nvmlDeviceGetCurrentClocksEventReasons(device, reasons);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetSupportedClocksEventReasons(nvmlDevice_t device,
                                                                  unsigned long long *reasons)
{
    DEVICE(device, dev);
    CHECK_PTR(reasons);
    if (dev->rusd == NULL)
        return NVML_ERROR_NOT_SUPPORTED;
    *reasons = NVML_EVENT_REASONS_KNOWN;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetSupportedClocksThrottleReasons(nvmlDevice_t device,
                                                                     unsigned long long *reasons)
{
    return nvmlDeviceGetSupportedClocksEventReasons(device, reasons);
}

/* ---- field values ----------------------------------------------------------- */

static void set_uint(nvmlFieldValue_t *v, unsigned u)
{
    v->valueType = NVML_VALUE_TYPE_UNSIGNED_INT;
    v->value.uiVal = u;
    v->nvmlReturn = NVML_SUCCESS;
}

static void field_value(struct nvmlDevice_st *dev, nvmlFieldValue_t *v)
{
    v->nvmlReturn = NVML_ERROR_NOT_SUPPORTED;

    switch (v->fieldId) {
    case NVML_FI_DEV_POWER_AVERAGE: {
        RUSD_AVG_POWER_INFO p;
        if (v->scopeId == NVML_POWER_SCOPE_GPU && NVML_RUSD_READ(dev, avgPowerUsage, &p))
            set_uint(v, p.averageGpuPower);
        break;
    }
    case NVML_FI_DEV_POWER_INSTANT: {
        RUSD_INST_POWER_INFO p;
        if (v->scopeId == NVML_POWER_SCOPE_GPU && NVML_RUSD_READ(dev, instPowerUsage, &p))
            set_uint(v, p.instGpuPower);
        break;
    }
    case NVML_FI_DEV_POWER_MIN_LIMIT:
    case NVML_FI_DEV_POWER_MAX_LIMIT:
    case NVML_FI_DEV_POWER_DEFAULT_LIMIT: {
        unsigned lim[3];
        v->valueType = NVML_VALUE_TYPE_UNSIGNED_INT;   /* set even when unsupported */
        if (v->scopeId != NVML_POWER_SCOPE_GPU)
            break;
        v->nvmlReturn = nvml_power_limits(dev, &lim[0], &lim[2], &lim[1]);
        if (v->nvmlReturn == NVML_SUCCESS)
            v->value.uiVal = lim[v->fieldId - NVML_FI_DEV_POWER_MIN_LIMIT];
        break;
    }
    case NVML_FI_DEV_POWER_CURRENT_LIMIT: {
        RUSD_POWER_LIMIT_INFO l;
        v->valueType = NVML_VALUE_TYPE_UNSIGNED_INT;
        if (v->scopeId == NVML_POWER_SCOPE_GPU && NVML_RUSD_READ(dev, powerLimitGpu, &l))
            set_uint(v, l.enforcedmW);
        break;
    }
    case NVML_FI_DEV_GPU_TEMP: {
        RUSD_TEMPERATURE t;
        if (dev->rusd != NULL &&
            nvml_rusd_read(dev, &dev->rusd->temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp,
                           (void *)&dev->rusd->temperatures[RUSD_TEMPERATURE_TYPE_GPU].temperature,
                           &t.temperature, sizeof(t.temperature))) {
            v->valueType = NVML_VALUE_TYPE_DOUBLE;
            v->value.dVal = t.temperature / 256.0;   /* NvTemp: 24.8 fixed point */
            v->nvmlReturn = NVML_SUCCESS;
        }
        break;
    }
    case NVML_FI_DEV_TEMPERATURE_SHUTDOWN_TLIMIT:
    case NVML_FI_DEV_TEMPERATURE_SLOWDOWN_TLIMIT:
    case NVML_FI_DEV_TEMPERATURE_MEM_MAX_TLIMIT:
    case NVML_FI_DEV_TEMPERATURE_GPU_MAX_TLIMIT: {
        int t;
        v->valueType = NVML_VALUE_TYPE_SIGNED_INT;   /* set even when unsupported */
        v->nvmlReturn = nvml_tlimit_field(dev, v->fieldId, &t);
        if (v->nvmlReturn == NVML_SUCCESS)
            v->value.siVal = t;
        break;
    }
    default:
        break;
    }
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetFieldValues(nvmlDevice_t device, int valuesCount,
                                                  nvmlFieldValue_t *values)
{
    struct timespec ts;
    DEVICE(device, dev);
    if (valuesCount < 0 || (valuesCount > 0 && values == NULL))
        return NVML_ERROR_INVALID_ARGUMENT;

    clock_gettime(CLOCK_REALTIME, &ts);
    for (int i = 0; i < valuesCount; i++) {
        values[i].timestamp = (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
        values[i].latencyUsec = 0;
        field_value(dev, &values[i]);
    }
    return NVML_SUCCESS;
}
