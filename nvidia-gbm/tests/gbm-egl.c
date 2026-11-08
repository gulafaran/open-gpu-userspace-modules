/*
 * SPDX-License-Identifier: MIT
 *
 * End-to-end check of BOs from the active GBM backend through NVIDIA's EGL:
 *
 *  - the EGL display is created on the GBM device (EGL_PLATFORM_GBM_KHR), so
 *    libnvidia-egl-gbm has to accept the backend;
 *  - every BO is exported as a dma-buf and imported with
 *    EGL_EXT_image_dma_buf_import_modifiers, so eglcore has to agree with
 *    the layout (stride, size) implied by the modifier;
 *  - a pattern is rendered into it and read back with glReadPixels.
 *
 * A linear BO the GL can't render to (NVIDIA's closed GL can't) is reported
 * as "not renderable" and not counted as a failure; the summary says whether
 * this GL renders to linear, which decides NV_GBM_LINEAR_RENDER.
 *
 *   GBM_BACKENDS_PATH=build/nvidia-gbm GBM_BACKEND=nvidia-open build/nvidia-gbm/gbm-egl
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>


static PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_target_rb;

static int failures;
static int linear_rendered, linear_unrenderable;

static int open_nvidia_node(void)
{
    drmDevicePtr devs[16];
    int n = drmGetDevices2(0, devs, 16);
    int fd = -1;

    for (int i = 0; i < n && fd < 0; i++) {
        if (!(devs[i]->available_nodes & (1 << DRM_NODE_RENDER)))
            continue;
        int f = open(devs[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
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

static void test_bo(EGLDisplay dpy, int fd, struct gbm_device *gbm,
                    const char *tag, uint32_t w, uint32_t h, uint32_t fourcc,
                    uint32_t usage, const uint64_t *mods, unsigned nmods)
{
    struct gbm_bo *bo = mods ?
        gbm_bo_create_with_modifiers2(gbm, w, h, fourcc, mods, nmods, usage) :
        gbm_bo_create(gbm, w, h, fourcc, usage);

    printf("%-12s %5ux%-5u %.4s ", tag, w, h, (const char *)&fourcc);
    if (bo == NULL) {
        printf("create FAILED (%m)\n");
        failures++;
        return;
    }

    uint64_t mod = gbm_bo_get_modifier(bo);
    int dmabuf = gbm_bo_get_fd(bo);
    uint32_t stride = gbm_bo_get_stride_for_plane(bo, 0);
    uint32_t offset = gbm_bo_get_offset(bo, 0);
    off_t size = lseek(dmabuf, 0, SEEK_END);
    printf("mod=0x%016" PRIx64 " stride=%u size=%lld: ", mod, stride, (long long)size);

    const EGLAttrib attrs[] = {
        EGL_WIDTH, w,
        EGL_HEIGHT, h,
        EGL_LINUX_DRM_FOURCC_EXT, fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLAttrib)(mod & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLAttrib)(mod >> 32),
        EGL_NONE,
    };
    EGLint iattrs[sizeof(attrs) / sizeof(attrs[0])];
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++)
        iattrs[i] = (EGLint)attrs[i];

    EGLImageKHR img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                   NULL, iattrs);
    close(dmabuf);
    if (img == EGL_NO_IMAGE_KHR) {
        printf("import FAILED (0x%x)\n", eglGetError());
        failures++;
        gbm_bo_destroy(bo);
        return;
    }

    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    image_target_rb(GL_RENDERBUFFER, img);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    bool ok = st == GL_FRAMEBUFFER_COMPLETE;
    if (!ok && mod == DRM_FORMAT_MOD_LINEAR) {
        printf("import OK, not renderable (fbo 0x%x)\n", st);
        linear_unrenderable++;
        goto out;
    }
    if (!ok)
        printf("fbo incomplete 0x%x ", st);

    if (ok) {
        /* Red everywhere, blue in the lower-left quadrant (rows 0..h/2-1 in
         * memory). */
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (w >= 2 && h >= 2) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, w / 2, h / 2);
            glClearColor(0, 0, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }

        uint8_t px[4];
        struct { uint32_t x, y; int blue; } probes[] = {
            { 0, 0, w >= 2 && h >= 2 }, { w - 1, h - 1, 0 },
            { w - 1, 0, 0 }, { 0, h - 1, 0 },
        };
        for (unsigned i = 0; i < 4 && ok; i++) {
            glReadPixels(probes[i].x, probes[i].y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            int want_r = !probes[i].blue, want_b = probes[i].blue;
            if ((px[0] > 128) != want_r || (px[2] > 128) != want_b) {
                printf("readback (%u,%u) = %02x%02x%02x ", probes[i].x,
                       probes[i].y, px[0], px[1], px[2]);
                ok = false;
            }
        }
        glFinish();
    }

    printf("%s\n", ok ? "OK" : "FAILED");
    if (!ok)
        failures++;
    else if (mod == DRM_FORMAT_MOD_LINEAR)
        linear_rendered++;

