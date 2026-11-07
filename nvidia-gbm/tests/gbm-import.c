/*
 * SPDX-License-Identifier: MIT
 *
 * gbm_bo_import checks for the active GBM backend:
 *
 *  - round trip: a BO is exported and imported on the same device. DRM
 *    returns the existing GEM handle, so destroying the import must not
 *    close it under the original BO;
 *  - a foreign buffer (another DRM file) imported twice shares one handle,
 *    which must survive until the last BO is destroyed;
 *  - a buffer exported by EGL (eglExportDMABUFImageMESA), like a client's;
 *  - drmModeAddFB2WithModifiers on imported buffers, which makes NVKMS
 *    validate the layout (primary node only, no DRM master needed);
 *  - invalid imports are rejected.
 *
 *   GBM_BACKENDS_PATH=build/nvidia-gbm GBM_BACKEND=nvidia-open build/nvidia-gbm/gbm-import
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int failures;

#define CHECK(cond, ...) do {                       \
        if (cond) {                                 \
            printf("  ok    ");                     \
        } else {                                    \
            printf("  FAIL  ");                     \
            failures++;                             \
        }                                           \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
    } while (0)

static int open_nvidia_node(int type)
{
    drmDevicePtr devs[16];
    int n = drmGetDevices2(0, devs, 16);
    int fd = -1;

    for (int i = 0; i < n && fd < 0; i++) {
        if (!(devs[i]->available_nodes & (1 << type)))
            continue;
        int f = open(devs[i]->nodes[type], O_RDWR | O_CLOEXEC);
        if (f < 0)
            continue;
        drmVersionPtr v = drmGetVersion(f);
        if (v && strcmp(v->name, "nvidia-drm") == 0)
            fd = f;
        else
            close(f);
        drmFreeVersion(v);
    }
    if (n > 0)
        drmFreeDevices(devs, n);
    return fd;
}

/* Whether handle is still a live GEM handle on fd. */
static bool handle_alive(int fd, uint32_t handle)
{
    int dmabuf;

    if (drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &dmabuf) != 0)
        return false;
    close(dmabuf);
    return true;
}

static struct gbm_bo *import_bo(struct gbm_device *gbm, struct gbm_bo *src,
                                uint32_t usage)
{
    struct gbm_import_fd_modifier_data d = {
        .width = gbm_bo_get_width(src),
        .height = gbm_bo_get_height(src),
        .format = gbm_bo_get_format(src),
        .num_fds = (uint32_t)gbm_bo_get_plane_count(src),
        .modifier = gbm_bo_get_modifier(src),
    };
    for (unsigned p = 0; p < d.num_fds; p++) {
        d.fds[p] = gbm_bo_get_fd_for_plane(src, (int)p);
        d.strides[p] = (int)gbm_bo_get_stride_for_plane(src, (int)p);
        d.offsets[p] = (int)gbm_bo_get_offset(src, (int)p);
    }
    struct gbm_bo *bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &d, usage);
    for (unsigned p = 0; p < d.num_fds; p++)
        close(d.fds[p]);
    return bo;
}

/* -1: not attempted (render node), 0: rejected, 1: accepted. */
static int try_addfb(int fd, bool primary, struct gbm_bo *bo, int *err)
{
    uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 }, fb;
    uint64_t mods[4] = { 0 };

    if (!primary)
        return -1;
    for (int p = 0; p < gbm_bo_get_plane_count(bo); p++) {
        handles[p] = gbm_bo_get_handle_for_plane(bo, p).u32;
        pitches[p] = gbm_bo_get_stride_for_plane(bo, p);
        offsets[p] = gbm_bo_get_offset(bo, p);
        mods[p] = gbm_bo_get_modifier(bo);
    }
    if (drmModeAddFB2WithModifiers(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                                   gbm_bo_get_format(bo), handles, pitches,
                                   offsets, mods, &fb, DRM_MODE_FB_MODIFIERS) != 0) {
        *err = errno;
        return 0;
    }
    drmModeRmFB(fd, fb);
    return 1;
}

