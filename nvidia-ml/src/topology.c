/*
 * SPDX-License-Identifier: MIT
 *
 * How the GPUs of a system relate: the PCI path between two of them, the
 * GPUs near a CPU, peer-to-peer capabilities and the bridges of a
 * multi-GPU board.
 *
 * The PCI topology comes from sysfs: /sys/bus/pci/devices/<id> resolves to
 * the chain of bridges from the host bridge down to the device, e.g.
 * /sys/devices/pci0000:00/0000:00:01.0/0000:01:00.0.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvml-priv.h"

#include "nvstatus.h"
#include "ctrl/ctrl0000/ctrl0000system.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

/* ---- PCI paths ------------------------------------------------------------------ */

#define PATH_MAX_DEPTH 32

struct pci_path {
    char buf[PATH_MAX];
    unsigned depth;                 /* host bridge + bridges; the device itself excluded */
    const char *node[PATH_MAX_DEPTH];
};

/* Splits the device's sysfs path into host bridge and bridges. */
static bool pci_path(struct nvmlDevice_st *dev, struct pci_path *p)
{
    char link[96], real[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/bus/pci/devices/%04x:%02x:%02x.0",
             dev->pci.domain, dev->pci.bus, dev->pci.device);
    if (realpath(link, real) == NULL)
        return false;
    const char *s = strstr(real, "/pci");
    if (s == NULL)
        return false;
    snprintf(p->buf, sizeof(p->buf), "%s", s + 1);
    p->depth = 0;
    for (char *tok = strtok(p->buf, "/"); tok != NULL; tok = strtok(NULL, "/")) {
        if (p->depth == PATH_MAX_DEPTH)
            return false;
        p->node[p->depth++] = tok;
    }
    if (p->depth < 2)
        return false;
    p->depth--;                     /* the last component is the device */
    return true;
}

static int read_int(const char *fmt, struct nvmlDevice_st *dev)
{
    char path[128], buf[32];
    snprintf(path, sizeof(path), fmt, dev->pci.domain, dev->pci.bus, dev->pci.device);
    FILE *f = fopen(path, "re");
    if (f == NULL)
        return -1;
    int v = fgets(buf, sizeof(buf), f) != NULL ? atoi(buf) : -1;
    fclose(f);
    return v;
}

/* More than one NUMA node: only then is "same node" narrower than "same system". */
static bool multiple_numa_nodes(void)
{
    FILE *f = fopen("/sys/devices/system/node/node1/cpulist", "re");
    if (f == NULL)
        return false;
    fclose(f);
    return true;
}

