/*
 * SPDX-License-Identifier: MIT
 *
 * Memory error reporting: ECC mode and error counters, retired pages,
 * bank remapping, DRAM encryption and the InfoROM's black box times.
 *
 * None of this exists on consumer boards, where RM answers the first
 * query with NV_ERR_NOT_SUPPORTED.
 */

#include <stdlib.h>
#include <string.h>

#include "nvml-priv.h"

#include "nvmisc.h"
#include "nvstatus.h"
#include "class/cl90e7.h"
#include "ctrl/ctrl2080/ctrl2080ecc.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl90e7/ctrl90e7bbx.h"

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

/* ---- ECC mode ---------------------------------------------------------------- */

/*
 * Whether any ECC unit is enabled. The state can't change while the driver
 * is loaded, so one query serves every later call.
 */
static nvmlReturn_t ecc_enabled(struct nvmlDevice_st *dev, bool *enabled)
{
    pthread_mutex_lock(&dev->lock);
    if (!dev->ecc_known) {
        NV2080_CTRL_GPU_QUERY_ECC_STATUS_PARAMS *p = calloc(1, sizeof(*p));
        if (p == NULL) {
            pthread_mutex_unlock(&dev->lock);
            return NVML_ERROR_MEMORY;
        }
        dev->ecc_status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_QUERY_ECC_STATUS, p, sizeof(*p));
        dev->ecc_enabled = false;
        for (unsigned i = 0; dev->ecc_status == NV_OK && i < NV2080_CTRL_GPU_ECC_UNIT_COUNT; i++)
            if (p->units[i].enabled)
                dev->ecc_enabled = true;
        free(p);
        /* A lost GPU may come back; everything else is final. */
        dev->ecc_known = dev->ecc_status != NV_ERR_GPU_IS_LOST;
    }
    NvU32 st = dev->ecc_status;
    *enabled = dev->ecc_enabled;
    pthread_mutex_unlock(&dev->lock);
    return nvml_from_rm(st);
}

/* NOT_SUPPORTED unless ECC is on: the precondition of every counter query. */
static nvmlReturn_t ecc_required(struct nvmlDevice_st *dev)
{
    bool enabled;
    nvmlReturn_t ret = ecc_enabled(dev, &enabled);
    if (ret != NVML_SUCCESS)
        return ret;
    return enabled ? NVML_SUCCESS : NVML_ERROR_NOT_SUPPORTED;
}

static nvmlReturn_t ecc_configuration(struct nvmlDevice_st *dev, NvU32 *current, NvU32 *def)
{
    NV2080_CTRL_GPU_QUERY_ECC_CONFIGURATION_PARAMS p = { 0 };
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_QUERY_ECC_CONFIGURATION, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *current = p.currentConfiguration;
    *def = p.defaultConfiguration;
    return NVML_SUCCESS;
}

