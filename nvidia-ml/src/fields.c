/*
 * SPDX-License-Identifier: MIT
 *
 * Clocks event (throttle) reasons and nvmlDeviceGetFieldValues.
 */

#include <stddef.h>
#include <time.h>

#include "nvml-priv.h"

#include "nvfixedtypes.h"
#include "ctrl/ctrl2080/ctrl2080bus.h"

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

/* ---- violation counters ----------------------------------------------------- */

/*
 * How long clocks were held down, per limiter. RUSD keeps, per perf
 * policy (NVML's nvmlPerfPolicyType_t order, 0 power .. 5 reliability),
 * the time spent below each perf point; NVML reports the time below the
 * turbo boost point, and for the totals the time below turbo boost (any
 * limiter, "application clocks") and rated TDP ("base clocks").
 */
static bool rusd_throttle(struct nvmlDevice_st *dev, RUSD_CLK_THROTTLE_INFO *t)
{
    return dev->rusd != NULL &&
           nvml_rusd_read(dev, &dev->rusd->clkThrottleInfo.lastModifiedTimestamp,
                          (void *)&dev->rusd->clkThrottleInfo.reasonMask, &t->reasonMask,
                          sizeof(*t) - offsetof(RUSD_CLK_THROTTLE_INFO, reasonMask));
}

/*
 * The thermal policy and the HW slowdowns come from the HW failsafe event
 * status (legacy THERMAL control 0x42, not in the open headers; layout
 * checked against the closed library): a mask of the events the board has
 * and, per event id, how often and how long (ns) it was asserted.
 */
#define NV2080_CTRL_CMD_THERMAL_HWFS_EVENT_STATUS_GET 0x20808542
#define HWFS_EVENT_COUNT    31
#define HWFS_EVENT_EXT_POWER 2   /* external power brake input */

struct hwfs_event_status {
    NvU32 eventMask;
    NvU32 pad;
    struct { NvU64 count, timeNs; } events[HWFS_EVENT_COUNT];
};

/* Time (ns) in HW power brake slowdown, or in any thermal HW slowdown.
 * Which events the closed library sums for the latter could not be seen
 * (they are all 0 here); this takes every event but the power brake. */
static nvmlReturn_t hwfs_time(struct nvmlDevice_st *dev, bool power_brake, unsigned long long *ns)
{
    struct hwfs_event_status p = { 0 };
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_THERMAL_HWFS_EVENT_STATUS_GET, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *ns = 0;
    for (unsigned i = 0; i < HWFS_EVENT_COUNT; i++)
        if ((p.eventMask & (1u << i)) && (i == HWFS_EVENT_EXT_POWER) == power_brake)
            *ns += p.events[i].timeNs;
    return NVML_SUCCESS;
}

