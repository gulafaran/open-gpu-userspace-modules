# open-gpu-userspace-modules

Open replacements for pieces of the closed NVIDIA userspace driver, built on
the nvidia-drm uapi of a patched
[open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
tree.

## Modules

| Directory | Replaces | Status |
|-----------|----------|--------|
| `nvidia-gbm/` | `nvidia-drm_gbm.so` (`libnvidia-allocator.so`) | bo_create, bo_import, bo_map, surfaces (via egl-gbm) |
| `nvchan/` | — (internal static library) | user-space GPU channels on `nvidia.ko`; copy engine, used by nvidia-gbm for block-linear CPU access |

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

This installs only `$libdir/gbm/nvidia-open_gbm.so` (for example
`/usr/lib64/gbm/nvidia-open_gbm.so`). It sits next to the driver's
`nvidia-drm_gbm.so` and does not replace it.

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

The backend reports the name `nvidia` because `libnvidia-egl-gbm` only
handles GBM devices with that backend name.

Environment options:

- `NV_GBM_LINEAR_RENDER=1`: allow `GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING`,
  which the closed allocator refuses (NVIDIA GL can't render to pitch-linear
  surfaces).

#### Tests

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
