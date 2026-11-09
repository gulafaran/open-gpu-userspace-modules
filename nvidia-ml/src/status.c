/*
 * SPDX-License-Identifier: MIT
 *
 * Health and status queries: uptime, PDI, capabilities, recovery action,
 * repair and row remapping state, fabric probe, protected memory,
 * accounting PIDs, addressing mode, CPU/memory affinity.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nvml-priv.h"

#include "nvstatus.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0000/ctrl0000gpuacct.h"
#include "ctrl/ctrl2080/ctrl2080ecc.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080nvlink.h"
#include "ctrl/ctrl208f/ctrl208fgpu.h"
#include "class/cl208f.h"

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

/*
 * GPU_GET_INFO_V2 indexes the open headers don't list: the two halves of
 * the PDI (per-device identifier), found by watching the closed library.
 */
#define GPU_INFO_INDEX_PDI_LO32 0x23
#define GPU_INFO_INDEX_PDI_HI32 0x24

static NvU32 gpu_info(struct nvmlDevice_st *dev, const NvU32 *index, NvU32 *data, unsigned n)
{
    NV2080_CTRL_GPU_GET_INFO_V2_PARAMS p = { .gpuInfoListSize = n };
    for (unsigned i = 0; i < n; i++)
        p.gpuInfoList[i].index = index[i];
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_INFO_V2, &p, sizeof(p));
    for (unsigned i = 0; st == NV_OK && i < n; i++)
        data[i] = p.gpuInfoList[i].data;
    return st;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetGpuUptime_v1(nvmlDevice_t device, nvmlGpuUptime_v1_t *uptime)
{
    static const NvU32 idx[] = { NV2080_CTRL_GPU_INFO_INDEX_GPU_UPTIME };
    NvU32 v;
    DEVICE(device, dev);
    CHECK_PTR(uptime);
    NvU32 st = gpu_info(dev, idx, &v, 1);
    if (st != NV_OK)
        return nvml_from_rm(st);
    uptime->uptime = v;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPdi(nvmlDevice_t device, nvmlPdi_t *pdi)
{
    static const NvU32 idx[] = { GPU_INFO_INDEX_PDI_LO32, GPU_INFO_INDEX_PDI_HI32 };
    NvU32 v[2];
    DEVICE(device, dev);
    CHECK_PTR(pdi);
    if (pdi->version != nvmlPdi_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    NvU32 st = gpu_info(dev, idx, v, 2);
    if (st != NV_OK)
        return nvml_from_rm(st);
    pdi->value = (unsigned long long)v[1] << 32 | v[0];
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCapabilities(nvmlDevice_t device, nvmlDeviceCapabilities_t *caps)
{
    static const NvU32 idx[] = { NV2080_CTRL_GPU_INFO_INDEX_GPU_LOCAL_EGM_CAPABILITY };
    NvU32 v;
    DEVICE(device, dev);
    CHECK_PTR(caps);
    if (caps->version != nvmlDeviceCapabilities_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    NvU32 st = gpu_info(dev, idx, &v, 1);
    if (st != NV_OK)
        return nvml_from_rm(st);
    caps->capMask = 0;
    if ((v & 1) == NV2080_CTRL_GPU_INFO_INDEX_GPU_LOCAL_EGM_CAPABILITY_YES)
        caps->capMask |= NVML_DEV_CAP_EGM;
    return NVML_SUCCESS;
}

/* NVML's recovery action values are RM's. */
nvmlReturn_t nvml_recovery_action(struct nvmlDevice_st *dev, unsigned *action)
{
    NV2080_CTRL_GPU_GET_RECOVERY_ACTION_PARAMS p = { 0 };
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_RECOVERY_ACTION, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *action = p.action;
    return NVML_SUCCESS;
}

nvmlReturn_t nvml_reset_status(struct nvmlDevice_st *dev, bool drain, unsigned *value)
{
    NvU32 st;
    if (drain) {
        NV2080_CTRL_GPU_GET_DRAIN_AND_RESET_STATUS_PARAMS p = { 0 };
        st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_DRAIN_AND_RESET_STATUS, &p, sizeof(p));
        *value = p.bDrainRecommended;
    } else {
        NV2080_CTRL_GPU_GET_RESET_STATUS_PARAMS p = { 0 };
        st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_RESET_STATUS, &p, sizeof(p));
        *value = p.bResetRequired;
    }
    return nvml_from_rm(st);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetRepairStatus(nvmlDevice_t device, nvmlRepairStatus_t *repairStatus)
{
    NV2080_CTRL_ECC_GET_REPAIR_STATUS_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(repairStatus);
    if (repairStatus->version != nvmlRepairStatus_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_ECC_GET_REPAIR_STATUS, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    repairStatus->bChannelRepairPending = p.bChannelRepairPending;
    repairStatus->bTpcRepairPending = p.bTpcRepairPending;
    return NVML_SUCCESS;
}

/* ---- row remapping ---------------------------------------------------------- */

static nvmlReturn_t remapped_rows(struct nvmlDevice_st *dev, nvmlRemappedRowsInfo_v2_t *info)
{
    NV2080_CTRL_FB_GET_REMAPPED_ROWS_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_GET_REMAPPED_ROWS, p, sizeof(*p));
    if (st != NV_OK) {
        free(p);
        return nvml_from_rm(st);
    }
    memset(info, 0, sizeof(*info));
    NvU32 n = p->entryCount < NV2080_CTRL_FB_REMAPPED_ROWS_MAX_ROWS ? p->entryCount
                                                                    : NV2080_CTRL_FB_REMAPPED_ROWS_MAX_ROWS;
    for (NvU32 i = 0; i < n; i++) {
        const NV2080_CTRL_FB_ROW_REMAP_ENTRY *e = &p->entries[i];
        bool active = e->flags & 2;   /* NV2080_CTRL_FB_REMAP_ENTRY_FLAGS_ACTIVE */
        if (e->source == NV2080_CTRL_FB_REMAPPED_ROW_SOURCE_SBE_FIELD)
            active ? info->corrActiveRemaps++ : info->corrInactiveRemaps++;
        else if (e->source == NV2080_CTRL_FB_REMAPPED_ROW_SOURCE_DBE_FIELD)
            active ? info->uncActiveRemaps++ : info->uncInactiveRemaps++;
    }
    info->bPending = p->flags & 1;          /* ..._FLAGS_PENDING */
    info->bFailureOccurred = (p->flags >> 1) & 1;   /* ..._FLAGS_FAILURE */
    free(p);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetRemappedRows_v2(nvmlDevice_t device, nvmlRemappedRowsInfo_v2_t *info)
{
    DEVICE(device, dev);
    CHECK_PTR(info);
    return remapped_rows(dev, info);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetRemappedRows(nvmlDevice_t device, unsigned int *corrRows,
                                                   unsigned int *uncRows, unsigned int *isPending,
                                                   unsigned int *failureOccurred)
{
    nvmlRemappedRowsInfo_v2_t i;
    DEVICE(device, dev);
    CHECK_PTR(corrRows);
    CHECK_PTR(uncRows);
    CHECK_PTR(isPending);
    CHECK_PTR(failureOccurred);
    nvmlReturn_t r = remapped_rows(dev, &i);
    if (r != NVML_SUCCESS)
        return r;
    *corrRows = i.corrActiveRemaps;
    *uncRows = i.uncActiveRemaps;
    *isPending = i.bPending;
    *failureOccurred = i.bFailureOccurred;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetRowRemapperHistogram(nvmlDevice_t device,
                                                           nvmlRowRemapperHistogramValues_t *values)
{
    NV2080_CTRL_FB_GET_ROW_REMAPPER_HISTOGRAM_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(values);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_GET_ROW_REMAPPER_HISTOGRAM, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    /* RM counts banks by rows remapped, NVML by rows still available. */
    values->max = p.histogram[NV2080_CTRL_FB_HISTOGRAM_IDX_NO_REMAPPED_ROWS];
    values->high = p.histogram[NV2080_CTRL_FB_HISTOGRAM_IDX_SINGLE_REMAPPED_ROW];
    values->partial = p.histogram[NV2080_CTRL_FB_HISTOGRAM_IDX_MIXED_REMAPPED_REMAINING_ROWS];
    values->low = p.histogram[NV2080_CTRL_FB_HISTOGRAM_IDX_SINGLE_REMAINING_ROW];
    values->none = p.histogram[NV2080_CTRL_FB_HISTOGRAM_IDX_MAX_REMAPPED_ROWS];
    return NVML_SUCCESS;
}

/* ---- fabric ----------------------------------------------------------------- */

/* NVML's fabric states, health mask and summary use RM's encodings. */
static nvmlReturn_t fabric_info(struct nvmlDevice_st *dev, NV2080_CTRL_CMD_GET_GPU_FABRIC_PROBE_INFO_PARAMS *p)
{
    memset(p, 0, sizeof(*p));
    return nvml_from_rm(nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GET_GPU_FABRIC_PROBE_INFO, p, sizeof(*p)));
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetGpuFabricInfo(nvmlDevice_t device, nvmlGpuFabricInfo_t *info)
{
    NV2080_CTRL_CMD_GET_GPU_FABRIC_PROBE_INFO_PARAMS p;
    DEVICE(device, dev);
    CHECK_PTR(info);
    nvmlReturn_t r = fabric_info(dev, &p);
    if (r != NVML_SUCCESS)
        return r;
    memcpy(info->clusterUuid, p.clusterUuid, NVML_GPU_FABRIC_UUID_LEN);
    info->status = nvml_from_rm(p.status);
    info->cliqueId = p.fabricCliqueId;
    info->state = p.state;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetGpuFabricInfoV(nvmlDevice_t device, nvmlGpuFabricInfoV_t *info)
{
    NV2080_CTRL_CMD_GET_GPU_FABRIC_PROBE_INFO_PARAMS p;
    DEVICE(device, dev);
    CHECK_PTR(info);
    if (info->version != nvmlGpuFabricInfo_v2 && info->version != nvmlGpuFabricInfo_v3)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    nvmlReturn_t r = fabric_info(dev, &p);
    if (r != NVML_SUCCESS)
        return r;
    memcpy(info->clusterUuid, p.clusterUuid, NVML_GPU_FABRIC_UUID_LEN);
    info->status = nvml_from_rm(p.status);
    info->cliqueId = p.fabricCliqueId;
    info->state = p.state;
    info->healthMask = p.fabricHealthMask;
    if (info->version == nvmlGpuFabricInfo_v3)
        info->healthSummary = p.fabricHealthSummary;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetGpuFabricInfo_v4(nvmlDevice_t device, nvmlGpuFabricInfo_v4_t *info)
{
    NV2080_CTRL_CMD_GET_GPU_FABRIC_PROBE_INFO_PARAMS p;
    DEVICE(device, dev);
    CHECK_PTR(info);
    nvmlReturn_t r = fabric_info(dev, &p);
    if (r != NVML_SUCCESS)
        return r;
    memset(info, 0, sizeof(*info));
    memcpy(info->clusterUuid, p.clusterUuid, NVML_GPU_FABRIC_UUID_LEN);
    info->status = nvml_from_rm(p.status);
    unsigned n = p.fabricNumCliques;
    if (n > NV2080_CTRL_GPU_FABRIC_CLIQUE_MAX_SIZE)
        n = NV2080_CTRL_GPU_FABRIC_CLIQUE_MAX_SIZE;
    if (n > NVML_GPU_FABRIC_CLIQUE_MAX)
        n = NVML_GPU_FABRIC_CLIQUE_MAX;
    for (unsigned i = 0; i < n; i++) {
        info->cliques[i].type = p.fabricCliques[i].type;
        info->cliques[i].id = p.fabricCliques[i].id;
    }
    info->numCliques = n;
    info->state = p.state;
    info->healthMask = p.fabricHealthMask;
    info->healthSummary = p.fabricHealthSummary;
    return NVML_SUCCESS;
}

/* ---- memory ----------------------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetConfComputeProtectedMemoryUsage(nvmlDevice_t device, nvmlMemory_t *memory)
{
    NV2080_CTRL_FB_GET_INFO_V2_PARAMS p = {
        .fbInfoListSize = 2,
        .fbInfoList = {
            { .index = NV2080_CTRL_FB_INFO_INDEX_PROTECTED_MEM_SIZE_TOTAL_KB },
            { .index = NV2080_CTRL_FB_INFO_INDEX_PROTECTED_MEM_SIZE_FREE_KB },
        },
    };
    DEVICE(device, dev);
    CHECK_PTR(memory);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_GET_INFO_V2, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    unsigned long long total = (unsigned long long)p.fbInfoList[0].data << 10;
    unsigned long long avail = (unsigned long long)p.fbInfoList[1].data << 10;
    memory->total = total;
    memory->free = avail < total ? avail : total;
    memory->used = total - memory->free;
    return NVML_SUCCESS;
}

/* ---- accounting ------------------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetAccountingPids(nvmlDevice_t device, unsigned int *count, unsigned int *pids)
{
    DEVICE(device, dev);
    CHECK_PTR(count);
    NV0000_CTRL_GPUACCT_GET_ACCOUNTING_PIDS_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    p->gpuId = dev->gpu_id;
    NvU32 st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_CMD_GPUACCT_GET_ACCOUNTING_PIDS,
                              p, sizeof(*p));
    if (st != NV_OK) {
        free(p);
        return nvml_from_rm(st);
    }
    unsigned n = p->pidCount < NV0000_GPUACCT_PID_MAX_COUNT ? p->pidCount : NV0000_GPUACCT_PID_MAX_COUNT;
    /* Like the closed library: a NULL buffer is always "too small", even for 0. */
    nvmlReturn_t r = NVML_SUCCESS;
    if (pids == NULL || *count < n)
        r = NVML_ERROR_INSUFFICIENT_SIZE;
    else
        memcpy(pids, p->pidTbl, n * sizeof(*pids));
    *count = n;
    free(p);
    return r;
}

/* ---- addressing mode -------------------------------------------------------- */

/* nvidia-uvm ioctls (kernel-open/nvidia-uvm/uvm_ioctl.h, uvm_linux_ioctl.h). */
#define UVM_INITIALIZE          0x30000001
#define UVM_PAGEABLE_MEM_ACCESS 39
#define UVM_MM_INITIALIZE       75

struct uvm_initialize_params { NvU64 flags; NV_STATUS rmStatus; };
struct uvm_mm_initialize_params { NvS32 uvmFd; NV_STATUS rmStatus; };
struct uvm_pageable_mem_access_params { NvBool pageableMemAccess; NV_STATUS rmStatus; };

/*
 * Whether UVM lets the GPU access pageable host memory (HMM, or ATS): asks
 * a fresh UVM VA space, like CUDA would get. Not cached, the module can be
 * reloaded with different parameters.
 */
static bool uvm_pageable_access(void)
{
    bool ret = false;
    int fd = open("/dev/nvidia-uvm", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    int mm_fd = open("/dev/nvidia-uvm", O_RDWR | O_CLOEXEC);
    struct uvm_initialize_params init = { 0 };
    struct uvm_mm_initialize_params mm = { .uvmFd = fd };
    struct uvm_pageable_mem_access_params pma = { 0 };
    if (mm_fd >= 0 &&
        ioctl(fd, UVM_INITIALIZE, &init) == 0 && init.rmStatus == NV_OK &&
        ioctl(mm_fd, UVM_MM_INITIALIZE, &mm) == 0 && mm.rmStatus == NV_OK &&
        ioctl(fd, UVM_PAGEABLE_MEM_ACCESS, &pma) == 0 && pma.rmStatus == NV_OK)
        ret = pma.pageableMemAccess;
    if (mm_fd >= 0)
        close(mm_fd);
    close(fd);
    return ret;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetAddressingMode(nvmlDevice_t device, nvmlDeviceAddressingMode_t *mode)
{
    DEVICE(device, dev);
    CHECK_PTR(mode);
    if (mode->version != nvmlDeviceAddressingMode_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = dev->gpu_id };
    NvU32 st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id));
    if (st != NV_OK)
        return nvml_from_rm(st);
    if (!uvm_pageable_access())
        mode->value = NVML_DEVICE_ADDRESSING_MODE_NONE;
    else if ((id.gpuFlags >> 6) & 1)   /* NV0000_CTRL_GPU_ID_INFO_ATS_ENABLED */
        mode->value = NVML_DEVICE_ADDRESSING_MODE_ATS;
    else
        mode->value = NVML_DEVICE_ADDRESSING_MODE_HMM;
    return NVML_SUCCESS;
}

/* ---- affinity --------------------------------------------------------------- */

static int read_sysfs(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "re");
    if (f == NULL)
        return -1;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = 0;
    return 0;
}

/* Parses a cpulist ("0-3,8") into a bitmap of `words` unsigned longs. */
static void parse_list(const char *s, unsigned long *set, unsigned words)
{
    const unsigned bits = 8 * sizeof(unsigned long);
    while (*s) {
        char *e;
        unsigned long a = strtoul(s, &e, 10), b = a;
        if (e == s)
            break;
        if (*e == '-')
            b = strtoul(e + 1, &e, 10);
        for (unsigned long i = a; i <= b && i < (unsigned long)words * bits; i++)
            set[i / bits] |= 1UL << (i % bits);
        s = *e == ',' ? e + 1 : e;
    }
}

static void sysfs_path(struct nvmlDevice_st *dev, char *path, size_t len, const char *file)
{
    snprintf(path, len, "/sys/bus/pci/devices/%04x:%02x:%02x.0/%s",
             dev->pci.domain, dev->pci.bus, dev->pci.device, file);
}

static int numa_node(struct nvmlDevice_st *dev)
{
    char path[96], buf[16];
    sysfs_path(dev, path, sizeof(path), "numa_node");
    if (read_sysfs(path, buf, sizeof(buf)) != 0)
        return -1;
    return atoi(buf);
}

/* CPUs of the GPU's NUMA node; with socket scope, of every CPU package they're on. */
static nvmlReturn_t cpu_affinity(struct nvmlDevice_st *dev, unsigned words, unsigned long *set,
                                 nvmlAffinityScope_t scope)
{
    const unsigned bits = 8 * sizeof(unsigned long);
    char path[96], buf[4096];
    if (words == 0 || set == NULL || scope > NVML_AFFINITY_SCOPE_SOCKET)
        return NVML_ERROR_INVALID_ARGUMENT;
    memset(set, 0, words * sizeof(*set));
    sysfs_path(dev, path, sizeof(path), "local_cpulist");
    if (read_sysfs(path, buf, sizeof(buf)) != 0)
        return NVML_ERROR_NOT_SUPPORTED;
    parse_list(buf, set, words);
    if (scope != NVML_AFFINITY_SCOPE_SOCKET)
        return NVML_SUCCESS;

    unsigned long pkgs[4] = { 0 };   /* package ids seen, up to 256 */
    for (unsigned c = 0; c < words * bits; c++) {
        if (!(set[c / bits] & (1UL << (c % bits))))
            continue;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/physical_package_id", c);
        if (read_sysfs(path, buf, sizeof(buf)) == 0) {
            unsigned p = strtoul(buf, NULL, 10);
            if (p < 256)
                pkgs[p / 64] |= 1UL << (p % 64);
        }
    }
    for (unsigned c = 0; c < words * bits; c++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/physical_package_id", c);
        if (read_sysfs(path, buf, sizeof(buf)) != 0)
            continue;
        unsigned p = strtoul(buf, NULL, 10);
        if (p < 256 && (pkgs[p / 64] & (1UL << (p % 64))))
            set[c / bits] |= 1UL << (c % bits);
    }
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCpuAffinityWithinScope(nvmlDevice_t device, unsigned int cpuSetSize,
                                                             unsigned long *cpuSet, nvmlAffinityScope_t scope)
{
    DEVICE(device, dev);
    return cpu_affinity(dev, cpuSetSize, cpuSet, scope);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCpuAffinity(nvmlDevice_t device, unsigned int cpuSetSize, unsigned long *cpuSet)
{
    DEVICE(device, dev);
    return cpu_affinity(dev, cpuSetSize, cpuSet, NVML_AFFINITY_SCOPE_NODE);
}

/* The GPU's NUMA node (node 0 without NUMA); with socket scope, every node sharing its CPUs' packages. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetMemoryAffinity(nvmlDevice_t device, unsigned int nodeSetSize,
                                                     unsigned long *nodeSet, nvmlAffinityScope_t scope)
{
    const unsigned bits = 8 * sizeof(unsigned long);
    DEVICE(device, dev);
    if (nodeSetSize == 0 || nodeSet == NULL || scope > NVML_AFFINITY_SCOPE_SOCKET)
        return NVML_ERROR_INVALID_ARGUMENT;
    memset(nodeSet, 0, nodeSetSize * sizeof(*nodeSet));
    int node = numa_node(dev);
    if (node < 0)
        node = 0;
    if ((unsigned)node < nodeSetSize * bits)
        nodeSet[node / bits] |= 1UL << (node % bits);
    if (scope != NVML_AFFINITY_SCOPE_SOCKET)
        return NVML_SUCCESS;

    unsigned long cpus[16];
    if (cpu_affinity(dev, 16, cpus, NVML_AFFINITY_SCOPE_SOCKET) != NVML_SUCCESS)
        return NVML_SUCCESS;
    for (unsigned n = 0; n < nodeSetSize * bits; n++) {
        char path[64], buf[4096];
        unsigned long ncpus[16] = { 0 };
        snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/cpulist", n);
        if (read_sysfs(path, buf, sizeof(buf)) != 0)
            continue;
        parse_list(buf, ncpus, 16);
        for (unsigned w = 0; w < 16; w++)
            if (ncpus[w] & cpus[w]) {
                nodeSet[n / bits] |= 1UL << (n % bits);
                break;
            }
    }
    return NVML_SUCCESS;
}

/* Only GPUs whose memory is a NUMA node (coherent platforms) have one. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetNumaNodeId(nvmlDevice_t device, unsigned int *node)
{
    DEVICE(device, dev);
    CHECK_PTR(node);
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = dev->gpu_id };
    NvU32 st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id));
    if (st != NV_OK)
        return nvml_from_rm(st);
    if (id.numaId < 0)
        return NVML_ERROR_NOT_SUPPORTED;
    *node = id.numaId;
    return NVML_SUCCESS;
}

/* SM version as the CUDA compute capability, e.g. 0x809 -> 8.9. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetCudaComputeCapability(nvmlDevice_t device, int *major, int *minor)
{
    static const NvU32 idx[] = { NV2080_CTRL_GPU_INFO_INDEX_GPU_SM_VERSION };
    NvU32 v;
    DEVICE(device, dev);
    CHECK_PTR(major);
    CHECK_PTR(minor);
    NvU32 st = gpu_info(dev, idx, &v, 1);
    if (st != NV_OK)
        return nvml_from_rm(st);
    *major = (v >> 8) & 0xff;
    *minor = v & 0xff;
    return NVML_SUCCESS;
}

/* ---- InfoROM ---------------------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceValidateInforom(nvmlDevice_t device)
{
    NV208F_CTRL_GPU_VERIFY_INFOROM_PARAMS p = { 0 };
    NvU32 st = NV_ERR_NOT_SUPPORTED;
    DEVICE(device, dev);
    pthread_mutex_lock(&dev->lock);
    if (dev->diag == 0) {
        NvHandle h = nvc_rm_handle(&dev->rm);
        if (nvc_rm_alloc(&dev->rm, dev->subdevice, h, NV20_SUBDEVICE_DIAG, NULL) == NV_OK)
            dev->diag = h;
    }
    if (dev->diag != 0)
        st = nvc_rm_control(&dev->rm, dev->diag, NV208F_CTRL_CMD_GPU_VERIFY_INFOROM, &p, sizeof(p));
    pthread_mutex_unlock(&dev->lock);
    if (st != NV_OK)
        return nvml_from_rm(st);
    switch (p.result) {
    case NV208F_CTRL_GPU_INFOROM_VERIFICATION_RESULT_VALID:   return NVML_SUCCESS;
    case NV208F_CTRL_GPU_INFOROM_VERIFICATION_RESULT_INVALID: return NVML_ERROR_CORRUPTED_INFOROM;
    case NV208F_CTRL_GPU_INFOROM_VERIFICATION_RESULT_NONE:    return NVML_ERROR_NOT_SUPPORTED;
    default:                                                  return NVML_ERROR_UNKNOWN;
    }
}

/* ---- SRAM ECC --------------------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetSramEccErrorStatus(nvmlDevice_t device, nvmlEccSramErrorStatus_t *status)
{
    NV2080_CTRL_ECC_GET_VOLATILE_COUNTS_PARAMS vol = { 0 };
    NV2080_CTRL_ECC_GET_CLIENT_EXPOSED_COUNTERS_PARAMS agg = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(status);
    if (status->version != nvmlEccSramErrorStatus_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_ECC_GET_VOLATILE_COUNTS, &vol, sizeof(vol));
    if (st == NV_OK)
        st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_ECC_GET_CLIENT_EXPOSED_COUNTERS, &agg, sizeof(agg));
    if (st != NV_OK)
        return nvml_from_rm(st);
    status->aggregateUncParity = agg.sramParityUncorrectedUnique;
    status->aggregateUncSecDed = agg.sramSecDedUncorrectedUnique;
    status->aggregateCor = agg.sramCorrectedUnique;
    status->volatileUncParity = vol.sramUncParityUni;
    status->volatileUncSecDed = vol.sramUncSecDedUni;
    status->volatileCor = vol.sramCorUni;
    status->aggregateUncBucketL2 = agg.sramBucketL2;
    status->aggregateUncBucketSm = agg.sramBucketSM;
    status->aggregateUncBucketPcie = agg.sramBucketPcie;
    status->aggregateUncBucketMcu = agg.sramBucketFirmware;
    status->aggregateUncBucketOther = agg.sramBucketOther;
    status->bThresholdExceeded = agg.sramErrorThresholdExceeded;
    return NVML_SUCCESS;
}

/* ---- platform --------------------------------------------------------------- */

/* v1 and v2 share the layout; host/node and module ids are 1-based in NVML
 * (as the closed library reports them), 0-based in RM. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetPlatformInfo(nvmlDevice_t device, nvmlPlatformInfo_t *info)
{
    NV2080_CTRL_NVLINK_GET_PLATFORM_INFO_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(info);
    if (info->version != nvmlPlatformInfo_v1 && info->version != nvmlPlatformInfo_v2)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_NVLINK_GET_PLATFORM_INFO, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    unsigned version = info->version;
    memset(info, 0, sizeof(*info));
    info->version = version;
    memcpy(info->ibGuid, p.ibGuid, sizeof(info->ibGuid));
    memcpy(info->chassisSerialNumber, p.rackGuid, sizeof(info->chassisSerialNumber));
    info->slotNumber = p.chassisPhysicalSlotNumber;
    info->trayIndex = p.computeSlotIndex;
    info->hostId = p.nodeIndex + 1;
    info->peerType = p.peerType;
    info->moduleId = p.moduleId + 1;
    return NVML_SUCCESS;
}
