# libnvidia-ml.so.1 (NVML) on nvchan's RM layer.
py = find_program('python3')

nvml_stubs = custom_target('nvml-stubs',
  input : 'exports.txt',
  output : 'stubs.c',
  command : [py, files('../tools/gen-nvml-stubs.py'), '@INPUT@', '@OUTPUT@'])

# The stubs' int(void) type differs from the real prototypes; LTO would
# reject that, so they stay out of it. The linker still prefers the strong
# definitions over these weak ones.
nvml_stubs_lib = static_library('nvml-stubs', nvml_stubs,
  c_args : cc.get_supported_arguments(['-fno-lto']),
  pic : true)

libnvidia_ml = shared_library('nvidia-ml',
  'src/core.c',
  'src/device.c',
  'src/etbl.c',
  'src/procs.c',
  'src/modes.c',
  'src/fields.c',
  'src/info.c',
  'src/fans.c',
  'src/thermal.c',
  'src/perf.c',
  'src/power.c',
  'src/status.c',
  version : '1.0.0',
  soversion : '1',
  include_directories : [include_directories('include', 'src', '../nvchan/src'),
                         nvchan_kernel_inc],
  # Deprecated NVML functions are implemented on purpose.
  c_args : nvchan_c_args + ['-Wno-deprecated-declarations'],
  link_with : libnvc_rm,
  link_whole : nvml_stubs_lib,
  dependencies : dependency('threads'),
  install : true)

executable('nvml-probe', 'tests/nvml-probe.c',
  c_args : ['-Wno-deprecated-declarations', '-Wno-unused-result'],
  include_directories : include_directories('include'),
  link_with : libnvidia_ml,
  install : false)
