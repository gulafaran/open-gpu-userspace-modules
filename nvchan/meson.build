# nvchan builds against the open-gpu-kernel-modules tree: RM structures and
# nvidia-push must match the running nvidia.ko exactly.
fs = import('fs')
# By default the copy in third_party/ (tools/sync-kernel-files.sh); set
# -Dkernel_src= to build against a full open-gpu-kernel-modules tree instead.
kernel_src = get_option('kernel_src')
if kernel_src == ''
  # Relative: include_directories() refuses absolute paths into the source tree.
  kernel_src = '..' / 'third_party/open-gpu-kernel-modules'
endif
if not fs.is_file(kernel_src / 'src/common/unix/nvidia-push/src/nvidia-push.c')
  error('open-gpu-kernel-modules not found at ' + kernel_src + ', set -Dkernel_src=')
endif

# The third_party copy only has the directories that hold used headers.
nvchan_kernel_dirs = []
foreach d : [
    kernel_src / 'src/common/unix/nvidia-push/interface',
    kernel_src / 'src/common/unix/nvidia-push/include',
    kernel_src / 'src/common/unix/common/inc',
    kernel_src / 'src/common/sdk/nvidia/inc',
    kernel_src / 'src/common/inc',
    kernel_src / 'src/common/shared/inc',
    kernel_src / 'src/nvidia/arch/nvalloc/unix/include',
    kernel_src / 'kernel-open/common/inc',
  ]
  if fs.is_dir(d)
    nvchan_kernel_dirs += d
  endif
endforeach
nvchan_kernel_inc = include_directories(nvchan_kernel_dirs, is_system : true)

nvcpu = host_machine.cpu_family() == 'x86' ? '-DNVCPU_X86' : '-DNVCPU_X86_64'
nvchan_c_args = cc.get_supported_arguments([
  nvcpu, '-DNV_LINUX', '-DNV_UNIX',
  '-Wno-sign-compare', '-Wno-missing-field-initializers',
])

# The RM API layer alone (nvidia-ml needs nothing else); RM's ioctl
# structures are the same for 32-bit clients.
libnvc_rm = static_library('nvc-rm',
  'src/rm.c',
  include_directories : nvchan_kernel_inc,
  c_args : nvchan_c_args,
  dependencies : dependency('threads'),
  pic : true)

if not get_option('nvchan').allowed()
  subdir_done()
endif
if host_machine.cpu_family() != 'x86_64'
  error('nvchan (channels, nvidia-push) is x86_64 only; disable it with -Dnvchan=disabled')
endif

# NVIDIA's code; warnings there are not ours to fix.
nvidia_push = static_library('nvidia-push',
  kernel_src / 'src/common/unix/nvidia-push/src/nvidia-push.c',
  kernel_src / 'src/common/unix/nvidia-push/src/nvidia-push-init.c',
  kernel_src / 'src/common/shared/nvstatus/nvstatus.c',
  include_directories : nvchan_kernel_inc,
  c_args : nvchan_c_args + ['-w'],
  pic : true)

libnvchan = static_library('nvchan',
  'src/device.c',
  'src/ce.c',
  include_directories : [nvchan_kernel_inc, include_directories('include')],
  c_args : nvchan_c_args,
  link_with : [nvidia_push, libnvc_rm],
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

# Uses the private header for the fault test.
executable('nvchan-stress', 'tests/nvchan-stress.c',
  include_directories : [nvchan_kernel_inc, include_directories('src')],
  c_args : nvchan_c_args,
  dependencies : nvchan_dep,
  install : false)
