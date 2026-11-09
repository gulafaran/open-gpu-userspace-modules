/*
 * SPDX-License-Identifier: MIT
 *
 * Event sets (nvmlEventSet*, nvmlDeviceRegisterEvents).
 *
 * Every NVML event type is one RM subdevice notifier. An event set gets,
 * per device, its own /dev/nvidiactl file bound to the device's RM client
 * as an OS event (NV_ESC_ALLOC_OS_EVENT); each registered type is an
 * NV01_EVENT_OS_EVENT object on the subdevice posting to that file, with
 * the notifier armed to repeat. RM queues the events with their info32 /
 * info16 on the file: waiting is poll() plus NV_ESC_RM_GET_EVENT_DATA.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nvml-priv.h"

#include "nvstatus.h"
#include "class/cl0005.h"
#include "class/cl2080_notification.h"
#include "ctrl/ctrl2080/ctrl2080event.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

/* NVML event type bit -> RM notifier; types without one are 0. */
static const NvU32 notifiers[NVML_EVENT_TYPES] = {
    [0]  = NV2080_NOTIFIERS_ECC_SBE,                 /* nvmlEventTypeSingleBitEccError */
    [1]  = NV2080_NOTIFIERS_ECC_DBE,                 /* nvmlEventTypeDoubleBitEccError */
    [2]  = NV2080_NOTIFIERS_PSTATE_CHANGE,           /* nvmlEventTypePState */
    [3]  = NV2080_NOTIFIERS_RC_ERROR,                /* nvmlEventTypeXidCriticalError */
    [4]  = NV2080_NOTIFIERS_CLOCKS_CHANGE,           /* nvmlEventTypeClock */
    [7]  = NV2080_NOTIFIERS_POWER_EVENT,             /* nvmlEventTypePowerSourceChange */
    [8]  = NV2080_NOTIFIERS_SMC_CONFIG_UPDATE,       /* nvmlEventMigConfigChange */
    [12] = NV2080_NOTIFIERS_POISON_ERROR_NON_FATAL,  /* nvmlEventTypeNonFatalPoisonError */
    [13] = NV2080_NOTIFIERS_POISON_ERROR_FATAL,      /* nvmlEventTypeFatalPoisonError */
    [14] = NV2080_NOTIFIERS_GPU_UNAVAILABLE,         /* nvmlEventTypeGpuUnavailableError */
    [15] = NV2080_NOTIFIERS_GPU_RECOVERY_ACTION,     /* nvmlEventTypeGpuRecoveryAction */
};

/* What the closed library offers on any GPU, plus the ECC errors when ECC
 * is enabled. */
#define EVENTS_ALWAYS (nvmlEventTypePState | nvmlEventTypeXidCriticalError | nvmlEventTypeClock | \
                       nvmlEventTypePowerSourceChange | nvmlEventMigConfigChange |                \
                       nvmlEventTypeNonFatalPoisonError | nvmlEventTypeFatalPoisonError |          \
                       nvmlEventTypeGpuUnavailableError | nvmlEventTypeGpuRecoveryAction)
#define EVENTS_ECC    (nvmlEventTypeSingleBitEccError | nvmlEventTypeDoubleBitEccError)

struct event_device {
    struct nvmlDevice_st *dev;  /* NULL: nothing registered for this device */
    int fd;                     /* OS event file */
    unsigned long long types;   /* registered */
    NvHandle objects[NVML_EVENT_TYPES];
};

struct nvmlEventSet_st {
    pthread_mutex_t lock;
    unsigned next;              /* device to look at first, for fairness */
    struct event_device devices[NVML_MAX_DEVICES];
};

static bool ecc_enabled(struct nvmlDevice_st *dev)
{
    NV2080_CTRL_GPU_QUERY_ECC_STATUS_PARAMS *p = calloc(1, sizeof(*p));
    bool on = false;
    if (p == NULL)
        return false;
    if (nvml_ctrl_sub(dev, NV2080_CTRL_CMD_GPU_QUERY_ECC_STATUS, p, sizeof(*p)) == NV_OK)
        for (unsigned i = 0; i < NV2080_CTRL_GPU_ECC_UNIT_COUNT; i++)
            on |= p->units[i].enabled;
    free(p);
    return on;
}

