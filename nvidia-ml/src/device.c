/*
 * SPDX-License-Identifier: MIT
 *
 * Per-device queries. Static values come from read_static_info() in
 * core.c, live sensor values from the RUSD page.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nvml-priv.h"

#include "nvmisc.h"
#include "nvfixedtypes.h"
#include "nvstatus.h"
#include "ctrl/ctrl2080/ctrl2080bus.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080mc.h"

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

/* ---- identity --------------------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t device, char *name, unsigned int length)
{
    DEVICE(device, dev);
    return nvml_copy_string(name, length, dev->name);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char *uuid, unsigned int length)
{
    DEVICE(device, dev);
    return nvml_copy_string(uuid, length, dev->uuid);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetIndex(nvmlDevice_t device, unsigned int *index)
{
    DEVICE(device, dev);
    CHECK_PTR(index);
    *index = dev->index;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMinorNumber(nvmlDevice_t device, unsigned int *minor)
{
    DEVICE(device, dev);
    CHECK_PTR(minor);
    *minor = (unsigned)dev->minor;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t device, nvmlPciInfo_t *pci)
{
    DEVICE(device, dev);
    CHECK_PTR(pci);
    *pci = dev->pci;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPciInfo_v2(nvmlDevice_t device, nvmlPciInfo_t *pci)
{
    return nvmlDeviceGetPciInfo_v3(device, pci);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPciInfo(nvmlDevice_t device, nvmlPciInfo_t *pci)
{
    return nvmlDeviceGetPciInfo_v3(device, pci);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetArchitecture(nvmlDevice_t device, nvmlDeviceArchitecture_t *arch)
{
    DEVICE(device, dev);
    CHECK_PTR(arch);
    switch (dev->arch) {
    case NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_TU100: *arch = NVML_DEVICE_ARCH_TURING; break;
    case NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GA100: *arch = NVML_DEVICE_ARCH_AMPERE; break;
    case NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_AD100: *arch = NVML_DEVICE_ARCH_ADA; break;
    case NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GH100: *arch = NVML_DEVICE_ARCH_HOPPER; break;
    case NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GB100:
    case NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GB200: *arch = NVML_DEVICE_ARCH_BLACKWELL; break;
    default:                                          *arch = NVML_DEVICE_ARCH_UNKNOWN; break;
    }
    return NVML_SUCCESS;
}

/* GPU part number, e.g. "27E0-750-A1" (export table slot 231). */
nvmlReturn_t nvml_gpu_part_number(nvmlDevice_t device, char *buf, unsigned length)
{
    DEVICE(device, dev);
    if (dev->chip_sku[0] == 0)
        return NVML_ERROR_NOT_SUPPORTED;
    return nvml_copy_string(buf, length, dev->chip_sku);
}

/* ---- memory ----------------------------------------------------------------- */