static void round_trip(int fd, bool primary, struct gbm_device *gbm,
                       const char *tag, uint32_t w, uint32_t h, uint32_t fourcc,
                       uint32_t usage)
{
    printf("round trip %s %ux%u %.4s\n", tag, w, h, (const char *)&fourcc);
    struct gbm_bo *src = gbm_bo_create(gbm, w, h, fourcc, usage);
    if (src == NULL) {
        CHECK(false, "create (%m)");
        return;
    }
    uint32_t handle = gbm_bo_get_handle(src).u32;

    struct gbm_bo *imp = import_bo(gbm, src, usage);
    CHECK(imp != NULL, "import (%s)", imp ? "-" : strerror(errno));
    if (imp == NULL) {
        gbm_bo_destroy(src);
        return;
    }
    CHECK(gbm_bo_get_handle(imp).u32 == handle, "same GEM handle (%u, %u)",
          handle, gbm_bo_get_handle(imp).u32);
    CHECK(gbm_bo_get_modifier(imp) == gbm_bo_get_modifier(src) &&
          gbm_bo_get_stride(imp) == gbm_bo_get_stride(src) &&
          gbm_bo_get_plane_count(imp) == gbm_bo_get_plane_count(src),
          "modifier/stride/planes preserved");

    int err = 0;
    int fb = try_addfb(fd, primary, imp, &err);
    if (fb >= 0) {
        /* The display can't read compressed surfaces. */
        bool compressed = ((gbm_bo_get_modifier(imp) >> 23) & 0x7) != 0;
        if (compressed)
            printf("  info  AddFB2 of compressed BO: %s\n",
                   fb ? "accepted" : strerror(err));
        else
            CHECK(fb == 1, "AddFB2 (%s)", fb ? "-" : strerror(err));
    }

    gbm_bo_destroy(imp);
    CHECK(handle_alive(fd, handle), "handle alive after destroying the import");
    CHECK(gbm_bo_get_fd(src) >= 0, "original still exports");
    gbm_bo_destroy(src);
    CHECK(!handle_alive(fd, handle), "handle closed after destroying both");
}

static void foreign_double(int fd, bool primary, struct gbm_device *gbm,
                           struct gbm_device *other)
{
    printf("foreign buffer imported twice\n");
    struct gbm_bo *src = gbm_bo_create(other, 1920, 1080, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (src == NULL) {
        CHECK(false, "create on the other fd (%m)");
        return;
    }
    struct gbm_bo *a = import_bo(gbm, src, GBM_BO_USE_SCANOUT);
    struct gbm_bo *b = import_bo(gbm, src, GBM_BO_USE_SCANOUT);
    CHECK(a && b, "both imports");
    if (!a || !b)
        goto out;
    uint32_t handle = gbm_bo_get_handle(a).u32;
    CHECK(gbm_bo_get_handle(b).u32 == handle, "same GEM handle");

    gbm_bo_destroy(a);
    a = NULL;
    CHECK(handle_alive(fd, handle), "handle alive after destroying one");
    int err = 0;
    int fb = try_addfb(fd, primary, b, &err);
    if (fb >= 0)
        CHECK(fb == 1, "AddFB2 of the remaining one (%s)", fb ? "-" : strerror(err));
    gbm_bo_destroy(b);
    b = NULL;
    CHECK(!handle_alive(fd, handle), "handle closed after destroying both");
out:
    if (a)
        gbm_bo_destroy(a);
    if (b)
        gbm_bo_destroy(b);
    gbm_bo_destroy(src);
}

static void egl_export(int fd, bool primary, struct gbm_device *gbm,
                       struct gbm_device *other)
{
    printf("EGL-exported buffer (eglExportDMABUFImageMESA)\n");

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLCREATEIMAGEKHRPROC create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC export_query =
        (void *)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    PFNEGLEXPORTDMABUFIMAGEMESAPROC export_image =
        (void *)eglGetProcAddress("eglExportDMABUFImageMESA");

    /* A different DRM file than gbm's, as a client's would be. */
    EGLDisplay dpy = get_display(EGL_PLATFORM_GBM_KHR, other, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL) ||
        !export_query || !export_image) {
        CHECK(false, "EGL setup (0x%x)", eglGetError());
        return;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1920, 1080, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFinish();

    EGLImageKHR img = create_image(dpy, ctx, EGL_GL_TEXTURE_2D_KHR,
                                   (EGLClientBuffer)(uintptr_t)tex, NULL);
    int fourcc = 0, nplanes = 0;
    EGLuint64KHR mod = 0;
    int fds[4] = { -1, -1, -1, -1 };
    EGLint strides[4] = { 0 }, offsets[4] = { 0 };

    if (img == EGL_NO_IMAGE_KHR ||
        !export_query(dpy, img, &fourcc, &nplanes, &mod) ||
        !export_image(dpy, img, fds, strides, offsets)) {
        CHECK(false, "export (0x%x)", eglGetError());
        goto out;
    }
    printf("  info  exported %.4s planes=%d mod=0x%016" PRIx64 " stride=%d\n",
           (const char *)&fourcc, nplanes, (uint64_t)mod, strides[0]);

    struct gbm_import_fd_modifier_data d = {
        .width = 1920, .height = 1080, .format = (uint32_t)fourcc,
        .num_fds = (uint32_t)nplanes, .modifier = mod,
    };
    for (int p = 0; p < nplanes; p++) {
        d.fds[p] = fds[p];
        d.strides[p] = strides[p];
        d.offsets[p] = offsets[p];
    }
    struct gbm_bo *bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &d,
                                      GBM_BO_USE_SCANOUT);
    CHECK(bo != NULL, "import (%s)", bo ? "-" : strerror(errno));
    if (bo) {
        int err = 0;
        int fb = try_addfb(fd, primary, bo, &err);
        if (fb >= 0)
            printf("  info  AddFB2: %s\n", fb ? "accepted" : strerror(err));
        gbm_bo_destroy(bo);
    }
    for (int p = 0; p < nplanes; p++)
        if (fds[p] >= 0)
            close(fds[p]);
out:
    if (img != EGL_NO_IMAGE_KHR)
        destroy_image(dpy, img);
    glDeleteTextures(1, &tex);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
}

