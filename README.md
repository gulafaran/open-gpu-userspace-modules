# open-gpu-userspace-modules

Open replacements for pieces of the closed NVIDIA userspace driver, built on
the nvidia-drm uapi of a patched
[open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
tree.

## Modules

| Directory | Replaces | Status |
|-----------|----------|--------|
| `nvidia-gbm/` | `nvidia-drm_gbm.so` (`libnvidia-allocator.so`) | bo_create, bo_import (dma-buf); no map or surfaces yet |

## Build

    meson setup build
    meson compile -C build

## nvidia-gbm

Installs `$libdir/gbm/nvidia-open_gbm.so` next to the driver's
`nvidia-drm_gbm.so` without replacing it. Try it in one process:

    GBM_BACKEND=nvidia-open <program>

Use it globally by pointing `/usr/lib64/gbm/nvidia-drm_gbm.so` at it, and go
back by pointing that symlink at `../libnvidia-allocator.so.1` again.
EGL/GL keep loading `libnvidia-allocator.so.1` by name, so they are not affected.

The backend reports the name `nvidia` because `libnvidia-egl-gbm` only
handles GBM devices with that backend name.

### Tests

- `gbm-layout`: prints the modifier, strides, offsets and dma-buf size for a
  matrix of sizes, formats, usages and modifier lists. Run it with both
  backends and diff the output.
- `gbm-egl`: creates an EGL display on the GBM device, imports every BO as
  a dma-buf, renders into it and reads it back.
- `gbm-import`: import round trips, shared GEM handles, an EGL-exported
  client buffer, AddFB2 of imported buffers (primary node) and invalid
  imports.
