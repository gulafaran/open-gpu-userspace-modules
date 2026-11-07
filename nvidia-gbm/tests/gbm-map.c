/*
 * SPDX-License-Identifier: MIT
 *
 * gbm_bo_map checks for the active GBM backend, against NVIDIA's GL:
 *
 *  - GL -> CPU: GL renders a coordinate pattern into the BO, then it is read
 *    with gbm_bo_map(READ), full and a sub-rectangle;
 *  - CPU -> GL: the pattern is written with gbm_bo_map(WRITE), then GL samples
 *    the BO as a texture and the result is read back;
 *  - raw: the BO is mapped directly through nvidia-drm and detiled here with
 *    the Turing+ color GOB layout, independently of the backend, to verify
 *    that layout and to see whether compressed memory is CPU readable.
 *
 * Only for Turing..Ada, and Blackwell with >= 32bpp formats; Blackwell uses
 * other GOB layouts for 8 and 16 bpp.
 *
 *   GBM_BACKENDS_PATH=build/nvidia-gbm GBM_BACKEND=nvidia-open build/nvidia-gbm/gbm-map
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>

#include "nvidia-drm/nv_drm_common_ioctl.h"

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_target_rb;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_tex;

static EGLDisplay dpy;
static GLuint prog_pattern, prog_sample, prog_sample_ext;
static int failures;

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

/* ---- the pattern: RGBA8 bytes as a function of the pixel position ------- */

static void pattern(uint32_t x, uint32_t y, uint8_t px[4])
{
    px[0] = x & 0xff;
    px[1] = y & 0xff;
    px[2] = (uint8_t)((x >> 8) | ((y >> 8) << 4));
    px[3] = 0xff;
}

static const char *vs_src =
    "attribute vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *fs_pattern_src =
    "precision highp float;\n"
    "void main() {\n"
    "  vec2 p = floor(gl_FragCoord.xy);\n"
    "  vec2 lo = mod(p, 256.0);\n"
    "  vec2 hi = floor(p / 256.0);\n"
    "  gl_FragColor = vec4(lo.x / 255.0, lo.y / 255.0,\n"
    "                      (hi.x + hi.y * 16.0) / 255.0, 1.0);\n"
    "}\n";

/* NVIDIA's GL imports linear dma-bufs as external-only images. */
static const char *fs_sample_ext_src =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision highp float;\n"
    "uniform samplerExternalOES tex;\n"
    "uniform vec2 size;\n"
    "void main() { gl_FragColor = texture2D(tex, gl_FragCoord.xy / size); }\n";

static const char *fs_sample_src =
    "precision highp float;\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 size;\n"
    "void main() { gl_FragColor = texture2D(tex, gl_FragCoord.xy / size); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        exit(1);
    }
    return s;
}

static GLuint link_program(const char *fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    return p;
}

static void draw_fullscreen(void)
{
    static const float tri[] = { -1, -1, 3, -1, -1, 3 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* ---- CPU view of block-linear memory (Turing+ color 2D GOBs) ------------- */

static size_t bl_offset(uint32_t xb, uint32_t y, uint32_t stride, uint32_t log2_gobs)
{
    uint32_t gob_x = xb / 64, gob_y = y / 8;
    uint32_t bx = xb % 64, by = y % 8;
    size_t block = (size_t)(gob_y >> log2_gobs) * (stride / 64) + gob_x;
    size_t gob = block * (512u << log2_gobs) + (gob_y & ((1u << log2_gobs) - 1)) * 512u;

    return gob + (bx >> 5) * 256 + (by >> 2) * 128 + ((bx >> 4) & 1) * 64 +
           (by & 3) * 16 + (bx & 15);
}

static void *raw_map(int fd, struct gbm_bo *bo, size_t size)
{
    struct drm_nvidia_gem_map_offset_params p = { .handle = gbm_bo_get_handle(bo).u32 };

    if (drmIoctl(fd, DRM_IOCTL_NVIDIA_GEM_MAP_OFFSET, &p) != 0)
        return NULL;
    void *ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, p.offset);
    return ptr == MAP_FAILED ? NULL : ptr;
}

/* ---- checks -------------------------------------------------------------- */

static bool is_linear(uint64_t mod) { return mod == DRM_FORMAT_MOD_LINEAR; }
static bool is_compressed(uint64_t mod) { return !is_linear(mod) && ((mod >> 23) & 7); }

/* Compares rows of RGBA8 pixels at base (stride bytes) with the pattern at
 * (x0, y0). Returns the number of mismatching pixels and reports the first. */
static unsigned compare(const uint8_t *base, uint32_t stride, uint32_t x0, uint32_t y0,
                        uint32_t w, uint32_t h, char *first, size_t len)
{
    unsigned bad = 0;

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t want[4];
            const uint8_t *got = base + (size_t)y * stride + x * 4;
            pattern(x0 + x, y0 + y, want);
            if (memcmp(got, want, 4) != 0) {
                if (bad++ == 0)
                    snprintf(first, len, "(%u,%u) got %02x%02x%02x%02x want %02x%02x%02x%02x",
                             x0 + x, y0 + y, got[0], got[1], got[2], got[3],
                             want[0], want[1], want[2], want[3]);
            }
        }
    }
    return bad;
}

