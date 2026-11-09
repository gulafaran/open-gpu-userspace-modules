/*
 * SPDX-License-Identifier: MIT
 *
 * Device modes: persistence, virtualization, display, MIG.
 */

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "nvml-priv.h"

#include "nvstatus.h"
#include "class/cl0073.h"
#include "ctrl/ctrl0073/ctrl0073system.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

/*
 * nvidia-persistenced keeps GPUs initialized by holding them open, which
 * RM doesn't know about, so ask the daemon first, over its ONC RPC socket
 * (program 35006 version 1, procedure 2: get persistence mode of a PCI
 * device; reply: status, mode). Returns -1 if it can't be asked.
 */
#define NVPD_SOCKET "/var/run/nvidia-persistenced/socket"
#define NVPD_PROG 35006
#define NVPD_VERS 1
#define NVPD_PROC_GET_PERSISTENCE_MODE 2

static int persistenced_mode(const struct nvmlDevice_st *dev)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX, .sun_path = NVPD_SOCKET };
    struct timeval tv = { .tv_sec = 1 };
    uint32_t call[15], reply[9];
    int fd, mode = -1;

    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto out;

    uint32_t xid = (uint32_t)getpid() ^ (uint32_t)(uintptr_t)dev;
    const uint32_t words[] = {
        0x80000000u | (14 * 4),          /* record mark: last fragment */
        xid, 0 /* CALL */, 2 /* RPC version */, NVPD_PROG, NVPD_VERS,
        NVPD_PROC_GET_PERSISTENCE_MODE,
        0, 0,                            /* credentials: AUTH_NONE */
        0, 0,                            /* verifier: AUTH_NONE */
        dev->pci.domain, dev->pci.bus, dev->pci.device, 0,
    };
    for (unsigned i = 0; i < 15; i++)
        call[i] = htonl(words[i]);
    if (write(fd, call, sizeof(call)) != sizeof(call))
        goto out;

    /* mark, xid, REPLY, MSG_ACCEPTED, verifier (2), SUCCESS, status, mode */
    if (read(fd, reply, sizeof(reply)) != sizeof(reply))
        goto out;
    if (ntohl(reply[1]) == xid && ntohl(reply[2]) == 1 && ntohl(reply[3]) == 0 &&
        ntohl(reply[6]) == 0 && ntohl(reply[7]) == 0)
        mode = ntohl(reply[8]) != 0;
out:
    close(fd);
    return mode;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPersistenceMode(nvmlDevice_t device, nvmlEnableState_t *mode)
{
    NV0080_CTRL_GPU_QUERY_SW_STATE_PERSISTENCE_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(mode);

    int pd = persistenced_mode(dev);
    if (pd >= 0) {
        *mode = pd ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
        return NVML_SUCCESS;
    }
    NvU32 st = nvml_ctrl_dev(dev, NV0080_CTRL_CMD_GPU_QUERY_SW_STATE_PERSISTENCE, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    *mode = p.swStatePersistence == NV0080_CTRL_GPU_SW_STATE_PERSISTENCE_ENABLED
            ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetVirtualizationMode(nvmlDevice_t device,
                                                         nvmlGpuVirtualizationMode_t *mode)
{
    NV0080_CTRL_GPU_GET_VIRTUALIZATION_MODE_PARAMS p = { 0 };
    DEVICE(device, dev);
    CHECK_PTR(mode);
    NvU32 st = nvml_ctrl_dev(dev, NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE, &p, sizeof(p));
    if (st != NV_OK)
        return nvml_from_rm(st);
    switch (p.virtualizationMode) {
    case NV0080_CTRL_GPU_VIRTUALIZATION_MODE_NMOS:      *mode = NVML_GPU_VIRTUALIZATION_MODE_PASSTHROUGH; break;
    case NV0080_CTRL_GPU_VIRTUALIZATION_MODE_VGX:       *mode = NVML_GPU_VIRTUALIZATION_MODE_VGPU; break;
    case NV0080_CTRL_GPU_VIRTUALIZATION_MODE_HOST:      *mode = NVML_GPU_VIRTUALIZATION_MODE_HOST_VGPU; break;
    case NV0080_CTRL_GPU_VIRTUALIZATION_MODE_HOST_VSGA: *mode = NVML_GPU_VIRTUALIZATION_MODE_HOST_VSGA; break;
    default:                                            *mode = NVML_GPU_VIRTUALIZATION_MODE_NONE; break;
    }
    return NVML_SUCCESS;
}

/* ---- display ---------------------------------------------------------------- */

/* NV04_DISPLAY_COMMON under the device, allocated on first use (dev->lock). */
static NvHandle display_object(struct nvmlDevice_st *dev)
{
    if (dev->display == 0) {
        NvHandle h = nvc_rm_handle(&dev->rm);
        if (nvc_rm_alloc(&dev->rm, dev->device, h, NV04_DISPLAY_COMMON, NULL) == NV_OK)
            dev->display = h;
    }
    return dev->display;
}

static NvU32 display_ctrl(struct nvmlDevice_st *dev, NvU32 cmd, void *params, NvU32 size)
{
    NvHandle disp = display_object(dev);
    return disp ? nvc_rm_control(&dev->rm, disp, cmd, params, size) : NV_ERR_NOT_SUPPORTED;
}

/* Whether a display is connected to the GPU. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetDisplayMode(nvmlDevice_t device, nvmlEnableState_t *display)
{
    NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS sup = { 0 };
    NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_PARAMS conn = { 0 };
    NvU32 st;
    DEVICE(device, dev);
    CHECK_PTR(display);

    pthread_mutex_lock(&dev->lock);
    st = display_ctrl(dev, NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, &sup, sizeof(sup));
    if (st == NV_OK) {
        conn.displayMask = sup.displayMask;
        st = display_ctrl(dev, NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, &conn, sizeof(conn));
    }
    pthread_mutex_unlock(&dev->lock);
    if (st != NV_OK)
        return nvml_from_rm(st);
    *display = conn.displayMask ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

/* Whether any head drives a display. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetDisplayActive(nvmlDevice_t device, nvmlEnableState_t *active)
{
    NV0073_CTRL_SYSTEM_GET_NUM_HEADS_PARAMS heads = { 0 };
    NvU32 st;
    bool any = false;
    DEVICE(device, dev);
    CHECK_PTR(active);

    pthread_mutex_lock(&dev->lock);
    st = display_ctrl(dev, NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS, &heads, sizeof(heads));
    for (NvU32 h = 0; st == NV_OK && h < heads.numHeads && !any; h++) {
        NV0073_CTRL_SYSTEM_GET_ACTIVE_PARAMS a = { .head = h };
        if (display_ctrl(dev, NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE, &a, sizeof(a)) == NV_OK)
            any = a.displayId != 0;
    }
    pthread_mutex_unlock(&dev->lock);
    if (st != NV_OK)
        return nvml_from_rm(st);
    *active = any ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

/* ---- MIG ---------------------------------------------------------------------
 * GeForce GPUs have no MIG; handles we give out are never MIG devices.
 * nvidia-smi needs these two to succeed to list processes. */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMaxMigDeviceCount(nvmlDevice_t device, unsigned int *count)
{
    DEVICE(device, dev);
    CHECK_PTR(count);
    *count = 0;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceIsMigDeviceHandle(nvmlDevice_t device, unsigned int *isMigDevice)
{
    DEVICE(device, dev);
    CHECK_PTR(isMigDevice);
    *isMigDevice = 0;
    return NVML_SUCCESS;
}
