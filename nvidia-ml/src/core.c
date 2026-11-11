/*
 * SPDX-License-Identifier: MIT
 *
 * Library state, device enumeration and handles.
 *
 * nvmlInit only lists the GPUs (NV0000 GET_PROBED_IDS); a GPU is opened
 * when a handle to it is first requested, like the closed library does, so
 * counting GPUs doesn't wake a runtime-suspended one.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nvml-priv.h"

#include "nv-ioctl.h"
#include "nvstatus.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0000/ctrl0000system.h"
#include "ctrl/ctrl00de.h"
#include "ctrl/ctrl2080/ctrl2080bios.h"
#include "ctrl/ctrl2080/ctrl2080bus.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080mc.h"

struct nvml_state nvml = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* Probed GPUs in NVML index order; devices[] holds the opened ones. */
struct probed_gpu {
    NvU32 gpu_id;
    NvU32 domain;
    NvU16 bus, slot;
};
static struct probed_gpu probed[NVML_MAX_DEVICES];

/* ---- helpers ---------------------------------------------------------------- */

nvmlReturn_t nvml_from_rm(NvU32 st)
{
    switch (st) {
    case NV_OK:                         return NVML_SUCCESS;
    case NV_ERR_NOT_SUPPORTED:
    case NV_ERR_INVALID_COMMAND:        return NVML_ERROR_NOT_SUPPORTED;
    case NV_ERR_INSUFFICIENT_PERMISSIONS: return NVML_ERROR_NO_PERMISSION;
    case NV_ERR_NO_MEMORY:              return NVML_ERROR_MEMORY;
    case NV_ERR_GPU_IS_LOST:            return NVML_ERROR_GPU_IS_LOST;
    case NV_ERR_TIMEOUT:                return NVML_ERROR_TIMEOUT;
    case NV_ERR_INVALID_ARGUMENT:
    case NV_ERR_INVALID_PARAMETER:      return NVML_ERROR_INVALID_ARGUMENT;
    case NV_ERR_OBJECT_NOT_FOUND:       return NVML_ERROR_NOT_FOUND;
    case NV_ERR_IN_USE:                 return NVML_ERROR_IN_USE;
    default:                            return NVML_ERROR_UNKNOWN;
    }
}

