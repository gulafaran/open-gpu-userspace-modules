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

# nvidia-gbm uses nvchan, so nvchan comes first.
if get_option('nvchan').allowed()
  subdir('nvchan')
endif

if get_option('nvidia-gbm').allowed()
  if not get_option('nvchan').allowed()
    error('nvidia-gbm needs nvchan (block-linear CPU access goes through the copy engine)')
  endif
  subdir('nvidia-gbm')
endif
