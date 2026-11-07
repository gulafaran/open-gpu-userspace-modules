/*
 * SPDX-License-Identifier: MIT
 *
 * Creates BOs over a matrix of sizes, formats and usages and prints the
 * layout the backend chose: modifier, planes, strides, offsets and the
 * dma-buf size. Run it against the closed and our backend and diff.
 *
 *   build/nvidia-gbm/gbm-layout > closed.txt
 *   GBM_BACKENDS_PATH=build/nvidia-gbm GBM_BACKEND=nvidia-open \
 *       build/nvidia-gbm/gbm-layout > open.txt
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>

static int open_nvidia_render_node(void)
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

static void describe_modifier(uint64_t mod, char *buf, size_t len)
{
    if (mod == DRM_FORMAT_MOD_LINEAR) {
        snprintf(buf, len, "LINEAR");
    } else if (mod == DRM_FORMAT_MOD_INVALID) {
        snprintf(buf, len, "INVALID");
    } else if ((mod >> 56) == DRM_FORMAT_MOD_VENDOR_NVIDIA) {
        snprintf(buf, len, "NV_BL(c=%u,s=%u,g=%u,k=0x%02x,h=%u)",
                 (unsigned)((mod >> 23) & 0x7), (unsigned)((mod >> 22) & 0x1),
                 (unsigned)((mod >> 20) & 0x3), (unsigned)((mod >> 12) & 0xff),
                 (unsigned)(mod & 0xf));
    } else {
        snprintf(buf, len, "0x%016" PRIx64, mod);
    }
}

struct usage_case {
    const char *name;
    uint32_t usage;
};

static void try_bo(struct gbm_device *gbm, const char *tag, uint32_t w, uint32_t h,
                   uint32_t format, uint32_t usage, const uint64_t *mods,
                   unsigned nmods)
{
    struct gbm_bo *bo;

    if (mods)
        bo = gbm_bo_create_with_modifiers2(gbm, w, h, format, mods, nmods, usage);
    else
        bo = gbm_bo_create(gbm, w, h, format, usage);

    printf("%-14s %5ux%-5u %.4s: ", tag, w, h, (const char *)&format);
    if (bo == NULL) {
        printf("FAILED (%m)\n");
        return;
    }

    char mod[64];
    describe_modifier(gbm_bo_get_modifier(bo), mod, sizeof(mod));
    int fd = gbm_bo_get_fd(bo);
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (fd >= 0)
        close(fd);

    printf("%s size=%lld", mod, (long long)size);
    int planes = gbm_bo_get_plane_count(bo);
    for (int p = 0; p < planes; p++)
        printf(" p%d[stride=%u off=%u]", p, gbm_bo_get_stride_for_plane(bo, p),
               gbm_bo_get_offset(bo, p));
    printf("\n");
    gbm_bo_destroy(bo);
}

int main(void)
{
    int fd = open_nvidia_render_node();
    if (fd < 0) {
        fprintf(stderr, "no nvidia-drm render node\n");
        return 1;
    }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (gbm == NULL) {
        fprintf(stderr, "gbm_create_device failed\n");
        return 1;
    }
    printf("backend: %s\n", gbm_device_get_backend_name(gbm));

    static const uint32_t sizes[][2] = {
        { 1, 1 }, { 16, 16 }, { 64, 8 }, { 64, 64 }, { 100, 37 }, { 256, 256 },
        { 640, 480 }, { 1000, 1000 }, { 1920, 1080 }, { 2560, 1440 },
        { 3840, 2160 }, { 5120, 1440 }, { 8192, 8192 },
    };
    static const struct usage_case usages[] = {
        { "render", GBM_BO_USE_RENDERING },
        { "scanout", GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING },
        { "linear", GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING },
        { "linear-scan", GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT },
        { "cursor", GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE },
    };
    static const uint32_t formats[] = {
        GBM_FORMAT_XRGB8888, GBM_FORMAT_ARGB8888, GBM_FORMAT_ABGR16161616F,
        GBM_FORMAT_XRGB2101010, GBM_FORMAT_RGB565, GBM_FORMAT_R8,
        DRM_FORMAT_NV12, DRM_FORMAT_P010,
    };

    printf("\n# format support (render / scanout)\n");
    for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++)
        printf("%.4s: %d / %d\n", (const char *)&formats[f],
               gbm_device_is_format_supported(gbm, formats[f], GBM_BO_USE_RENDERING),
               gbm_device_is_format_supported(gbm, formats[f], GBM_BO_USE_SCANOUT));

    printf("\n# gbm_bo_create, XRGB8888\n");
    for (unsigned u = 0; u < sizeof(usages) / sizeof(usages[0]); u++)
        for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
            try_bo(gbm, usages[u].name, sizes[s][0], sizes[s][1],
                   GBM_FORMAT_XRGB8888, usages[u].usage, NULL, 0);

    printf("\n# gbm_bo_create, formats at 1920x1080 and 100x37\n");
    for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        try_bo(gbm, "render", 1920, 1080, formats[f], GBM_BO_USE_RENDERING, NULL, 0);
        try_bo(gbm, "render", 100, 37, formats[f], GBM_BO_USE_RENDERING, NULL, 0);
    }

    /* Modifier lists as a compositor passes them from
     * eglQueryDmaBufModifiersEXT. The uncompressed base comes from a scanout
     * BO; the compressed one from a render BO. */
    struct gbm_bo *ref = gbm_bo_create(gbm, 256, 256, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    uint64_t plain = ref ? gbm_bo_get_modifier(ref) & ~0xfull : DRM_FORMAT_MOD_INVALID;
    if (ref)
        gbm_bo_destroy(ref);
    ref = gbm_bo_create(gbm, 256, 256, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    uint64_t comp = ref ? gbm_bo_get_modifier(ref) & ~0xfull : DRM_FORMAT_MOD_INVALID;
    if (ref)
        gbm_bo_destroy(ref);

    if ((plain >> 56) == DRM_FORMAT_MOD_VENDOR_NVIDIA) {
        uint64_t unc[7], all[13];
        unsigned nunc = 0, nall = 0;
        for (unsigned hh = 0; hh <= 5; hh++) {
            unc[nunc++] = plain | hh;
            all[nall++] = plain | hh;
            if (comp != plain && (comp >> 56) == DRM_FORMAT_MOD_VENDOR_NVIDIA)
                all[nall++] = comp | hh;
        }
        unc[nunc++] = DRM_FORMAT_MOD_LINEAR;
        all[nall++] = DRM_FORMAT_MOD_LINEAR;

        static const struct usage_case musages[] = {
            { "render", GBM_BO_USE_RENDERING },
            { "scanout", GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING },
            { "none", 0 },
        };
        for (unsigned u = 0; u < sizeof(musages) / sizeof(musages[0]); u++) {
            char tag[32];
            printf("\n# with_modifiers2 usage=%s, uncompressed h0-5 + linear\n",
                   musages[u].name);
            snprintf(tag, sizeof(tag), "unc-%s", musages[u].name);
            for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
                try_bo(gbm, tag, sizes[s][0], sizes[s][1], GBM_FORMAT_XRGB8888,
                       musages[u].usage, unc, nunc);
            printf("\n# with_modifiers2 usage=%s, compressed+uncompressed h0-5 + linear\n",
                   musages[u].name);
            snprintf(tag, sizeof(tag), "all-%s", musages[u].name);
            for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
                try_bo(gbm, tag, sizes[s][0], sizes[s][1], GBM_FORMAT_XRGB8888,
                       musages[u].usage, all, nall);
        }

        printf("\n# with_modifiers2, one modifier each\n");
        for (unsigned i = 0; i < nall; i++) {
            char tag[16];
            snprintf(tag, sizeof(tag), "only-%u", i);
            try_bo(gbm, tag, 1920, 1080, GBM_FORMAT_XRGB8888,
                   GBM_BO_USE_RENDERING, &all[i], 1);
            try_bo(gbm, tag, 100, 37, GBM_FORMAT_XRGB8888,
                   GBM_BO_USE_RENDERING, &all[i], 1);
            try_bo(gbm, tag, 100, 37, GBM_FORMAT_XRGB8888,
                   GBM_BO_USE_SCANOUT, &all[i], 1);
        }
        printf("\n# with_modifiers2 NV12 and AB4H, all\n");
        try_bo(gbm, "all-render", 1920, 1080, DRM_FORMAT_NV12, GBM_BO_USE_RENDERING, all, nall);
        try_bo(gbm, "all-render", 1920, 1080, GBM_FORMAT_ABGR16161616F, GBM_BO_USE_RENDERING, all, nall);
        try_bo(gbm, "all-render", 1920, 1080, GBM_FORMAT_RGB565, GBM_BO_USE_RENDERING, all, nall);
    }

    gbm_device_destroy(gbm);
    close(fd);
    return 0;
}