nvmlReturn_t nvml_copy_string(char *dst, unsigned length, const char *src)
{
    if (dst == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (strlen(src) + 1 > length)
        return NVML_ERROR_INSUFFICIENT_SIZE;
    memcpy(dst, src, strlen(src) + 1);
    return NVML_SUCCESS;
}

NvU32 nvml_ctrl_sub(struct nvmlDevice_st *dev, NvU32 cmd, void *params, NvU32 size)
{
    return nvc_rm_control(&dev->rm, dev->subdevice, cmd, params, size);
}

NvU32 nvml_ctrl_dev(struct nvmlDevice_st *dev, NvU32 cmd, void *params, NvU32 size)
{
    return nvc_rm_control(&dev->rm, dev->device, cmd, params, size);
}

nvmlReturn_t nvml_initialized(void)
{
    return __atomic_load_n(&nvml.init_count, __ATOMIC_ACQUIRE) ? NVML_SUCCESS
                                                               : NVML_ERROR_UNINITIALIZED;
}

nvmlReturn_t nvml_device(nvmlDevice_t handle, struct nvmlDevice_st **dev)
{
    if (nvml_initialized() != NVML_SUCCESS)
        return NVML_ERROR_UNINITIALIZED;
    if (handle == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    for (unsigned i = 0; i < nvml.num_devices; i++) {
        if (nvml.devices[i] == handle) {
            *dev = handle;
            return NVML_SUCCESS;
        }
    }
    return NVML_ERROR_INVALID_ARGUMENT;
}

/* ---- RUSD ------------------------------------------------------------------- */

bool nvml_rusd_read(struct nvmlDevice_st *dev, volatile NvU64 *timestamp,
                    void *src, void *dst, size_t size)
{
    for (int i = 0; i < 100; i++) {
        NvU64 t0 = __atomic_load_n(timestamp, __ATOMIC_ACQUIRE);
        if (t0 == RUSD_TIMESTAMP_INVALID)
            return false;
        if (t0 == RUSD_TIMESTAMP_WRITE_IN_PROGRESS || (t0 >= RUSD_SEQ_START && (t0 & 1)))
            continue;
        memcpy(dst, src, size);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(timestamp, __ATOMIC_ACQUIRE) == t0)
            return true;
    }
    (void)dev;
    return false;
}

/* Adds categories to what GSP-RM polls for this device's RUSD object. */
void nvml_rusd_request(struct nvmlDevice_st *dev, NvU64 mask)
{
    pthread_mutex_lock(&dev->lock);
    if (dev->rusd_obj && (dev->rusd_mask & mask) != mask) {
        NV00DE_CTRL_REQUEST_DATA_POLL_PARAMS p = { .polledDataMask = dev->rusd_mask | mask };
        if (nvc_rm_control(&dev->rm, dev->rusd_obj, NV00DE_CTRL_CMD_REQUEST_DATA_POLL,
                           &p, sizeof(p)) == NV_OK)
            dev->rusd_mask = p.polledDataMask;
    }
    pthread_mutex_unlock(&dev->lock);
}

static void rusd_open(struct nvmlDevice_st *dev)
{
    /* GSP-RM only updates the categories some RUSD object asked for. */
    NV00DE_ALLOC_PARAMETERS params = {
        .polledDataMask = NV00DE_RUSD_POLL_CLOCK | NV00DE_RUSD_POLL_PERF |
                          NV00DE_RUSD_POLL_POWER | NV00DE_RUSD_POLL_THERMAL |
                          NV00DE_RUSD_POLL_PCI | NV00DE_RUSD_POLL_FAN |
                          NV00DE_RUSD_POLL_MEMORY,
    };
    void *cpu = NULL;

    dev->rusd_obj = nvc_rm_handle(&dev->rm);
    if (nvc_rm_alloc(&dev->rm, dev->subdevice, dev->rusd_obj, RM_USER_SHARED_DATA,
                     &params) != NV_OK) {
        dev->rusd_obj = 0;
        return;
    }
    dev->rusd_mask = params.polledDataMask;
    if (nvc_rm_map(&dev->rm, dev->device, dev->rusd_obj, 0, sizeof(NV00DE_SHARED_DATA), &cpu,
                   DRF_DEF(OS33, _FLAGS, _ACCESS, _READ_ONLY)) != NV_OK)
        return;
    dev->rusd = cpu;
}

/* ---- devices ---------------------------------------------------------------- */

static int find_minor(int ctl_fd, NvU32 gpu_id)
{
    nv_ioctl_card_info_t cards[32];
    unsigned long cmd = _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC,
                             NV_ESC_CARD_INFO, sizeof(cards));

    memset(cards, 0, sizeof(cards));
    if (ioctl(ctl_fd, cmd, cards) < 0)
        return -errno;
    for (unsigned i = 0; i < 32; i++)
        if (cards[i].valid && cards[i].gpu_id == gpu_id)
            return (int)cards[i].minor_number;
    return -ENODEV;
}