static nvmlReturn_t violation_time(struct nvmlDevice_st *dev, unsigned policy, unsigned long long *ns)
{
    RUSD_CLK_THROTTLE_INFO t;
    if (policy == NVML_PERF_POLICY_THERMAL)
        return hwfs_time(dev, false, ns);
    if (policy > NVML_PERF_POLICY_RELIABILITY && policy != NVML_PERF_POLICY_TOTAL_APP_CLOCKS &&
        policy != NVML_PERF_POLICY_TOTAL_BASE_CLOCKS)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (!rusd_throttle(dev, &t))
        return NVML_ERROR_NOT_SUPPORTED;
    if (policy == NVML_PERF_POLICY_TOTAL_APP_CLOCKS)
        *ns = t.globalViolation.timeNs[RUSD_PERF_POINT_TURBO_BOOST];
    else if (policy == NVML_PERF_POLICY_TOTAL_BASE_CLOCKS)
        *ns = t.globalViolation.timeNs[RUSD_PERF_POINT_RATED_TDP];
    else if (t.supportedViolationTimeMask & (1u << policy))
        *ns = t.violation[policy].timeNs[RUSD_PERF_POINT_TURBO_BOOST];
    else
        return NVML_ERROR_NOT_SUPPORTED;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetViolationStatus(nvmlDevice_t device, nvmlPerfPolicyType_t perfPolicyType,
                                                      nvmlViolationTime_t *violTime)
{
    struct timespec ts;
    unsigned long long ns;
    DEVICE(device, dev);
    CHECK_PTR(violTime);
    nvmlReturn_t r = violation_time(dev, perfPolicyType, &ns);
    if (r != NVML_SUCCESS)
        return r;
    clock_gettime(CLOCK_REALTIME, &ts);
    violTime->referenceTime = (unsigned long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    violTime->violationTime = ns;
    return NVML_SUCCESS;
}

/* ---- field values ----------------------------------------------------------- */

static void set_uint(nvmlFieldValue_t *v, unsigned u)
{
    v->valueType = NVML_VALUE_TYPE_UNSIGNED_INT;
    v->value.uiVal = u;
    v->nvmlReturn = NVML_SUCCESS;
}

static void set_ull(nvmlFieldValue_t *v, nvmlReturn_t r, unsigned long long u)
{
    v->valueType = NVML_VALUE_TYPE_UNSIGNED_LONG_LONG;
    v->nvmlReturn = r;
    if (r == NVML_SUCCESS)
        v->value.ullVal = u;
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
    case NVML_FI_DEV_PERF_POLICY_POWER:
    case NVML_FI_DEV_PERF_POLICY_THERMAL:
    case NVML_FI_DEV_PERF_POLICY_SYNC_BOOST:
    case NVML_FI_DEV_PERF_POLICY_BOARD_LIMIT:
    case NVML_FI_DEV_PERF_POLICY_LOW_UTILIZATION: {
        unsigned long long ns = 0;
        nvmlReturn_t r = violation_time(dev, v->fieldId - NVML_FI_DEV_PERF_POLICY_POWER, &ns);
        set_ull(v, r, ns);
        break;
    }
    case NVML_FI_DEV_CLOCKS_EVENT_REASON_SW_THERM_SLOWDOWN: {
        RUSD_CLK_THROTTLE_INFO t;
        v->valueType = NVML_VALUE_TYPE_UNSIGNED_LONG_LONG;
        if (rusd_throttle(dev, &t) && (t.supportedViolationTimeMask & (1u << NVML_PERF_POLICY_THERMAL)))
            set_ull(v, NVML_SUCCESS, t.violation[NVML_PERF_POLICY_THERMAL].timeNs[RUSD_PERF_POINT_TURBO_BOOST]);
        break;
    }
    case NVML_FI_DEV_CLOCKS_EVENT_REASON_HW_THERM_SLOWDOWN:
    case NVML_FI_DEV_CLOCKS_EVENT_REASON_HW_POWER_BRAKE_SLOWDOWN: {
        unsigned long long ns = 0;
        nvmlReturn_t r = hwfs_time(dev, v->fieldId == NVML_FI_DEV_CLOCKS_EVENT_REASON_HW_POWER_BRAKE_SLOWDOWN, &ns);
        set_ull(v, r, ns);
        break;
    }
    case NVML_FI_DEV_PCIE_REPLAY_ROLLOVER_COUNTER: {
        unsigned u;
        v->nvmlReturn = nvml_pex_counter(dev, NV2080_CTRL_BUS_PEX_COUNTER_REPLAY_ROLLOVER_COUNT, &u);
        if (v->nvmlReturn == NVML_SUCCESS)
            set_uint(v, u);
        break;
    }
    case NVML_FI_DEV_GET_GPU_RECOVERY_ACTION:
    case NVML_FI_DEV_RESET_STATUS:
    case NVML_FI_DEV_DRAIN_AND_RESET_STATUS: {
        unsigned u;
        if (v->fieldId == NVML_FI_DEV_GET_GPU_RECOVERY_ACTION)
            v->nvmlReturn = nvml_recovery_action(dev, &u);
        else
            v->nvmlReturn = nvml_reset_status(dev, v->fieldId == NVML_FI_DEV_DRAIN_AND_RESET_STATUS, &u);
        if (v->nvmlReturn == NVML_SUCCESS)
            set_uint(v, u);
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
