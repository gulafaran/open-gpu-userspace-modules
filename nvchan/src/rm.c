/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rm.h"

#include "nvUnixVersion.h"         /* NV_VERSION_STRING */
#include "nvstatus.h"
#include "nv-ioctl.h"              /* nv_ioctl_*_t, NV_RM_API_VERSION_* */
#include "nv-ioctl-numbers.h"      /* NV_IOCTL_MAGIC, NV_ESC_* */
#include "nv_escape.h"             /* NV_ESC_RM_* */
#include "nv-unix-nvos-params-wrappers.h"
#include "class/cl0000.h"          /* NV01_ROOT */
#include "class/cl003e.h"          /* NV01_MEMORY_SYSTEM */
#include "class/cl0071.h"          /* NV01_MEMORY_SYSTEM_OS_DESCRIPTOR */

/* Handles we generate; RM's own are in other ranges. */
#define NVC_HANDLE_BASE 0xc1d00000u

struct nvc_rm_mapping {
    struct nvc_rm_mapping *next;
    void *cpu;
    size_t size;
    int fd;             /* the file holding the mapping context */
    NvP64 token;        /* RM's address for NV04_UNMAP_MEMORY */
    NvHandle device;
    NvHandle memory;
    bool from_alloc;    /* made by RM_ALLOC_MEMORY, goes away with RM_FREE */
};

static int nv_ioctl(int fd, unsigned nr, void *params, size_t size)
{
    unsigned long cmd;
    nv_ioctl_xfer_t xfer;
    void *arg = params;
    int ret;

    /* The ioctl size field has 14 bits; larger requests go through XFER. */
    if (size >= (1u << _IOC_SIZEBITS)) {
        xfer.cmd = nr;
        xfer.size = (NvU32)size;
        xfer.ptr = NV_PTR_TO_NvP64(params);
        nr = NV_ESC_IOCTL_XFER_CMD;
        size = sizeof(xfer);
        arg = &xfer;
    }

    cmd = _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, nr, size);
    do {
        ret = ioctl(fd, cmd, arg);
    } while (ret < 0 && (errno == EINTR || errno == EAGAIN));
    return ret < 0 ? -errno : 0;
}

/* NV_STATUS for an ioctl that failed at the OS level. */
static NvU32 os_status(int err)
{
    switch (err) {
    case -ENOMEM: return NV_ERR_NO_MEMORY;
    case -EINVAL: return NV_ERR_INVALID_ARGUMENT;
    case -EPERM:
    case -EACCES: return NV_ERR_INSUFFICIENT_PERMISSIONS;
    default:      return NV_ERR_GENERIC;
    }
}

static int open_ctl(void)
{
    return open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
}

static int open_gpu(int minor)
{
    char path[32];

    snprintf(path, sizeof(path), "/dev/nvidia%d", minor);
    return open(path, O_RDWR | O_CLOEXEC);
}

int nvc_rm_open(struct nvc_rm *rm)
{
    nv_ioctl_rm_api_version_t ver = { 0 };
    NVOS21_PARAMETERS alloc = { 0 };
    NvHandle client = 0;
    int ret;

    memset(rm, 0, sizeof(*rm));
    rm->dev_fd = -1;
    rm->minor = -1;
    rm->next_handle = NVC_HANDLE_BASE;
    pthread_mutex_init(&rm->lock, NULL);

    rm->ctl_fd = open_ctl();
    if (rm->ctl_fd < 0)
        return -errno;

    ver.cmd = NV_RM_API_VERSION_CMD_QUERY;
    ret = nv_ioctl(rm->ctl_fd, NV_ESC_CHECK_VERSION_STR, &ver, sizeof(ver));
    if (ret != 0)
        goto fail;
    snprintf(rm->version, sizeof(rm->version), "%s", ver.versionString);
    if (strcmp(ver.versionString, NV_VERSION_STRING) != 0) {
        /* RM structures may differ: built against another kernel tree. */
        ret = -EPROTO;
        goto fail;
    }

    alloc.hClass = NV01_ROOT;
    alloc.pAllocParms = NV_PTR_TO_NvP64(&client);
    ret = nv_ioctl(rm->ctl_fd, NV_ESC_RM_ALLOC, &alloc, sizeof(alloc));
    if (ret == 0 && alloc.status != NV_OK)
        ret = -EIO;
    if (ret != 0)
        goto fail;
    rm->client = alloc.hObjectNew ? alloc.hObjectNew : client;
    return 0;

fail:
    close(rm->ctl_fd);
    rm->ctl_fd = -1;
    return ret;
}

