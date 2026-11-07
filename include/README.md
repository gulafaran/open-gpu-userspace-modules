# Copied kernel uapi headers

These are copied verbatim from the patched open-gpu-kernel-modules tree
(`patches` branch). Our kernel patch stack changes them, so resync after
adding or changing ioctls there and record the source commit below.

| File | Source path | Source commit |
|------|-------------|---------------|
| `nvidia-drm/nv_drm_common_ioctl.h` | `kernel-open/nvidia-drm/nv_drm_common_ioctl.h` | `cf4c3bcd` (620.70.06 + patches) |

Never edit them here; change the kernel tree and copy again.
