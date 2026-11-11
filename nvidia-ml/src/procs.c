/*
 * SPDX-License-Identifier: MIT
 *
 * Processes using the GPU: RM lists the processes holding objects of a
 * class (NV2080_CTRL_CMD_GPU_GET_PIDS), so graphics processes are those
 * with a 3D engine object and compute processes those with a compute
 * object, and reports per-process video memory (GET_PID_INFO).
 */

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvml-priv.h"

#include "nvstatus.h"
#include "class/cl2080.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"

enum proc_kind { PROC_GRAPHICS, PROC_COMPUTE, PROC_MPS_COMPUTE, PROC_ALL };

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
    /* Every client of the GPU holds a subdevice, so its class lists them all. */
    cls = kind == PROC_ALL ? NV20_SUBDEVICE_0
        : kind == PROC_GRAPHICS ? dev->gfx_class : dev->compute_class;
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
 * Closed order: NULL list, version, mode. A too small array (or a NULL one
 * with a non-zero capacity) is INSUFFICIENT_SIZE; the count is only updated
 * when the array was given.
 */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetRunningProcessDetailList(nvmlDevice_t device,
                                                               nvmlProcessDetailList_t *plist)
{
    static const enum proc_kind kinds[NVML_PROCESS_MODE_MAX] = {
        [NVML_PROCESS_MODE_COMPUTE] = PROC_COMPUTE,
        [NVML_PROCESS_MODE_GRAPHICS] = PROC_GRAPHICS,
        [NVML_PROCESS_MODE_MPS] = PROC_MPS_COMPUTE,
        [NVML_PROCESS_MODE_ALL] = PROC_ALL,
    };
    struct proc procs[NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT];
    struct nvmlDevice_st *dev;
    nvmlReturn_t ret = nvml_device(device, &dev);

    if (ret != NVML_SUCCESS)
        return ret;
    if (plist == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (plist->version != nvmlProcessDetailList_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    if (plist->mode >= NVML_PROCESS_MODE_MAX)
        return NVML_ERROR_INVALID_ARGUMENT;
    int n = list_procs(dev, kinds[plist->mode], procs, NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT);
    if (n < 0)
        return NVML_ERROR_UNKNOWN;
    if (plist->procArray == NULL && plist->numProcArrayEntries != 0)
        return NVML_ERROR_INSUFFICIENT_SIZE;
    if ((unsigned)n > plist->numProcArrayEntries) {
        plist->numProcArrayEntries = (unsigned)n;
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    plist->numProcArrayEntries = (unsigned)n;
    for (int i = 0; i < n; i++)
        plist->procArray[i] = (nvmlProcessDetail_v1_t){
            .pid = procs[i].pid, .usedGpuMemory = procs[i].used,
            .gpuInstanceId = 0xFFFFFFFF, .computeInstanceId = 0xFFFFFFFF,
        };
    return NVML_SUCCESS;
}

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

/*
 * Export table slot 145 (nvidia-smi pmon): per-process engine utilization
 * samples newer than lastSeen, from RM's GPU monitoring ring
 * (PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2, 72 samples, ~10 s). Each
 * sample is converted to a CPU timestamp in microseconds (OS clock) and
 * whole percent; per engine: util, vGPU scale, pid, sub-process pid, name.
 */
struct etbl_eng_sample {
    unsigned util;          /* % */
    unsigned vgpu_scale;
    unsigned pid;
    unsigned sub_pid;
    char sub_name[64];
};

struct etbl_util_sample {
    unsigned long long timestamp;   /* us, CPU (OS) time */
    /* nvidia-smi pmon's column order (sm, mem, ...), not RM's (fb first) */
    struct etbl_eng_sample gr, fb, nvenc, nvdec, nvjpg, nvofa;
};

#include "ctrl/ctrl2080/ctrl2080gpumon.h"
#include "ctrl/ctrl2080/ctrl2080perf.h"
#include "ctrl/ctrl2080/ctrl2080tmr.h"

static void convert_engine(struct etbl_eng_sample *o, const NV2080_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE *s)
{
    o->util = s->util / 100;
    o->vgpu_scale = 0;   /* the closed library reports 0 */
    o->pid = s->procId;
    o->sub_pid = s->subProcessID;
    snprintf(o->sub_name, sizeof(o->sub_name), "%.*s", (int)sizeof(s->subProcessName), s->subProcessName);
}

#define GPUMON_SAMPLES NV2080_CTRL_PERF_GPUMON_SAMPLE_COUNT_PERFMON_UTIL

/*
 * The samples newer than lastSeen, oldest first, with their CPU timestamps
 * (us). With procs, only those attributed to a process; the closed library
 * skips the rest too: samples taken while nobody polled per-process data
 * name no process. Returns NOT_FOUND if there are none; free *params
 * afterwards.
 */
static nvmlReturn_t gpumon_samples(struct nvmlDevice_st *dev, unsigned long long lastSeen, bool procs,
                                   NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS **params,
                                   const NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE *samples[],
                                   unsigned long long us[], unsigned *count)
{
    NV2080_CTRL_TIMER_GET_GPU_CPU_TIME_CORRELATION_INFO_PARAMS tc = {
        .cpuClkId = NV2080_TIMER_GPU_CPU_TIME_CPU_CLK_ID_OSTIME,   /* us since the epoch */
        .sampleCount = 1,
    };
    NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    p->type = NV2080_CTRL_GPUMON_SAMPLE_TYPE_PERFMON_UTIL;
    p->bufSize = sizeof(p->samples);
    NvU32 st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_TIMER_GET_GPU_CPU_TIME_CORRELATION_INFO, &tc, sizeof(tc));
    if (st == NV_OK)
        st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2, p, sizeof(*p));
    if (st != NV_OK) {
        free(p);
        return nvml_from_rm(st);
    }

    /* The ring starts at the tracker (a byte offset into samples[]) once
     * it has wrapped. */
    const unsigned first = p->tracker / sizeof(p->samples[0]);
    unsigned got = 0;
    for (unsigned k = 0; k < GPUMON_SAMPLES; k++) {
        const NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE *s = &p->samples[(first + k) % GPUMON_SAMPLES];
        if (s->base.timeStamp == 0 ||
            (procs && s->gr.procId == NV2080_GPUMON_PID_INVALID &&
             s->fb.procId == NV2080_GPUMON_PID_INVALID))
            continue;
        unsigned long long t = tc.samples[0].cpuTime +
            (long long)(s->base.timeStamp - tc.samples[0].gpuTime) / 1000;
        if (t <= lastSeen)
            continue;
        samples[got] = s;
        us[got] = t;
        got++;
    }
    if (got == 0) {
        free(p);
        return NVML_ERROR_NOT_FOUND;
    }
    *params = p;
    *count = got;
    return NVML_SUCCESS;
}

nvmlReturn_t nvml_etbl_process_samples(nvmlDevice_t device, unsigned long long lastSeen,
                                       nvmlValueType_t *type, unsigned *count, void *out)
{
    struct nvmlDevice_st *dev;
    nvmlReturn_t ret = nvml_device(device, &dev);
    if (ret != NVML_SUCCESS)
        return ret;
    if (type == NULL || count == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    *type = NVML_VALUE_TYPE_UNSIGNED_INT;
    /* GSP-RM only attributes samples to processes while per-process
     * utilization is polled; ask for it from the first query on. */
    nvml_rusd_request(dev, NV00DE_RUSD_POLL_PROC_UTIL);
    if (out == NULL) {
        *count = GPUMON_SAMPLES;
        return NVML_SUCCESS;
    }

    NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS *p;
    const NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE *s[GPUMON_SAMPLES];
    unsigned long long us[GPUMON_SAMPLES];
    unsigned n;
    ret = gpumon_samples(dev, lastSeen, true, &p, s, us, &n);
    if (ret != NVML_SUCCESS) {
        *count = 0;
        return ret;
    }
    struct etbl_util_sample *o = out;
    if (n > *count)
        n = *count;
    for (unsigned i = 0; i < n; i++) {
        o[i].timestamp = us[i];
        convert_engine(&o[i].gr, &s[i]->gr);
        convert_engine(&o[i].fb, &s[i]->fb);
        convert_engine(&o[i].nvenc, &s[i]->nvenc);
        convert_engine(&o[i].nvdec, &s[i]->nvdec);
        convert_engine(&o[i].nvjpg, &s[i]->nvjpg);
        convert_engine(&o[i].nvofa, &s[i]->nvofa);
    }
    free(p);
    *count = n;
    return NVML_SUCCESS;
}

/*
 * Per-process utilization (public API): the samples above, folded per
 * process. Each engine of a sample is credited to the process it names; a
 * process's utilization is the sum of its credited samples over the number
 * of samples, and its timestamp that of its newest credited sample. This
 * follows the closed library to within a point or so, but not exactly:
 * its arithmetic could not be pinned down from its inputs and outputs.
 */
struct proc_util {
    unsigned pid;
    unsigned long long timestamp;
    unsigned long long eng[6];  /* sm, mem, enc, dec, jpg, ofa; pct*100 sums */
};

static nvmlReturn_t fold_process_samples(struct nvmlDevice_st *dev, unsigned long long lastSeen,
                                         struct proc_util *out, unsigned *count, unsigned *nsamples)
{
    NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS *p;
    const NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE *s[GPUMON_SAMPLES];
    unsigned long long us[GPUMON_SAMPLES];
    unsigned n, procs = 0;

    nvml_rusd_request(dev, NV00DE_RUSD_POLL_PROC_UTIL);
    nvmlReturn_t ret = gpumon_samples(dev, lastSeen, true, &p, s, us, &n);
    if (ret != NVML_SUCCESS)
        return ret;
    for (unsigned i = 0; i < n; i++) {
        const NV2080_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE *e[6] = {
            &s[i]->gr, &s[i]->fb, &s[i]->nvenc, &s[i]->nvdec, &s[i]->nvjpg, &s[i]->nvofa,
        };
        for (unsigned k = 0; k < 6; k++) {
            unsigned pid = e[k]->procId, j;
            if (pid == 0 || pid == NV2080_GPUMON_PID_INVALID)
                continue;
            for (j = 0; j < procs && out[j].pid != pid; j++)
                ;
            if (j == procs) {
                if (procs == GPUMON_SAMPLES)
                    continue;
                memset(&out[j], 0, sizeof(out[j]));
                out[j].pid = pid;
                procs++;
            }
            out[j].eng[k] += e[k]->util;
            if (us[i] > out[j].timestamp)
                out[j].timestamp = us[i];
        }
    }
    free(p);
    *count = procs;
    *nsamples = n;
    return NVML_SUCCESS;
}

/* pct*100 sum over n samples -> whole percent */
static unsigned avg_pct(unsigned long long sum, unsigned n)
{
    return (unsigned)(sum / 100 / n);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetProcessUtilization(nvmlDevice_t device,
                                                         nvmlProcessUtilizationSample_t *utilization,
                                                         unsigned int *processSamplesCount,
                                                         unsigned long long lastSeenTimeStamp)
{
    struct proc_util p[GPUMON_SAMPLES];
    unsigned n, ns;
    struct nvmlDevice_st *dev;
    nvmlReturn_t ret = nvml_device(device, &dev);
    if (ret != NVML_SUCCESS)
        return ret;
    if (processSamplesCount == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (utilization == NULL || *processSamplesCount < GPUMON_SAMPLES) {
        *processSamplesCount = GPUMON_SAMPLES;
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    ret = fold_process_samples(dev, lastSeenTimeStamp, p, &n, &ns);
    if (ret != NVML_SUCCESS)
        return ret;
    for (unsigned i = 0; i < n; i++) {
        utilization[i] = (nvmlProcessUtilizationSample_t){
            .pid = p[i].pid,
            .timeStamp = p[i].timestamp,
            .smUtil = avg_pct(p[i].eng[0], ns),
            .memUtil = avg_pct(p[i].eng[1], ns),
            .encUtil = avg_pct(p[i].eng[2], ns),
            .decUtil = avg_pct(p[i].eng[3], ns),
        };
    }
    *processSamplesCount = n;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetProcessesUtilizationInfo(nvmlDevice_t device,
                                                               nvmlProcessesUtilizationInfo_t *procesesUtilInfo)
{
    struct proc_util p[GPUMON_SAMPLES];
    nvmlProcessesUtilizationInfo_t *info = procesesUtilInfo;
    unsigned n, ns;
    struct nvmlDevice_st *dev;
    nvmlReturn_t ret = nvml_device(device, &dev);
    if (ret != NVML_SUCCESS)
        return ret;
    if (info == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (info->version != nvmlProcessesUtilizationInfo_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    if (info->procUtilArray == NULL || info->processSamplesCount < GPUMON_SAMPLES) {
        info->processSamplesCount = GPUMON_SAMPLES;
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    ret = fold_process_samples(dev, info->lastSeenTimeStamp, p, &n, &ns);
    if (ret != NVML_SUCCESS)
        return ret;
    for (unsigned i = 0; i < n; i++) {
        info->procUtilArray[i] = (nvmlProcessUtilizationInfo_v1_t){
            .pid = p[i].pid,
            .timeStamp = p[i].timestamp,
            .smUtil = avg_pct(p[i].eng[0], ns),
            .memUtil = avg_pct(p[i].eng[1], ns),
            .encUtil = avg_pct(p[i].eng[2], ns),
            .decUtil = avg_pct(p[i].eng[3], ns),
            .jpgUtil = avg_pct(p[i].eng[4], ns),
            .ofaUtil = avg_pct(p[i].eng[5], ns),
        };
    }
    info->processSamplesCount = n;
    return NVML_SUCCESS;
}

/*
 * Device utilization samples (nvmlDeviceGetSamples) from the same ring,
 * whole percent per sample. The power and clock sample types read
 * buffers that GSP-RM leaves empty here (the closed library always gets
 * NOT_FOUND for them), so their layouts are unknown: not supported.
 */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetSamples(nvmlDevice_t device, nvmlSamplingType_t type,
                                              unsigned long long lastSeenTimeStamp,
                                              nvmlValueType_t *sampleValType, unsigned int *sampleCount,
                                              nvmlSample_t *samples)
{
    struct nvmlDevice_st *dev;
    nvmlReturn_t ret = nvml_device(device, &dev);
    if (ret != NVML_SUCCESS)
        return ret;
    if (sampleValType == NULL || sampleCount == NULL || type >= NVML_SAMPLINGTYPE_COUNT)
        return NVML_ERROR_INVALID_ARGUMENT;

    size_t off;
    switch (type) {
    case NVML_GPU_UTILIZATION_SAMPLES: off = offsetof(NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE, gr); break;
    case NVML_MEMORY_UTILIZATION_SAMPLES: off = offsetof(NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE, fb); break;
    case NVML_ENC_UTILIZATION_SAMPLES: off = offsetof(NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE, nvenc); break;
    case NVML_DEC_UTILIZATION_SAMPLES: off = offsetof(NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE, nvdec); break;
    case NVML_JPG_UTILIZATION_SAMPLES: off = offsetof(NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE, nvjpg); break;
    case NVML_OFA_UTILIZATION_SAMPLES: off = offsetof(NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE, nvofa); break;
    case NVML_MODULE_POWER_SAMPLES:
        return NVML_ERROR_INVALID_ARGUMENT;   /* closed: not a device sample type */
    default:
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *sampleValType = NVML_VALUE_TYPE_UNSIGNED_INT;
    if (samples == NULL) {
        *sampleCount = GPUMON_SAMPLES;
        return NVML_SUCCESS;
    }

    NV2080_CTRL_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2_PARAMS *p;
    const NV2080_CTRL_PERF_GPUMON_PERFMON_UTIL_SAMPLE *s[GPUMON_SAMPLES];
    unsigned long long us[GPUMON_SAMPLES];
    unsigned n;
    ret = gpumon_samples(dev, lastSeenTimeStamp, false, &p, s, us, &n);
    if (ret != NVML_SUCCESS) {
        *sampleCount = 0;
        return ret;
    }
    if (n > *sampleCount)
        n = *sampleCount;
    for (unsigned i = 0; i < n; i++) {
        const NV2080_CTRL_PERF_GPUMON_ENGINE_UTIL_SAMPLE *e =
            (const void *)((const char *)s[i] + off);
        samples[i].timeStamp = us[i];
        samples[i].sampleValue.uiVal = e->util / 100;
    }
    free(p);
    *sampleCount = n;
    return NVML_SUCCESS;
}