int nvc_rm_open_gpu(struct nvc_rm *rm, int minor)
{
    nv_ioctl_wait_open_complete_t wait = { 0 };
    nv_ioctl_register_fd_t reg = { .ctl_fd = rm->ctl_fd };
    int fd, ret;

    fd = open_gpu(minor);
    if (fd < 0)
        return -errno;

    ret = nv_ioctl(fd, NV_ESC_WAIT_OPEN_COMPLETE, &wait, sizeof(wait));
    if (ret == 0 && (wait.rc != 0 || wait.adapterStatus != NV_OK))
        ret = -EIO;
    if (ret == 0)
        ret = nv_ioctl(fd, NV_ESC_REGISTER_FD, &reg, sizeof(reg));
    if (ret != 0) {
        close(fd);
        return ret;
    }

    rm->dev_fd = fd;
    rm->minor = minor;
    return 0;
}

void nvc_rm_close(struct nvc_rm *rm)
{
    struct nvc_rm_mapping *m, *next;

    for (m = rm->mappings; m != NULL; m = next) {
        next = m->next;
        munmap(m->cpu, m->size);
        close(m->fd);
        free(m);
    }
    rm->mappings = NULL;

    if (rm->ctl_fd >= 0 && rm->client != 0) {
        NVOS00_PARAMETERS p = { .hRoot = rm->client, .hObjectParent = rm->client,
                                .hObjectOld = rm->client };
        nv_ioctl(rm->ctl_fd, NV_ESC_RM_FREE, &p, sizeof(p));
    }
    if (rm->dev_fd >= 0)
        close(rm->dev_fd);
    if (rm->ctl_fd >= 0)
        close(rm->ctl_fd);
    free(rm->sysmem);
    pthread_mutex_destroy(&rm->lock);
    memset(rm, 0, sizeof(*rm));
    rm->ctl_fd = rm->dev_fd = -1;
}

NvHandle nvc_rm_handle(struct nvc_rm *rm)
{
    NvHandle h;

    pthread_mutex_lock(&rm->lock);
    h = rm->next_handle++;
    pthread_mutex_unlock(&rm->lock);
    return h;
}

static bool is_sysmem(struct nvc_rm *rm, NvHandle memory)
{
    for (unsigned i = 0; i < rm->num_sysmem; i++)
        if (rm->sysmem[i] == memory)
            return true;
    return false;
}

static void add_sysmem(struct nvc_rm *rm, NvHandle memory)
{
    if (rm->num_sysmem == rm->max_sysmem) {
        unsigned max = rm->max_sysmem ? rm->max_sysmem * 2 : 16;
        NvHandle *p = realloc(rm->sysmem, max * sizeof(*p));
        if (p == NULL)
            return;
        rm->sysmem = p;
        rm->max_sysmem = max;
    }
    rm->sysmem[rm->num_sysmem++] = memory;
}

static void remove_sysmem(struct nvc_rm *rm, NvHandle memory)
{
    for (unsigned i = 0; i < rm->num_sysmem; i++) {
        if (rm->sysmem[i] == memory) {
            rm->sysmem[i] = rm->sysmem[--rm->num_sysmem];
            return;
        }
    }
}

/* Takes a mapping out of the list (rm->lock held). */
static struct nvc_rm_mapping *take_mapping(struct nvc_rm *rm, void *cpu,
                                           NvHandle memory, bool from_alloc)
{
    for (struct nvc_rm_mapping **pm = &rm->mappings; *pm != NULL; pm = &(*pm)->next) {
        struct nvc_rm_mapping *m = *pm;
        if ((cpu != NULL && m->cpu == cpu) ||
            (cpu == NULL && m->memory == memory && m->from_alloc == from_alloc)) {
            *pm = m->next;
            return m;
        }
    }
    return NULL;
}

NvU32 nvc_rm_alloc(struct nvc_rm *rm, NvHandle parent, NvHandle object,
                   NvU32 cls, void *params)
{
    /* RM derives the parameter size from the class. */
    NVOS21_PARAMETERS p = {
        .hRoot = rm->client,
        .hObjectParent = parent,
        .hObjectNew = object,
        .hClass = cls,
        .pAllocParms = NV_PTR_TO_NvP64(params),
    };
    int ret = nv_ioctl(rm->ctl_fd, NV_ESC_RM_ALLOC, &p, sizeof(p));

    return ret ? os_status(ret) : p.status;
}

