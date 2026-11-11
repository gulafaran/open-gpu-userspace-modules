/*
 * SPDX-License-Identifier: MIT
 *
 * System-level queries: confidential computing, excluded GPUs, driver
 * branch, drain state, device memory cgroup limits, per-process
 * accounting records.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvml-priv.h"

#include "nvstatus.h"
#include "class/clcb33.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0000/ctrl0000gpuacct.h"
#include "ctrl/ctrl0000/ctrl0000system.h"
#include "ctrl/ctrl0000/ctrl0000unix.h"
#include "ctrl/ctrl2080/ctrl2080mc.h"
#include "ctrl/ctrl208f/ctrl208fgpu.h"
#include "ctrl/ctrlc56f.h"
#include "ctrl/ctrlcb33.h"
#include "class/cl208f.h"

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define INITIALIZED()                                         \
    do {                                                      \
        nvmlReturn_t r_ = nvml_initialized();                 \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

/* ---- confidential computing ------------------------------------------------------ */

/*
 * One control on a NV_CONFIDENTIAL_COMPUTE object of the given client; the
 * object only lives for the call.
 */
static NvU32 cc_control(struct nvc_rm *rm, NvU32 cmd, void *params, NvU32 size)
{
    NV_CONFIDENTIAL_COMPUTE_ALLOC_PARAMS alloc = { .hClient = rm->client };
    NvHandle h = nvc_rm_handle(rm);
    NvU32 st = nvc_rm_alloc(rm, rm->client, h, NV_CONFIDENTIAL_COMPUTE, &alloc);
    if (st != NV_OK)
        return st;
    st = nvc_rm_control(rm, h, cmd, params, size);
    nvc_rm_free(rm, rm->client, h);
    return st;
}

static nvmlReturn_t cc_capabilities(NV_CONF_COMPUTE_CTRL_CMD_SYSTEM_GET_CAPABILITIES_PARAMS *p)
{
    memset(p, 0, sizeof(*p));
    return nvml_from_rm(cc_control(&nvml.sys, NV_CONF_COMPUTE_CTRL_CMD_SYSTEM_GET_CAPABILITIES,
                                   p, sizeof(*p)));
}