static void read_static_info(struct nvmlDevice_st *dev, const struct probed_gpu *p)
{
    NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS name = {
        .gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII,
    };
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &name, sizeof(name)) == NV_OK)
        snprintf(dev->name, sizeof(dev->name), "%s", (const char *)name.gpuNameString.ascii);

    NV2080_CTRL_GPU_GET_GID_INFO_PARAMS gid = {
        .flags = DRF_DEF(2080_GPU_CMD, _GPU_GET_GID_FLAGS, _FORMAT, _ASCII),
    };
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_GID_INFO, &gid, sizeof(gid)) == NV_OK)
        snprintf(dev->uuid, sizeof(dev->uuid), "%.*s", (int)gid.length, (const char *)gid.data);

    snprintf(dev->bus_id, sizeof(dev->bus_id), "%08x:%02x:%02x.0", p->domain, p->bus, p->slot);
    snprintf(dev->bus_id_legacy, sizeof(dev->bus_id_legacy), "%04x:%02x:%02x.0",
             p->domain, p->bus, p->slot);

    NV2080_CTRL_BUS_GET_PCI_INFO_PARAMS pci = { 0 };
    nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_PCI_INFO, &pci, sizeof(pci));
    snprintf(dev->pci.busIdLegacy, sizeof(dev->pci.busIdLegacy), "%s", dev->bus_id_legacy);
    snprintf(dev->pci.busId, sizeof(dev->pci.busId), "%s", dev->bus_id);
    dev->pci.domain = p->domain;
    dev->pci.bus = p->bus;
    dev->pci.device = p->slot;
    dev->pci.pciDeviceId = pci.pciDeviceId;
    dev->pci.pciSubSystemId = pci.pciSubSystemId;

    NV2080_CTRL_GPU_GET_CHIP_DETAILS_PARAMS chip = { 0 };
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_GET_CHIP_DETAILS, &chip, sizeof(chip)) == NV_OK)
        snprintf(dev->chip_sku, sizeof(dev->chip_sku), "%04X-%s-%c%u",
                 dev->pci.pciDeviceId >> 16, (const char *)chip.chipSku,
                 'A' + chip.chipMajor - 0xA, chip.chipMinor);

    NV2080_CTRL_MC_GET_ARCH_INFO_PARAMS arch = { 0 };
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_MC_GET_ARCH_INFO, &arch, sizeof(arch)) == NV_OK)
        dev->arch = arch.architecture;

    NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb = {
        .fbInfoListSize = 1,
        .fbInfoList = { { .index = NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE } },
    };
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof(fb)) == NV_OK)
        dev->fb_total = (NvU64)fb.fbInfoList[0].data << 10;

    NV2080_CTRL_BUS_GET_INFO_V2_PARAMS bus = {
        .busInfoListSize = 2,
        .busInfoList = {
            { .index = NV2080_CTRL_BUS_INFO_INDEX_PCIE_GPU_LINK_CAPS },
            { .index = NV2080_CTRL_BUS_INFO_INDEX_PCIE_ROOT_LINK_CAPS },
        },
    };
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_BUS_GET_INFO_V2, &bus, sizeof(bus)) == NV_OK) {
        dev->link_caps = bus.busInfoList[0].data;
        dev->root_link_caps = bus.busInfoList[1].data;
    }
}

static nvmlReturn_t device_open(unsigned index, struct nvmlDevice_st **out)
{
    const struct probed_gpu *p = &probed[index];
    struct nvmlDevice_st *dev = calloc(1, sizeof(*dev));
    nvmlReturn_t ret = NVML_ERROR_UNKNOWN;
    NvU32 st;

    if (dev == NULL)
        return NVML_ERROR_MEMORY;
    pthread_mutex_init(&dev->lock, NULL);
    dev->index = index;
    dev->gpu_id = p->gpu_id;

    if (nvc_rm_open(&dev->rm) != 0)
        goto fail_free;

    NV0000_CTRL_GPU_ATTACH_IDS_PARAMS attach = { .gpuIds = { p->gpu_id, NV0000_CTRL_GPU_INVALID_ID } };
    nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_CMD_GPU_ATTACH_IDS,
                   &attach, sizeof(attach));

    dev->minor = find_minor(dev->rm.ctl_fd, p->gpu_id);
    if (dev->minor < 0 || nvc_rm_open_gpu(&dev->rm, dev->minor) != 0) {
        ret = NVML_ERROR_GPU_IS_LOST;
        goto fail;
    }

    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = p->gpu_id };
    st = nvc_rm_control(&dev->rm, dev->rm.client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2,
                        &id, sizeof(id));
    if (st != NV_OK) {
        ret = nvml_from_rm(st);
        goto fail;
    }
    dev->device_instance = id.deviceInstance;

    NV0080_ALLOC_PARAMETERS dev_params = { .deviceId = dev->device_instance };
    dev->device = nvc_rm_handle(&dev->rm);
    st = nvc_rm_alloc(&dev->rm, dev->rm.client, dev->device, NV01_DEVICE_0, &dev_params);
    if (st != NV_OK) {
        ret = nvml_from_rm(st);
        goto fail;
    }

    NV2080_ALLOC_PARAMETERS sub_params = { .subDeviceId = 0 };
    dev->subdevice = nvc_rm_handle(&dev->rm);
    st = nvc_rm_alloc(&dev->rm, dev->device, dev->subdevice, NV20_SUBDEVICE_0, &sub_params);
    if (st != NV_OK) {
        ret = nvml_from_rm(st);
        goto fail;
    }

    read_static_info(dev, p);
    rusd_open(dev);
    *out = dev;
    return NVML_SUCCESS;