out:
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &rb);
    destroy_image(dpy, img);
    gbm_bo_destroy(bo);
}

int main(void)
{
    int fd = open_nvidia_node();
    if (fd < 0) {
        fprintf(stderr, "no nvidia-drm node\n");
        return 1;
    }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (gbm == NULL) {
        fprintf(stderr, "gbm_create_device failed\n");
        return 1;
    }
    printf("backend: %s\n", gbm_device_get_backend_name(gbm));

    get_platform_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    image_target_rb = (void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");

    EGLDisplay dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint maj, min;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
        fprintf(stderr, "EGL on the GBM device failed: 0x%x\n", eglGetError());
        return 1;
    }
    printf("EGL %d.%d on GBM: %s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "surfaceless context failed: 0x%x\n", eglGetError());
        return 1;
    }

    static const uint32_t sizes[][2] = {
        { 1, 1 }, { 16, 16 }, { 100, 37 }, { 640, 480 }, { 1000, 1000 },
        { 1920, 1080 }, { 3840, 2160 }, { 5120, 1440 },
    };
    struct { const char *name; uint32_t usage; } usages[] = {
        { "render", GBM_BO_USE_RENDERING },
        { "scanout", GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING },
        /* Without RENDERING so the backend hands them out by default; the
         * test still tries to render to them. */
        { "linear", GBM_BO_USE_LINEAR },
        { "linear-scan", GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT },
    };

    for (unsigned u = 0; u < sizeof(usages) / sizeof(usages[0]); u++)
        for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
            test_bo(dpy, fd, gbm, usages[u].name, sizes[s][0], sizes[s][1],
                    GBM_FORMAT_XRGB8888, usages[u].usage, NULL, 0);

    test_bo(dpy, fd, gbm, "render", 1920, 1080, GBM_FORMAT_ARGB8888,
            GBM_BO_USE_RENDERING, NULL, 0);
    test_bo(dpy, fd, gbm, "render", 1920, 1080, GBM_FORMAT_ABGR8888,
            GBM_BO_USE_RENDERING, NULL, 0);

    /* 10 bpc, as HDR compositors allocate their swapchains. RGBA1010102 and
     * BGRA1010102 are allocated too, but NVIDIA's GL can't render to them
     * (incomplete attachment, with the closed allocator's BOs as well). */
    static const uint32_t formats_10bpc[] = {
        GBM_FORMAT_XBGR2101010, GBM_FORMAT_ABGR2101010, GBM_FORMAT_ARGB2101010,
    };
    for (unsigned f = 0; f < sizeof(formats_10bpc) / sizeof(formats_10bpc[0]); f++) {
        test_bo(dpy, fd, gbm, "render", 1920, 1080, formats_10bpc[f],
                GBM_BO_USE_RENDERING, NULL, 0);
        test_bo(dpy, fd, gbm, "scanout", 1920, 1080, formats_10bpc[f],
                GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING, NULL, 0);
    }

    /* Every GOB height, compressed and not, as explicit single modifiers.
     * The kind values come from a render (compressed) and a scanout BO. */
    struct gbm_bo *ref = gbm_bo_create(gbm, 64, 64, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    uint64_t plain = ref ? gbm_bo_get_modifier(ref) & ~0xfull : 0;
    if (ref)
        gbm_bo_destroy(ref);
    ref = gbm_bo_create(gbm, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    uint64_t comp = ref ? gbm_bo_get_modifier(ref) & ~0xfull : 0;
    if (ref)
        gbm_bo_destroy(ref);

    if (plain != 0 && plain != (DRM_FORMAT_MOD_LINEAR & ~0xfull)) {
        for (unsigned hh = 0; hh <= 5; hh++) {
            uint64_t m[2] = { plain | hh, comp | hh };
            char tag[16];
            snprintf(tag, sizeof(tag), "h%u", hh);
            test_bo(dpy, fd, gbm, tag, 1920, 1080, GBM_FORMAT_XRGB8888,
                    GBM_BO_USE_RENDERING, &m[0], 1);
            test_bo(dpy, fd, gbm, tag, 100, 37, GBM_FORMAT_XRGB8888,
                    GBM_BO_USE_RENDERING, &m[0], 1);
            snprintf(tag, sizeof(tag), "h%u-comp", hh);
            test_bo(dpy, fd, gbm, tag, 1920, 1080, GBM_FORMAT_XRGB8888,
                    GBM_BO_USE_RENDERING, &m[1], 1);
            test_bo(dpy, fd, gbm, tag, 100, 37, GBM_FORMAT_XRGB8888,
                    GBM_BO_USE_RENDERING, &m[1], 1);
        }
    }

    printf("\nlinear BOs: %d rendered, %d not renderable by this GL\n",
           linear_rendered, linear_unrenderable);
    printf("%d failure(s)\n", failures);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);
    return failures != 0;
}