static nvmlReturn_t common_ancestor(struct nvmlDevice_st *a, struct nvmlDevice_st *b,
                                    nvmlGpuTopologyLevel_t *level)
{
    struct pci_path pa, pb;
    if (a == b || (a->pci.domain == b->pci.domain && a->pci.bus == b->pci.bus &&
                   a->pci.device == b->pci.device))
        return NVML_ERROR_NOT_SUPPORTED;
    if (!pci_path(a, &pa) || !pci_path(b, &pb))
        return NVML_ERROR_UNKNOWN;

    unsigned common = 0;
    while (common < pa.depth && common < pb.depth && strcmp(pa.node[common], pb.node[common]) == 0)
        common++;

    if (common == 0) {
        /* Different host bridges: the same NUMA node at best. */
        int na = read_int("/sys/bus/pci/devices/%04x:%02x:%02x.0/numa_node", a);
        int nb = read_int("/sys/bus/pci/devices/%04x:%02x:%02x.0/numa_node", b);
        *level = na >= 0 && na == nb && multiple_numa_nodes() ? NVML_TOPOLOGY_NODE : NVML_TOPOLOGY_SYSTEM;
        return NVML_SUCCESS;
    }
    if (common == 1) {
        *level = NVML_TOPOLOGY_HOSTBRIDGE;
        return NVML_SUCCESS;
    }
    /*
     * They share a PCI bridge. One switch lies between them if each GPU's
     * own bridge is that bridge or sits directly below it (a switch is an
     * upstream port with downstream ports under it); otherwise several.
     */
    if (pa.depth - common > 1 || pb.depth - common > 1) {
        *level = NVML_TOPOLOGY_MULTIPLE;
        return NVML_SUCCESS;
    }
    unsigned multi = 0;
    nvmlReturn_t ret = nvmlDeviceGetMultiGpuBoard((nvmlDevice_t)a, &multi);
    if (ret != NVML_SUCCESS)
        return ret;
    *level = multi ? NVML_TOPOLOGY_INTERNAL : NVML_TOPOLOGY_SINGLE;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetTopologyCommonAncestor(nvmlDevice_t device1, nvmlDevice_t device2,
                                                             nvmlGpuTopologyLevel_t *pathInfo)
{
    struct nvmlDevice_st *a, *b;
    if (nvml_device(device1, &a) != NVML_SUCCESS || nvml_device(device2, &b) != NVML_SUCCESS ||
        pathInfo == NULL) {
        nvmlReturn_t init = nvml_initialized();
        return init != NVML_SUCCESS ? init : NVML_ERROR_INVALID_ARGUMENT;
    }
    return common_ancestor(a, b, pathInfo);
}

/* Whether a GPU related to another at `path` belongs to the set asked for with `level`. */
static bool in_level(nvmlGpuTopologyLevel_t level, nvmlGpuTopologyLevel_t path)
{
    switch (path) {
    case NVML_TOPOLOGY_INTERNAL:
    case NVML_TOPOLOGY_SINGLE:
    case NVML_TOPOLOGY_MULTIPLE:
    case NVML_TOPOLOGY_HOSTBRIDGE:
        return level >= path;
    case NVML_TOPOLOGY_NODE:
        /* common_ancestor() only reports NODE for GPUs of one node. */
        return level == NVML_TOPOLOGY_SYSTEM || level == NVML_TOPOLOGY_NODE;
    default:
        return true;
    }
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetTopologyNearestGpus(nvmlDevice_t device, nvmlGpuTopologyLevel_t level,
                                                          unsigned int *count, nvmlDevice_t *deviceArray)
{
    DEVICE(device, dev);
    CHECK_PTR(count);
    if ((*count != 0 && deviceArray == NULL) || (*count == 0 && deviceArray != NULL))
        return NVML_ERROR_INVALID_ARGUMENT;

    unsigned n = 0, total = 0;
    nvmlReturn_t ret = nvmlDeviceGetCount_v2(&total);
    if (ret != NVML_SUCCESS)
        return ret;
    for (unsigned i = 0; i < total; i++) {
        nvmlDevice_t other;
        nvmlGpuTopologyLevel_t path;
        if (i == dev->index)
            continue;
        ret = nvmlDeviceGetHandleByIndex_v2(i, &other);
        if (ret != NVML_SUCCESS)
            return ret;
        ret = common_ancestor(dev, (struct nvmlDevice_st *)other, &path);
        if (ret != NVML_SUCCESS)
            return ret;
        if (!in_level(level, path))
            continue;
        if (n < *count)
            deviceArray[n] = other;
        n++;
    }
    *count = n;
    return NVML_SUCCESS;
}

/* Whether the list ("0-3,8") names the CPU. */
static bool cpulist_has(const char *s, unsigned cpu)
{
    while (*s) {
        char *e;
        unsigned long a = strtoul(s, &e, 10), b = a;
        if (e == s)
            break;
        if (*e == '-')
            b = strtoul(e + 1, &e, 10);
        if (cpu >= a && cpu <= b)
            return true;
        s = *e == ',' ? e + 1 : e;
    }
    return false;
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetTopologyGpuSet(unsigned int cpuNumber, unsigned int *count,
                                                     nvmlDevice_t *deviceArray)
{
    nvmlReturn_t ret = nvml_initialized();
    if (ret != NVML_SUCCESS)
        return ret;
    CHECK_PTR(count);
    if ((*count != 0 && deviceArray == NULL) || (*count == 0 && deviceArray != NULL))
        return NVML_ERROR_INVALID_ARGUMENT;

    unsigned n = 0, total = 0;
    ret = nvmlDeviceGetCount_v2(&total);
    if (ret != NVML_SUCCESS)
        return ret;
    for (unsigned i = 0; i < total; i++) {
        nvmlDevice_t h;
        char path[96], buf[4096];
        /* GPUs this process may not open aren't part of any set. */
        if (nvmlDeviceGetHandleByIndex_v2(i, &h) != NVML_SUCCESS)
            continue;
        struct nvmlDevice_st *dev = (struct nvmlDevice_st *)h;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%04x:%02x:%02x.0/local_cpulist",
                 dev->pci.domain, dev->pci.bus, dev->pci.device);
        FILE *f = fopen(path, "re");
        if (f == NULL)
            continue;
        bool local = fgets(buf, sizeof(buf), f) != NULL && cpulist_has(buf, cpuNumber);
        fclose(f);
        if (!local)
            continue;
        if (*count != 0 && n < *count)
            deviceArray[n] = h;
        n++;
    }
    /* With an array the count is left alone: unused entries stay untouched. */
    if (*count == 0)
        *count = n;
    return NVML_SUCCESS;
}

/* ---- boards --------------------------------------------------------------------- */

/* GPUs of one board share its serial number. */
NVML_EXPORT nvmlReturn_t nvmlDeviceOnSameBoard(nvmlDevice_t device1, nvmlDevice_t device2, int *onSameBoard)
{
    char s1[NVML_DEVICE_SERIAL_BUFFER_SIZE], s2[NVML_DEVICE_SERIAL_BUFFER_SIZE];
    nvmlReturn_t ret = nvml_initialized();
    if (ret != NVML_SUCCESS)
        return ret;
    CHECK_PTR(onSameBoard);
    ret = nvmlDeviceGetSerial(device1, s1, sizeof(s1));
    if (ret == NVML_SUCCESS)
        ret = nvmlDeviceGetSerial(device2, s2, sizeof(s2));
    if (ret != NVML_SUCCESS)
        return ret == NVML_ERROR_GPU_IS_LOST ? ret : NVML_ERROR_NOT_SUPPORTED;
    *onSameBoard = strcmp(s1, s2) == 0;
    return NVML_SUCCESS;
}

/* NV2080_CTRL_GPU_BRIDGE_VERSION_PARAMS.hwbcResourceType */
enum { HWBC_NVIDIA_BR04 = 5, HWBC_PLX_PEX8747 = 8 };

static int bridge_cmp(const void *a, const void *b)
{
    const NV2080_CTRL_GPU_BRIDGE_VERSION_PARAMS *x = a, *y = b;
    return (int)y->bus - (int)x->bus;      /* nearest to the GPU first */
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetBridgeChipInfo(nvmlDevice_t device, nvmlBridgeChipHierarchy_t *bridgeHierarchy)
{
    NV2080_CTRL_GPU_GET_ALL_BRIDGES_UPSTREAM_OF_GPU_PARAMS p = { 0 };
    nvmlBridgeChipHierarchy_t out = { 0 };
    unsigned multi = 0;
    DEVICE(device, dev);
    CHECK_PTR(bridgeHierarchy);
    /* Only boards with several GPUs have bridge chips of their own. */
    nvmlReturn_t ret = nvmlDeviceGetMultiGpuBoard(device, &multi);
    if (ret != NVML_SUCCESS)
        return ret == NVML_ERROR_GPU_IS_LOST ? ret : NVML_ERROR_UNKNOWN;
    if (!multi)
        return NVML_ERROR_NOT_SUPPORTED;
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_ALL_BRIDGES_UPSTREAM_OF_GPU, &p, sizeof(p));
    if (st != NV_OK)
        return st == NV_ERR_NOT_SUPPORTED || st == NV_ERR_GPU_IS_LOST ? nvml_from_rm(st) : NVML_ERROR_UNKNOWN;
    unsigned n = p.bridgeCount;
    if (n == 0)
        return NVML_ERROR_NOT_SUPPORTED;
    if (n > NV2080_CTRL_MAX_PHYSICAL_BRIDGE)
        n = NV2080_CTRL_MAX_PHYSICAL_BRIDGE;
    qsort(p.bridgeList, n, sizeof(p.bridgeList[0]), bridge_cmp);
    for (unsigned i = 0; i < n; i++) {
        switch (p.bridgeList[i].hwbcResourceType) {
        case HWBC_PLX_PEX8747: out.bridgeChipInfo[i].type = NVML_BRIDGE_CHIP_PLX; break;
        case HWBC_NVIDIA_BR04: out.bridgeChipInfo[i].type = NVML_BRIDGE_CHIP_BRO4; break;
        default:               return NVML_ERROR_NOT_SUPPORTED;   /* chipset bridges */
        }
        out.bridgeChipInfo[i].fwVersion = p.bridgeList[i].fwVersion;
    }
    out.bridgeCount = (unsigned char)n;
    bridgeHierarchy->bridgeCount = out.bridgeCount;
    memcpy(bridgeHierarchy->bridgeChipInfo, out.bridgeChipInfo, n * sizeof(out.bridgeChipInfo[0]));
    return NVML_SUCCESS;
}

/* Host interface cards belonged to the QuadroPlex units; there are none. */
NVML_EXPORT nvmlReturn_t nvmlSystemGetHicVersion(unsigned int *hwbcCount, nvmlHwbcEntry_t *hwbcEntries)
{
    (void)hwbcEntries;
    nvmlReturn_t ret = nvml_initialized();
    if (ret != NVML_SUCCESS)
        return ret;
    CHECK_PTR(hwbcCount);
    *hwbcCount = 0;
    return NVML_SUCCESS;
}

/* ---- peer to peer ---------------------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetP2PStatus(nvmlDevice_t device1, nvmlDevice_t device2,
                                                nvmlGpuP2PCapsIndex_t p2pIndex, nvmlGpuP2PStatus_t *p2pStatus)
{
    struct nvmlDevice_st *a, *b;
    if (nvml_device(device1, &a) != NVML_SUCCESS || nvml_device(device2, &b) != NVML_SUCCESS ||
        p2pStatus == NULL) {
        nvmlReturn_t init = nvml_initialized();
        return init != NVML_SUCCESS ? init : NVML_ERROR_INVALID_ARGUMENT;
    }
    NV0000_CTRL_SYSTEM_GET_P2P_CAPS_V2_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    p->gpuCount = 2;
    p->gpuIds[0] = a->gpu_id;
    p->gpuIds[1] = b->gpu_id;
    NvU32 st = nvc_rm_control(&a->rm, a->rm.client, NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS_V2, p, sizeof(*p));
    if (st != NV_OK) {
        free(p);
        return nvml_from_rm(st);
    }
    /* The first NVML indexes are RM's; the status codes are the same too.
     * Anything past them reads as OK, like the NVIDIA library does. */
    unsigned status = NVML_P2P_STATUS_OK;
    if ((unsigned)p2pIndex < NVML_P2P_CAPS_INDEX_UNKNOWN) {
        status = p->p2pCapsStatus[p2pIndex];
        if (status > NVML_P2P_STATUS_UNKNOWN)
            status = NVML_P2P_STATUS_UNKNOWN;
    }
    free(p);
    *p2pStatus = (nvmlGpuP2PStatus_t)status;
    return NVML_SUCCESS;
}