static void report(const char *what, unsigned bad, unsigned total, const char *first,
                   bool expected_to_work)
{
    if (bad == 0) {
        printf("    ok    %s\n", what);
    } else if (!expected_to_work) {
        printf("    info  %s: %u/%u wrong, first %s\n", what, bad, total, first);
    } else {
        printf("    FAIL  %s: %u/%u wrong, first %s\n", what, bad, total, first);
        failures++;
    }
}

static EGLImageKHR import(struct gbm_bo *bo)
{
    int fd = gbm_bo_get_fd(bo);
    uint64_t mod = gbm_bo_get_modifier(bo);
    const EGLint attrs[] = {
        EGL_WIDTH, (EGLint)gbm_bo_get_width(bo),
        EGL_HEIGHT, (EGLint)gbm_bo_get_height(bo),
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)gbm_bo_get_format(bo),
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)gbm_bo_get_offset(bo, 0),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),
        EGL_NONE,
    };
    EGLImageKHR img = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    close(fd);
    return img;
}

static bool gl_render_pattern(struct gbm_bo *bo)
{
    EGLImageKHR img = import(bo);
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
    if (ok) {
        glViewport(0, 0, gbm_bo_get_width(bo), gbm_bo_get_height(bo));
        glUseProgram(prog_pattern);
        draw_fullscreen();
        glFinish();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &rb);
    destroy_image(dpy, img);
    return ok;
}

/* Samples the BO as a texture into a GL-owned RGBA8 target and reads it. */
static uint8_t *gl_sample(struct gbm_bo *bo)
{
    uint32_t w = gbm_bo_get_width(bo), h = gbm_bo_get_height(bo);
    EGLImageKHR img = import(bo);
    if (img == EGL_NO_IMAGE_KHR)
        return NULL;

    GLuint src, dst, fbo;
    glGenTextures(1, &src);
    /* GL_TEXTURE_2D, or GL_TEXTURE_EXTERNAL_OES for external-only images. */
    GLenum target = GL_TEXTURE_2D;
    GLuint prog = prog_sample;

    while (glGetError() != GL_NO_ERROR)
        ;
    glBindTexture(target, src);
    image_target_tex(target, img);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &src);
        glGenTextures(1, &src);
        target = GL_TEXTURE_EXTERNAL_OES;
        prog = prog_sample_ext;
        glBindTexture(target, src);
        image_target_tex(target, img);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR)
            printf("    info  external texture import: GL error 0x%x\n", err);
    }
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);

    uint8_t *out = NULL;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, w, h);
        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(target, src);
        glUniform1i(glGetUniformLocation(prog, "tex"), 0);
        glUniform2f(glGetUniformLocation(prog, "size"), (float)w, (float)h);
        draw_fullscreen();
        out = malloc((size_t)w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &dst);
    glDeleteTextures(1, &src);
    destroy_image(dpy, img);
    return out;
}