NvU32 nvc_rm_free(struct nvc_rm *rm, NvHandle parent, NvHandle object)
{
    NVOS00_PARAMETERS p = {
        .hRoot = rm->client,
        .hObjectParent = parent,
        .hObjectOld = object,
    };
    struct nvc_rm_mapping *m;
    int ret;

    /* A CPU mapping made at allocation goes away with the memory. */
    pthread_mutex_lock(&rm->lock);
    m = take_mapping(rm, NULL, object, true);
    remove_sysmem(rm, object);
    pthread_mutex_unlock(&rm->lock);
    if (m != NULL) {
        munmap(m->cpu, m->size);
        close(m->fd);
        free(m);
    }

    ret = nv_ioctl(rm->ctl_fd, NV_ESC_RM_FREE, &p, sizeof(p));
    return ret ? os_status(ret) : p.status;
}

NvU32 nvc_rm_control(struct nvc_rm *rm, NvHandle object, NvU32 cmd,
                     void *params, NvU32 size)
{
    NVOS54_PARAMETERS p = {
        .hClient = rm->client,
        .hObject = object,
        .cmd = cmd,
        .params = NV_PTR_TO_NvP64(params),
        .paramsSize = size,
    };
    int ret = nv_ioctl(rm->ctl_fd, NV_ESC_RM_CONTROL, &p, sizeof(p));

    return ret ? os_status(ret) : p.status;
}

NvU32 nvc_rm_vid_heap_control(struct nvc_rm *rm, NVOS32_PARAMETERS *params)
{
    int ret;

    params->hRoot = rm->client;
    ret = nv_ioctl(rm->ctl_fd, NV_ESC_RM_VID_HEAP_CONTROL, params, sizeof(*params));
    return ret ? os_status(ret) : params->status;
}

NvU32 nvc_rm_alloc_memory(struct nvc_rm *rm, NvHandle parent, NvHandle memory,
                          NvU32 cls, NvU32 flags, void **cpu, NvU64 *limit)
{
    const bool sysmem = cls == NV01_MEMORY_SYSTEM ||
                        cls == NV01_MEMORY_SYSTEM_OS_DESCRIPTOR;
    const bool want_map = sysmem && cpu != NULL &&
                          FLD_TEST_DRF(OS02, _FLAGS, _MAPPING, _DEFAULT, flags);
    nv_ioctl_nvos02_parameters_with_fd p = {
        .params = {
            .hRoot = rm->client,
            .hObjectParent = parent,
            .hObjectNew = memory,
            .hClass = cls,
            .flags = flags,
            .pMemory = NV_PTR_TO_NvP64(cpu ? *cpu : NULL),
            .limit = limit ? *limit : 0,
        },
        .fd = -1,
    };
    int ret;

    if (rm->dev_fd < 0)
        return NV_ERR_INVALID_STATE;

    if (want_map) {
        p.fd = open_ctl();
        if (p.fd < 0)
            return os_status(-errno);
    }

    /* Only accepted on a GPU device file. */
    ret = nv_ioctl(rm->dev_fd, NV_ESC_RM_ALLOC_MEMORY, &p, sizeof(p));
    if (ret != 0 || p.params.status != NV_OK) {
        if (p.fd >= 0)
            close(p.fd);
        return ret ? os_status(ret) : p.params.status;
    }
    if (limit)
        *limit = p.params.limit;

    pthread_mutex_lock(&rm->lock);
    if (sysmem)
        add_sysmem(rm, memory);
    pthread_mutex_unlock(&rm->lock);

    if (!want_map) {
        if (cpu)
            *cpu = NULL;
        return NV_OK;
    }

    size_t size = (size_t)p.params.limit + 1;
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, p.fd, 0);
    struct nvc_rm_mapping *m = ptr != MAP_FAILED ? calloc(1, sizeof(*m)) : NULL;

    if (m == NULL) {
        if (ptr != MAP_FAILED)
            munmap(ptr, size);
        close(p.fd);
        nvc_rm_free(rm, parent, memory);
        return NV_ERR_OPERATING_SYSTEM;
    }
    *m = (struct nvc_rm_mapping){
        .cpu = ptr, .size = size, .fd = p.fd, .token = p.params.pMemory,
        .device = parent, .memory = memory, .from_alloc = true,
    };
    pthread_mutex_lock(&rm->lock);
    m->next = rm->mappings;
    rm->mappings = m;
    pthread_mutex_unlock(&rm->lock);

    *cpu = ptr;
    return NV_OK;
}

