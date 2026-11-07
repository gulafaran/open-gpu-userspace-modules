/*
 * SPDX-License-Identifier: MIT
 *
 * GBM window surfaces through NVIDIA's EGL (libnvidia-egl-gbm), the way a
 * KMS compositor uses them: gbm_surface_create, an EGL window surface on it,
 * render a frame, eglSwapBuffers, gbm_surface_lock_front_buffer, AddFB2,
 * and release the previous front buffer.
 *
 * Each frame is cleared to its own colour; the locked front buffer is read
 * back through an EGLImage and must have that colour. AddFB2 needs the
 * primary node (no DRM master needed).
 *
 *   GBM_BACKENDS_PATH=build/nvidia-gbm GBM_BACKEND=nvidia-open build/nvidia-gbm/gbm-surface
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
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_target_rb;

static EGLDisplay dpy;
static int fd;
static bool primary;
static int failures;

#define CHECK(cond, ...) do {                       \
        printf((cond) ? "  ok    " : "  FAIL  ");   \
        if (!(cond))                                \
            failures++;                             \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
    } while (0)

static int open_nvidia_node(int type)
{
    drmDevicePtr devs[16];
    int n = drmGetDevices2(0, devs, 16);
    int out = -1;

    for (int i = 0; i < n && out < 0; i++) {
        if (!(devs[i]->available_nodes & (1 << type)))
            continue;
        int f = open(devs[i]->nodes[type], O_RDWR | O_CLOEXEC);
        if (f < 0)
            continue;
        drmVersionPtr v = drmGetVersion(f);
        if (v && strcmp(v->name, "nvidia-drm") == 0)
            out = f;
        else
            close(f);
        drmFreeVersion(v);
    }
    if (n > 0)
        drmFreeDevices(devs, n);
    return out;
}

static EGLConfig choose_config(uint32_t fourcc)
{
    EGLint n = 0;
    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1, EGL_GREEN_SIZE, 1, EGL_BLUE_SIZE, 1,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLConfig configs[256];

    if (!eglChooseConfig(dpy, attrs, configs, 256, &n))
        return NULL;
    for (EGLint i = 0; i < n; i++) {
        EGLint id = 0;
        eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &id);
        if ((uint32_t)id == fourcc)
            return configs[i];
    }
    return NULL;
}

/* Reads pixel (x, y) of a BO as RGBA8: through gbm_bo_map where the backend
 * can map it, else through an EGLImage (GL can't attach linear images). */
static bool read_pixel(struct gbm_bo *bo, uint32_t x, uint32_t y, uint8_t px[4])
{
    uint32_t stride;
    void *md = NULL;
    const uint8_t *p = gbm_bo_map(bo, x, y, 1, 1, GBM_BO_TRANSFER_READ, &stride, &md);
    if (p != NULL) {
        /* XRGB8888/ARGB8888: B, G, R, X in memory */
        px[0] = p[2];
        px[1] = p[1];
        px[2] = p[0];
        px[3] = 0xff;
        gbm_bo_unmap(bo, md);
        return true;
    }

    int dmabuf = gbm_bo_get_fd(bo);
    uint64_t mod = gbm_bo_get_modifier(bo);
    const EGLint attrs[] = {
        EGL_WIDTH, (EGLint)gbm_bo_get_width(bo),
        EGL_HEIGHT, (EGLint)gbm_bo_get_height(bo),
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)gbm_bo_get_format(bo),
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)gbm_bo_get_offset(bo, 0),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),
        EGL_NONE,
    };
    EGLImageKHR img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    close(dmabuf);
    if (img == EGL_NO_IMAGE_KHR)
        return false;

    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    image_target_rb(GL_RENDERBUFFER, img);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok)
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &rb);
    destroy_image(dpy, img);
    return ok;
}

static int try_addfb(struct gbm_bo *bo, int *err)
{
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
    uint32_t pitches[4] = { gbm_bo_get_stride(bo) };
    uint32_t offsets[4] = { gbm_bo_get_offset(bo, 0) };
    uint64_t mods[4] = { gbm_bo_get_modifier(bo) };
    uint32_t fb;

    if (!primary)
        return -1;
    if (drmModeAddFB2WithModifiers(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                                   gbm_bo_get_format(bo), handles, pitches, offsets,
                                   mods, &fb, DRM_MODE_FB_MODIFIERS) != 0) {
        *err = errno;
        return 0;
    }
    drmModeRmFB(fd, fb);
    return 1;
}

