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
#include "ctrl/ctrl2080/ctrl2080gpu.h"

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
    case NVML_FI_DEV_PERF_POLICY_LOW_UTILIZATION:
    case NVML_FI_DEV_PERF_POLICY_RELIABILITY:
    case NVML_FI_DEV_PERF_POLICY_TOTAL_APP_CLOCKS:
    case NVML_FI_DEV_PERF_POLICY_TOTAL_BASE_CLOCKS: {
        static const unsigned policy[] = {
            NVML_PERF_POLICY_POWER, NVML_PERF_POLICY_THERMAL, NVML_PERF_POLICY_SYNC_BOOST,
            NVML_PERF_POLICY_BOARD_LIMIT, NVML_PERF_POLICY_LOW_UTILIZATION, NVML_PERF_POLICY_RELIABILITY,
            NVML_PERF_POLICY_TOTAL_APP_CLOCKS, NVML_PERF_POLICY_TOTAL_BASE_CLOCKS,
        };
        unsigned long long ns = 0;
        nvmlReturn_t r = violation_time(dev, policy[v->fieldId - NVML_FI_DEV_PERF_POLICY_POWER], &ns);
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
    case NVML_FI_DEV_ECC_CURRENT:
    case NVML_FI_DEV_ECC_PENDING: {
        nvmlEnableState_t cur, pend;
        v->valueType = NVML_VALUE_TYPE_UNSIGNED_LONG_LONG;
        v->nvmlReturn = nvmlDeviceGetEccMode(dev, &cur, &pend);
        if (v->nvmlReturn == NVML_SUCCESS)
            v->value.ullVal = (v->fieldId == NVML_FI_DEV_ECC_CURRENT ? cur : pend) == NVML_FEATURE_ENABLED;
        break;
    }
    /* Closed returns SUCCESS for these without writing a value when ECC is
     * off; we say NOT_SUPPORTED, like nvmlDeviceGetTotalEccErrors. */
    case NVML_FI_DEV_ECC_SBE_VOL_TOTAL:
    case NVML_FI_DEV_ECC_DBE_VOL_TOTAL:
    case NVML_FI_DEV_ECC_SBE_AGG_TOTAL:
    case NVML_FI_DEV_ECC_DBE_AGG_TOTAL: {
        unsigned i = v->fieldId - NVML_FI_DEV_ECC_SBE_VOL_TOTAL;
        unsigned long long n = 0;
        nvmlReturn_t r = nvml_ecc_total(dev, i & 1 ? NVML_MEMORY_ERROR_TYPE_UNCORRECTED
                                                   : NVML_MEMORY_ERROR_TYPE_CORRECTED,
                                        i & 2 ? NVML_AGGREGATE_ECC : NVML_VOLATILE_ECC, &n);
        set_ull(v, r, n);
        break;
    }
    case NVML_FI_DEV_RETIRED_SBE:
    case NVML_FI_DEV_RETIRED_DBE:
    case NVML_FI_DEV_RETIRED_PENDING:
    case NVML_FI_DEV_RETIRED_PENDING_SBE:
    case NVML_FI_DEV_RETIRED_PENDING_DBE: {
        unsigned long long n = 0;
        nvmlReturn_t r = nvml_retired_pages_field(dev, v->fieldId, &n);
        set_ull(v, r, n);
        break;
    }
    case NVML_FI_DEV_REMAPPED_COR:
    case NVML_FI_DEV_REMAPPED_UNC:
    case NVML_FI_DEV_REMAPPED_PENDING:
    case NVML_FI_DEV_REMAPPED_FAILURE:
    case NVML_FI_DEV_REMAPPED_ROWS_COR_INACTIVE:
    case NVML_FI_DEV_REMAPPED_ROWS_UNC_INACTIVE: {
        nvmlRemappedRowsInfo_v2_t ri;
        unsigned long long n = 0;
        nvmlReturn_t r = nvmlDeviceGetRemappedRows_v2(dev, &ri);
        switch (v->fieldId) {
        case NVML_FI_DEV_REMAPPED_COR:               n = ri.corrActiveRemaps; break;
        case NVML_FI_DEV_REMAPPED_UNC:               n = ri.uncActiveRemaps; break;
        case NVML_FI_DEV_REMAPPED_PENDING:           n = ri.bPending; break;
        case NVML_FI_DEV_REMAPPED_FAILURE:           n = ri.bFailureOccurred; break;
        case NVML_FI_DEV_REMAPPED_ROWS_COR_INACTIVE: n = ri.corrInactiveRemaps; break;
        case NVML_FI_DEV_REMAPPED_ROWS_UNC_INACTIVE: n = ri.uncInactiveRemaps; break;
        }
        set_ull(v, r, n);
        break;
    }
    case NVML_FI_DEV_TOTAL_ENERGY_CONSUMPTION:
    case NVML_FI_DEV_ENERGY: {
        unsigned long long mj = 0;
        nvmlReturn_t r = nvmlDeviceGetTotalEnergyConsumption(dev, &mj);
        set_ull(v, r, mj);
        break;
    }
    /* One PCIe counter each. */
    case NVML_FI_DEV_PCIE_REPLAY_COUNTER:
    case NVML_FI_DEV_PCIE_L0_TO_RECOVERY_COUNTER:
    case NVML_FI_DEV_PCIE_COUNT_CORRECTABLE_ERRORS:
    case NVML_FI_DEV_PCIE_COUNT_NAKS_RECEIVED:
    case NVML_FI_DEV_PCIE_COUNT_RECEIVER_ERROR:
    case NVML_FI_DEV_PCIE_COUNT_BAD_TLP:
    case NVML_FI_DEV_PCIE_COUNT_NAKS_SENT:
    case NVML_FI_DEV_PCIE_COUNT_BAD_DLLP:
    case NVML_FI_DEV_PCIE_COUNT_NON_FATAL_ERROR:
    case NVML_FI_DEV_PCIE_COUNT_FATAL_ERROR:
    case NVML_FI_DEV_PCIE_COUNT_UNSUPPORTED_REQ:
    case NVML_FI_DEV_PCIE_COUNT_LCRC_ERROR: {
        NvU32 bit;
        unsigned u;
        switch (v->fieldId) {
        case NVML_FI_DEV_PCIE_REPLAY_COUNTER:           bit = NV2080_CTRL_BUS_PEX_COUNTER_REPLAY_COUNT; break;
        case NVML_FI_DEV_PCIE_L0_TO_RECOVERY_COUNTER:   bit = NV2080_CTRL_BUS_PEX_COUNTER_L0_TO_RECOVERY_COUNT; break;
        case NVML_FI_DEV_PCIE_COUNT_CORRECTABLE_ERRORS: bit = NV2080_CTRL_BUS_PEX_COUNTER_CORR_ERROR_COUNT; break;
        case NVML_FI_DEV_PCIE_COUNT_NAKS_RECEIVED:      bit = NV2080_CTRL_BUS_PEX_COUNTER_NAKS_RCVD_COUNT; break;
        case NVML_FI_DEV_PCIE_COUNT_RECEIVER_ERROR:     bit = NV2080_CTRL_BUS_PEX_COUNTER_RECEIVER_ERRORS; break;
        case NVML_FI_DEV_PCIE_COUNT_BAD_TLP:            bit = NV2080_CTRL_BUS_PEX_COUNTER_BAD_TLP_COUNT; break;
        case NVML_FI_DEV_PCIE_COUNT_NAKS_SENT:          bit = NV2080_CTRL_BUS_PEX_COUNTER_NAKS_SENT_COUNT; break;
        case NVML_FI_DEV_PCIE_COUNT_BAD_DLLP:           bit = NV2080_CTRL_BUS_PEX_COUNTER_BAD_DLLP_COUNT; break;
        case NVML_FI_DEV_PCIE_COUNT_NON_FATAL_ERROR:    bit = NV2080_CTRL_BUS_PEX_COUNTER_NON_FATAL_ERROR_COUNT; break;
        case NVML_FI_DEV_PCIE_COUNT_FATAL_ERROR:        bit = NV2080_CTRL_BUS_PEX_COUNTER_FATAL_ERROR_COUNT; break;
        case NVML_FI_DEV_PCIE_COUNT_UNSUPPORTED_REQ:    bit = NV2080_CTRL_BUS_PEX_COUNTER_UNSUPP_REQ_COUNT; break;
        default:                                        bit = NV2080_CTRL_BUS_PEX_COUNTER_LCRC_ERRORS_COUNT; break;
        }
        v->nvmlReturn = nvml_pex_counter(dev, bit, &u);
        if (v->nvmlReturn == NVML_SUCCESS)
            set_uint(v, u);
        break;
    }
    /* scopeId = lane. */
    case NVML_FI_DEV_PCIE_COUNT_LANE_ERROR: {
        NV2080_CTRL_CMD_BUS_GET_PEX_LANE_COUNTERS_PARAMS p = { 0 };
        v->valueType = NVML_VALUE_TYPE_UNSIGNED_INT;
        if (v->scopeId >= NV2080_CTRL_PEX_MAX_LANES) {
            v->nvmlReturn = NVML_ERROR_INVALID_ARGUMENT;
            break;
        }
        NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_PEX_LANE_COUNTERS, &p, sizeof(p));
        v->nvmlReturn = nvml_from_rm(st);
        if (st == NV_OK)
            set_uint(v, p.pexLaneCounter[v->scopeId]);
        break;
    }
    /* Free-running byte counters (they wrap). */
    case NVML_FI_DEV_PCIE_COUNT_TX_BYTES:
    case NVML_FI_DEV_PCIE_COUNT_RX_BYTES: {
        bool tx = v->fieldId == NVML_FI_DEV_PCIE_COUNT_TX_BYTES;
        NV2080_CTRL_BUS_GET_PEX_UTIL_COUNTERS_PARAMS p = {
            .pexCounterMask = tx ? NV2080_CTRL_BUS_PEX_UTIL_COUNTER_TX_BYTES : NV2080_CTRL_BUS_PEX_UTIL_COUNTER_RX_BYTES,
        };
        NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_PEX_UTIL_COUNTERS, &p, sizeof(p));
        v->nvmlReturn = nvml_from_rm(st);
        if (st == NV_OK)
            set_uint(v, p.pexCounters[tx ? 0 : 1]);
        break;
    }
    /* Outbound = what the GPU can request on the bus, inbound = what it completes. */
    case NVML_FI_DEV_PCIE_OUTBOUND_ATOMICS_MASK: {
        NV2080_CTRL_CMD_BUS_GET_PCIE_REQ_ATOMICS_CAPS_PARAMS p = {
            .capType = NV2080_CTRL_CMD_BUS_PCIE_ATOMICS_CAPTYPE_GPU,
        };
        NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_PCIE_REQ_ATOMICS_CAPS, &p, sizeof(p));
        v->nvmlReturn = nvml_from_rm(st);
        if (st == NV_OK)
            set_uint(v, p.atomicsCaps);
        break;
    }
    case NVML_FI_DEV_PCIE_INBOUND_ATOMICS_MASK: {
        NV2080_CTRL_CMD_BUS_GET_PCIE_CPL_ATOMICS_CAPS_PARAMS p = { 0 };
        NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_PCIE_CPL_ATOMICS_CAPS, &p, sizeof(p));
        v->nvmlReturn = nvml_from_rm(st);
        if (st == NV_OK)
            set_uint(v, p.atomicsCaps);
        break;
    }
    /* Closed reads GPU_INFO SMC_MODE; 0 (unsupported) here. Which modes it
     * counts as capable is a guess. */
    case NVML_FI_DEV_IS_MIG_MODE_INDEPENDENT_MIG_QUERY_CAPABLE: {
        NV2080_CTRL_GPU_GET_INFO_V2_PARAMS p = {
            .gpuInfoListSize = 1,
            .gpuInfoList = { { .index = NV2080_CTRL_GPU_INFO_INDEX_GPU_SMC_MODE } },
        };
        NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_INFO_V2, &p, sizeof(p));
        v->nvmlReturn = nvml_from_rm(st);
        if (st == NV_OK)
            set_uint(v, p.gpuInfoList[0].data != NV2080_CTRL_GPU_INFO_GPU_SMC_MODE_UNSUPPORTED);
        break;
    }
    /* Closed decides from its cached clock tables (no RM call of its own):
     * RUNTIME here, where the P-states have several memory clocks. Taken as
     * "more than one supported memory clock"; no rate limit known. */
    case NVML_FI_DEV_MCLK_SWITCH_TYPE: {
        unsigned clocks[32], n = 32;
        nvmlReturn_t r = nvmlDeviceGetSupportedMemoryClocks(dev, &n, clocks);
        v->valueType = NVML_VALUE_TYPE_UNSIGNED_INT;
        if (r == NVML_ERROR_INSUFFICIENT_SIZE)
            r = NVML_SUCCESS;
        if (r == NVML_SUCCESS)
            set_uint(v, n > 1 ? NVML_MCLK_SWITCH_TYPE_RUNTIME : NVML_MCLK_SWITCH_TYPE_NOT_SUPPORTED);
        else
            v->nvmlReturn = r;
        break;
    }
    case NVML_FI_DEV_MCLK_MIN_SWITCH_INTERVAL_MILLISECONDS:
        set_uint(v, 0);
        break;
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
        if (values[i].fieldId == 0 || values[i].fieldId >= NVML_FI_MAX)
            values[i].nvmlReturn = NVML_ERROR_INVALID_ARGUMENT;
        else
            field_value(dev, &values[i]);
    }
    return NVML_SUCCESS;
}
