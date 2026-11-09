/*
 * SPDX-License-Identifier: MIT
 *
 * libnvidia-ml (NVML) on the RM API of the open kernel modules.
 *
 * Every GPU gets its own RM client (struct nvc_rm, from nvchan) with a
 * device and subdevice object, plus an RM_USER_SHARED_DATA (RUSD) page:
 * GSP-RM writes the live sensor values (clocks, temperatures, power,
 * utilization, P-state, ...) there, so reading them costs no RM call.
 * Values that don't change are queried once when the device is opened.
 */

#ifndef NVML_PRIV_H
#define NVML_PRIV_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define NVML_NO_UNVERSIONED_FUNC_DEFS
#include "nvml.h"

#include "rm.h"
#include "class/cl00de.h"

#define NVML_EXPORT __attribute__((visibility("default")))

#define NVML_MAX_DEVICES 32
#define NVML_EVENT_TYPES 16      /* event type bits with an RM notifier, events.c */

struct nvmlDevice_st {
    unsigned index;             /* NVML index, PCI bus order */
    int minor;                  /* /dev/nvidia<minor> */
    NvU32 gpu_id;               /* RM GPU id */
    NvU32 device_instance;

    struct nvc_rm rm;
    NvHandle device, subdevice, rusd_obj;
    NvHandle display;           /* NV04_DISPLAY_COMMON, lazily (lock) */
    NvHandle diag;              /* NV20_SUBDEVICE_DIAG, lazily (lock) */
    unsigned event_refs[NVML_EVENT_TYPES];   /* event sets armed per type (lock) */
    NV00DE_SHARED_DATA *rusd;   /* NULL if RUSD isn't available */
    NvU64 rusd_mask;            /* NV00DE_RUSD_POLL_* requested (lock) */

    /* Static information, filled in at open. */
    char name[NVML_DEVICE_NAME_V2_BUFFER_SIZE];
    char uuid[NVML_DEVICE_UUID_V2_BUFFER_SIZE];
    char bus_id[NVML_DEVICE_PCI_BUS_ID_BUFFER_SIZE];          /* 0000:01:00.0 */
    char bus_id_legacy[NVML_DEVICE_PCI_BUS_ID_BUFFER_V2_SIZE];
    nvmlPciInfo_t pci;
    char vbios[NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE];
    char chip_sku[32];          /* GPU part number, e.g. 27E0-750-A1 */
    NvU32 arch;                 /* NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_* */
    NvU32 brand;                /* nvmlBrandType_t */
    NvU32 link_caps, root_link_caps;   /* NV2080_CTRL_BUS_INFO_PCIE_*_LINK_CAPS */
    NvU64 fb_total;             /* bytes */
    bool classes_known;         /* gfx_class/compute_class looked up (lock) */
    NvU32 gfx_class, compute_class;
    bool coolers_known;         /* cooler_mask looked up (lock) */
    NvU32 cooler_mask;          /* fan coolers the board has */
    bool policies_known;        /* thermal policy info looked up (lock) */
    NvU32 policy_mask;          /* thermal policies the board has */
    NvU8 policy_gps, policy_acoustic, policy_mem, policy_slowdown;  /* indexes */
    NvS32 tlimit_ref;           /* slowdown policy's rated limit, 1/256 C; 0: none */
    bool pstates_known;         /* P-state / clock domain info looked up (lock) */
    NvU32 pstate_mask;          /* P-state board objects */
    int top_pstate;             /* object index of the fastest P-state; -1: none */
    int clk_index[NVML_CLOCK_COUNT];   /* clock domain object per nvmlClockType_t; -1 */
    unsigned num_pclks;         /* P-states with clock ranges below */
    struct { NvU8 pstate; NvU32 mem, gpc_min, gpc_max; } pclk[32];   /* Pn order; kHz */
    bool clocks_known;          /* gpc_freqs, min_target_mhz looked up (lock) */
    NvU32 min_target_mhz;       /* lowest graphics clock the NAFLL can target */
    unsigned num_gpc_freqs;
    NvU16 gpc_freqs[512];       /* graphics clock frequency points, MHz, ascending */
    bool power_known;           /* power policy limits looked up (lock) */
    NvU32 power_min, power_default, power_max;   /* mW; 0: unsupported */

    pthread_mutex_t lock;
};

struct nvml_state {
    pthread_mutex_t lock;       /* init count and device table */
    unsigned init_count;
    struct nvc_rm sys;          /* GPU-less client for NV0000 queries */
    unsigned num_devices;
    struct nvmlDevice_st *devices[NVML_MAX_DEVICES];
};

extern struct nvml_state nvml;

/* Validates a handle and the library state. */
nvmlReturn_t nvml_device(nvmlDevice_t handle, struct nvmlDevice_st **dev);
nvmlReturn_t nvml_initialized(void);

/* RM status -> NVML return code. */
nvmlReturn_t nvml_from_rm(NvU32 status);

/* RM controls on the device's own client. */
NvU32 nvml_ctrl_sub(struct nvmlDevice_st *dev, NvU32 cmd, void *params, NvU32 size);
NvU32 nvml_ctrl_dev(struct nvmlDevice_st *dev, NvU32 cmd, void *params, NvU32 size);

/*
 * Copies a polled RUSD field consistently (see RusdReadData in cl00de.h).
 * field points at a struct starting with the volatile timestamp; data and
 * size describe the part after it. Returns false if the data isn't valid.
 */
void nvml_rusd_request(struct nvmlDevice_st *dev, NvU64 mask);
bool nvml_rusd_read(struct nvmlDevice_st *dev, volatile NvU64 *timestamp,
                    void *src, void *dst, size_t size);

#define NVML_RUSD_READ(dev, field, dst) \
    ((dev)->rusd != NULL && \
     nvml_rusd_read((dev), &(dev)->rusd->field.lastModifiedTimestamp, \
                    (void *)&(dev)->rusd->field.info, (dst), sizeof(*(dst))))

/* T.Limit field values (NVML_FI_DEV_TEMPERATURE_*_TLIMIT), thermal.c. */
nvmlReturn_t nvml_tlimit_field(struct nvmlDevice_st *dev, unsigned field, int *value);

/* TGP power policy limits in mW (power.c). */
nvmlReturn_t nvml_power_limits(struct nvmlDevice_st *dev, unsigned *min, unsigned *def, unsigned *max);

/* GPU recovery action / reset (or drain) recommended, status.c. */
nvmlReturn_t nvml_recovery_action(struct nvmlDevice_st *dev, unsigned *action);
nvmlReturn_t nvml_reset_status(struct nvmlDevice_st *dev, bool drain, unsigned *value);

/* One PCIe counter (NV2080_CTRL_BUS_PEX_COUNTER_* bit), info.c. */
nvmlReturn_t nvml_pex_counter(struct nvmlDevice_st *dev, NvU32 bit, unsigned *value);

/* Copies a string into a caller buffer, NVML-style. */
nvmlReturn_t nvml_copy_string(char *dst, unsigned length, const char *src);

/* Private export table, see etbl.c. */
NVML_EXPORT nvmlReturn_t nvmlInternalGetExportTable(const void **table,
                                                    const unsigned char *id);

#endif
