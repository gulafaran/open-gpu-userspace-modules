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
  dependencies : [dep_libdrm, dep_gbm_headers, dependency('threads')],
  install : true,
  install_dir : gbm_backends_dir)

foreach t : ['gbm-probe', 'gbm-layout']
  executable(t, 'tests' / t + '.c',
    dependencies : [dependency('gbm'), dep_libdrm],
    install : false)
endforeach

dep_egl = dependency('egl', required : false)
dep_glesv2 = dependency('glesv2', required : false)
if dep_egl.found() and dep_glesv2.found()
  executable('gbm-import', 'tests/gbm-import.c',
    dependencies : [dependency('gbm'), dep_libdrm, dep_egl, dep_glesv2],
    install : false)
  executable('gbm-surface', 'tests/gbm-surface.c',
    include_directories : nv_uapi_inc,
    dependencies : [dependency('gbm'), dep_libdrm, dep_egl, dep_glesv2],
    install : false)
  executable('gbm-map', 'tests/gbm-map.c',
    include_directories : nv_uapi_inc,
    dependencies : [dependency('gbm'), dep_libdrm, dep_egl, dep_glesv2],
    install : false)
  executable('gbm-egl', 'tests/gbm-egl.c',
    include_directories : nv_uapi_inc,
    dependencies : [dependency('gbm'), dep_libdrm, dep_egl, dep_glesv2],
    install : false)
endif
