#include <nvml.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void mark(const char *s) { char b[256]; int n = snprintf(b, sizeof b, "== %s\n", s); write(2, b, n); }
#define C(name, ...) do { mark(#name); nvmlReturn_t r_ = name(__VA_ARGS__); fprintf(stderr, "   -> %s\n", nvmlErrorString(r_)); } while (0)
int main(void) {
    nvmlDevice_t d; unsigned u, u2; char s[256]; unsigned long long ull; int i;
    nvmlMemory_t m; nvmlMemory_v2_t m2 = { .version = nvmlMemory_v2 }; nvmlUtilization_t ut; nvmlPciInfo_t pci;
    nvmlBAR1Memory_t bar1; nvmlPstates_t ps; nvmlProcessInfo_t procs[32]; nvmlEnableState_t en, en2;
    C(nvmlInit_v2);
    C(nvmlSystemGetDriverVersion, s, sizeof s);
    C(nvmlSystemGetNVMLVersion, s, sizeof s);
    C(nvmlSystemGetCudaDriverVersion_v2, &i);
    C(nvmlDeviceGetCount_v2, &u);
    C(nvmlDeviceGetHandleByIndex_v2, 0, &d);
    C(nvmlDeviceGetPciInfo_v3, d, &pci);
    C(nvmlDeviceGetHandleByPciBusId_v2, pci.busId, &d);
    C(nvmlDeviceGetHandleByUUID, "GPU-c14b4260-e302-0ed3-abed-c517e7fee34f", &d);
    C(nvmlDeviceGetName, d, s, sizeof s);
    C(nvmlDeviceGetUUID, d, s, sizeof s);
    C(nvmlDeviceGetIndex, d, &u);
    C(nvmlDeviceGetMinorNumber, d, &u);
    C(nvmlDeviceGetBrand, d, (nvmlBrandType_t *)&u);
    C(nvmlDeviceGetArchitecture, d, (nvmlDeviceArchitecture_t *)&u);
    C(nvmlDeviceGetVbiosVersion, d, s, sizeof s);
    C(nvmlDeviceGetMemoryInfo, d, &m);
    C(nvmlDeviceGetMemoryInfo_v2, d, &m2);
    C(nvmlDeviceGetBAR1MemoryInfo, d, &bar1);
    C(nvmlDeviceGetUtilizationRates, d, &ut);
    C(nvmlDeviceGetEncoderUtilization, d, &u, &u2);
    C(nvmlDeviceGetDecoderUtilization, d, &u, &u2);
    C(nvmlDeviceGetTemperature, d, NVML_TEMPERATURE_GPU, &u);
    C(nvmlDeviceGetTemperatureThreshold, d, NVML_TEMPERATURE_THRESHOLD_SLOWDOWN, &u);
    C(nvmlDeviceGetTemperatureThreshold, d, NVML_TEMPERATURE_THRESHOLD_SHUTDOWN, &u);
    C(nvmlDeviceGetClockInfo, d, NVML_CLOCK_GRAPHICS, &u);
    C(nvmlDeviceGetClockInfo, d, NVML_CLOCK_SM, &u);
    C(nvmlDeviceGetClockInfo, d, NVML_CLOCK_MEM, &u);
    C(nvmlDeviceGetClockInfo, d, NVML_CLOCK_VIDEO, &u);
    C(nvmlDeviceGetMaxClockInfo, d, NVML_CLOCK_GRAPHICS, &u);
    C(nvmlDeviceGetMaxClockInfo, d, NVML_CLOCK_MEM, &u);
    C(nvmlDeviceGetClock, d, NVML_CLOCK_GRAPHICS, NVML_CLOCK_ID_CURRENT, &u);
    C(nvmlDeviceGetPowerUsage, d, &u);
    C(nvmlDeviceGetTotalEnergyConsumption, d, &ull);
    C(nvmlDeviceGetPowerManagementLimit, d, &u);
    C(nvmlDeviceGetEnforcedPowerLimit, d, &u);
    C(nvmlDeviceGetPowerManagementDefaultLimit, d, &u);
    C(nvmlDeviceGetPowerManagementLimitConstraints, d, &u, &u2);
    C(nvmlDeviceGetPowerState, d, &ps);
    C(nvmlDeviceGetPerformanceState, d, &ps);
    C(nvmlDeviceGetFanSpeed, d, &u);
    C(nvmlDeviceGetNumFans, d, &u);
    C(nvmlDeviceGetCurrentClocksEventReasons, d, &ull);
    C(nvmlDeviceGetSupportedClocksEventReasons, d, &ull);
    C(nvmlDeviceGetPcieThroughput, d, NVML_PCIE_UTIL_TX_BYTES, &u);
    C(nvmlDeviceGetPcieThroughput, d, NVML_PCIE_UTIL_RX_BYTES, &u);
    C(nvmlDeviceGetCurrPcieLinkGeneration, d, &u);
    C(nvmlDeviceGetCurrPcieLinkWidth, d, &u);
    C(nvmlDeviceGetMaxPcieLinkGeneration, d, &u);
    C(nvmlDeviceGetMaxPcieLinkWidth, d, &u);
    C(nvmlDeviceGetPersistenceMode, d, &en);
    C(nvmlDeviceGetDisplayMode, d, &en);
    C(nvmlDeviceGetDisplayActive, d, &en);
    C(nvmlDeviceGetComputeMode, d, (nvmlComputeMode_t *)&u);
    C(nvmlDeviceGetEccMode, d, &en, &en2);
    u = 32; C(nvmlDeviceGetGraphicsRunningProcesses_v3, d, &u, procs);
    u = 32; C(nvmlDeviceGetComputeRunningProcesses_v3, d, &u, procs);
    C(nvmlSystemGetProcessName, procs[0].pid, s, sizeof s);
    C(nvmlShutdown);
    return 0;
}
