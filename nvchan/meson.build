# nvchan builds against the open-gpu-kernel-modules tree: RM structures and
# nvidia-push must match the running nvidia.ko exactly.
fs = import('fs')
kernel_src = get_option('kernel_src')
if kernel_src == ''
  kernel_src = fs.parent(meson.project_source_root()) / 'open-gpu-kernel-modules'
endif
if not fs.is_file(kernel_src / 'src/common/unix/nvidia-push/src/nvidia-push.c')
  error('open-gpu-kernel-modules not found at ' + kernel_src + ', set -Dkernel_src=')
endif

nvchan_kernel_inc = include_directories(
  kernel_src / 'src/common/unix/nvidia-push/interface',
  kernel_src / 'src/common/unix/nvidia-push/include',
  kernel_src / 'src/common/unix/common/inc',
  kernel_src / 'src/common/sdk/nvidia/inc',
  kernel_src / 'src/common/inc',
  kernel_src / 'src/common/shared/inc',
  kernel_src / 'src/nvidia/arch/nvalloc/unix/include',
  kernel_src / 'kernel-open/common/inc',
  is_system : true)

nvchan_c_args = cc.get_supported_arguments([
  '-DNVCPU_X86_64', '-DNV_LINUX', '-DNV_UNIX',
  '-Wno-sign-compare', '-Wno-missing-field-initializers',
])

# NVIDIA's code; warnings there are not ours to fix.
nvidia_push = static_library('nvidia-push',
  kernel_src / 'src/common/unix/nvidia-push/src/nvidia-push.c',
  kernel_src / 'src/common/unix/nvidia-push/src/nvidia-push-init.c',
  kernel_src / 'src/common/shared/nvstatus/nvstatus.c',
  include_directories : nvchan_kernel_inc,
  c_args : nvchan_c_args + ['-w'],
  pic : true)

libnvchan = static_library('nvchan',
  'src/rm.c',
  'src/device.c',
  'src/ce.c',
  include_directories : [nvchan_kernel_inc, include_directories('include')],
  c_args : nvchan_c_args,
  link_with : nvidia_push,
  dependencies : dependency('threads'),
  pic : true)

nvchan_dep = declare_dependency(
  include_directories : include_directories('include'),
  link_with : libnvchan,
  dependencies : dependency('threads'))

foreach t : ['nvchan-info', 'nvchan-copy']
  executable(t, 'tests' / t + '.c',
    dependencies : nvchan_dep,
    install : false)
endforeach