static nvmlEnableState_t ecc_config_state(NvU32 configuration)
{
    return configuration == NV2080_CTRL_GPU_ECC_CONFIGURATION_ENABLED ? NVML_FEATURE_ENABLED
                                                                      : NVML_FEATURE_DISABLED;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetEccMode(nvmlDevice_t device, nvmlEnableState_t *current,
                                              nvmlEnableState_t *pending)
{
    NvU32 cfg = 0, def = 0;
    bool enabled;
    DEVICE(device, dev);
    if (current == NULL || pending == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    /* Boards whose InfoROM has no ECC object can't switch ECC. */
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_QUERY_INFOROM_ECC_SUPPORT, NULL, 0);
    if (st != NV_OK)
        return nvml_from_rm(st);
    /* The configuration stored in the InfoROM is what the next boot uses. */
    nvmlReturn_t ret = ecc_configuration(dev, &cfg, &def);
    if (ret != NVML_SUCCESS)
        return ret;
    ret = ecc_enabled(dev, &enabled);
    if (ret != NVML_SUCCESS)
        return ret;
    *pending = ecc_config_state(cfg);
    *current = enabled ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetDefaultEccMode(nvmlDevice_t device, nvmlEnableState_t *defaultMode)
{
    NvU32 cfg = 0, def = 0;
    DEVICE(device, dev);
    CHECK_PTR(defaultMode);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_QUERY_INFOROM_ECC_SUPPORT, NULL, 0);
    if (st != NV_OK)
        return nvml_from_rm(st);
    nvmlReturn_t ret = ecc_configuration(dev, &cfg, &def);
    if (ret != NVML_SUCCESS)
        return ret;
    *defaultMode = ecc_config_state(def);
    return NVML_SUCCESS;
}

/* ---- ECC error counters -------------------------------------------------------- */

struct ecc_totals {
    unsigned long long dram[NVML_MEMORY_ERROR_TYPE_COUNT];   /* corrected, uncorrected */
    unsigned long long sram[NVML_MEMORY_ERROR_TYPE_COUNT];
};

/*
 * Since Turing RM keeps two counter sets, one for the frame buffer (DRAM)
 * and one for every on-chip memory together (SRAM): since the driver was
 * loaded (volatile) and over the board's lifetime (aggregate, InfoROM).
 */
static nvmlReturn_t ecc_totals(struct nvmlDevice_st *dev, nvmlEccCounterType_t counter,
                               struct ecc_totals *t)
{
    NvU32 st;
    if (counter == NVML_VOLATILE_ECC) {
        NV2080_CTRL_ECC_GET_VOLATILE_COUNTS_PARAMS p = { 0 };
        st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_ECC_GET_VOLATILE_COUNTS, &p, sizeof(p));
        t->dram[NVML_MEMORY_ERROR_TYPE_CORRECTED] = p.dramCorTot;
        t->dram[NVML_MEMORY_ERROR_TYPE_UNCORRECTED] = p.dramUncTot;
        t->sram[NVML_MEMORY_ERROR_TYPE_CORRECTED] = p.sramCorUni;
        t->sram[NVML_MEMORY_ERROR_TYPE_UNCORRECTED] = p.sramUncParityUni + p.sramUncSecDedUni;
    } else {
        NV2080_CTRL_ECC_GET_CLIENT_EXPOSED_COUNTERS_PARAMS p = { 0 };
        st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_ECC_GET_CLIENT_EXPOSED_COUNTERS, &p, sizeof(p));
        t->dram[NVML_MEMORY_ERROR_TYPE_CORRECTED] = p.dramCorrectedTotal;
        t->dram[NVML_MEMORY_ERROR_TYPE_UNCORRECTED] = p.dramUncorrectedTotal;
        t->sram[NVML_MEMORY_ERROR_TYPE_CORRECTED] = p.sramCorrectedUnique;
        t->sram[NVML_MEMORY_ERROR_TYPE_UNCORRECTED] =
            p.sramParityUncorrectedUnique + p.sramSecDedUncorrectedUnique;
    }
    return nvml_from_rm(st);
}

static bool ecc_args_valid(nvmlMemoryErrorType_t type, nvmlEccCounterType_t counter)
{
    return (unsigned)type < NVML_MEMORY_ERROR_TYPE_COUNT &&
           (counter == NVML_VOLATILE_ECC || counter == NVML_AGGREGATE_ECC);
}