NvU32 nvc_rm_map(struct nvc_rm *rm, NvHandle device, NvHandle memory,
                 NvU64 offset, NvU64 length, void **cpu, NvU32 flags)
{
    nv_ioctl_nvos33_parameters_with_fd p = {
        .params = {
            .hClient = rm->client,
            .hDevice = device,
            .hMemory = memory,
            .offset = offset,
            .length = length,
            .flags = flags,
        },
    };
    bool sysmem;
    int ret;

    pthread_mutex_lock(&rm->lock);
    sysmem = is_sysmem(rm, memory);
    pthread_mutex_unlock(&rm->lock);

    p.fd = sysmem ? open_ctl() : open_gpu(rm->minor);
    if (p.fd < 0)
        return os_status(-errno);

    ret = nv_ioctl(rm->ctl_fd, NV_ESC_RM_MAP_MEMORY, &p, sizeof(p));
    if (ret != 0 || p.params.status != NV_OK) {
        close(p.fd);
        return ret ? os_status(ret) : p.params.status;
    }

    void *ptr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, p.fd, 0);
    struct nvc_rm_mapping *m = ptr != MAP_FAILED ? calloc(1, sizeof(*m)) : NULL;

    if (m == NULL) {
        NVOS34_PARAMETERS u = {
            .hClient = rm->client, .hDevice = device, .hMemory = memory,
            .pLinearAddress = p.params.pLinearAddress, .flags = flags,
        };
        if (ptr != MAP_FAILED)
            munmap(ptr, length);
        nv_ioctl(rm->ctl_fd, NV_ESC_RM_UNMAP_MEMORY, &u, sizeof(u));
        close(p.fd);
        return NV_ERR_OPERATING_SYSTEM;
    }
    *m = (struct nvc_rm_mapping){
        .cpu = ptr, .size = length, .fd = p.fd, .token = p.params.pLinearAddress,
        .device = device, .memory = memory, .from_alloc = false,
    };
    pthread_mutex_lock(&rm->lock);
    m->next = rm->mappings;
    rm->mappings = m;
    pthread_mutex_unlock(&rm->lock);

    *cpu = ptr;
    return NV_OK;
}

NvU32 nvc_rm_unmap(struct nvc_rm *rm, NvHandle device, NvHandle memory,
                   void *cpu, NvU32 flags)
{
    struct nvc_rm_mapping *m;
    int ret;

    pthread_mutex_lock(&rm->lock);
    m = take_mapping(rm, cpu, memory, false);
    pthread_mutex_unlock(&rm->lock);
    if (m == NULL)
        return NV_ERR_INVALID_ARGUMENT;

    NVOS34_PARAMETERS p = {
        .hClient = rm->client, .hDevice = device, .hMemory = memory,
        .pLinearAddress = m->token, .flags = flags,
    };
    munmap(m->cpu, m->size);
    ret = nv_ioctl(rm->ctl_fd, NV_ESC_RM_UNMAP_MEMORY, &p, sizeof(p));
    close(m->fd);
    free(m);
    return ret ? os_status(ret) : p.status;
}

NvU32 nvc_rm_map_dma(struct nvc_rm *rm, NvHandle device, NvHandle dma,
                     NvHandle memory, NvU64 offset, NvU64 length, NvU32 flags,
                     NvU64 *dma_offset)
{
    NVOS46_PARAMETERS p = {
        .hClient = rm->client,
        .hDevice = device,
        .hDma = dma,
        .hMemory = memory,
        .offset = offset,
        .length = length,
        .flags = flags,
        .dmaOffset = *dma_offset,
    };
    int ret = nv_ioctl(rm->ctl_fd, NV_ESC_RM_MAP_MEMORY_DMA, &p, sizeof(p));

    if (ret == 0 && p.status == NV_OK)
        *dma_offset = p.dmaOffset;
    return ret ? os_status(ret) : p.status;
}

NvU32 nvc_rm_unmap_dma(struct nvc_rm *rm, NvHandle device, NvHandle dma,
                       NvHandle memory, NvU32 flags, NvU64 dma_offset)
{
    NVOS47_PARAMETERS p = {
        .hClient = rm->client,
        .hDevice = device,
        .hDma = dma,
        .hMemory = memory,
        .flags = flags,
        .dmaOffset = dma_offset,
    };
    int ret = nv_ioctl(rm->ctl_fd, NV_ESC_RM_UNMAP_MEMORY_DMA, &p, sizeof(p));

    return ret ? os_status(ret) : p.status;
}