static unsigned long long supported_types(struct nvmlDevice_st *dev)
{
    return EVENTS_ALWAYS | (ecc_enabled(dev) ? EVENTS_ECC : 0);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetSupportedEventTypes(nvmlDevice_t device, unsigned long long *eventTypes)
{
    DEVICE(device, dev);
    if (eventTypes == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    *eventTypes = supported_types(dev);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlEventSetCreate(nvmlEventSet_t *set)
{
    nvmlReturn_t r = nvml_initialized();
    if (r != NVML_SUCCESS)
        return r;
    if (set == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    struct nvmlEventSet_st *s = calloc(1, sizeof(*s));
    if (s == NULL)
        return NVML_ERROR_MEMORY;
    pthread_mutex_init(&s->lock, NULL);
    for (unsigned i = 0; i < NVML_MAX_DEVICES; i++)
        s->devices[i].fd = -1;
    *set = s;
    return NVML_SUCCESS;
}

/*
 * Arms or disarms a notifier on the device. The arming belongs to the RM
 * client, which all event sets of the process share, so it's counted.
 */
static NvU32 notifier_ref(struct nvmlDevice_st *dev, unsigned type, bool get)
{
    NvU32 st = NV_OK;
    pthread_mutex_lock(&dev->lock);
    if (get ? dev->event_refs[type]++ == 0 : --dev->event_refs[type] == 0) {
        NV2080_CTRL_EVENT_SET_NOTIFICATION_PARAMS p = {
            .event = notifiers[type],
            .action = get ? NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_REPEAT
                          : NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_DISABLE,
        };
        st = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION, &p, sizeof(p));
        if (st != NV_OK && get)
            dev->event_refs[type]--;
    }
    pthread_mutex_unlock(&dev->lock);
    return st;
}

/* Disarm first: RM only takes SET_NOTIFICATION while an event object is
 * plugged into the subdevice, and re-arming needs it disarmed. */
static void unregister_type(struct event_device *d, unsigned type)
{
    notifier_ref(d->dev, type, false);
    nvc_rm_free(&d->dev->rm, d->dev->subdevice, d->objects[type]);
    d->objects[type] = 0;
    d->types &= ~(1ULL << type);
}

static NvU32 register_type(struct event_device *d, unsigned type)
{
    struct nvmlDevice_st *dev = d->dev;
    NV0005_ALLOC_PARAMETERS ev = {
        .hParentClient = dev->rm.client,
        .hClass = NV01_EVENT_OS_EVENT,
        .notifyIndex = notifiers[type],   /* with event data (info32/info16) */
        .data = NV_PTR_TO_NvP64((NvUPtr)d->fd),
    };
    NvHandle h = nvc_rm_handle(&dev->rm);
    NvU32 st = nvc_rm_alloc(&dev->rm, dev->subdevice, h, NV01_EVENT_OS_EVENT, &ev);
    if (st != NV_OK)
        return st;
    st = notifier_ref(dev, type, true);
    if (st != NV_OK) {
        nvc_rm_free(&dev->rm, dev->subdevice, h);
        return st;
    }
    d->objects[type] = h;
    d->types |= 1ULL << type;
    return NV_OK;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceRegisterEvents(nvmlDevice_t device, unsigned long long eventTypes,
                                                  nvmlEventSet_t set)
{
    DEVICE(device, dev);
    if (set == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (eventTypes & ~supported_types(dev))
        return NVML_ERROR_NOT_SUPPORTED;

    nvmlReturn_t r = NVML_SUCCESS;
    pthread_mutex_lock(&set->lock);
    struct event_device *d = &set->devices[dev->index];
    if (d->dev == NULL) {
        d->fd = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
        if (d->fd < 0) {
            r = NVML_ERROR_UNKNOWN;
            goto out;
        }
        NvU32 st = nvc_rm_alloc_os_event(&dev->rm, d->fd);
        if (st != NV_OK) {
            close(d->fd);
            d->fd = -1;
            r = nvml_from_rm(st);
            goto out;
        }
        d->dev = dev;
    }
    for (unsigned t = 0; t < NVML_EVENT_TYPES; t++) {
        if (!(eventTypes & (1ULL << t)) || (d->types & (1ULL << t)))
            continue;
        NvU32 st = register_type(d, t);
        if (st != NV_OK) {
            r = nvml_from_rm(st);
            break;
        }
    }
out:
    pthread_mutex_unlock(&set->lock);
    return r;
}

static unsigned long long type_of(const struct event_device *d, NvU32 notifier)
{
    for (unsigned t = 0; t < NVML_EVENT_TYPES; t++)
        if ((d->types & (1ULL << t)) && notifiers[t] == notifier)
            return 1ULL << t;
    return 0;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The set's lock is held only while collecting the files and reading an
 * event, not while sleeping: freeing a set someone waits on is the
 * caller's bug, as with the closed library. */
static nvmlReturn_t wait_event(nvmlEventSet_t set, nvmlEventData_t *data, unsigned timeoutms)
{
    struct pollfd pfd[NVML_MAX_DEVICES];
    unsigned idx[NVML_MAX_DEVICES];
    long long deadline = now_ms() + timeoutms;

    for (;;) {
        unsigned n = 0;
        pthread_mutex_lock(&set->lock);
        for (unsigned k = 0; k < NVML_MAX_DEVICES; k++) {
            unsigned i = (set->next + k) % NVML_MAX_DEVICES;
            if (set->devices[i].dev == NULL)
                continue;
            pfd[n] = (struct pollfd){ .fd = set->devices[i].fd, .events = POLLIN | POLLPRI };
            idx[n++] = i;
        }
        pthread_mutex_unlock(&set->lock);

        long long left = deadline - now_ms();
        int ret = poll(pfd, n, left > 0 ? (int)left : 0);
        if (ret < 0)
            return errno == EINTR ? NVML_ERROR_TIMEOUT : NVML_ERROR_UNKNOWN;
        if (ret == 0)
            return NVML_ERROR_TIMEOUT;

        for (unsigned k = 0; k < n; k++) {
            if (!pfd[k].revents)
                continue;
            pthread_mutex_lock(&set->lock);
            struct event_device *d = &set->devices[idx[k]];
            NvUnixEvent ev = { 0 };
            NvU32 more = 0;
            NvU32 st = d->dev ? nvc_rm_get_event_data(&d->dev->rm, d->fd, &ev, &more) : NV_ERR_INVALID_STATE;
            unsigned long long type = st == NV_OK ? type_of(d, ev.NotifyIndex) : 0;
            if (type != 0) {
                data->device = d->dev;
                data->eventType = type;
                data->eventData = type == nvmlEventTypeXidCriticalError ? ev.info32 : 0;
                data->gpuInstanceId = 0xffffffff;      /* no MIG */
                data->computeInstanceId = 0xffffffff;
                set->next = idx[k] + 1;
            }
            pthread_mutex_unlock(&set->lock);
            if (type != 0)
                return NVML_SUCCESS;
            if (st == NV_ERR_GPU_IS_LOST)
                return NVML_ERROR_GPU_IS_LOST;
        }
        /* Woken without an event for us (already taken, or a type that was
         * unregistered): keep waiting for the rest of the timeout. */
    }
}

NVML_EXPORT nvmlReturn_t nvmlEventSetWait_v2(nvmlEventSet_t set, nvmlEventData_t *data, unsigned int timeoutms)
{
    nvmlReturn_t r = nvml_initialized();
    if (r != NVML_SUCCESS)
        return r;
    if (set == NULL || data == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    return wait_event(set, data, timeoutms);
}

NVML_EXPORT nvmlReturn_t nvmlEventSetWait(nvmlEventSet_t set, nvmlEventData_t *data, unsigned int timeoutms)
{
    return nvmlEventSetWait_v2(set, data, timeoutms);
}

NVML_EXPORT nvmlReturn_t nvmlEventSetFree(nvmlEventSet_t set)
{
    nvmlReturn_t r = nvml_initialized();
    if (r != NVML_SUCCESS)
        return r;
    if (set == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    for (unsigned i = 0; i < NVML_MAX_DEVICES; i++) {
        struct event_device *d = &set->devices[i];
        if (d->dev == NULL)
            continue;
        for (unsigned t = 0; t < NVML_EVENT_TYPES; t++)
            if (d->types & (1ULL << t))
                unregister_type(d, t);
        nvc_rm_free_os_event(&d->dev->rm, d->fd);
        close(d->fd);
    }
    pthread_mutex_destroy(&set->lock);
    free(set);
    return NVML_SUCCESS;
}