static void run(struct gbm_device *gbm, const char *tag, uint32_t w, uint32_t h,
                uint32_t fourcc, uint32_t flags, const uint64_t *mods, unsigned nmods)
{
    static const float colors[][3] = {
        { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 1, 0, 1 },
    };
    const unsigned frames = 6;

    printf("%s %ux%u %.4s\n", tag, w, h, (const char *)&fourcc);

    EGLConfig config = choose_config(fourcc);
    if (config == NULL) {
        CHECK(false, "no EGL config with native visual %.4s", (const char *)&fourcc);
        return;
    }

    struct gbm_surface *gs = mods ?
        gbm_surface_create_with_modifiers2(gbm, w, h, fourcc, mods, nmods, flags) :
        gbm_surface_create(gbm, w, h, fourcc, flags);
    CHECK(gs != NULL, "gbm_surface_create (%s)", gs ? "-" : strerror(errno));
    if (gs == NULL)
        return;

    EGLSurface es = eglCreatePlatformWindowSurface(dpy, config, gs, NULL);
    CHECK(es != EGL_NO_SURFACE, "eglCreatePlatformWindowSurface (0x%x)",
          es == EGL_NO_SURFACE ? eglGetError() : 0);
    if (es == EGL_NO_SURFACE) {
        gbm_surface_destroy(gs);
        return;
    }

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    eglMakeCurrent(dpy, es, es, ctx);

    struct gbm_bo *prev = NULL;
    unsigned color_ok = 0, addfb_ok = 0, addfb_tried = 0, mod_ok = 0;
    uint64_t first_mod = 0;
    int addfb_err = 0;

    for (unsigned f = 0; f < frames; f++) {
        glViewport(0, 0, w, h);
        glClearColor(colors[f][0], colors[f][1], colors[f][2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!eglSwapBuffers(dpy, es)) {
            CHECK(false, "frame %u: eglSwapBuffers (0x%x)", f, eglGetError());
            break;
        }

        struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
        if (bo == NULL) {
            CHECK(false, "frame %u: lock_front_buffer (%s)", f, strerror(errno));
            break;
        }

        uint64_t mod = gbm_bo_get_modifier(bo);
        if (f == 0) {
            first_mod = mod;
            printf("  info  front buffer mod=0x%016" PRIx64 " stride=%u planes=%d\n",
                   mod, gbm_bo_get_stride(bo), gbm_bo_get_plane_count(bo));
        }
        bool in_list = mods == NULL;
        for (unsigned i = 0; i < nmods && !in_list; i++)
            in_list = mods[i] == mod;
        mod_ok += in_list && gbm_bo_get_width(bo) == w && gbm_bo_get_height(bo) == h &&
                  gbm_bo_get_format(bo) == fourcc;

        uint8_t px[4] = { 0 };
        if (read_pixel(bo, w / 2, h / 2, px) &&
            (px[0] > 128) == (colors[f][0] > 0.5f) &&
            (px[1] > 128) == (colors[f][1] > 0.5f) &&
            (px[2] > 128) == (colors[f][2] > 0.5f))
            color_ok++;

        int err = 0, r = try_addfb(bo, &err);
        if (r >= 0) {
            addfb_tried++;
            addfb_ok += r;
            if (r == 0)
                addfb_err = err;
        }

        /* Like a compositor: keep the new front buffer, release the old. */
        if (prev)
            gbm_surface_release_buffer(gs, prev);
        prev = bo;
    }
    CHECK(mod_ok == frames, "front buffers have size/format and an allowed modifier (%u/%u)",
          mod_ok, frames);
    CHECK(color_ok == frames, "front buffers show the frame just rendered (%u/%u)",
          color_ok, frames);
    if (addfb_tried) {
        bool compressed = first_mod != DRM_FORMAT_MOD_LINEAR && ((first_mod >> 23) & 7);
        if (compressed)
            printf("  info  AddFB2 of compressed front buffers: %u/%u (%s)\n",
                   addfb_ok, addfb_tried, addfb_ok ? "-" : strerror(addfb_err));
        else
            CHECK(addfb_ok == addfb_tried, "AddFB2 of front buffers (%u/%u%s%s)",
                  addfb_ok, addfb_tried, addfb_err ? ", " : "",
                  addfb_err ? strerror(addfb_err) : "");
    }

    if (prev)
        gbm_surface_release_buffer(gs, prev);
    CHECK(gbm_surface_has_free_buffers(gs), "free buffers after releasing all");

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, es);
    gbm_surface_destroy(gs);
}

int main(void)
{
    fd = open_nvidia_node(DRM_NODE_PRIMARY);
    primary = fd >= 0;
    if (!primary)
        fd = open_nvidia_node(DRM_NODE_RENDER);
    if (fd < 0) {
        fprintf(stderr, "no nvidia-drm node\n");
        return 1;
    }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (gbm == NULL) {
        fprintf(stderr, "gbm_create_device failed\n");
        return 1;
    }
    printf("backend: %s, %s node\n", gbm_device_get_backend_name(gbm),
           primary ? "primary" : "render (no AddFB2 checks)");

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    image_target_rb = (void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");

    dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        fprintf(stderr, "EGL on GBM failed: 0x%x\n", eglGetError());
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    /* Before an EGL window surface exists, the backend's own entry points
     * are used: they must fail cleanly. */
    struct gbm_surface *bare = gbm_surface_create(gbm, 64, 64, GBM_FORMAT_XRGB8888,
                                                  GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    printf("bare surface\n");
    CHECK(bare != NULL, "gbm_surface_create without an EGL surface");
    if (bare) {
        CHECK(gbm_surface_lock_front_buffer(bare) == NULL, "lock_front_buffer before any frame fails");
        gbm_surface_destroy(bare);
    }

    const uint32_t usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
    run(gbm, "plain      ", 1920, 1080, GBM_FORMAT_XRGB8888, usage, NULL, 0);
    run(gbm, "plain      ", 2560, 1600, GBM_FORMAT_ARGB8888, usage, NULL, 0);
    run(gbm, "odd size   ", 1001, 333, GBM_FORMAT_XRGB8888, usage, NULL, 0);

    /* Explicit lists as a compositor passes them from the plane's
     * IN_FORMATS: uncompressed block-linear, and linear only. */
    struct gbm_bo *ref = gbm_bo_create(gbm, 64, 64, GBM_FORMAT_XRGB8888, usage);
    if (ref) {
        uint64_t plain = gbm_bo_get_modifier(ref) & ~0xfull;
        gbm_bo_destroy(ref);
        uint64_t bl[6];
        for (unsigned i = 0; i < 6; i++)
            bl[i] = plain | i;
        run(gbm, "mods BL    ", 1920, 1080, GBM_FORMAT_XRGB8888, usage, bl, 6);
        uint64_t lin = DRM_FORMAT_MOD_LINEAR;
        run(gbm, "mods linear", 1920, 1080, GBM_FORMAT_XRGB8888, usage, &lin, 1);
    }

    printf("\n%d failure(s)\n", failures);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);
    return failures != 0;
}