fail:
    nvc_rm_close(&dev->rm);
fail_free:
    pthread_mutex_destroy(&dev->lock);
    free(dev);
    return ret;
}

static void device_close(struct nvmlDevice_st *dev)
{
    nvc_rm_close(&dev->rm);   /* frees the client and every object, unmaps */
    pthread_mutex_destroy(&dev->lock);
    free(dev);
}

static int cmp_probed(const void *a, const void *b)
{
    const struct probed_gpu *x = a, *y = b;
    if (x->domain != y->domain)
        return x->domain < y->domain ? -1 : 1;
    if (x->bus != y->bus)
        return x->bus < y->bus ? -1 : 1;
    return (int)x->slot - (int)y->slot;
}

static nvmlReturn_t enumerate(void)
{
    NV0000_CTRL_GPU_GET_PROBED_IDS_PARAMS ids;
    unsigned n = 0;

    memset(&ids, 0, sizeof(ids));
    if (nvc_rm_control(&nvml.sys, nvml.sys.client, NV0000_CTRL_CMD_GPU_GET_PROBED_IDS,
                       &ids, sizeof(ids)) != NV_OK)
        return NVML_ERROR_UNKNOWN;

    for (unsigned i = 0; i < NV0000_CTRL_GPU_MAX_PROBED_GPUS && n < NVML_MAX_DEVICES; i++) {
        if (ids.gpuIds[i] == NV0000_CTRL_GPU_INVALID_ID)
            continue;
        NV0000_CTRL_GPU_GET_PCI_INFO_PARAMS pci = { .gpuId = ids.gpuIds[i] };
        if (nvc_rm_control(&nvml.sys, nvml.sys.client, NV0000_CTRL_CMD_GPU_GET_PCI_INFO,
                           &pci, sizeof(pci)) != NV_OK)
            continue;
        probed[n++] = (struct probed_gpu){ ids.gpuIds[i], pci.domain, pci.bus, pci.slot };
    }
    qsort(probed, n, sizeof(probed[0]), cmp_probed);
    nvml.num_devices = n;
    return NVML_SUCCESS;
}

/* Opens the device at index on first use. */
static nvmlReturn_t get_device(unsigned index, nvmlDevice_t *out)
{
    nvmlReturn_t ret = NVML_SUCCESS;

    pthread_mutex_lock(&nvml.lock);
    if (nvml.devices[index] == NULL)
        ret = device_open(index, &nvml.devices[index]);
    if (ret == NVML_SUCCESS)
        *out = nvml.devices[index];
    pthread_mutex_unlock(&nvml.lock);
    return ret;
}

