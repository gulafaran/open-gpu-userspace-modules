# libnvidia-ml.so.1 (NVML) on nvchan's RM layer.
py = find_program('python3')

nvml_stubs = custom_target('nvml-stubs',
  input : 'exports.txt',
  output : 'stubs.c',
  command : [py, files('../tools/gen-nvml-stubs.py'), '@INPUT@', '@OUTPUT@'])

libnvidia_ml = shared_library('nvidia-ml',
  'src/core.c',
  'src/device.c',
  'src/etbl.c',
  nvml_stubs,
  version : '1.0.0',
  soversion : '1',
  include_directories : [include_directories('include', 'src', '../nvchan/src'),
                         nvchan_kernel_inc],
  # Deprecated NVML functions are implemented on purpose.
  c_args : nvchan_c_args + ['-Wno-deprecated-declarations'],
  link_with : libnvchan,
  dependencies : dependency('threads'),
  install : true)

executable('nvml-probe', 'tests/nvml-probe.c',
  c_args : ['-Wno-deprecated-declarations', '-Wno-unused-result'],
  include_directories : include_directories('include'),
  link_with : libnvidia_ml,
  install : false)
