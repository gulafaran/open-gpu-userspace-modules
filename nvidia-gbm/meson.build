dep_libdrm = dependency('libdrm', version : '>= 2.4.110')

build_backend = get_option('nvidia-gbm').allowed()
replace_allocator = get_option('nvidia-allocator').enabled()

srcs = ['src/gem_ref.c']
deps = [dep_libdrm, dependency('threads')]

if build_backend
  # Only the headers: a backend must not link libgbm, the loader is the caller.
  dep_gbm_headers = dependency('gbm', version : '>= 21.3').partial_dependency(compile_args : true)
  if not cc.has_header('gbm_backend_abi.h', dependencies : dep_gbm_headers)
    error('gbm_backend_abi.h not found (Mesa >= 21.3 installs it)')
  endif
  srcs += 'src/nvidia_gbm.c'
  deps += [dep_gbm_headers, nvchan_dep]
endif

gbm_backends_dir = get_option('libdir') / 'gbm'

if replace_allocator
  # Same layout as the driver: one library that NVIDIA's EGL/GL/Vulkan dlopen
  # as libnvidia-allocator.so.1 for GEMAddReference/GEMRemoveReference, and
  # that Mesa loads as the nvidia-drm GBM backend.
  shared_library('nvidia-allocator', srcs,
    version : '1.0.0',
    soversion : '1',
    include_directories : nv_uapi_inc,
    dependencies : deps,
    install : true)
  if build_backend
    foreach name : ['nvidia-drm_gbm.so', 'nvidia-open_gbm.so']
      install_symlink(name,
        pointing_to : '..' / 'libnvidia-allocator.so.1',
        install_dir : gbm_backends_dir)
    endforeach
  endif
elif build_backend
  # Installed under its own name so it doesn't fight nvidia-drivers over
  # nvidia-drm_gbm.so; select it with GBM_BACKEND=nvidia-open.
  shared_module('nvidia-open_gbm', srcs,
    name_prefix : '',
    include_directories : nv_uapi_inc,
    dependencies : deps,
    install : true,
    install_dir : gbm_backends_dir)
endif

if not build_backend
  subdir_done()
endif

foreach t : ['gbm-probe', 'gbm-layout', 'gbm-prime-import', 'gbm-fork']
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