/* ---- init ------------------------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlInitWithFlags(unsigned int flags)
{
    nvmlReturn_t ret = NVML_SUCCESS;
    int r;

    (void)flags;
    pthread_mutex_lock(&nvml.lock);
    if (nvml.init_count == 0) {
        r = nvc_rm_open(&nvml.sys);
        if (r == -EPROTO)
            ret = NVML_ERROR_LIB_RM_VERSION_MISMATCH;
        else if (r != 0)
            ret = NVML_ERROR_DRIVER_NOT_LOADED;
        else if ((ret = enumerate()) != NVML_SUCCESS)
            nvc_rm_close(&nvml.sys);
    }
    if (ret == NVML_SUCCESS)
        __atomic_add_fetch(&nvml.init_count, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&nvml.lock);
    return ret;
}

NVML_EXPORT nvmlReturn_t nvmlInit_v2(void)
{
    return nvmlInitWithFlags(0);
}

NVML_EXPORT nvmlReturn_t nvmlInit(void)
{
    return nvmlInitWithFlags(0);
}

NVML_EXPORT nvmlReturn_t nvmlShutdown(void)
{
    nvmlReturn_t ret = NVML_SUCCESS;

    pthread_mutex_lock(&nvml.lock);
    if (nvml.init_count == 0) {
        ret = NVML_ERROR_UNINITIALIZED;
    } else if (__atomic_sub_fetch(&nvml.init_count, 1, __ATOMIC_RELEASE) == 0) {
        for (unsigned i = 0; i < NVML_MAX_DEVICES; i++) {
            if (nvml.devices[i] != NULL)
                device_close(nvml.devices[i]);
            nvml.devices[i] = NULL;
        }
        nvml.num_devices = 0;
        nvc_rm_close(&nvml.sys);
    }
    pthread_mutex_unlock(&nvml.lock);
    return ret;
}

/* ---- handles ---------------------------------------------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *count)
{
    if (nvml_initialized() != NVML_SUCCESS)
        return NVML_ERROR_UNINITIALIZED;
    if (count == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    *count = nvml.num_devices;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCount(unsigned int *count)
{
    return nvmlDeviceGetCount_v2(count);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t *device)
{
    if (nvml_initialized() != NVML_SUCCESS)
        return NVML_ERROR_UNINITIALIZED;
    if (device == NULL || index >= nvml.num_devices)
        return NVML_ERROR_INVALID_ARGUMENT;
    return get_device(index, device);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t *device)
{
    return nvmlDeviceGetHandleByIndex_v2(index, device);
}

/* Accepts "0000:01:00.0", "00000000:01:00.0" and "01:00.0", any case. */
static bool parse_bus_id(const char *s, unsigned *domain, unsigned *bus, unsigned *slot)
{
    unsigned fn;
    if (sscanf(s, "%x:%x:%x.%x", domain, bus, slot, &fn) == 4)
        return true;
    *domain = 0;
    return sscanf(s, "%x:%x.%x", bus, slot, &fn) == 3;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(const char *pciBusId, nvmlDevice_t *device)
{
    unsigned domain, bus, slot;

    if (nvml_initialized() != NVML_SUCCESS)
        return NVML_ERROR_UNINITIALIZED;
    if (pciBusId == NULL || device == NULL || !parse_bus_id(pciBusId, &domain, &bus, &slot))
        return NVML_ERROR_INVALID_ARGUMENT;
    for (unsigned i = 0; i < nvml.num_devices; i++)
        if (probed[i].domain == domain && probed[i].bus == bus && probed[i].slot == slot)
            return get_device(i, device);
    return NVML_ERROR_NOT_FOUND;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetHandleByPciBusId(const char *pciBusId, nvmlDevice_t *device)
{
    return nvmlDeviceGetHandleByPciBusId_v2(pciBusId, device);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetHandleByUUID(const char *uuid, nvmlDevice_t *device)
{
    if (nvml_initialized() != NVML_SUCCESS)
        return NVML_ERROR_UNINITIALIZED;
    if (uuid == NULL || device == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    /* Needs the GPU open to know its UUID. */
    for (unsigned i = 0; i < nvml.num_devices; i++) {
        nvmlDevice_t d;
        if (get_device(i, &d) == NVML_SUCCESS && strcasecmp(d->uuid, uuid) == 0) {
            *device = d;
            return NVML_SUCCESS;
        }
    }
    return NVML_ERROR_NOT_FOUND;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetHandleByUUIDV(const nvmlUUID_t *uuid, nvmlDevice_t *device)
{
    if (uuid == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (uuid->type != NVML_UUID_TYPE_ASCII)
        return NVML_ERROR_NOT_SUPPORTED;
    return nvmlDeviceGetHandleByUUID(uuid->value.str, device);
}

/* Bus id of a GPU by index without opening it (export table slot 81). */
nvmlReturn_t nvml_bus_id_for_index(unsigned index, char *busId, unsigned length)
{
    char s[32];

    if (nvml_initialized() != NVML_SUCCESS)
        return NVML_ERROR_UNINITIALIZED;
    if (index >= nvml.num_devices || busId == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    snprintf(s, sizeof(s), "%04x:%02x:%02x.0", probed[index].domain, probed[index].bus,
             probed[index].slot);
    return nvml_copy_string(busId, length, s);
}

/* RM gpuId of a probed GPU by PCI address, without opening it. */
nvmlReturn_t nvml_gpu_id_by_pci(unsigned domain, unsigned bus, unsigned slot, NvU32 *gpu_id)
{
    for (unsigned i = 0; i < nvml.num_devices; i++)
        if (probed[i].domain == domain && probed[i].bus == bus && probed[i].slot == slot) {
            *gpu_id = probed[i].gpu_id;
            return NVML_SUCCESS;
        }
    return NVML_ERROR_NOT_FOUND;
}

/* ---- system ----------------------------------------------------------------- */

static nvmlReturn_t build_version(char *driver, size_t size)
{
    char ver[64] = "", title[64] = "";
    NV0000_CTRL_SYSTEM_GET_BUILD_VERSION_PARAMS p = {
        .sizeOfStrings = sizeof(ver),
        .pDriverVersionBuffer = NV_PTR_TO_NvP64(driver),
        .pVersionBuffer = NV_PTR_TO_NvP64(ver),
        .pTitleBuffer = NV_PTR_TO_NvP64(title),
    };
    if (size < sizeof(ver))
        return NVML_ERROR_INSUFFICIENT_SIZE;
    return nvml_from_rm(nvc_rm_control(&nvml.sys, nvml.sys.client,
                                       NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION, &p, sizeof(p)));
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetDriverVersion(char *version, unsigned int length)
{
    char v[64] = "";
    nvmlReturn_t ret;

    if (nvml_initialized() != NVML_SUCCESS)
        return NVML_ERROR_UNINITIALIZED;
    if ((ret = build_version(v, sizeof(v))) != NVML_SUCCESS)
        return ret;
    return nvml_copy_string(version, length, v);
}

/* The NVML version is "<CUDA major>.<driver version>", e.g. 13.620.70.06. */
NVML_EXPORT nvmlReturn_t nvmlSystemGetNVMLVersion(char *version, unsigned int length)
{
    char v[64] = "", s[80];
    nvmlReturn_t ret;

    if ((ret = build_version(v, sizeof(v))) != NVML_SUCCESS)
        return ret;
    snprintf(s, sizeof(s), "%d.%s", NVML_API_VERSION, v);
    return nvml_copy_string(version, length, s);
}

/* The CUDA version the driver supports; libcuda isn't asked, it's fixed
 * per driver release (cuDriverGetVersion of 620.70.06). */
#define NVML_CUDA_DRIVER_VERSION 13050

NVML_EXPORT nvmlReturn_t nvmlSystemGetCudaDriverVersion_v2(int *cudaDriverVersion)
{
    if (cudaDriverVersion == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    *cudaDriverVersion = NVML_CUDA_DRIVER_VERSION;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetCudaDriverVersion(int *cudaDriverVersion)
{
    return nvmlSystemGetCudaDriverVersion_v2(cudaDriverVersion);
}

NVML_EXPORT nvmlReturn_t nvmlSystemGetProcessName(unsigned int pid, char *name, unsigned int length)
{
    char path[64], buf[4096];
    ssize_t n;

    if (nvml_initialized() != NVML_SUCCESS)
        return NVML_ERROR_UNINITIALIZED;
    if (name == NULL || length == 0)
        return NVML_ERROR_INVALID_ARGUMENT;
    /* argv[0], as the closed library reports it; readable even for
     * processes whose exe link isn't (kwin_wayland). Kernel threads and
     * zombies have an empty command line. */
    FILE *f;
    snprintf(path, sizeof(path), "/proc/%u/cmdline", pid);
    if ((f = fopen(path, "r")) == NULL)
        return NVML_ERROR_NOT_FOUND;
    n = (ssize_t)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n <= 0 || buf[0] == 0)
        return NVML_ERROR_NO_DATA;
    buf[n] = 0;
    /* The closed library truncates instead of failing. */
    snprintf(name, length, "%s", buf);
    return NVML_SUCCESS;
}

/* ---- errors ----------------------------------------------------------------- */

NVML_EXPORT const char *nvmlErrorString(nvmlReturn_t result)
{
    static const char *const strings[] = {
        [NVML_SUCCESS] = "Success",
        [NVML_ERROR_UNINITIALIZED] = "Uninitialized",
        [NVML_ERROR_INVALID_ARGUMENT] = "Invalid Argument",
        [NVML_ERROR_NOT_SUPPORTED] = "Not Supported",
        [NVML_ERROR_NO_PERMISSION] = "Insufficient Permissions",
        [NVML_ERROR_ALREADY_INITIALIZED] = "Already Initialized",
        [NVML_ERROR_NOT_FOUND] = "Not Found",
        [NVML_ERROR_INSUFFICIENT_SIZE] = "Insufficient Size",
        [NVML_ERROR_INSUFFICIENT_POWER] = "Insufficient External Power",
        [NVML_ERROR_DRIVER_NOT_LOADED] = "Driver Not Loaded",
        [NVML_ERROR_TIMEOUT] = "Timeout",
        [NVML_ERROR_IRQ_ISSUE] = "Interrupt Request Issue",
        [NVML_ERROR_LIBRARY_NOT_FOUND] = "NVML Shared Library Not Found",
        [NVML_ERROR_FUNCTION_NOT_FOUND] = "Function Not Found",
        [NVML_ERROR_CORRUPTED_INFOROM] = "Corrupted infoROM",
        [NVML_ERROR_GPU_IS_LOST] = "GPU is lost",
        [NVML_ERROR_RESET_REQUIRED] = "GPU requires reset",
        [NVML_ERROR_OPERATING_SYSTEM] = "Generic operating system error",
        [NVML_ERROR_LIB_RM_VERSION_MISMATCH] = "Driver/library version mismatch",
        [NVML_ERROR_IN_USE] = "In use by another client",
        [NVML_ERROR_MEMORY] = "Insufficient Memory",
        [NVML_ERROR_NO_DATA] = "No data",
        [NVML_ERROR_VGPU_ECC_NOT_SUPPORTED] = "vGPU ECC not supported",
        [NVML_ERROR_INSUFFICIENT_RESOURCES] = "Insufficient Resources",
        [NVML_ERROR_FREQ_NOT_SUPPORTED] = "Invalid Frequency",
        [NVML_ERROR_ARGUMENT_VERSION_MISMATCH] = "Argument version mismatch",
        [NVML_ERROR_DEPRECATED] = "Requested functionality has been deprecated",
        [NVML_ERROR_NOT_READY] = "System is not in ready state",
        [NVML_ERROR_GPU_NOT_FOUND] = "No supported GPUs were found",
        [NVML_ERROR_INVALID_STATE] = "Resource not in correct state to perform requested operation",
        [NVML_ERROR_RESET_TYPE_NOT_SUPPORTED] = "Reset type is not supported for given device",
    };
    if ((unsigned)result < sizeof(strings) / sizeof(strings[0]) && strings[result])
        return strings[result];
    return "Unknown Error";
}