static void test_case(int fd, struct gbm_device *gbm, const char *tag,
                      uint32_t w, uint32_t h, uint32_t usage,
                      const uint64_t *mods, unsigned nmods)
{
    struct gbm_bo *bo = mods ?
        gbm_bo_create_with_modifiers2(gbm, w, h, DRM_FORMAT_ABGR8888, mods, nmods, usage) :
        gbm_bo_create(gbm, w, h, DRM_FORMAT_ABGR8888, usage);
    if (bo == NULL) {
        printf("%s %ux%u: create FAILED (%m)\n", tag, w, h);
        failures++;
        return;
    }
    uint64_t mod = gbm_bo_get_modifier(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    printf("%s %ux%u mod=0x%016" PRIx64 " stride=%u\n", tag, w, h, mod, stride);

    char first[128] = "";
    unsigned bad;

    /* GL -> CPU (GL can't render to linear) */
    if (!is_linear(mod)) {
        if (!gl_render_pattern(bo)) {
            printf("    FAIL  GL render\n");
            failures++;
            goto out;
        }

        int dmabuf = gbm_bo_get_fd(bo);
        size_t size = (size_t)lseek(dmabuf, 0, SEEK_END);
        close(dmabuf);
        uint8_t *raw = raw_map(fd, bo, size);
        if (raw == NULL) {
            printf("    info  raw map failed (%s)\n", strerror(errno));
        } else {
            uint8_t *lin = malloc((size_t)w * h * 4);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    memcpy(lin + ((size_t)y * w + x) * 4,
                           raw + bl_offset(x * 4, y, stride, mod & 0xf), 4);
            bad = compare(lin, w * 4, 0, 0, w, h, first, sizeof(first));
            report("raw CPU read, detiled with the Turing+ GOB layout", bad, w * h,
                   first, !is_compressed(mod));
            free(lin);
            munmap(raw, size);
        }

        uint32_t mstride;
        void *md = NULL;
        uint8_t *p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &mstride, &md);
        if (p == NULL) {
            printf("    info  gbm_bo_map(READ) failed (%s)\n", strerror(errno));
        } else {
            bad = compare(p, mstride, 0, 0, w, h, first, sizeof(first));
            report("gbm_bo_map(READ) full", bad, w * h, first, true);
            gbm_bo_unmap(bo, md);
        }

        uint32_t sx = w > 13 ? 13 : 0, sy = h > 7 ? 7 : 0;
        uint32_t sw = w - sx < 50 ? w - sx : 50, sh = h - sy < 20 ? h - sy : 20;
        p = gbm_bo_map(bo, sx, sy, sw, sh, GBM_BO_TRANSFER_READ, &mstride, &md);
        if (p != NULL) {
            bad = compare(p, mstride, sx, sy, sw, sh, first, sizeof(first));
            report("gbm_bo_map(READ) sub-rectangle", bad, sw * sh, first, true);
            gbm_bo_unmap(bo, md);
        }
    }

    /* CPU -> GL */
    {
        uint32_t mstride;
        void *md = NULL;
        uint8_t *p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &mstride, &md);
        if (p == NULL) {
            printf("    info  gbm_bo_map(WRITE) failed (%s)\n", strerror(errno));
            goto out;
        }
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                pattern(x, y, p + (size_t)y * mstride + x * 4);
        gbm_bo_unmap(bo, md);

        /* Did the write land? Read it back through the backend first. */
        p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &mstride, &md);
        if (p != NULL) {
            bad = compare(p, mstride, 0, 0, w, h, first, sizeof(first));
            report("gbm_bo_map(WRITE) then gbm_bo_map(READ)", bad, w * h, first, true);
            gbm_bo_unmap(bo, md);
        }

        uint8_t *seen = gl_sample(bo);
        if (seen == NULL) {
            printf("    FAIL  GL sample\n");
            failures++;
            goto out;
        }
        bad = compare(seen, w * 4, 0, 0, w, h, first, sizeof(first));
        report("gbm_bo_map(WRITE) then GL samples it", bad, w * h, first, true);
        free(seen);
    }
out:
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

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
    destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
    image_target_rb = (void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    image_target_tex = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        fprintf(stderr, "EGL failed: 0x%x\n", eglGetError());
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    prog_pattern = link_program(fs_pattern_src);
    prog_sample = link_program(fs_sample_src);
    prog_sample_ext = link_program(fs_sample_ext_src);

    /* Uncompressed block-linear modifiers for explicit GOB heights. */
    struct gbm_bo *ref = gbm_bo_create(gbm, 64, 64, DRM_FORMAT_ABGR8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    uint64_t plain = gbm_bo_get_modifier(ref) & ~0xfull;
    gbm_bo_destroy(ref);

    static const uint32_t sizes[][2] = { { 64, 64 }, { 100, 37 }, { 1920, 1080 } };
    for (unsigned s = 0; s < 3; s++) {
        uint32_t w = sizes[s][0], h = sizes[s][1];
        test_case(fd, gbm, "scanout   ", w, h, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING, NULL, 0);
        test_case(fd, gbm, "compressed", w, h, GBM_BO_USE_RENDERING, NULL, 0);
        test_case(fd, gbm, "linear    ", w, h, GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT, NULL, 0);
    }
    for (unsigned hh = 0; hh <= 5; hh += 5) {
        uint64_t m = plain | hh;
        char tag[16];
        snprintf(tag, sizeof(tag), "h%u        ", hh);
        test_case(fd, gbm, tag, 300, 200, GBM_BO_USE_RENDERING, &m, 1);
    }

    printf("\n%d failure(s)\n", failures);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);
    return failures != 0;
}
