Tools used to work out what the closed libnvidia-ml (620.70.06) does, for
nvidia-ml/. Not built; compile by hand against nvml.h.

- `iolog.c`: LD_PRELOAD library that logs NVIDIA RM escapes (control
  command, alloc class, status, duration) and mmaps of /dev/nvidia* to
  stderr.
- `probe.c`: calls NVML functions one by one with a marker before each, so
  with iolog the RM calls behind every function can be read off
  (`probe.log` is a run against the closed library).
- `trace.gdb`: dprintf on every NVML export; `slots.gdb`: the same for the
  slots of the private export table nvidia-smi uses; `et.gdb`: makes
  nvmlInternalGetExportTable fail; `dump.gdb`: dumps the table.
- `vals.c`: prints ~30 values; run it against the closed library and
  against ours (LD_LIBRARY_PATH) and diff.
- `slots-*.txt`: slot indices nvidia-smi called per mode (plain, -q,
  --query-gpu, dmon, pmon, -L, -q -d).