/* Used/free come from the physical memory allocator, as RUSD reports them. */
static bool fb_usage(struct nvmlDevice_st *dev, NvU64 *free_bytes, NvU64 *reserved)
{
    NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb = {
        .fbInfoListSize = 2,
        .fbInfoList = {
            { .index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE },
            { .index = NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE },
        },
    };
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof(fb)) != NV_OK)
        return false;
    *free_bytes = (NvU64)fb.fbInfoList[0].data << 10;
    /* What RM keeps for itself is outside the heap. */
    *reserved = dev->fb_total - ((NvU64)fb.fbInfoList[1].data << 10);
    return true;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t device, nvmlMemory_v2_t *memory)
{
    NvU64 free_bytes, reserved;
    DEVICE(device, dev);
    CHECK_PTR(memory);
    if (memory->version != nvmlMemory_v2)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    if (!fb_usage(dev, &free_bytes, &reserved))
        return NVML_ERROR_UNKNOWN;
    memory->total = dev->fb_total;
    memory->reserved = reserved;
    memory->free = free_bytes;
    memory->used = dev->fb_total - free_bytes - reserved;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t *memory)
{
    NvU64 free_bytes, reserved;
    DEVICE(device, dev);
    CHECK_PTR(memory);
    if (!fb_usage(dev, &free_bytes, &reserved))
        return NVML_ERROR_UNKNOWN;
    /* v1 counts the reserved memory as used. */
    memory->total = dev->fb_total;
    memory->free = free_bytes;
    memory->used = dev->fb_total - free_bytes;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetBAR1MemoryInfo(nvmlDevice_t device, nvmlBAR1Memory_t *bar1)
{
    DEVICE(device, dev);
    CHECK_PTR(bar1);
    if (dev->rusd == NULL)
        return NVML_ERROR_NOT_SUPPORTED;
    RUSD_BAR1_MEMORY_INFO b;
    if (!nvml_rusd_read(dev, &dev->rusd->bar1MemoryInfo.lastModifiedTimestamp,
                        (void *)&dev->rusd->bar1MemoryInfo.bar1Size,
                        &b.bar1Size, sizeof(b) - sizeof(b.lastModifiedTimestamp)))
        return NVML_ERROR_NOT_SUPPORTED;
    bar1->bar1Total = (unsigned long long)b.bar1Size << 10;
    bar1->bar1Free = (unsigned long long)b.bar1AvailSize << 10;
    bar1->bar1Used = bar1->bar1Total - bar1->bar1Free;
    return NVML_SUCCESS;
}

/* ---- live sensors (RUSD) ---------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device,
                                                  nvmlTemperatureSensors_t sensor,
                                                  unsigned int *temp)
{
    RUSD_TEMPERATURE t;
    DEVICE(device, dev);
    CHECK_PTR(temp);
    if (sensor == NVML_TEMPERATURE_GPU_MAX)    /* no hotspot sensor in RUSD */
        return NVML_ERROR_NOT_SUPPORTED;
    if (sensor != NVML_TEMPERATURE_GPU)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (dev->rusd == NULL ||
        !nvml_rusd_read(dev, &dev->rusd->temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp,
                        (void *)&dev->rusd->temperatures[RUSD_TEMPERATURE_TYPE_GPU].temperature,
                        &t.temperature, sizeof(t.temperature)))
        return NVML_ERROR_NOT_SUPPORTED;
    *temp = (unsigned)NV_TYPES_NV_TEMP_TO_CELSIUS_ROUNDED(t.temperature);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetTemperatureV(nvmlDevice_t device, nvmlTemperature_t *temperature)
{
    CHECK_PTR(temperature);
    if (temperature->version != nvmlTemperature_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    unsigned t;
    nvmlReturn_t ret = nvmlDeviceGetTemperature(device, temperature->sensorType, &t);
    if (ret == NVML_SUCCESS)
        temperature->temperature = (int)t;
    return ret;
}

static int rusd_clock_domain(nvmlClockType_t type)
{
    switch (type) {
    case NVML_CLOCK_GRAPHICS: return RUSD_CLK_PUBLIC_DOMAIN_GRAPHICS;
    case NVML_CLOCK_SM:       return RUSD_CLK_PUBLIC_DOMAIN_SM;
    case NVML_CLOCK_MEM:      return RUSD_CLK_PUBLIC_DOMAIN_MEMORY;
    case NVML_CLOCK_VIDEO:    return RUSD_CLK_PUBLIC_DOMAIN_VIDEO;
    default:                  return -1;
    }
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetClockInfo(nvmlDevice_t device, nvmlClockType_t type,
                                                unsigned int *clock)
{
    RUSD_CLK_PUBLIC_DOMAIN_INFO info[RUSD_CLK_PUBLIC_DOMAIN_MAX_TYPE];
    int d = rusd_clock_domain(type);
    DEVICE(device, dev);
    CHECK_PTR(clock);
    if (d < 0)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (!NVML_RUSD_READ(dev, clkPublicDomainInfos, &info))
        return NVML_ERROR_NOT_SUPPORTED;
    /* No separate SM domain on GPUs where it is the graphics clock. */
    if (d == RUSD_CLK_PUBLIC_DOMAIN_SM && info[d].actualClkKHz == 0)
        d = RUSD_CLK_PUBLIC_DOMAIN_GRAPHICS;
    *clock = info[d].actualClkKHz / 1000;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetClock(nvmlDevice_t device, nvmlClockType_t type,
                                            nvmlClockId_t id, unsigned int *clockMHz)
{
    if (id != NVML_CLOCK_ID_CURRENT)
        return NVML_ERROR_NOT_SUPPORTED;
    return nvmlDeviceGetClockInfo(device, type, clockMHz);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t *util)
{
    RUSD_PERF_DEVICE_UTILIZATION_INFO u;
    DEVICE(device, dev);
    CHECK_PTR(util);
    if (!NVML_RUSD_READ(dev, perfDevUtil, &u))
        return NVML_ERROR_NOT_SUPPORTED;
    util->gpu = u.gpuPercentBusy;
    util->memory = u.memoryPercentBusy;
    return NVML_SUCCESS;
}

static nvmlReturn_t engine_util(nvmlDevice_t device, unsigned eng, unsigned *util, unsigned *period)
{
    RUSD_PERF_DEVICE_UTILIZATION_INFO u;
    DEVICE(device, dev);
    CHECK_PTR(util);
    CHECK_PTR(period);
    if (!NVML_RUSD_READ(dev, perfDevUtil, &u))
        return NVML_ERROR_NOT_SUPPORTED;
    *util = u.engUtil[eng].clkPercentBusy;
    *period = u.engUtil[eng].samplingPeriodUs;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetEncoderUtilization(nvmlDevice_t device, unsigned int *util,
                                                         unsigned int *samplingPeriodUs)
{
    return engine_util(device, RUSD_ENG_UTILIZATION_VID_ENG_NVENC, util, samplingPeriodUs);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetDecoderUtilization(nvmlDevice_t device, unsigned int *util,
                                                         unsigned int *samplingPeriodUs)
{
    return engine_util(device, RUSD_ENG_UTILIZATION_VID_ENG_NVDEC, util, samplingPeriodUs);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetJpgUtilization(nvmlDevice_t device, unsigned int *util,
                                                     unsigned int *samplingPeriodUs)
{
    return engine_util(device, RUSD_ENG_UTILIZATION_VID_ENG_NVJPG, util, samplingPeriodUs);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetOfaUtilization(nvmlDevice_t device, unsigned int *util,
                                                     unsigned int *samplingPeriodUs)
{
    return engine_util(device, RUSD_ENG_UTILIZATION_VID_ENG_NVOFA, util, samplingPeriodUs);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t device, unsigned int *power)
{
    RUSD_AVG_POWER_INFO avg;
    DEVICE(device, dev);
    CHECK_PTR(power);
    if (!NVML_RUSD_READ(dev, avgPowerUsage, &avg))
        return NVML_ERROR_NOT_SUPPORTED;
    *power = avg.averageGpuPower;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetEnforcedPowerLimit(nvmlDevice_t device, unsigned int *limit)
{
    RUSD_POWER_LIMIT_INFO l;
    DEVICE(device, dev);
    CHECK_PTR(limit);
    if (!NVML_RUSD_READ(dev, powerLimitGpu, &l))
        return NVML_ERROR_NOT_SUPPORTED;
    *limit = l.enforcedmW;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPerformanceState(nvmlDevice_t device, nvmlPstates_t *pstate)
{
    NvU32 p;
    DEVICE(device, dev);
    CHECK_PTR(pstate);
    if (dev->rusd == NULL ||
        !nvml_rusd_read(dev, &dev->rusd->perfCurrentPstate.lastModifiedTimestamp,
                        (void *)&dev->rusd->perfCurrentPstate.currentPstate, &p, sizeof(p)))
        return NVML_ERROR_NOT_SUPPORTED;
    /* NV2080_CTRL_PERF_PSTATES_Pn is the bit 1 << n. */
    if (p == 0) {
        *pstate = NVML_PSTATE_UNKNOWN;
        return NVML_SUCCESS;
    }
    *pstate = (nvmlPstates_t)__builtin_ctz(p);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPowerState(nvmlDevice_t device, nvmlPstates_t *pstate)
{
    return nvmlDeviceGetPerformanceState(device, pstate);
}

/* ---- PCIe ------------------------------------------------------------------- */

/* NV2080_CTRL_BUS_INFO_INDEX_PCIE_GEN_INFO, which RUSD keeps current:
 * _GEN = what the link can do, _CURR_LEVEL = now, _GPU_GEN = the GPU's own
 * maximum; each is generation - 1. */
static nvmlReturn_t gen_info(struct nvmlDevice_st *dev, NvU32 *info)
{
    NvU32 data[RUSD_BUS_DATA_COUNT];
    if (!NVML_RUSD_READ(dev, pciBusData, &data))
        return NVML_ERROR_NOT_SUPPORTED;
    *info = data[RUSD_BUS_DATA_PCIE_GEN_INFO];
    return NVML_SUCCESS;
}

#define GEN_FIELD(info, f) (DRF_VAL(2080_CTRL_BUS_INFO, _PCIE_LINK_CAP, f, (info)) + 1)

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMaxPcieLinkGeneration(nvmlDevice_t device, unsigned int *gen)
{
    NvU32 info;
    nvmlReturn_t ret;
    DEVICE(device, dev);
    CHECK_PTR(gen);
    if ((ret = gen_info(dev, &info)) != NVML_SUCCESS)
        return ret;
    *gen = GEN_FIELD(info, _GEN);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetGpuMaxPcieLinkGeneration(nvmlDevice_t device, unsigned int *gen)
{
    NvU32 info;
    nvmlReturn_t ret;
    DEVICE(device, dev);
    CHECK_PTR(gen);
    if ((ret = gen_info(dev, &info)) != NVML_SUCCESS)
        return ret;
    *gen = GEN_FIELD(info, _GPU_GEN);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCurrPcieLinkGeneration(nvmlDevice_t device, unsigned int *gen)
{
    NvU32 info;
    nvmlReturn_t ret;
    DEVICE(device, dev);
    CHECK_PTR(gen);
    if ((ret = gen_info(dev, &info)) != NVML_SUCCESS)
        return ret;
    *gen = GEN_FIELD(info, _CURR_LEVEL);
    return NVML_SUCCESS;
}

/* Max link generation of the root port (export table slot 230); _MAX_SPEED
 * counts 2.5 GT/s as 1, i.e. it is the generation. */
nvmlReturn_t nvml_host_max_link_gen(nvmlDevice_t device, unsigned *gen)
{
    DEVICE(device, dev);
    CHECK_PTR(gen);
    if (dev->root_link_caps == 0)
        return NVML_ERROR_NOT_SUPPORTED;
    *gen = DRF_VAL(2080_CTRL_BUS_INFO, _PCIE_LINK_CAP, _MAX_SPEED, dev->root_link_caps);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMaxPcieLinkWidth(nvmlDevice_t device, unsigned int *width)
{
    DEVICE(device, dev);
    CHECK_PTR(width);
    if (dev->link_caps == 0)
        return NVML_ERROR_NOT_SUPPORTED;
    *width = DRF_VAL(2080_CTRL_BUS_INFO, _PCIE_LINK_CAP, _MAX_WIDTH, dev->link_caps);
    return NVML_SUCCESS;
}

static nvmlReturn_t link_status(struct nvmlDevice_st *dev, NvU32 *status)
{
    NV2080_CTRL_BUS_GET_INFO_V2_PARAMS bus = {
        .busInfoListSize = 1,
        .busInfoList = { { .index = NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CTRL_STATUS } },
    };
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_INFO_V2, &bus, sizeof(bus));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *status = bus.busInfoList[0].data;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCurrPcieLinkWidth(nvmlDevice_t device, unsigned int *width)
{
    NvU32 status;
    nvmlReturn_t ret;
    DEVICE(device, dev);
    CHECK_PTR(width);
    if ((ret = link_status(dev, &status)) != NVML_SUCCESS)
        return ret;
    *width = DRF_VAL(2080_CTRL_BUS_INFO, _PCIE_LINK_CTRL_STATUS, _LINK_WIDTH, status);
    return NVML_SUCCESS;
}

/* ---- modes ------------------------------------------------------------------ */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetComputeMode(nvmlDevice_t device, nvmlComputeMode_t *mode)
{
    NV2080_CTRL_GPU_QUERY_COMPUTE_MODE_RULES_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(mode);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_QUERY_COMPUTE_MODE_RULES, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    switch (p.rules) {
    case NV2080_CTRL_GPU_COMPUTE_MODE_RULES_NONE:                     *mode = NVML_COMPUTEMODE_DEFAULT; break;
    case NV2080_CTRL_GPU_COMPUTE_MODE_RULES_COMPUTE_PROHIBITED:       *mode = NVML_COMPUTEMODE_PROHIBITED; break;
    case NV2080_CTRL_GPU_COMPUTE_MODE_RULES_EXCLUSIVE_COMPUTE_PROCESS: *mode = NVML_COMPUTEMODE_EXCLUSIVE_PROCESS; break;
    default:                                                          *mode = NVML_COMPUTEMODE_EXCLUSIVE_THREAD; break;
    }
    return NVML_SUCCESS;
}

/* ---- PCIe throughput -----------------------------------------------------------
 * RM counts bytes per direction; sample over 20 ms like the closed library. */

#define PCIE_SAMPLE_MS 20

static bool pex_counter(struct nvmlDevice_st *dev, unsigned bit, NvU32 *value)
{
    NV2080_CTRL_BUS_GET_PEX_UTIL_COUNTERS_PARAMS p = { .pexCounterMask = 1u << bit };
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_PEX_UTIL_COUNTERS, &p, sizeof(p)) != NV_OK)
        return false;
    *value = p.pexCounters[bit];
    return true;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPcieThroughput(nvmlDevice_t device, nvmlPcieUtilCounter_t counter,
                                                     unsigned int *value)
{
    struct timespec t0, t1, wait = { 0, PCIE_SAMPLE_MS * 1000000L };
    unsigned bit;
    NvU32 c0, c1;
    DEVICE(device, dev);
    CHECK_PTR(value);
    switch (counter) {
    case NVML_PCIE_UTIL_TX_BYTES: bit = 0; break;   /* NV2080_CTRL_BUS_PEX_UTIL_COUNTER_TX_BYTES */
    case NVML_PCIE_UTIL_RX_BYTES: bit = 1; break;   /* ..._RX_BYTES */
    default: return NVML_ERROR_INVALID_ARGUMENT;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (!pex_counter(dev, bit, &c0))
        return NVML_ERROR_NOT_SUPPORTED;
    nanosleep(&wait, NULL);
    if (!pex_counter(dev, bit, &c1))
        return NVML_ERROR_NOT_SUPPORTED;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    *value = (unsigned)((NvU32)(c1 - c0) / s / 1024.0);   /* KB/s */
    return NVML_SUCCESS;
}