static void invalid(struct gbm_device *gbm)
{
    printf("invalid imports\n");
    struct gbm_bo *src = gbm_bo_create(gbm, 640, 480, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (src == NULL) {
        CHECK(false, "create (%m)");
        return;
    }
    int fd = gbm_bo_get_fd(src);
    struct gbm_import_fd_modifier_data good = {
        .width = 640, .height = 480, .format = GBM_FORMAT_XRGB8888, .num_fds = 1,
        .fds = { fd }, .strides = { (int)gbm_bo_get_stride(src) },
        .modifier = gbm_bo_get_modifier(src),
    };
    struct gbm_import_fd_modifier_data d;
    struct gbm_bo *bo;

#define EXPECT_FAIL(what) do {                                                 \
        bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &d, 0);             \
        CHECK(bo == NULL, "rejects %s (%s)", what, bo ? "accepted" : strerror(errno)); \
        if (bo)                                                                \
            gbm_bo_destroy(bo);                                                \
    } while (0)

    d = good; d.height = 4096;              EXPECT_FAIL("height beyond the dma-buf");
    d = good; d.strides[0] = 64;            EXPECT_FAIL("stride below width * cpp");
    d = good; d.strides[0] += 16;           EXPECT_FAIL("block-linear stride not GOB aligned");
    d = good; d.offsets[0] = 1 << 20;       EXPECT_FAIL("offset past the end");
    d = good; d.num_fds = 2; d.fds[1] = fd; d.strides[1] = d.strides[0];
                                            EXPECT_FAIL("plane count mismatch");
    d = good; d.fds[0] = -1;                EXPECT_FAIL("bad fd");
    d = good; d.width = 0;                  EXPECT_FAIL("zero width");

    bo = gbm_bo_import(gbm, GBM_BO_IMPORT_WL_BUFFER, NULL, 0);
    CHECK(bo == NULL, "WL_BUFFER unsupported (%s)", bo ? "accepted" : strerror(errno));

    struct gbm_import_fd_data legacy = {
        .fd = fd, .width = 640, .height = 480,
        .stride = gbm_bo_get_stride(src), .format = GBM_FORMAT_XRGB8888,
    };
    bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &legacy, 0);
    CHECK(bo != NULL && gbm_bo_get_modifier(bo) == DRM_FORMAT_MOD_INVALID,
          "GBM_BO_IMPORT_FD works with an implicit modifier");
    if (bo)
        gbm_bo_destroy(bo);

    close(fd);
    gbm_bo_destroy(src);
}

int main(void)
{
    int fd = open_nvidia_node(DRM_NODE_PRIMARY);
    bool primary = fd >= 0;
    if (!primary)
        fd = open_nvidia_node(DRM_NODE_RENDER);
    int other_fd = open_nvidia_node(DRM_NODE_RENDER);
    if (fd < 0 || other_fd < 0) {
        fprintf(stderr, "no nvidia-drm node\n");
        return 1;
    }
    struct gbm_device *gbm = gbm_create_device(fd);
    struct gbm_device *other = gbm_create_device(other_fd);
    if (gbm == NULL || other == NULL) {
        fprintf(stderr, "gbm_create_device failed\n");
        return 1;
    }
    printf("backend: %s, %s node\n", gbm_device_get_backend_name(gbm),
           primary ? "primary" : "render (no AddFB2 checks)");

    round_trip(fd, primary, gbm, "scanout", 1920, 1080, GBM_FORMAT_XRGB8888,
               GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    round_trip(fd, primary, gbm, "render", 1920, 1080, GBM_FORMAT_ARGB8888,
               GBM_BO_USE_RENDERING);
    round_trip(fd, primary, gbm, "linear", 256, 256, GBM_FORMAT_ARGB8888,
               GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT);
    round_trip(fd, primary, gbm, "nv12", 1920, 1080, GBM_FORMAT_NV12,
               GBM_BO_USE_SCANOUT);
    foreign_double(fd, primary, gbm, other);
    egl_export(fd, primary, gbm, other);
    invalid(gbm);

    printf("\n%d failure(s)\n", failures);
    gbm_device_destroy(other);
    gbm_device_destroy(gbm);
    close(other_fd);
    close(fd);
    return failures != 0;
}
