# Files from open-gpu-kernel-modules

nvchan builds NVIDIA's nvidia-push library and uses the RM ioctl headers.
This is the subset of a patched
[open-gpu-kernel-modules](https://github.com/gulafaran/open-gpu-kernel-modules)
tree (`patches` branch) that the build needs, at the original paths. All of
it is MIT licensed by NVIDIA; see the header of each file.

`VERSION` records the driver version and source commit. RM structures and
the version string must match the running `nvidia.ko`: nvchan refuses to open
a device when the versions differ.

Never edit these files here. Change the kernel tree, then resync:

    tools/sync-kernel-files.sh ../open-gpu-kernel-modules

`FILES` is the list that gets copied. If a build needs a new header, add its
path there and resync. To build against a full tree instead, use
`meson setup build -Dkernel_src=/path/to/open-gpu-kernel-modules`.
