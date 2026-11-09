# open-gpu-userspace-modules

Open replacements for pieces of the closed NVIDIA userspace driver, built on
the nvidia-drm uapi of a patched
[open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
tree.

## Modules

| Directory | Replaces | Status |
|-----------|----------|--------|
| `nvidia-gbm/` | `nvidia-drm_gbm.so` and, optionally, `libnvidia-allocator.so.1` | bo_create, bo_import, bo_map, surfaces (via egl-gbm); GEM handle refcounting for NVIDIA EGL/GL/Vulkan |
| `nvchan/` | — (internal static library) | user-space GPU channels on `nvidia.ko`; copy engine, used by nvidia-gbm for block-linear CPU access |
| `nvidia-ml/` | `libnvidia-ml.so.1` (NVML) | **work in progress, experimental**, see [below](#replacing-libnvidia-mlso1-experimental) |

## What conflicts with the driver

Some modules install files under the same names as the NVIDIA driver and
replace its copies. There is no fallback to the driver's library: once one
is replaced, everything that loads it uses this one, bugs included. Those
modules are therefore **disabled by default**; a plain build only installs
files that sit next to the driver's.

| Meson option | Default | Installs | Conflicts with the driver |
|--------------|---------|----------|---------------------------|
| `nvidia-gbm` | enabled | `gbm/nvidia-open_gbm.so` | no, own name, selected with `GBM_BACKEND=nvidia-open` |
| `nvchan` | enabled | nothing (internal) | no |
| `nvidia-allocator` | **disabled** | `libnvidia-allocator.so.1`, `gbm/nvidia-drm_gbm.so` and `gbm/nvidia-open_gbm.so` (symlinks to it) | **yes**: `libnvidia-allocator.so*`, `gbm/nvidia-drm_gbm.so` |
| `nvidia-ml` | **disabled** | `libnvidia-ml.so.1` | **yes**: `libnvidia-ml.so*` |

Before enabling a conflicting module, remove the driver's copies of those
files, or the package manager and `meson install` will overwrite each
other. Do the same for the 32-bit driver libraries if you install the
32-bit build.

## Requirements

- NVIDIA open kernel modules **620.70.06** loaded with `nvidia-drm.modeset=1`.
  The version must match exactly (it's checked when a device is opened).
  The patched tree at
  [gulafaran/open-gpu-kernel-modules](https://github.com/gulafaran/open-gpu-kernel-modules)
  (`patches` branch) is recommended. Among other things it fixes CPU mmaps
  of large video-memory BOs and PRIME export of linear BOs to other GPUs.
- The matching NVIDIA userspace driver (EGL/GL, `libnvidia-egl-gbm`) 620.70.06.
- Mesa >= 21.3 (`libgbm` and `gbm_backend_abi.h`), libdrm >= 2.4.110,
  meson >= 1.1 and a C11 compiler.

The open-gpu-kernel-modules files nvchan needs are vendored in
`third_party/`, so you don't need a kernel tree to build.

## Build and install

    meson setup build --prefix=/usr
    meson compile -C build
    sudo meson install -C build

With the default options this installs only `$libdir/gbm/nvidia-open_gbm.so`
(for example `/usr/lib64/gbm/nvidia-open_gbm.so`). It sits next to the
driver's `nvidia-drm_gbm.so` and does not replace it. The modules that
replace driver files have to be enabled explicitly, see
[What conflicts with the driver](#what-conflicts-with-the-driver).

## Using nvidia-gbm

Select the backend with the `GBM_BACKEND` environment variable:

    GBM_BACKEND=nvidia-open <program>

To use it for the whole desktop session, set it before the compositor starts,
for example in `~/.config/environment.d/gbm.conf`:

    GBM_BACKEND=nvidia-open

or in `/etc/environment`, then log in again. To go back, remove the line and
log in again. If the backend can't open the device, Mesa falls back to the
driver's `nvidia-drm_gbm.so`.

If you installed to a prefix Mesa doesn't search (such as `~/.local`), also
set `GBM_BACKENDS_PATH`, for example
`GBM_BACKENDS_PATH=$HOME/.local/lib64/gbm:/usr/lib64/gbm`.

EGL/GL keep loading `libnvidia-allocator.so.1` by name, so this only affects
GBM users (compositors, Xwayland, browsers, and so on).

## Replacing libnvidia-allocator.so.1

With `-Dnvidia-allocator=enabled` the build installs
`$libdir/libnvidia-allocator.so.1` instead of the GBM module, plus
`gbm/nvidia-drm_gbm.so` and `gbm/nvidia-open_gbm.so` symlinks to it, the same
layout as the driver. Mesa then uses it for nvidia-drm without
`GBM_BACKEND`, and NVIDIA's EGL, GL and Vulkan libraries (which `dlopen`
`libnvidia-allocator.so.1` for `GEMAddReference`/`GEMRemoveReference`) share
its GEM handle table. There is no fallback to the driver's copy, so remove
the driver's `libnvidia-allocator.so*` and `gbm/nvidia-drm_gbm.so` first.

32-bit GL and Vulkan load the 32-bit library. nvchan is x86_64 only, so a
32-bit build is the allocator alone:

    CC="gcc -m32" PKG_CONFIG_LIBDIR=/usr/lib/pkgconfig \
        meson setup build32 --prefix=/usr --libdir=lib \
        -Dnvidia-allocator=enabled -Dnvidia-gbm=disabled -Dnvchan=disabled

The driver's library also exports `NvRmShim*` functions; nothing in the
desktop driver uses them, so they are not provided.

The backend reports the name `nvidia` because `libnvidia-egl-gbm` only
handles GBM devices with that backend name.

Environment options:

- `NV_GBM_LINEAR_RENDER=1`: allow `GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING`,
  which the closed allocator refuses (NVIDIA GL can't render to pitch-linear
  surfaces).

### Tests

The tests aren't installed. Run them from the build directory against the
backend you just built:

    GBM_BACKENDS_PATH=build/nvidia-gbm GBM_BACKEND=nvidia-open build/nvidia-gbm/gbm-probe

- `gbm-layout`: prints the modifier, strides, offsets and dma-buf size for a
  matrix of sizes, formats, usages and modifier lists. Run it with both
  backends and diff the output.
- `gbm-egl`: creates an EGL display on the GBM device, imports every BO as
  a dma-buf, renders into it and reads it back.
- `gbm-import`: import round trips, shared GEM handles, an EGL-exported
  client buffer, AddFB2 of imported buffers (primary node) and invalid
  imports.
- `gbm-map`: CPU maps checked against GL rendering and sampling, plus an
  independent detile of a raw mapping.
- `gbm-surface`: EGL window surfaces on GBM surfaces like a KMS compositor
  uses them (swap, lock front buffer, AddFB2, release).

## Replacing libnvidia-ml.so.1 (experimental)

`nvidia-ml/` is a replacement for NVML, the library behind `nvidia-smi`,
nvtop, MangoHud, btop, conky, LACT and others. **It is a work in progress
and experimental**: expect missing values and wrong ones, and don't use it
where you depend on accurate monitoring or on changing GPU settings. It has
only been tested on one laptop GPU.

    meson setup build --prefix=/usr -Dnvidia-ml=enabled

installs `$libdir/libnvidia-ml.so.1`, which replaces the driver's (see
[What conflicts with the driver](#what-conflicts-with-the-driver)). It talks
to `nvidia.ko` through nvchan's RM layer, which also builds for 32-bit x86,
so a 32-bit build can contain NVML:

    CC="gcc -m32" PKG_CONFIG_LIBDIR=/usr/lib/pkgconfig \
        meson setup build32 --prefix=/usr --libdir=lib \
        -Dnvidia-ml=enabled -Dnvidia-gbm=disabled -Dnvchan=disabled

Current state:

- Every function the driver's library exports (listed in
  `nvidia-ml/exports.txt`) exists, so programs that resolve all symbols at
  load time work. Functions that aren't implemented yet return
  `NVML_ERROR_NOT_SUPPORTED`; nothing is forwarded to the closed library.
- Implemented: device enumeration and identification, memory, BAR1,
  utilization (including encoder, decoder, JPEG and OFA), clocks (current
  and maximum) and clock event reasons, P-states, temperature,
  temperature thresholds and margin, power draw and limits, fan count and
  RPM, PCIe link, running processes with their memory, per-process
  utilization, field values, and the parts of NVIDIA's private export
  table that `nvidia-smi` needs for its default view, `-q`, `--query-gpu`,
  `dmon` and `pmon`.
- Missing or partial: a number of `nvidia-smi -q` fields still show N/A
  (application and supported clocks, violation counters, energy, remapped
  rows, encoder capacity, ...), events, fan speed in percent, and every
  function that changes a setting (fan control, power limit, clocks, ...);
  those return `NVML_ERROR_NOT_SUPPORTED`.
- Behaviour the GPU or driver doesn't support is reported the same way as
  the closed library does, but this is only verified on the tested GPU.

`build/nvidia-ml/nvml-probe` calls a list of NVML functions and prints the
result of each; run it with `LD_LIBRARY_PATH` pointing at the closed and at
this library and diff the output. The tools in `tools/nvml-re/` were used
to work out what the closed library does.