nvmlReturn_t nvml_ecc_total(struct nvmlDevice_st *dev, nvmlMemoryErrorType_t type,
                            nvmlEccCounterType_t counter, unsigned long long *count)
{
    struct ecc_totals t;
    nvmlReturn_t ret = ecc_required(dev);
    if (ret != NVML_SUCCESS)
        return ret;
    ret = ecc_totals(dev, counter, &t);
    if (ret != NVML_SUCCESS)
        return ret;
    *count = t.dram[type] + t.sram[type];
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetTotalEccErrors(nvmlDevice_t device, nvmlMemoryErrorType_t errorType,
                                                     nvmlEccCounterType_t counterType,
                                                     unsigned long long *eccCounts)
{
    DEVICE(device, dev);
    if (!ecc_args_valid(errorType, counterType) || eccCounts == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    return nvml_ecc_total(dev, errorType, counterType, eccCounts);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMemoryErrorCounter(nvmlDevice_t device, nvmlMemoryErrorType_t errorType,
                                                         nvmlEccCounterType_t counterType,
                                                         nvmlMemoryLocation_t locationType,
                                                         unsigned long long *count)
{
    struct ecc_totals t;
    DEVICE(device, dev);
    if (!ecc_args_valid(errorType, counterType) ||
        (unsigned)locationType >= NVML_MEMORY_LOCATION_COUNT || count == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    nvmlReturn_t ret = ecc_required(dev);
    if (ret != NVML_SUCCESS)
        return ret;
    /* The per-cache locations ended with Volta; only the two sets remain. */
    if (locationType != NVML_MEMORY_LOCATION_DRAM && locationType != NVML_MEMORY_LOCATION_SRAM)
        return NVML_ERROR_NOT_SUPPORTED;
    ret = ecc_totals(dev, counterType, &t);
    if (ret != NVML_SUCCESS)
        return ret;
    *count = locationType == NVML_MEMORY_LOCATION_DRAM ? t.dram[errorType] : t.sram[errorType];
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetDetailedEccErrors(nvmlDevice_t device, nvmlMemoryErrorType_t errorType,
                                                        nvmlEccCounterType_t counterType,
                                                        nvmlEccErrorCounts_t *eccCounts)
{
    static const nvmlMemoryLocation_t loc[] = {
        NVML_MEMORY_LOCATION_L1_CACHE, NVML_MEMORY_LOCATION_L2_CACHE,
        NVML_MEMORY_LOCATION_DEVICE_MEMORY, NVML_MEMORY_LOCATION_REGISTER_FILE,
    };
    unsigned long long v[4] = { 0 };
    bool any = false;
    DEVICE(device, dev);
    if (!ecc_args_valid(errorType, counterType) || eccCounts == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    /* A location without a counter reads as 0 as long as one of the four
     * has one. */
    for (unsigned i = 0; i < 4; i++) {
        nvmlReturn_t ret = nvmlDeviceGetMemoryErrorCounter(device, errorType, counterType, loc[i], &v[i]);
        if (ret == NVML_SUCCESS)
            any = true;
        else if (ret != NVML_ERROR_NOT_SUPPORTED)
            return ret;
        else
            v[i] = 0;
    }
    if (!any)
        return NVML_ERROR_NOT_SUPPORTED;
    eccCounts->l1Cache = v[0];
    eccCounts->l2Cache = v[1];
    eccCounts->deviceMemory = v[2];
    eccCounts->registerFile = v[3];
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetSramUniqueUncorrectedEccErrorCounts(
    nvmlDevice_t device, nvmlEccSramUniqueUncorrectedErrorCounts_t *errorCounts)
{
    DEVICE(device, dev);
    CHECK_PTR(errorCounts);
    if (errorCounts->version != nvmlEccSramUniqueUncorrectedErrorCounts_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    nvmlReturn_t ret = ecc_required(dev);
    if (ret != NVML_SUCCESS)
        return ret;

    NV2080_CTRL_ECC_GET_SRAM_UNIQUE_UNCORR_COUNTS_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_ECC_GET_SRAM_UNIQUE_UNCORR_COUNTS, p, sizeof(*p));
    if (st != NV_OK) {
        free(p);
        return nvml_from_rm(st);
    }
    unsigned n = p->entryCount;
    if (n > NV2080_CTRL_ECC_SRAM_UNIQUE_UNCORR_COUNTS_MAX_COUNT)
        n = NV2080_CTRL_ECC_SRAM_UNIQUE_UNCORR_COUNTS_MAX_COUNT;
    unsigned room = errorCounts->entryCount;
    errorCounts->entryCount = n;
    if (n > room || (n != 0 && errorCounts->entries == NULL)) {
        free(p);
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    for (unsigned i = 0; i < n; i++)
        errorCounts->entries[i] = (nvmlEccSramUniqueUncorrectedErrorEntry_v1_t){
            .unit = p->entries[i].unit,
            .location = p->entries[i].location.location,
            .sublocation = p->entries[i].location.sublocation,
            .extlocation = p->entries[i].location.extlocation,
            .address = p->entries[i].address,
            .isParity = p->entries[i].bIsParity,
            .count = p->entries[i].count,
        };
    free(p);
    return NVML_SUCCESS;
}

/* ---- retired pages ------------------------------------------------------------- */

static nvmlReturn_t offlined_pages(struct nvmlDevice_st *dev, NV2080_CTRL_FB_GET_OFFLINED_PAGES_PARAMS *p)
{
    memset(p, 0, sizeof(*p));
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_GET_OFFLINED_PAGES, p, sizeof(*p));
    /* No InfoROM page list: the board predates or doesn't do page retirement. */
    if (st == NV_ERR_OBJECT_NOT_FOUND)
        return NVML_ERROR_NOT_SUPPORTED;
    if (st == NV_OK && p->validEntries > NV2080_CTRL_FB_OFFLINED_PAGES_MAX_PAGES)
        p->validEntries = NV2080_CTRL_FB_OFFLINED_PAGES_MAX_PAGES;
    return nvml_from_rm(st);
}

nvmlReturn_t nvml_retired_pages_field(struct nvmlDevice_st *dev, unsigned field, unsigned long long *value)
{
    NV2080_CTRL_FB_GET_OFFLINED_PAGES_PARAMS p;
    nvmlReturn_t ret = offlined_pages(dev, &p);
    if (ret != NVML_SUCCESS)
        return ret;
    unsigned sbe = 0, dbe = 0;
    for (unsigned i = 0; i < p.validEntries; i++) {
        if (p.offlined[i].source == NV2080_CTRL_FB_OFFLINED_PAGES_SOURCE_DPR_MULTIPLE_SBE)
            sbe++;
        else if (p.offlined[i].source == NV2080_CTRL_FB_OFFLINED_PAGES_SOURCE_DPR_DBE)
            dbe++;
    }
    switch (field) {
    case NVML_FI_DEV_RETIRED_SBE:         *value = sbe; break;
    case NVML_FI_DEV_RETIRED_DBE:         *value = dbe; break;
    case NVML_FI_DEV_RETIRED_PENDING:     *value = p.bRetirementPending ? 1 : 0; break;
    case NVML_FI_DEV_RETIRED_PENDING_SBE: *value = p.retirementPending & 1; break;
    case NVML_FI_DEV_RETIRED_PENDING_DBE: *value = (p.retirementPending >> 1) & 1; break;
    default:                              return NVML_ERROR_INVALID_ARGUMENT;
    }
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetRetiredPages_v2(nvmlDevice_t device, nvmlPageRetirementCause_t cause,
                                                      unsigned int *pageCount, unsigned long long *addresses,
                                                      unsigned long long *timestamps)
{
    NV2080_CTRL_FB_GET_OFFLINED_PAGES_PARAMS p;
    DEVICE(device, dev);
    if ((unsigned)cause >= NVML_PAGE_RETIREMENT_CAUSE_COUNT || pageCount == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    nvmlReturn_t ret = offlined_pages(dev, &p);
    if (ret != NVML_SUCCESS)
        return ret;

    NvU32 source = cause == NVML_PAGE_RETIREMENT_CAUSE_DOUBLE_BIT_ECC_ERROR
                   ? NV2080_CTRL_FB_OFFLINED_PAGES_SOURCE_DPR_DBE
                   : NV2080_CTRL_FB_OFFLINED_PAGES_SOURCE_DPR_MULTIPLE_SBE;
    unsigned n = 0, room = *pageCount;
    for (unsigned i = 0; i < p.validEntries; i++)
        if (p.offlined[i].source == source)
            n++;
    *pageCount = n;
    if (room < n || room == 0)
        return NVML_ERROR_INSUFFICIENT_SIZE;
    CHECK_PTR(addresses);

    /* A page has a different address depending on whether ECC is on. */
    bool ecc;
    ret = ecc_enabled(dev, &ecc);
    if (ret != NVML_SUCCESS)
        return ret == NVML_ERROR_GPU_IS_LOST || ret == NVML_ERROR_NOT_SUPPORTED ? ret : NVML_ERROR_UNKNOWN;
    for (unsigned i = 0, k = 0; i < p.validEntries; i++) {
        if (p.offlined[i].source != source)
            continue;
        if (timestamps != NULL)
            timestamps[k] = p.offlined[i].timestamp;
        addresses[k++] = ecc ? p.offlined[i].pageAddressWithEccOn : p.offlined[i].pageAddressWithEccOff;
    }
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetRetiredPages(nvmlDevice_t device, nvmlPageRetirementCause_t cause,
                                                   unsigned int *pageCount, unsigned long long *addresses)
{
    return nvmlDeviceGetRetiredPages_v2(device, cause, pageCount, addresses, NULL);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetRetiredPagesPendingStatus(nvmlDevice_t device, nvmlEnableState_t *isPending)
{
    NV2080_CTRL_FB_GET_OFFLINED_PAGES_PARAMS p;
    DEVICE(device, dev);
    CHECK_PTR(isPending);
    nvmlReturn_t ret = offlined_pages(dev, &p);
    if (ret != NVML_SUCCESS)
        return ret;
    *isPending = p.bRetirementPending ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

/* ---- bank remapping, unrepairable memory ---------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetBankRemapperStatus_v1(nvmlDevice_t device,
                                                            nvmlEccBankRemapperStatus_v1_t *status)
{
    DEVICE(device, dev);
    CHECK_PTR(status);
    NV2080_CTRL_FB_GET_REMAPPED_BANKS_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_GET_REMAPPED_BANKS, p, sizeof(*p));
    if (st != NV_OK) {
        free(p);
        return nvml_from_rm(st);
    }
    unsigned n = p->entryCount, active = 0;
    if (n > NV2080_CTRL_FB_REMAPPED_BANKS_MAX_BANKS)
        n = NV2080_CTRL_FB_REMAPPED_BANKS_MAX_BANKS;
    /* A remapping recorded in the InfoROM takes effect at the next reset. */
    for (unsigned i = 0; i < n; i++)
        if (FLD_TEST_DRF(2080_CTRL_FB, _REMAP_ENTRY_FLAGS, _ACTIVE, _TRUE, p->entries[i].flags))
            active++;
    memset(status, 0, sizeof(*status));
    status->activeRemappings = active;
    status->inactiveRemappings = n - active;
    status->bPending = p->flags & 1;
    status->histogram.maxSpareGroupCount = p->histogram.maxSpareGroupCount;
    status->histogram.noSpareGroupCount = p->histogram.noSpareGroupCount;
    free(p);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetUnrepairableMemoryFlag_v1(nvmlDevice_t device,
                                                                nvmlUnrepairableMemoryStatus_v1_t *status)
{
    NV2080_CTRL_ECC_GET_UNREPAIRABLE_MEMORY_FLAG_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(status);
    nvmlReturn_t ret = ecc_required(dev);
    if (ret != NVML_SUCCESS)
        return ret;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_ECC_GET_UNREPAIRABLE_MEMORY_FLAG, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    status->bUnrepairableMemory = p.bUnrepairableMemory ? 1 : 0;
    return NVML_SUCCESS;
}

/* ---- DRAM encryption ------------------------------------------------------------ */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetDramEncryptionMode(nvmlDevice_t device, nvmlDramEncryptionInfo_t *current,
                                                         nvmlDramEncryptionInfo_t *pending)
{
    NV2080_CTRL_FB_DRAM_ENCRYPTION_INFOROM_SUPPORT_PARAMS sup = { 0 };
    NV2080_CTRL_FB_QUERY_DRAM_ENCRYPTION_PENDING_CONFIGURATION_PARAMS pend = { 0 };
    NV2080_CTRL_FB_QUERY_DRAM_ENCRYPTION_STATUS_PARAMS cur = { 0 };
    DEVICE(device, dev);
    if (current == NULL || pending == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_QUERY_DRAM_ENCRYPTION_INFOROM_SUPPORT, &sup, sizeof(sup));
    if (st != NV_OK)
        return nvml_from_rm(st);
    if (sup.isSupported != NV2080_CTRL_CMD_FB_QUERY_DRAM_ENCRYPTION_INFOROM_SUPPORT_ENABLED)
        return NVML_ERROR_NOT_SUPPORTED;
    if (current->version != nvmlDramEncryptionInfo_v1 || pending->version != nvmlDramEncryptionInfo_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_QUERY_DRAM_ENCRYPTION_PENDING_CONFIGURATION, &pend, sizeof(pend));
    if (st == NV_OK)
        st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_QUERY_DRAM_ENCRYPTION_STATUS, &cur, sizeof(cur));
    if (st != NV_OK)
        return nvml_from_rm(st);
    current->encryptionState = cur.currentStatus == NV2080_CTRL_CMD_FB_QUERY_DRAM_ENCRYPTION_STATUS_ENABLED
                               ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
    pending->encryptionState =
        pend.currentConfiguration == NV2080_CTRL_FB_QUERY_DRAM_ENCRYPTION_PENDING_CONFIGURATION_ENABLED
        ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

/* ---- InfoROM black box (BBX) ----------------------------------------------------- */

/* The InfoROM object, allocated at first use; 0 if the board has none. */
static NvHandle inforom_object(struct nvmlDevice_st *dev)
{
    pthread_mutex_lock(&dev->lock);
    if (dev->inforom == 0) {
        NvHandle h = nvc_rm_handle(&dev->rm);
        if (nvc_rm_alloc(&dev->rm, dev->subdevice, h, GF100_SUBDEVICE_INFOROM, NULL) == NV_OK)
            dev->inforom = h;
    }
    NvHandle h = dev->inforom;
    pthread_mutex_unlock(&dev->lock);
    return h;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetBBXTimeData_v1(nvmlDevice_t device, nvmlBBXTimeData_v1_t *timeData)
{
    NV90E7_CTRL_BBX_GET_TIME_DATA_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(timeData);
    NvHandle h = inforom_object(dev);
    if (h == 0)
        return NVML_ERROR_NOT_SUPPORTED;
    NvU32 st = nvc_rm_control(&dev->rm, h, NV90E7_CTRL_CMD_BBX_GET_TIME_DATA, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    timeData->timeRun = p.timeRun;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetLastBBXFlushTime(nvmlDevice_t device, unsigned long long *timestamp,
                                                       unsigned long *durationUs)
{
    NV90E7_CTRL_BBX_GET_LAST_FLUSH_TIME_PARAMS p = { 0 };
    DEVICE(device, dev);
    NvHandle h = inforom_object(dev);
    if (h == 0)
        return NVML_ERROR_NOT_SUPPORTED;
    NvU32 st = nvc_rm_control(&dev->rm, h, NV90E7_CTRL_CMD_BBX_GET_LAST_FLUSH_TIME, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    if (timestamp == NULL || durationUs == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    *timestamp = p.timestamp;
    *durationUs = p.durationUs;
    return NVML_SUCCESS;
}
