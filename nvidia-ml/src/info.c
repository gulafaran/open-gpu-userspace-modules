/*
 * SPDX-License-Identifier: MIT
 *
 * Board, firmware and session information.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvml-priv.h"

#include "nvstatus.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0000/ctrl0000gpuacct.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080/ctrl2080bios.h"
#include "ctrl/ctrl2080/ctrl2080bus.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080gr.h"
#include "ctrl/ctrl2080/ctrl2080gsp.h"
#include "ctrl/ctrl2080/ctrl2080nvlink.h"

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

NVML_EXPORT nvmlReturn_t nvmlDeviceGetBrand(nvmlDevice_t device, nvmlBrandType_t *type)
{
    NV0080_CTRL_GPU_GET_BRAND_CAPS_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(type);
    NvU32 st = nvml_ctrl_dev(dev, NV0080_CTRL_CMD_GPU_GET_BRAND_CAPS, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    if (p.brands & NV0080_CTRL_GPU_GET_BRAND_CAPS_QUADRO)
        *type = NVML_BRAND_QUADRO;
    else if (p.brands & NV0080_CTRL_GPU_GET_BRAND_CAPS_NVS)
        *type = NVML_BRAND_NVS;
    else if (p.brands & NV0080_CTRL_GPU_GET_BRAND_CAPS_TITAN)
        *type = NVML_BRAND_TITAN;
    else
        *type = NVML_BRAND_GEFORCE;
    return NVML_SUCCESS;
}

/* "95.04.3C.00.4C": the four bytes of the revision, then the OEM revision. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetVbiosVersion(nvmlDevice_t device, char *version, unsigned int length)
{
    NV2080_CTRL_BIOS_GET_INFO_V2_PARAMS p = {
        .biosInfoListSize = 2,
        .biosInfoList = {
            { .index = NV2080_CTRL_BIOS_INFO_INDEX_REVISION },
            { .index = NV2080_CTRL_BIOS_INFO_INDEX_OEM_REVISION },
        },
    };
    char s[NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE];
    DEVICE(device, dev);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BIOS_GET_INFO_V2, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    NvU32 r = p.biosInfoList[0].data;
    snprintf(s, sizeof(s), "%02X.%02X.%02X.%02X.%02X", r >> 24, (r >> 16) & 0xff,
             (r >> 8) & 0xff, r & 0xff, p.biosInfoList[1].data & 0xff);
    return nvml_copy_string(version, length, s);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPciInfoExt(nvmlDevice_t device, nvmlPciInfoExt_t *pci)
{
    NV2080_CTRL_BUS_GET_PCI_INFO_V2_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(pci);
    if (pci->version != nvmlPciInfoExt_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_PCI_INFO_V2, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    pci->domain = dev->pci.domain;
    pci->bus = dev->pci.bus;
    pci->device = dev->pci.device;
    pci->pciDeviceId = p.pciDeviceId;
    pci->pciSubSystemId = p.pciSubSystemId;
    pci->baseClass = p.pciBaseClass;
    pci->subClass = p.pciSubClass;
    snprintf(pci->busId, sizeof(pci->busId), "%s", dev->bus_id);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetBoardId(nvmlDevice_t device, unsigned int *boardId)
{
    DEVICE(device, dev);
    CHECK_PTR(boardId);
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = dev->gpu_id };
    NvU32 st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2,
                              &id, sizeof(id));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *boardId = id.boardId;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetModuleId(nvmlDevice_t device, unsigned int *moduleId)
{
    NV2080_CTRL_NVLINK_GET_PLATFORM_INFO_PARAMS p;
    DEVICE(device, dev);
    CHECK_PTR(moduleId);
    memset(&p, 0, sizeof(p));
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_NVLINK_GET_PLATFORM_INFO, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    /* The closed library reports the platform's 0-based id plus one
     * (1 on boards without NVLink). */
    *moduleId = p.moduleId + 1u;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetInforomImageVersion(nvmlDevice_t device, char *version,
                                                          unsigned int length)
{
    NV2080_CTRL_GPU_GET_INFOROM_IMAGE_VERSION_PARAMS p = { 0 };
    char s[NV2080_CTRL_GPU_INFOROM_IMAGE_VERSION_LEN + 1];
    DEVICE(device, dev);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_INFOROM_IMAGE_VERSION, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    snprintf(s, sizeof(s), "%.*s", (int)sizeof(p.version), (const char *)p.version);
    return nvml_copy_string(version, length, s);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetInforomVersion(nvmlDevice_t device, nvmlInforomObject_t object,
                                                     char *version, unsigned int length)
{
    /* RM's inforom object names; NVML's "OEM object" is OBD. */
    static const char *const names[NVML_INFOROM_COUNT] = {
        [NVML_INFOROM_OEM] = "OBD", [NVML_INFOROM_ECC] = "ECC", [NVML_INFOROM_POWER] = "PWR",
        [NVML_INFOROM_DEN] = "DEN",
    };
    NV2080_CTRL_GPU_GET_INFOROM_OBJECT_VERSION_PARAMS p = { 0 };
    char s[16];
    DEVICE(device, dev);
    if ((unsigned)object >= NVML_INFOROM_COUNT || names[object] == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    /* The ECC object exists on boards without ECC too; report it only when
     * the inforom supports ECC. */
    if (object == NVML_INFOROM_ECC) {
        /* No parameters; the status is the answer. */
        if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_QUERY_INFOROM_ECC_SUPPORT, NULL, 0) != NV_OK)
            return NVML_ERROR_NOT_SUPPORTED;
    }
    memcpy(p.objectType, names[object], NV2080_CTRL_GPU_INFOROM_OBJ_TYPE_LEN);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_INFOROM_OBJECT_VERSION, &p, sizeof(p));
    if (st != NV_OK)
        return NVML_ERROR_NOT_SUPPORTED;
    snprintf(s, sizeof(s), "%u.%u", p.version, p.subversion);
    return nvml_copy_string(version, length, s);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetGspFirmwareVersion(nvmlDevice_t device, char *version)
{
    NV2080_CTRL_GSP_GET_FEATURES_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(version);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GSP_GET_FEATURES, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    if (!p.bValid)
        return NVML_ERROR_NOT_SUPPORTED;
    /* The buffer is NVML_GSP_FIRMWARE_VERSION_BUF_SIZE by contract. */
    snprintf(version, NVML_GSP_FIRMWARE_VERSION_BUF_SIZE, "%s", (const char *)p.firmwareVersion);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetGspFirmwareMode(nvmlDevice_t device, unsigned int *isEnabled,
                                                      unsigned int *defaultMode)
{
    NV2080_CTRL_GSP_GET_FEATURES_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(isEnabled);
    CHECK_PTR(defaultMode);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GSP_GET_FEATURES, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *isEnabled = p.bValid;
    *defaultMode = p.bDefaultGspRmGpu;
    return NVML_SUCCESS;
}

nvmlReturn_t nvml_pex_counter(struct nvmlDevice_st *dev, NvU32 bit, unsigned *value)
{
    NV2080_CTRL_BUS_GET_PEX_COUNTERS_PARAMS p = { .pexCounterMask = bit };
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_PEX_COUNTERS, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *value = p.pexCounters[__builtin_ctz(bit)];
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPcieReplayCounter(nvmlDevice_t device, unsigned int *value)
{
    DEVICE(device, dev);
    CHECK_PTR(value);
    return nvml_pex_counter(dev, NV2080_CTRL_BUS_PEX_COUNTER_REPLAY_COUNT, value);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetEncoderStats(nvmlDevice_t device, unsigned int *sessionCount,
                                                   unsigned int *averageFps, unsigned int *averageLatency)
{
    NV2080_CTRL_GPU_GET_NVENC_SW_SESSION_STATS_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(sessionCount);
    CHECK_PTR(averageFps);
    CHECK_PTR(averageLatency);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_GPU_GET_NVENC_SW_SESSION_STATS, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *sessionCount = p.encoderSessionCount;
    *averageFps = p.averageEncodeFps;
    *averageLatency = p.averageEncodeLatency;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetFBCStats(nvmlDevice_t device, nvmlFBCStats_t *stats)
{
    NV2080_CTRL_GPU_GET_NVFBC_SW_SESSION_STATS_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(stats);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_GPU_GET_NVFBC_SW_SESSION_STATS, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    stats->sessionsCount = p.sessionCount;
    stats->averageFPS = p.averageFPS;
    stats->averageLatency = p.averageLatency;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetAccountingMode(nvmlDevice_t device, nvmlEnableState_t *mode)
{
    NV0000_CTRL_GPUACCT_GET_ACCOUNTING_STATE_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(mode);
    p.gpuId = dev->gpu_id;
    NvU32 st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_CMD_GPUACCT_GET_ACCOUNTING_STATE,
                              &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *mode = p.state == NV0000_CTRL_GPU_ACCOUNTING_STATE_ENABLED ? NVML_FEATURE_ENABLED
                                                                : NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

/* Number of processes NVML keeps accounting records for (its own limit). */
#define NVML_ACCOUNTING_BUFFER_SIZE 4000

NVML_EXPORT nvmlReturn_t nvmlDeviceGetAccountingBufferSize(nvmlDevice_t device, unsigned int *bufferSize)
{
    DEVICE(device, dev);
    CHECK_PTR(bufferSize);
    *bufferSize = NVML_ACCOUNTING_BUFFER_SIZE;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMultiGpuBoard(nvmlDevice_t device, unsigned int *multiGpuBool)
{
    NV2080_CTRL_GPU_GET_INFO_V2_PARAMS p = {
        .gpuInfoListSize = 1,
        .gpuInfoList = { { .index = NV2080_CTRL_GPU_INFO_INDEX_GEMINI_BOARD } },
    };
    DEVICE(device, dev);
    CHECK_PTR(multiGpuBool);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_INFO_V2, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *multiGpuBool = p.gpuInfoList[0].data == NV2080_CTRL_GPU_INFO_INDEX_GEMINI_BOARD_YES;
    return NVML_SUCCESS;
}

/* ---- bus, board ------------------------------------------------------------- */

static nvmlReturn_t bus_info(struct nvmlDevice_st *dev, NvU32 index, NvU32 *data)
{
    NV2080_CTRL_BUS_GET_INFO_V2_PARAMS bus = {
        .busInfoListSize = 1,
        .busInfoList = { { .index = index } },
    };
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_INFO_V2, &bus, sizeof(bus));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *data = bus.busInfoList[0].data;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetBusType(nvmlDevice_t device, nvmlBusType_t *type)
{
    NvU32 data;
    DEVICE(device, dev);
    CHECK_PTR(type);
    nvmlReturn_t ret = bus_info(dev, NV2080_CTRL_BUS_INFO_INDEX_TYPE, &data);
    if (ret != NVML_SUCCESS)
        return ret;
    switch (data) {
    case NV2080_CTRL_BUS_INFO_TYPE_PCI:         *type = NVML_BUS_TYPE_PCI; break;
    case NV2080_CTRL_BUS_INFO_TYPE_PCI_EXPRESS: *type = NVML_BUS_TYPE_PCIE; break;
    case NV2080_CTRL_BUS_INFO_TYPE_FPCI:        *type = NVML_BUS_TYPE_FPCI; break;
    default:                                    *type = NVML_BUS_TYPE_UNKNOWN; break;
    }
    return NVML_SUCCESS;
}

/* The link's current speed in Mbps per lane. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetPcieSpeed(nvmlDevice_t device, unsigned int *pcieSpeed)
{
    static const unsigned mbps[] = { 0, 2500, 5000, 8000, 16000, 32000, 64000 };
    NvU32 data;
    DEVICE(device, dev);
    CHECK_PTR(pcieSpeed);
    nvmlReturn_t ret = bus_info(dev, NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CTRL_STATUS, &data);
    if (ret != NVML_SUCCESS)
        return ret;
    NvU32 speed = DRF_VAL(2080_CTRL_BUS_INFO, _PCIE_LINK_CTRL_STATUS, _LINK_SPEED, data);
    if (speed == 0 || speed >= sizeof(mbps) / sizeof(mbps[0]))
        return NVML_ERROR_UNKNOWN;
    *pcieSpeed = mbps[speed];
    return NVML_SUCCESS;
}

/* NVML_PCIE_LINK_MAX_SPEED_*: the link capability's speed code, as is. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetPcieLinkMaxSpeed(nvmlDevice_t device, unsigned int *maxSpeed)
{
    NvU32 data;
    DEVICE(device, dev);
    CHECK_PTR(maxSpeed);
    nvmlReturn_t ret = bus_info(dev, NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CAPS, &data);
    if (ret != NVML_SUCCESS)
        return ret;
    *maxSpeed = DRF_VAL(2080_CTRL_BUS_INFO, _PCIE_LINK_CAP, _MAX_SPEED, data);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetIrqNum(nvmlDevice_t device, unsigned int *irqNum)
{
    NvU32 data;
    DEVICE(device, dev);
    CHECK_PTR(irqNum);
    nvmlReturn_t ret = bus_info(dev, NV2080_CTRL_BUS_INFO_INDEX_INTLINE, &data);
    if (ret == NVML_SUCCESS)
        *irqNum = data;
    return ret;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetNumGpuCores(nvmlDevice_t device, unsigned int *numCores)
{
    NV2080_CTRL_GR_GET_INFO_V2_PARAMS gr = {
        .grInfoListSize = 1,
        .grInfoList = { { .index = NV2080_CTRL_GR_INFO_INDEX_GPU_CORE_COUNT } },
    };
    DEVICE(device, dev);
    CHECK_PTR(numCores);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GR_GET_INFO_V2, &gr, sizeof(gr));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *numCores = gr.grInfoList[0].data;
    return NVML_SUCCESS;
}

/* Width of the memory interface in bits. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetMemoryBusWidth(nvmlDevice_t device, unsigned int *busWidth)
{
    NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb = {
        .fbInfoListSize = 1,
        .fbInfoList = { { .index = NV2080_CTRL_FB_INFO_INDEX_BUS_WIDTH } },
    };
    DEVICE(device, dev);
    CHECK_PTR(busWidth);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof(fb));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *busWidth = fb.fbInfoList[0].data;
    return NVML_SUCCESS;
}

/* Free encoder capacity in percent. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetEncoderCapacity(nvmlDevice_t device, nvmlEncoderType_t encoderQueryType,
                                                      unsigned int *encoderCapacity)
{
    NV2080_CTRL_GPU_GET_ENCODER_CAPACITY_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(encoderCapacity);
    switch (encoderQueryType) {
    case NVML_ENCODER_QUERY_H264: p.queryType = NV2080_CTRL_GPU_GET_ENCODER_CAPACITY_H264; break;
    case NVML_ENCODER_QUERY_HEVC: p.queryType = NV2080_CTRL_GPU_GET_ENCODER_CAPACITY_HEVC; break;
    case NVML_ENCODER_QUERY_AV1:  p.queryType = NV2080_CTRL_GPU_GET_ENCODER_CAPACITY_AV1; break;
    default:
        return NVML_ERROR_INVALID_ARGUMENT;
    }
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_ENCODER_CAPACITY, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *encoderCapacity = p.encoderCapacity;
    return NVML_SUCCESS;
}

static nvmlReturn_t oem_board_info(struct nvmlDevice_st *dev, NV2080_CTRL_GPU_GET_OEM_BOARD_INFO_PARAMS *p)
{
    memset(p, 0, sizeof(*p));
    nvmlReturn_t ret = nvml_from_rm(nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_OEM_BOARD_INFO, p, sizeof(*p)));
    p->serialNumber[sizeof(p->serialNumber) - 1] = '\0';
    p->productPartNumber[sizeof(p->productPartNumber) - 1] = '\0';
    return ret;
}

/* Boards without a serial number in their InfoROM (GeForce) have none. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetSerial(nvmlDevice_t device, char *serial, unsigned int length)
{
    NV2080_CTRL_GPU_GET_OEM_BOARD_INFO_PARAMS p;
    DEVICE(device, dev);
    CHECK_PTR(serial);
    nvmlReturn_t ret = oem_board_info(dev, &p);
    if (ret != NVML_SUCCESS)
        return ret;
    if (p.serialNumber[0] == '\0')
        return NVML_ERROR_NOT_SUPPORTED;
    return nvml_copy_string(serial, length, (const char *)p.serialNumber);
}

/* The product part number; an empty string where the InfoROM has none. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetBoardPartNumber(nvmlDevice_t device, char *partNumber, unsigned int length)
{
    NV2080_CTRL_GPU_GET_OEM_BOARD_INFO_PARAMS p;
    DEVICE(device, dev);
    CHECK_PTR(partNumber);
    nvmlReturn_t ret = oem_board_info(dev, &p);
    if (ret != NVML_SUCCESS)
        return ret;
    return nvml_copy_string(partNumber, length, (const char *)p.productPartNumber);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetHandleBySerial(const char *serial, nvmlDevice_t *device)
{
    char buf[NVML_DEVICE_SERIAL_BUFFER_SIZE];
    unsigned count;
    nvmlReturn_t ret = nvmlDeviceGetCount_v2(&count);
    if (ret != NVML_SUCCESS)
        return ret;
    if (serial == NULL || device == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    for (unsigned i = 0; i < count; i++) {
        nvmlDevice_t d;
        if (nvmlDeviceGetHandleByIndex_v2(i, &d) == NVML_SUCCESS &&
            nvmlDeviceGetSerial(d, buf, sizeof(buf)) == NVML_SUCCESS && strcmp(buf, serial) == 0) {
            *device = d;
            return NVML_SUCCESS;
        }
    }
    return NVML_ERROR_NOT_FOUND;
}

/* A GPU handle is not a MIG device handle. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetGpuInstanceId(nvmlDevice_t device, unsigned int *id)
{
    DEVICE(device, dev);
    (void)id;
    return NVML_ERROR_INVALID_ARGUMENT;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetComputeInstanceId(nvmlDevice_t device, unsigned int *id)
{
    DEVICE(device, dev);
    (void)id;
    return NVML_ERROR_INVALID_ARGUMENT;
}

/* S-class units no longer exist. */
NVML_EXPORT nvmlReturn_t nvmlUnitGetCount(unsigned int *unitCount)
{
    nvmlReturn_t ret = nvml_initialized();
    if (ret != NVML_SUCCESS)
        return ret;
    CHECK_PTR(unitCount);
    *unitCount = 0;
    return NVML_SUCCESS;
}

/* ---- encoder and frame buffer capture sessions -------------------------------- */

/*
 * Both lists: with *sessionCount 0 only the number of sessions is returned;
 * otherwise the sessions if they fit, else INSUFFICIENT_SIZE with the
 * number needed.
 */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetEncoderSessions(nvmlDevice_t device, unsigned int *sessionCount,
                                                      nvmlEncoderSessionInfo_t *sessionInfos)
{
    NV2080_CTRL_GPU_GET_NVENC_SW_SESSION_INFO_PARAMS p = { 0 };
    DEVICE(device, dev);
    if (sessionCount == NULL || (*sessionCount != 0 && sessionInfos == NULL))
        return NVML_ERROR_INVALID_ARGUMENT;

    NV2080_CTRL_NVENC_SW_SESSION_INFO *tbl = NULL;
    if (*sessionCount != 0) {
        p.sessionInfoTblEntry = NV2080_CTRL_GPU_NVENC_SESSION_INFO_MAX_COPYOUT_ENTRIES;
        tbl = calloc(p.sessionInfoTblEntry, sizeof(*tbl));
        if (tbl == NULL)
            return NVML_ERROR_MEMORY;
        p.sessionInfoTbl = NV_PTR_TO_NvP64(tbl);
    }
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_GPU_GET_NVENC_SW_SESSION_INFO, &p, sizeof(p));
    if (st != NV_OK) {
        free(tbl);
        return nvml_from_rm(st);
    }
    unsigned n = p.sessionInfoTblEntry, cap = *sessionCount;
    *sessionCount = n;
    if (cap == 0) {
        free(tbl);
        return NVML_SUCCESS;
    }
    if (n > cap || n > NV2080_CTRL_GPU_NVENC_SESSION_INFO_MAX_COPYOUT_ENTRIES) {
        free(tbl);
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    for (unsigned i = 0; i < n; i++) {
        sessionInfos[i] = (nvmlEncoderSessionInfo_t) {
            .sessionId = tbl[i].sessionId,
            .pid = tbl[i].processId,
            .vgpuInstance = 0,
            .codecType = (nvmlEncoderType_t)tbl[i].codecType,   /* same numbering */
            .hResolution = tbl[i].hResolution,
            .vResolution = tbl[i].vResolution,
            .averageFps = tbl[i].averageEncodeFps,
            .averageLatency = tbl[i].averageEncodeLatency,
        };
    }
    free(tbl);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetFBCSessions(nvmlDevice_t device, unsigned int *sessionCount,
                                                  nvmlFBCSessionInfo_t *sessionInfo)
{
    DEVICE(device, dev);
    if (sessionCount == NULL || (*sessionCount != 0 && sessionInfo == NULL))
        return NVML_ERROR_INVALID_ARGUMENT;

    NV2080_CTRL_GPU_GET_NVFBC_SW_SESSION_INFO_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_GPU_GET_NVFBC_SW_SESSION_INFO, p, sizeof(*p));
    if (st != NV_OK) {
        free(p);
        return nvml_from_rm(st);
    }
    unsigned n = p->sessionInfoCount, cap = *sessionCount;
    if (n > NV2080_GPU_NVFBC_MAX_SESSION_COUNT)
        n = NV2080_GPU_NVFBC_MAX_SESSION_COUNT;
    *sessionCount = n;
    if (cap == 0) {
        free(p);
        return NVML_SUCCESS;
    }
    if (n > cap) {
        free(p);
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    for (unsigned i = 0; i < n; i++) {
        const NV2080_CTRL_NVFBC_SW_SESSION_INFO *s = &p->sessionInfoTbl[i];
        sessionInfo[i] = (nvmlFBCSessionInfo_t) {
            .sessionId = s->sessionId,
            .pid = s->processId,
            .vgpuInstance = 0,
            .displayOrdinal = s->displayOrdinal,
            .sessionType = (nvmlFBCSessionType_t)s->sessionType,   /* same numbering */
            .sessionFlags = s->sessionFlags,                      /* same bits */
            .hMaxResolution = s->hMaxResolution,
            .vMaxResolution = s->vMaxResolution,
            .hResolution = s->hResolution,
            .vResolution = s->vResolution,
            .averageFPS = s->averageFPS,
            .averageLatency = s->averageLatency,
        };
    }
    free(p);
    return NVML_SUCCESS;
}
