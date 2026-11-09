/*
 * SPDX-License-Identifier: MIT
 *
 * Processes using the GPU: RM lists the processes holding objects of a
 * class (NV2080_CTRL_CMD_GPU_GET_PIDS), so graphics processes are those
 * with a 3D engine object and compute processes those with a compute
 * object, and reports per-process video memory (GET_PID_INFO).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvml-priv.h"

#include "nvstatus.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"

enum proc_kind { PROC_GRAPHICS, PROC_COMPUTE, PROC_MPS_COMPUTE };

/* What the lists report per process. */
struct proc {
    unsigned pid;
    unsigned long long used;    /* bytes of video memory */
};

/* The newest 3D (..97) and compute (..c0) classes the GPU has; clients
 * allocate those. */
static void find_classes(struct nvmlDevice_st *dev)
{
    NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS cl = { 0 };

    if (dev->classes_known)
        return;
    dev->classes_known = true;
    if (nvml_ctrl_dev(dev, NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &cl, sizeof(cl)) != NV_OK)
        return;
    for (unsigned i = 0; i < cl.numClasses; i++) {
        NvU32 c = cl.classList[i];
        if ((c & 0xff) == 0x97 && c > dev->gfx_class && c < 0x10000)
            dev->gfx_class = c;
        if ((c & 0xff) == 0xc0 && c > dev->compute_class && c < 0x10000)
            dev->compute_class = c;
    }
}

/* Fills up to max entries, returns the number of processes or -1. */
static int list_procs(struct nvmlDevice_st *dev, enum proc_kind kind, struct proc *out, unsigned max)
{
    NV2080_CTRL_GPU_GET_PIDS_PARAMS pids;
    NV2080_CTRL_GPU_GET_PID_INFO_PARAMS *info;
    NvU32 cls;

    if (kind == PROC_MPS_COMPUTE)
        return 0;   /* no MPS server support */

    pthread_mutex_lock(&dev->lock);
    find_classes(dev);
    cls = kind == PROC_GRAPHICS ? dev->gfx_class : dev->compute_class;
    if (cls == 0) {
        pthread_mutex_unlock(&dev->lock);
        return 0;
    }

    memset(&pids, 0, sizeof(pids));
    pids.idType = NV2080_CTRL_GPU_GET_PIDS_ID_TYPE_CLASS;
    pids.id = cls;
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_PIDS, &pids, sizeof(pids)) != NV_OK) {
        pthread_mutex_unlock(&dev->lock);
        return -1;
    }

    unsigned n = pids.pidTblCount;
    if (n > NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT)
        n = NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT;
    if ((info = calloc(1, sizeof(*info))) == NULL) {
        pthread_mutex_unlock(&dev->lock);
        return -1;
    }
    info->pidInfoListCount = n;
    for (unsigned i = 0; i < n; i++) {
        info->pidInfoList[i].pid = pids.pidTbl[i];
        info->pidInfoList[i].index = NV2080_CTRL_GPU_PID_INFO_INDEX_VIDEO_MEMORY_USAGE;
    }
    if (n && nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_PID_INFO, info, sizeof(*info)) != NV_OK)
        n = 0;

    for (unsigned i = 0; i < n && i < max; i++) {
        const NV2080_CTRL_GPU_PID_INFO_VIDEO_MEMORY_USAGE_DATA *m =
            &info->pidInfoList[i].data.vidMemUsage;
        out[i].pid = info->pidInfoList[i].pid;
        out[i].used = info->pidInfoList[i].result == NV_OK
                      ? m->memPrivate + m->memSharedOwned : 0;
    }
    free(info);
    pthread_mutex_unlock(&dev->lock);
    return (int)n;
}

/* Common count/capacity handling of the NVML process list calls. */
#define LIST_BEGIN(device, count, infos, kind)                              \
    struct nvmlDevice_st *dev;                                              \
    struct proc procs[NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT];              \
    nvmlReturn_t r_ = nvml_device((device), &dev);                          \
    if (r_ != NVML_SUCCESS)                                                 \
        return r_;                                                          \
    if ((count) == NULL || (*(count) && (infos) == NULL))                   \
        return NVML_ERROR_INVALID_ARGUMENT;                                 \
    int n_ = list_procs(dev, (kind), procs, NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT); \
    if (n_ < 0)                                                             \
        return NVML_ERROR_UNKNOWN;                                          \
    if ((unsigned)n_ > *(count)) {                                          \
        *(count) = (unsigned)n_;                                            \
        return NVML_ERROR_INSUFFICIENT_SIZE;                                \
    }                                                                       \
    *(count) = (unsigned)n_;

