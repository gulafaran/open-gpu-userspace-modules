dep_libdrm = dependency('libdrm', version : '>= 2.4.110')
# Only the headers: a backend must not link libgbm, the loader is the caller.
dep_gbm_headers = dependency('gbm', version : '>= 21.3').partial_dependency(compile_args : true)

if not cc.has_header('gbm_backend_abi.h', dependencies : dep_gbm_headers)
  error('gbm_backend_abi.h not found (Mesa >= 21.3 installs it)')
endif

gbm_backends_dir = get_option('libdir') / 'gbm'

# Installed under its own name so it doesn't fight nvidia-drivers over
# nvidia-drm_gbm.so. Select it per process with GBM_BACKEND=nvidia-open, or
# globally by pointing the nvidia-drm_gbm.so symlink at it.
shared_module('nvidia-open_gbm',
  'src/nvidia_gbm.c',
  name_prefix : '',
  include_directories : nv_uapi_inc,
  dependencies : [dep_libdrm, dep_gbm_headers],
  install : true,
  install_dir : gbm_backends_dir)

executable('gbm-probe',
  'tests/gbm-probe.c',
  dependencies : [dependency('gbm'), dep_libdrm],
  install : false)
