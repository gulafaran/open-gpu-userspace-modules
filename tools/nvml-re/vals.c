#include "nvml.h"
#include <stdio.h>
#define P(label, call, fmt, ...) do { nvmlReturn_t r_ = call; if (r_) printf("%-28s %s\n", label, nvmlErrorString(r_)); else printf("%-28s " fmt "\n", label, __VA_ARGS__); } while (0)
int main(void) { nvmlDevice_t d; unsigned u, u2; char s[256]; int i; nvmlMemory_t m; nvmlMemory_v2_t m2 = { .version = nvmlMemory_v2 };
  nvmlUtilization_t ut; nvmlPciInfo_t pci; nvmlBAR1Memory_t b; nvmlPstates_t ps; nvmlDeviceArchitecture_t ar; nvmlComputeMode_t cm;
  nvmlInit_v2(); nvmlDeviceGetHandleByIndex_v2(0, &d);
  P("driver", nvmlSystemGetDriverVersion(s, sizeof s), "%s", s);
  P("nvml version", nvmlSystemGetNVMLVersion(s, sizeof s), "%s", s);
  P("cuda version", nvmlSystemGetCudaDriverVersion_v2(&i), "%d", i);
  P("name", nvmlDeviceGetName(d, s, sizeof s), "%s", s);
  P("uuid", nvmlDeviceGetUUID(d, s, sizeof s), "%s", s);
  P("pci", nvmlDeviceGetPciInfo_v3(d, &pci), "%s / %s dom %x bus %x dev %x id %08x sub %08x", pci.busId, pci.busIdLegacy, pci.domain, pci.bus, pci.device, pci.pciDeviceId, pci.pciSubSystemId);
  P("minor", nvmlDeviceGetMinorNumber(d, &u), "%u", u);
  P("arch", nvmlDeviceGetArchitecture(d, &ar), "%u", ar);
  P("memory v1", nvmlDeviceGetMemoryInfo(d, &m), "total %llu free %llu used %llu", m.total, m.free, m.used);
  P("memory v2", nvmlDeviceGetMemoryInfo_v2(d, &m2), "total %llu free %llu used %llu reserved %llu", m2.total, m2.free, m2.used, m2.reserved);
  P("bar1", nvmlDeviceGetBAR1MemoryInfo(d, &b), "total %llu free %llu used %llu", b.bar1Total, b.bar1Free, b.bar1Used);
  P("util", nvmlDeviceGetUtilizationRates(d, &ut), "gpu %u mem %u", ut.gpu, ut.memory);
  P("enc", nvmlDeviceGetEncoderUtilization(d, &u, &u2), "%u (%u us)", u, u2);
  P("dec", nvmlDeviceGetDecoderUtilization(d, &u, &u2), "%u (%u us)", u, u2);
  P("temp", nvmlDeviceGetTemperature(d, NVML_TEMPERATURE_GPU, &u), "%u", u);
  for (int c = 0; c < 4; c++) { char l[32]; snprintf(l, 32, "clock %d", c); P(l, nvmlDeviceGetClockInfo(d, c, &u), "%u", u); }
  P("power", nvmlDeviceGetPowerUsage(d, &u), "%u mW", u);
  P("enforced limit", nvmlDeviceGetEnforcedPowerLimit(d, &u), "%u mW", u);
  P("pstate", nvmlDeviceGetPerformanceState(d, &ps), "P%d", ps);
  P("pcie gen cur", nvmlDeviceGetCurrPcieLinkGeneration(d, &u), "%u", u);
  P("pcie gen max", nvmlDeviceGetMaxPcieLinkGeneration(d, &u), "%u", u);
  P("pcie gen gpu max", nvmlDeviceGetGpuMaxPcieLinkGeneration(d, &u), "%u", u);
  P("pcie width cur", nvmlDeviceGetCurrPcieLinkWidth(d, &u), "%u", u);
  P("pcie width max", nvmlDeviceGetMaxPcieLinkWidth(d, &u), "%u", u);
  P("compute mode", nvmlDeviceGetComputeMode(d, &cm), "%u", cm);
  nvmlShutdown(); }
