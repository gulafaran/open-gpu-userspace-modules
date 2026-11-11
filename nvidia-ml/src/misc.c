/*
 * SPDX-License-Identifier: MIT
 *
 * Queries for features a GeForce board on Linux doesn't have: Windows
 * driver models, GPU operation modes, vGPU metadata, C2C links, clock
 * monitors, workload power profiles and the like. The closed library
 * answers them with NOT_SUPPORTED after checking its arguments; so do we,
 * with the same checks in the same order.
 */

#include "nvml-priv.h"

#define DEVICE(handle)                                                 \
    do {                                                               \
        struct nvmlDevice_st *dev_;                                    \
        nvmlReturn_t r_ = nvml_device((handle), &dev_);                \
        if (r_ != NVML_SUCCESS)                                        \
            return r_;                                                 \
    } while (0)

#define CHECK_PTR(p) do { if ((p) == NULL) return NVML_ERROR_INVALID_ARGUMENT; } while (0)

#define CHECK_VERSION(p, v) \
    do { if ((p)->version != (v)) return NVML_ERROR_ARGUMENT_VERSION_MISMATCH; } while (0)

/* Unsupported getter taking one output pointer. */
#define NOT_SUPPORTED_PTR(name, type)                                  \
    NVML_EXPORT nvmlReturn_t name(nvmlDevice_t device, type *out)      \
    {                                                                  \
        DEVICE(device);                                                \
        CHECK_PTR(out);                                                \
        return NVML_ERROR_NOT_SUPPORTED;                               \
    }

/* The same with a versioned struct. */
#define NOT_SUPPORTED_VERSIONED(name, type, version)                   \
    NVML_EXPORT nvmlReturn_t name(nvmlDevice_t device, type *out)      \
    {                                                                  \
        DEVICE(device);                                                \
        CHECK_PTR(out);                                                \
        CHECK_VERSION(out, version);                                   \
        return NVML_ERROR_NOT_SUPPORTED;                               \
    }

NOT_SUPPORTED_PTR(nvmlDeviceGetAttributes_v2, nvmlDeviceAttributes_t)
NOT_SUPPORTED_PTR(nvmlDeviceGetAttributes, nvmlDeviceAttributes_t)
NOT_SUPPORTED_PTR(nvmlDeviceGetClkMonStatus, nvmlClkMonStatus_t)
NOT_SUPPORTED_PTR(nvmlDeviceGetC2cModeInfoV, nvmlC2cModeInfo_v1_t)
NOT_SUPPORTED_PTR(nvmlDeviceGetHostname_v1, nvmlHostname_v1_t)
NOT_SUPPORTED_PTR(nvmlDeviceGetAdaptiveTgpModeInfo_v1, nvmlAdaptiveTgpModeInfo_v1_t)
NOT_SUPPORTED_PTR(nvmlDevicePerfMetricsGetSamples_v1, nvmlPerfMetricsSamples_v1_t)

NOT_SUPPORTED_VERSIONED(nvmlDeviceGetCoolerInfo, nvmlCoolerInfo_t, nvmlCoolerInfo_v1)
NOT_SUPPORTED_VERSIONED(nvmlDeviceWorkloadPowerProfileGetProfilesInfo,
                        nvmlWorkloadPowerProfileProfilesInfo_t, nvmlWorkloadPowerProfileProfilesInfo_v1)
NOT_SUPPORTED_VERSIONED(nvmlDeviceWorkloadPowerProfileGetCurrentProfiles,
                        nvmlWorkloadPowerProfileCurrentProfiles_t, nvmlWorkloadPowerProfileCurrentProfiles_v1)

/* WDDM/TCC: Windows only. Either pointer may be NULL, not both. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetDriverModel_v2(nvmlDevice_t device, nvmlDriverModel_t *current,
                                                     nvmlDriverModel_t *pending)
{
    DEVICE(device);
    if (current == NULL && pending == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    return NVML_ERROR_NOT_SUPPORTED;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetDriverModel(nvmlDevice_t device, nvmlDriverModel_t *current,
                                                  nvmlDriverModel_t *pending)
{
    return nvmlDeviceGetDriverModel_v2(device, current, pending);
}

/* GOM exists on Tesla/Quadro boards only; closed checks nothing first. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetGpuOperationMode(nvmlDevice_t device, nvmlGpuOperationMode_t *current,
                                                       nvmlGpuOperationMode_t *pending)
{
    DEVICE(device);
    return NVML_ERROR_NOT_SUPPORTED;
}

/* vGPU host metadata. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetPgpuMetadataString(nvmlDevice_t device, char *pgpuMetadata,
                                                         unsigned int *bufferSize)
{
    DEVICE(device);
    CHECK_PTR(bufferSize);
    return NVML_ERROR_NOT_SUPPORTED;
}