static nvmlReturn_t list_v2(nvmlDevice_t device, unsigned *count, nvmlProcessInfo_v2_t *infos,
                            enum proc_kind kind)
{
    LIST_BEGIN(device, count, infos, kind);
    for (int i = 0; i < n_; i++)
        infos[i] = (nvmlProcessInfo_v2_t){
            .pid = procs[i].pid, .usedGpuMemory = procs[i].used,
            .gpuInstanceId = 0xFFFFFFFF, .computeInstanceId = 0xFFFFFFFF,
        };
    return NVML_SUCCESS;
}

static nvmlReturn_t list_v1(nvmlDevice_t device, unsigned *count, nvmlProcessInfo_v1_t *infos,
                            enum proc_kind kind)
{
    LIST_BEGIN(device, count, infos, kind);
    for (int i = 0; i < n_; i++)
        infos[i] = (nvmlProcessInfo_v1_t){ .pid = procs[i].pid, .usedGpuMemory = procs[i].used };
    return NVML_SUCCESS;
}

#define PROC_FNS(Name, kind)                                                                     \
    NVML_EXPORT nvmlReturn_t nvmlDeviceGet##Name##RunningProcesses_v3(                           \
        nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_t *infos)                  \
    { return list_v2(device, infoCount, (nvmlProcessInfo_v2_t *)infos, kind); }                  \
    NVML_EXPORT nvmlReturn_t nvmlDeviceGet##Name##RunningProcesses_v2(                           \
        nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v2_t *infos)               \
    { return list_v2(device, infoCount, infos, kind); }                                          \
    NVML_EXPORT nvmlReturn_t nvmlDeviceGet##Name##RunningProcesses(                              \
        nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v1_t *infos)               \
    { return list_v1(device, infoCount, infos, kind); }

PROC_FNS(Graphics, PROC_GRAPHICS)
PROC_FNS(Compute, PROC_COMPUTE)
PROC_FNS(MPSCompute, PROC_MPS_COMPUTE)

/*
 * Export table slots 213 (compute), 214 (MPS compute), 215 (graphics):
 * the same lists, each entry an nvmlProcessDetail_v1_t followed by the
 * process command line (/proc/pid/cmdline: NUL-separated, zero-padded).
 * nvidia-smi passes two more pointers that may be NULL; their purpose is
 * unknown and they are ignored.
 */
#define NVML_ETBL_PROC_NAME_SIZE 0x1000   /* entry stride 0x1020 */

struct etbl_proc {
    nvmlProcessDetail_v1_t detail;
    char cmdline[NVML_ETBL_PROC_NAME_SIZE];
};

static void read_cmdline(unsigned pid, char *buf, size_t size)
{
    char path[64];
    ssize_t n = 0;
    int fd;

    memset(buf, 0, size);
    snprintf(path, sizeof(path), "/proc/%u/cmdline", pid);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
        n = read(fd, buf, size - 1);
        close(fd);
    }
    if (n <= 0) {
        /* Kernel threads and unreadable processes: fall back to comm. */
        snprintf(path, sizeof(path), "/proc/%u/comm", pid);
        if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
            n = read(fd, buf, size - 1);
            close(fd);
            if (n > 0 && buf[n - 1] == '\n')
                buf[n - 1] = 0;
        }
    }
}

static nvmlReturn_t etbl_list(nvmlDevice_t device, unsigned *count, struct etbl_proc *infos,
                              enum proc_kind kind)
{
    LIST_BEGIN(device, count, infos, kind);
    for (int i = 0; i < n_; i++) {
        infos[i].detail = (nvmlProcessDetail_v1_t){
            .pid = procs[i].pid, .usedGpuMemory = procs[i].used,
            .gpuInstanceId = 0xFFFFFFFF, .computeInstanceId = 0xFFFFFFFF,
        };
        read_cmdline(procs[i].pid, infos[i].cmdline, sizeof(infos[i].cmdline));
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvml_etbl_compute_procs(nvmlDevice_t device, unsigned *count, void *infos,
                                     void *unused1, void *unused2)
{
    return etbl_list(device, count, infos, PROC_COMPUTE);
}

nvmlReturn_t nvml_etbl_mps_procs(nvmlDevice_t device, unsigned *count, void *infos,
                                 void *unused1, void *unused2)
{
    return etbl_list(device, count, infos, PROC_MPS_COMPUTE);
}

nvmlReturn_t nvml_etbl_graphics_procs(nvmlDevice_t device, unsigned *count, void *infos,
                                      void *unused1, void *unused2)
{
    return etbl_list(device, count, infos, PROC_GRAPHICS);
}