/* RM tells the confidential computing flavours apart, NVML only BMSAI. */
static unsigned cc_feature(NvU8 feature)
{
    switch (feature) {
    case NV_CONF_COMPUTE_SYSTEM_FEATURE_DISABLED:      return NVML_CC_SYSTEM_FEATURE_DISABLED;
    case NV_CONF_COMPUTE_SYSTEM_FEATURE_BMSAI_ENABLED: return NVML_CC_SYSTEM_FEATURE_BMSAI_ENABLED;
    default:                                           return NVML_CC_SYSTEM_FEATURE_ENABLED;
    }
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetConfComputeCapabilities(nvmlConfComputeSystemCaps_t *capabilities)
{
    NV_CONF_COMPUTE_CTRL_CMD_SYSTEM_GET_CAPABILITIES_PARAMS p;
    INITIALIZED();
    CHECK_PTR(capabilities);
    nvmlReturn_t ret = cc_capabilities(&p);
    if (ret != NVML_SUCCESS)
        return ret;
    capabilities->cpuCaps = p.cpuCapability;
    switch (p.gpusCapability) {
    case NV_CONF_COMPUTE_SYSTEM_GPUS_CAPABILITY_NONE:  capabilities->gpusCaps = NVML_CC_SYSTEM_GPUS_CC_NOT_CAPABLE; break;
    case NV_CONF_COMPUTE_SYSTEM_GPUS_CAPABILITY_BMSAI: capabilities->gpusCaps = NVML_CC_SYSTEM_GPUS_CC_BMSAI_CAPABLE; break;
    default:                                           capabilities->gpusCaps = NVML_CC_SYSTEM_GPUS_CC_CAPABLE; break;
    }
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetConfComputeState(nvmlConfComputeSystemState_t *state)
{
    NV_CONF_COMPUTE_CTRL_CMD_SYSTEM_GET_CAPABILITIES_PARAMS p;
    INITIALIZED();
    CHECK_PTR(state);
    nvmlReturn_t ret = cc_capabilities(&p);
    if (ret != NVML_SUCCESS)
        return ret;
    state->environment = p.environment;
    state->ccFeature = cc_feature(p.ccFeature);
    state->devToolsMode = p.devToolsMode;
    return NVML_SUCCESS;
}

static nvmlReturn_t cc_settings(nvmlSystemConfComputeSettings_v2_t *s)
{
    NV_CONF_COMPUTE_CTRL_CMD_SYSTEM_GET_CAPABILITIES_PARAMS p;
    nvmlReturn_t ret = cc_capabilities(&p);
    if (ret != NVML_SUCCESS)
        return ret;
    s->environment = p.environment;
    s->ccFeature = cc_feature(p.ccFeature);
    s->devToolsMode = p.devToolsMode;
    s->multiGpuMode = p.multiGpuMode;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetConfComputeSettings(nvmlSystemConfComputeSettings_t *settings)
{
    nvmlSystemConfComputeSettings_v2_t s = { 0 };
    INITIALIZED();
    CHECK_PTR(settings);
    if (settings->version != nvmlSystemConfComputeSettings_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    nvmlReturn_t ret = cc_settings(&s);
    if (ret != NVML_SUCCESS)
        return ret;
    settings->environment = s.environment;
    settings->ccFeature = s.ccFeature;
    settings->devToolsMode = s.devToolsMode;
    settings->multiGpuMode = s.multiGpuMode;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetConfComputeSettings_v2(nvmlSystemConfComputeSettings_v2_t *settings)
{
    nvmlSystemConfComputeSettings_v2_t s = { 0 };
    INITIALIZED();
    CHECK_PTR(settings);
    /* A version 1 structure is filled in as far as it goes. */
    if (settings->version != nvmlSystemConfComputeSettings_v2 &&
        settings->version != nvmlSystemConfComputeSettings_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    nvmlReturn_t ret = cc_settings(&s);
    if (ret != NVML_SUCCESS)
        return ret;
    settings->environment = s.environment;
    settings->ccFeature = s.ccFeature;
    settings->devToolsMode = s.devToolsMode;
    settings->multiGpuMode = s.multiGpuMode;
    if (settings->version == nvmlSystemConfComputeSettings_v2) {
        NV_CONF_COMPUTE_CTRL_CMD_SYSTEM_GET_GPUS_TDISP_STATE_PARAMS t = { 0 };
        NvU32 st = cc_control(&nvml.sys, NV_CONF_COMPUTE_CTRL_CMD_SYSTEM_GET_GPUS_TDISP_STATE, &t, sizeof(t));
        /* NOT_SUPPORTED: TDISP isn't enabled. */
        settings->tdispState = st != NV_OK ? NVML_CC_SYSTEM_TDISP_STATE_DISABLED
                             : t.bTdiSecure ? NVML_CC_SYSTEM_TDISP_STATE_READY
                                            : NVML_CC_SYSTEM_TDISP_STATE_NOT_READY;
    }
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetConfComputeGpusReadyState(unsigned int *isAcceptingWork)
{
    NV_CONF_COMPUTE_CTRL_CMD_SYSTEM_GET_GPUS_STATE_PARAMS p = { 0 };
    INITIALIZED();
    CHECK_PTR(isAcceptingWork);
    NvU32 st = cc_control(&nvml.sys, NV_CONF_COMPUTE_CTRL_CMD_SYSTEM_GET_GPUS_STATE, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *isAcceptingWork = p.bAcceptClientRequest ? NVML_CC_ACCEPTING_CLIENT_REQUESTS_TRUE
                                              : NVML_CC_ACCEPTING_CLIENT_REQUESTS_FALSE;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetConfComputeKeyRotationThresholdInfo(
    nvmlConfComputeGetKeyRotationThresholdInfo_t *info)
{
    NV_CONF_COMPUTE_CTRL_GET_SECURITY_POLICY_PARAMS p = { 0 };
    INITIALIZED();
    CHECK_PTR(info);
    if (info->version != nvmlConfComputeGetKeyRotationThresholdInfo_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    NvU32 st = cc_control(&nvml.sys, NV_CONF_COMPUTE_CTRL_GET_SECURITY_POLICY, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    info->attackerAdvantage = p.attackerAdvantage;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetConfComputeMemSizeInfo(nvmlDevice_t device,
                                                             nvmlConfComputeMemSizeInfo_t *memInfo)
{
    DEVICE(device, dev);
    CHECK_PTR(memInfo);
    NV_CONF_COMPUTE_CTRL_CMD_GPU_GET_VIDMEM_SIZE_PARAMS p = { .hSubDevice = dev->subdevice };
    NvU32 st = cc_control(&dev->rm, NV_CONF_COMPUTE_CTRL_CMD_GPU_GET_VIDMEM_SIZE, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    memInfo->protectedMemSizeKib = p.protectedMemSizeInKb;
    memInfo->unprotectedMemSizeKib = p.unprotectedMemSizeInKb;
    return NVML_SUCCESS;
}

/* Certificate and report failures aren't told apart further. */
static nvmlReturn_t cc_attestation_error(NvU32 st)
{
    switch (st) {
    case NV_ERR_NOT_SUPPORTED:
    case NV_ERR_GPU_IS_LOST:
        return nvml_from_rm(st);
    default:
        return NVML_ERROR_UNKNOWN;
    }
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetConfComputeGpuCertificate(nvmlDevice_t device,
                                                                nvmlConfComputeGpuCertificate_t *cert)
{
    DEVICE(device, dev);
    CHECK_PTR(cert);
    NV_CONF_COMPUTE_CTRL_CMD_GET_GPU_CERTIFICATE_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    p->hSubDevice = dev->subdevice;
    NvU32 st = cc_control(&dev->rm, NV_CONF_COMPUTE_CTRL_CMD_GET_GPU_CERTIFICATE, p, sizeof(*p));
    if (st == NV_OK) {
        _Static_assert(sizeof(cert->certChain) == sizeof(p->certChain), "certificate chain size");
        _Static_assert(sizeof(cert->attestationCertChain) == sizeof(p->attestationCertChain),
                       "attestation certificate chain size");
        cert->certChainSize = p->certChainSize;
        cert->attestationCertChainSize = p->attestationCertChainSize;
        memcpy(cert->certChain, p->certChain, sizeof(cert->certChain));
        memcpy(cert->attestationCertChain, p->attestationCertChain, sizeof(cert->attestationCertChain));
    }
    free(p);
    return st == NV_OK ? NVML_SUCCESS : cc_attestation_error(st);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetConfComputeGpuAttestationReport(
    nvmlDevice_t device, nvmlConfComputeGpuAttestationReport_t *report)
{
    DEVICE(device, dev);
    CHECK_PTR(report);
    NV_CONF_COMPUTE_CTRL_CMD_GET_GPU_ATTESTATION_REPORT_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    _Static_assert(sizeof(report->nonce) == sizeof(p->nonce), "nonce size");
    _Static_assert(sizeof(report->attestationReport) == sizeof(p->attestationReport), "report size");
    _Static_assert(sizeof(report->cecAttestationReport) == sizeof(p->cecAttestationReport),
                   "CEC report size");
    p->hSubDevice = dev->subdevice;
    memcpy(p->nonce, report->nonce, sizeof(p->nonce));
    NvU32 st = cc_control(&dev->rm, NV_CONF_COMPUTE_CTRL_CMD_GET_GPU_ATTESTATION_REPORT, p, sizeof(*p));
    if (st == NV_OK) {
        report->isCecAttestationReportPresent = p->isCecAttestationReportPresent ? 1 : 0;
        report->attestationReportSize = p->attestationReportSize;
        report->cecAttestationReportSize = p->cecAttestationReportSize;
        memcpy(report->attestationReport, p->attestationReport, sizeof(report->attestationReport));
        memcpy(report->cecAttestationReport, p->cecAttestationReport, sizeof(report->cecAttestationReport));
    }
    free(p);
    return st == NV_OK ? NVML_SUCCESS : cc_attestation_error(st);
}

/*
 * GPU performance monitoring (GPM) came with Hopper, and its counters can't
 * be read while confidential computing or protected PCIe guards the GPU.
 */
NVML_EXPORT nvmlReturn_t nvmlGpmQueryDeviceSupport(nvmlDevice_t device, nvmlGpmSupport_t *gpmSupport)
{
    nvmlSystemConfComputeSettings_v2_t s = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(gpmSupport);
    if (gpmSupport->version != NVML_GPM_SUPPORT_VERSION)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    gpmSupport->isSupportedDevice = 0;
    nvmlReturn_t ret = cc_settings(&s);
    if (ret != NVML_ERROR_NOT_SUPPORTED &&
        (ret != NVML_SUCCESS || s.ccFeature == NVML_CC_SYSTEM_FEATURE_ENABLED ||
         s.multiGpuMode == NVML_CC_SYSTEM_MULTIGPU_PROTECTED_PCIE))
        return NVML_SUCCESS;
    /* Hopper and Blackwell+ dGPUs (guess; Ada, numerically after Hopper, has no GPM: 0 here
     * like closed). Tegra arch ids have the 0xE0000000 bits. */
    gpmSupport->isSupportedDevice = dev->arch == NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GH100 ||
                                    (dev->arch >= NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GB100 &&
                                     dev->arch < 0xE0000000);
    return NVML_SUCCESS;
}

/* ---- excluded GPUs ----------------------------------------------------------------- */

/* GPUs the kernel module was told to leave alone (the nvidia.ko exclude list). */
static nvmlReturn_t excluded_ids(NvU32 *ids, unsigned *count)
{
    NV0000_CTRL_GPU_GET_PROBED_IDS_PARAMS p;
    memset(&p, 0, sizeof(p));
    NvU32 st = nvc_rm_control(&nvml.sys, nvml.sys.client, NV0000_CTRL_CMD_GPU_GET_PROBED_IDS, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    unsigned n = 0;
    for (unsigned i = 0; i < NV0000_CTRL_GPU_MAX_PROBED_GPUS; i++)
        if (p.excludedGpuIds[i] != NV0000_CTRL_GPU_INVALID_ID)
            ids[n++] = p.excludedGpuIds[i];
    *count = n;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlGetExcludedDeviceCount(unsigned int *deviceCount)
{
    NvU32 ids[NV0000_CTRL_GPU_MAX_PROBED_GPUS];
    INITIALIZED();
    CHECK_PTR(deviceCount);
    return excluded_ids(ids, deviceCount);
}

NVML_EXPORT nvmlReturn_t nvmlGetExcludedDeviceInfoByIndex(unsigned int index, nvmlExcludedDeviceInfo_t *info)
{
    NvU32 ids[NV0000_CTRL_GPU_MAX_PROBED_GPUS];
    unsigned n = 0;
    INITIALIZED();
    CHECK_PTR(info);
    nvmlReturn_t ret = excluded_ids(ids, &n);
    if (ret != NVML_SUCCESS)
        return ret;
    if (index >= n)
        return NVML_ERROR_INVALID_ARGUMENT;

    NV0000_CTRL_GPU_GET_PCI_INFO_PARAMS pci = { .gpuId = ids[index] };
    NvU32 st = nvc_rm_control(&nvml.sys, nvml.sys.client, NV0000_CTRL_CMD_GPU_GET_PCI_INFO, &pci, sizeof(pci));
    if (st != NV_OK)
        return nvml_from_rm(st);
    NV0000_CTRL_GPU_GET_UUID_FROM_GPU_ID_PARAMS uuid = {
        .gpuId = ids[index],
        .flags = NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID_FLAGS_FORMAT_ASCII,
    };
    st = nvc_rm_control(&nvml.sys, nvml.sys.client, NV0000_CTRL_CMD_GPU_GET_UUID_FROM_GPU_ID,
                        &uuid, sizeof(uuid));
    if (st != NV_OK)
        return nvml_from_rm(st);

    /* The GPU was never opened, so its PCI ids aren't known. */
    memset(info, 0, sizeof(*info));
    info->pciInfo.domain = pci.domain;
    info->pciInfo.bus = pci.bus;
    info->pciInfo.device = pci.slot;
    snprintf(info->pciInfo.busId, sizeof(info->pciInfo.busId), "%08X:%02X:%02X.0",
             pci.domain, pci.bus, pci.slot);
    snprintf(info->pciInfo.busIdLegacy, sizeof(info->pciInfo.busIdLegacy), "%04X:%02X:%02X.0",
             pci.domain, pci.bus, pci.slot);
    uuid.gpuUuid[sizeof(uuid.gpuUuid) - 1] = 0;
    snprintf(info->uuid, sizeof(info->uuid), "%s", (const char *)uuid.gpuUuid);
    return NVML_SUCCESS;
}

/* The names NVML had before "excluded". */
NVML_EXPORT nvmlReturn_t nvmlGetBlacklistDeviceCount(unsigned int *deviceCount)
{
    return nvmlGetExcludedDeviceCount(deviceCount);
}

NVML_EXPORT nvmlReturn_t nvmlGetBlacklistDeviceInfoByIndex(unsigned int index, nvmlExcludedDeviceInfo_t *info)
{
    return nvmlGetExcludedDeviceInfoByIndex(index, info);
}

/* ---- driver branch, drain state ---------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlSystemGetDriverBranch(nvmlSystemDriverBranchInfo_t *branchInfo, unsigned int length)
{
    NV0000_CTRL_SYSTEM_GET_BUILD_VERSION_V2_PARAMS p;
    INITIALIZED();
    CHECK_PTR(branchInfo);
    if (branchInfo->version != nvmlSystemDriverBranchInfo_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    memset(&p, 0, sizeof(p));
    NvU32 st = nvc_rm_control(&nvml.sys, nvml.sys.client, NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION_V2,
                              &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    p.driverBranch[sizeof(p.driverBranch) - 1] = 0;
    return nvml_copy_string(branchInfo->branch, length, p.driverBranch);
}

/* Whether the GPU at a PCI address is being drained (no new clients) for removal. */
NVML_EXPORT nvmlReturn_t nvmlDeviceQueryDrainState(nvmlPciInfo_t *pciInfo, nvmlEnableState_t *currentState)
{
    NvU32 gpu_id;
    INITIALIZED();
    if (pciInfo == NULL || currentState == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    nvmlReturn_t ret = nvml_gpu_id_by_pci(pciInfo->domain, pciInfo->bus, pciInfo->device, &gpu_id);
    if (ret != NVML_SUCCESS)
        return ret;
    NV0000_CTRL_GPU_QUERY_DRAIN_STATE_PARAMS p = { .gpuId = gpu_id };
    NvU32 st = nvc_rm_control(&nvml.sys, nvml.sys.client, NV0000_CTRL_CMD_GPU_QUERY_DRAIN_STATE, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *currentState = p.drainState == NV0000_CTRL_GPU_DRAIN_STATE_ENABLED ? NVML_FEATURE_ENABLED
                                                                        : NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

/* ---- device memory limits of a cgroup ---------------------------------------------- */

/* The GPU's value in a dmem.* file: lines of "<region> <bytes|max>". */
static bool dmem_value(int dir, const char *file, const char *region, unsigned long long *value)
{
    char line[256];
    size_t len = strlen(region);
    bool found = false;
    int fd = openat(dir, file, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    FILE *f = fdopen(fd, "r");
    if (f == NULL) {
        close(fd);
        return false;
    }
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, region, len) != 0 || line[len] != ' ')
            continue;
        const char *v = line + len + 1;
        *value = strncmp(v, "max", 3) == 0 ? NVML_DEVICE_MEMORY_LIMIT_MAX : strtoull(v, NULL, 10);
        found = true;
    }
    fclose(f);
    return found;
}

static bool same_file(int a, int b)
{
    struct stat sa, sb;
    return fstat(a, &sa) == 0 && fstat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/*
 * With the kernel's device memory cgroup controller (dmem) the limits are
 * the cgroup's dmem.min / dmem.max for the GPU's "vidmem" region; a cgroup
 * that doesn't have the controller enabled is governed by the nearest
 * ancestor that has.
 */
static nvmlReturn_t dmem_limits(struct nvmlDevice_st *dev, int cgroup, nvmlGetMemoryLimits_v1_t *limits)
{
    char region[64];
    nvmlReturn_t ret = NVML_ERROR_OPERATING_SYSTEM;
    int root = open("/sys/fs/cgroup", O_RDONLY | O_CLOEXEC);
    int dir = dup(cgroup);
    snprintf(region, sizeof(region), "nvidia/%s/vidmem", dev->bus_id);
    while (root >= 0 && dir >= 0) {
        if (faccessat(dir, "dmem.max", F_OK, AT_EACCESS) == 0) {
            unsigned long long soft = 0, hard = NVML_DEVICE_MEMORY_LIMIT_MAX, used = 0;
            dmem_value(dir, "dmem.max", region, &hard);
            dmem_value(dir, "dmem.min", region, &soft);
            dmem_value(dir, "dmem.current", region, &used);
            limits->softLimit = soft;
            limits->hardLimit = hard;
            limits->currentUsed = used;
            ret = NVML_SUCCESS;
            break;
        }
        /* The root group has no limits; above it is no cgroup at all. */
        if (same_file(dir, root)) {
            ret = NVML_ERROR_NOT_SUPPORTED;
            break;
        }
        int parent = openat(dir, "..", O_RDONLY | O_CLOEXEC);
        if (parent < 0 || same_file(parent, dir)) {
            if (parent >= 0)
                close(parent);
            break;
        }
        close(dir);
        dir = parent;
    }
    if (dir >= 0)
        close(dir);
    if (root >= 0)
        close(root);
    return ret;
}

/* Without dmem the driver accounts memory itself and RM knows the limits. */
static nvmlReturn_t rm_limits(struct nvmlDevice_st *dev, int cgroup, nvmlGetMemoryLimits_v1_t *limits)
{
    NV0000_CTRL_OS_UNIX_MEMACCT_GET_LIMITS_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    p->cgroupFd = cgroup;
    p->count = 1;
    p->limits[0].gpuId = dev->gpu_id;
    NvU32 st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_OS_UNIX_CMD_MEMACCT_GET_LIMITS,
                              p, sizeof(*p));
    if (st == NV_OK) {
        limits->softLimit = p->limits[0].soft;
        limits->hardLimit = p->limits[0].hard;
        limits->currentUsed = p->limits[0].current;
    }
    free(p);
    return nvml_from_rm(st);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMemoryLimits_v1(nvmlDevice_t device, nvmlGetMemoryLimits_v1_t *limits)
{
    NV0000_CTRL_OS_UNIX_MEMACCT_GET_IMPL_PARAMS impl = { 0 };
    DEVICE(device, dev);
    if (limits == NULL || limits->nameSpace == NULL || limits->nameSpace[0] == '\0')
        return NVML_ERROR_INVALID_ARGUMENT;
    /* The namespace is the cgroup's directory. */
    int cgroup = open(limits->nameSpace, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (cgroup < 0)
        return NVML_ERROR_OPERATING_SYSTEM;
    nvmlReturn_t ret;
    NvU32 st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_OS_UNIX_CMD_MEMACCT_GET_IMPL,
                              &impl, sizeof(impl));
    if (st != NV_OK)
        ret = nvml_from_rm(st);
    else if (impl.impl == NV0000_CTRL_CMD_OS_UNIX_MEMACCT_IMPL_OS)
        ret = dmem_limits(dev, cgroup, limits);
    else if (impl.impl == NV0000_CTRL_CMD_OS_UNIX_MEMACCT_IMPL_FALLBACK)
        ret = rm_limits(dev, cgroup, limits);
    else
        ret = NVML_ERROR_NOT_SUPPORTED;
    close(cgroup);
    return ret;
}

/* ---- accounting -------------------------------------------------------------------- */

static nvmlReturn_t accounting_info(struct nvmlDevice_st *dev, unsigned pid,
                                    NV0000_CTRL_GPUACCT_GET_PROC_ACCOUNTING_INFO_PARAMS *p)
{
    memset(p, 0, sizeof(*p));
    p->gpuId = dev->gpu_id;
    p->pid = pid;
    return nvml_from_rm(nvc_rm_control(&dev->rm, dev->rm.client,
                                       NV0000_CTRL_CMD_GPUACCT_GET_PROC_ACCOUNTING_INFO, p, sizeof(*p)));
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetAccountingStats(nvmlDevice_t device, unsigned int pid,
                                                      nvmlAccountingStats_t *stats)
{
    NV0000_CTRL_GPUACCT_GET_PROC_ACCOUNTING_INFO_PARAMS p;
    DEVICE(device, dev);
    CHECK_PTR(stats);
    nvmlReturn_t ret = accounting_info(dev, pid, &p);
    if (ret != NVML_SUCCESS)
        return ret;
    memset(stats, 0, sizeof(*stats));
    stats->gpuUtilization = p.gpuUtil;
    stats->memoryUtilization = p.fbUtil;
    stats->maxMemoryUsage = p.maxFbUsage;
    stats->startTime = p.startTime;
    /* No end time yet: the process is alive and has no run time to report. */
    stats->isRunning = p.endTime == 0;
    stats->time = p.endTime != 0 ? (p.endTime - p.startTime) / 1000 : 0;
    return NVML_SUCCESS;
}

/* stats->pid selects the process. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetAccountingStats_v2(nvmlDevice_t device, nvmlAccountingStats_v2_t *stats)
{
    NV0000_CTRL_GPUACCT_GET_PROC_ACCOUNTING_INFO_V2_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(stats);
    p.gpuId = dev->gpu_id;
    p.pid = stats->pid;
    NvU32 st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_CMD_GPUACCT_GET_PROC_ACCOUNTING_INFO_V2,
                              &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    unsigned pid = stats->pid;
    memset(stats, 0, sizeof(*stats));
    stats->pid = pid;
    stats->isRunning = p.endTime == 0;
    stats->gpuUtilization = p.gpuUtil;
    stats->memoryUtilization = p.fbUtil;
    stats->maxMemoryUsage = p.maxFbUsage;
    stats->sampleCount = p.sampleCount;
    stats->sumGpuUtil = p.sumGpuUtil;
    stats->sumFbUtil = p.sumFbUtil;
    stats->time = p.endTime != 0 ? (p.endTime - p.startTime) / 1000 : 0;
    stats->startTime = p.startTime;
    return NVML_SUCCESS;
}

/* ---- InfoROM ------------------------------------------------------------------------ */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetInforomConfigurationChecksum(nvmlDevice_t device, unsigned int *checksum)
{
    NV208F_CTRL_GPU_VERIFY_INFOROM_PARAMS p = { 0 };
    NvU32 st = NV_ERR_NOT_SUPPORTED;
    DEVICE(device, dev);
    CHECK_PTR(checksum);
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
    case NV208F_CTRL_GPU_INFOROM_VERIFICATION_RESULT_VALID:
        *checksum = p.checksum;
        return NVML_SUCCESS;
    case NV208F_CTRL_GPU_INFOROM_VERIFICATION_RESULT_INVALID:
        return NVML_ERROR_CORRUPTED_INFOROM;
    default:
        return NVML_ERROR_UNKNOWN;
    }
}
