project('open-gpu-userspace-modules', 'c',
  version : '0.1.0',
  license : 'MIT',
  meson_version : '>= 1.1',
  default_options : ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-fvisibility=hidden',
  '-Werror=implicit-function-declaration',
  '-Werror=incompatible-pointer-types',
  '-Wno-unused-parameter',
]), language : 'c')

# Shared uapi headers copied from the kernel module tree, see include/README.md.
nv_uapi_inc = include_directories('include')

# nvidia-gbm and nvidia-ml use nvchan, so it comes first. nvidia-ml only
# needs its RM API layer, which also builds for 32-bit x86.
if get_option('nvchan').allowed() or get_option('nvidia-ml').allowed()
  subdir('nvchan')
endif

if get_option('nvidia-gbm').allowed() and not get_option('nvchan').allowed()
  error('nvidia-gbm needs nvchan (block-linear CPU access goes through the copy engine)')
endif

if get_option('nvidia-ml').allowed()
  subdir('nvidia-ml')
endif

# The allocator alone (no GBM backend) is what a 32-bit build installs.
if get_option('nvidia-gbm').allowed() or get_option('nvidia-allocator').enabled()
  subdir('nvidia-gbm')
endif
