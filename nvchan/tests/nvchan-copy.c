/*
 * SPDX-License-Identifier: MIT
 *
 * Copy engine work through nvchan, checked on the CPU:
 *  - fill in system memory;
 *  - system -> video -> system round trip, also at unaligned offsets;
 *  - pitch -> block-linear -> pitch round trip of an image in video
 *    memory, and the block-linear bytes read through BAR1 and detiled with
 *    the Turing+ GOB layout (the one DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D
 *    describes), so the copy engine and GBM agree on the layout;
 *  - throughput.
 *
 *   build/nvchan/nvchan-copy [minor]
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvchan.h"

static int failures;

#define CHECK(cond, ...) do {                       \
        printf((cond) ? "  ok    " : "  FAIL  ");   \
        if (!(cond))                                \
            failures++;                             \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
    } while (0)

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double cpu_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint32_t pattern(uint64_t i)
{
    uint64_t x = i * 0x9e3779b97f4a7c15ull;
    return (uint32_t)(x >> 32) ^ (uint32_t)i;
}

/* Byte offset of byte xb in row y of a block-linear surface (Turing+ color
 * GOBs), as in nvidia-gbm. */
static size_t bl_offset(uint32_t xb, uint32_t y, uint32_t stride, uint32_t log2_gobs)
{
    uint32_t gob_x = xb / 64, gob_y = y / 8, bx = xb % 64, by = y % 8;
    size_t block = (size_t)(gob_y >> log2_gobs) * (stride / 64) + gob_x;
    size_t gob = (block << log2_gobs) + (gob_y & ((1u << log2_gobs) - 1));

    return gob * 512 + (bx >> 5) * 256 + (by >> 2) * 128 + ((bx >> 4) & 1) * 64 +
           (by & 3) * 16 + (bx & 15);
}

static struct nvchan_buffer *buffer(struct nvchan_device *dev, uint64_t size,
                                    enum nvchan_location loc, enum nvchan_layout layout)
{
    char err[256];
    struct nvchan_buffer *b = nvchan_buffer_create(dev, size, loc, layout, err, sizeof(err));
    if (b == NULL) {
        printf("  FAIL  buffer: %s\n", err);
        failures++;
    }
    return b;
}

static int finish(struct nvchan_channel *ch, const char *what)
{
    int ret = nvchan_channel_finish(ch, 5000);
    if (ret != 0) {
        printf("  FAIL  %s: finish %d\n", what, ret);
        failures++;
    }
    return ret;
}

static void test_fill(struct nvchan_device *dev, struct nvchan_channel *ch)
{
    printf("fill, system memory\n");
    const uint64_t size = 4 << 20;
    struct nvchan_buffer *b = buffer(dev, size, NVCHAN_SYSMEM, NVCHAN_PITCH);
    if (!b)
        return;
    uint32_t *cpu = nvchan_buffer_map(b);
    memset(cpu, 0, size);
    nvchan_fill32(ch, b, 4096, size - 8192, 0xdeadbeef);
    if (finish(ch, "fill") == 0) {
        size_t bad = 0;
        for (size_t i = 0; i < size / 4; i++) {
            uint32_t want = (i >= 1024 && i < size / 4 - 1024) ? 0xdeadbeef : 0;
            bad += cpu[i] != want;
        }
        CHECK(bad == 0, "4 MiB minus 4 KiB at each end filled, rest untouched (%zu wrong)", bad);
    }
    nvchan_buffer_destroy(b);
}

static void test_roundtrip(struct nvchan_device *dev, struct nvchan_channel *ch)
{
    printf("system -> video -> system\n");
    const uint64_t size = 64 << 20;
    struct nvchan_buffer *src = buffer(dev, size, NVCHAN_SYSMEM, NVCHAN_PITCH);
    struct nvchan_buffer *vid = buffer(dev, size, NVCHAN_VIDMEM, NVCHAN_PITCH);
    struct nvchan_buffer *dst = buffer(dev, size, NVCHAN_SYSMEM, NVCHAN_PITCH);
    if (!src || !vid || !dst)
        goto out;

    uint32_t *s = nvchan_buffer_map(src), *d = nvchan_buffer_map(dst);
    for (size_t i = 0; i < size / 4; i++)
        s[i] = pattern(i);
    memset(d, 0, size);

    nvchan_copy(ch, vid, 0, src, 0, size);
    nvchan_copy(ch, dst, 0, vid, 0, size);
    if (finish(ch, "round trip") == 0)
        CHECK(memcmp(s, d, size) == 0, "64 MiB pattern survives the round trip");

    /* Unaligned: 3 bytes in, 1000003 bytes, to offset 7. */
    memset(d, 0, size);
    nvchan_copy(ch, vid, 5, src, 3, 1000003);
    nvchan_copy(ch, dst, 7, vid, 5, 1000003);
    if (finish(ch, "unaligned") == 0) {
        const uint8_t *sb = (const uint8_t *)s, *db = (const uint8_t *)d;
        bool ok = memcmp(sb + 3, db + 7, 1000003) == 0;
        for (size_t i = 0; i < 7 && ok; i++)
            ok = db[i] == 0;
        ok = ok && db[7 + 1000003] == 0;
        CHECK(ok, "unaligned offsets and length, neighbours untouched");
    }

    /* Video memory through BAR1. */
    uint32_t *v = nvchan_buffer_map(vid);
    if (v == NULL) {
        CHECK(false, "CPU map of video memory");
    } else {
        nvchan_copy(ch, vid, 0, src, 0, size);
        if (finish(ch, "upload") == 0) {
            size_t bad = 0;
            for (size_t i = 0; i < size / 4; i += 997)
                bad += v[i] != s[i];
            CHECK(bad == 0, "video memory read through BAR1 matches (sampled)");
        }
    }
out:
    nvchan_buffer_destroy(src);
    nvchan_buffer_destroy(vid);
    nvchan_buffer_destroy(dst);
}

static void test_block_linear(struct nvchan_device *dev, struct nvchan_channel *ch,
                              uint32_t w, uint32_t h, uint32_t log2_gobs)
{
    printf("pitch -> block-linear (h=%u) -> pitch, %ux%u x 4 bytes\n", log2_gobs, w, h);
    const uint32_t row = w * 4, pitch = (row + 255) & ~255u;
    const uint32_t bl_stride = (row + 63) & ~63u;
    const uint32_t rows = (h + (8u << log2_gobs) - 1) / (8u << log2_gobs) * (8u << log2_gobs);
    const uint64_t bl_size = (uint64_t)bl_stride * rows;

    struct nvchan_buffer *src = buffer(dev, (uint64_t)pitch * h, NVCHAN_SYSMEM, NVCHAN_PITCH);
    struct nvchan_buffer *dst = buffer(dev, (uint64_t)pitch * h, NVCHAN_SYSMEM, NVCHAN_PITCH);
    struct nvchan_buffer *bl = buffer(dev, bl_size, NVCHAN_VIDMEM, NVCHAN_BLOCK_LINEAR);
    if (!src || !dst || !bl)
        goto out;

    uint8_t *s = nvchan_buffer_map(src), *d = nvchan_buffer_map(dst);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            ((uint32_t *)(s + (size_t)y * pitch))[x] = pattern((uint64_t)y << 20 | x);
    memset(d, 0, (size_t)pitch * h);

    struct nvchan_surface ps = { .buf = src, .pitch = pitch, .height = h };
    struct nvchan_surface pd = { .buf = dst, .pitch = pitch, .height = h };
    struct nvchan_surface bs = { .buf = bl, .pitch = bl_stride, .height = h,
                                 .block_linear = true, .log2_gobs = log2_gobs };

    nvchan_copy_2d(ch, &bs, 0, 0, &ps, 0, 0, row, h);
    nvchan_copy_2d(ch, &pd, 0, 0, &bs, 0, 0, row, h);
    if (finish(ch, "block-linear") == 0) {
        size_t bad = 0;
        for (uint32_t y = 0; y < h; y++)
            bad += memcmp(s + (size_t)y * pitch, d + (size_t)y * pitch, row) != 0;
        CHECK(bad == 0, "round trip matches (%zu rows wrong)", bad);
    }

    const uint8_t *raw = nvchan_buffer_map(bl);
    if (raw == NULL) {
        CHECK(false, "CPU map of the block-linear buffer");
    } else {
        size_t bad = 0;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t got, want = ((uint32_t *)(s + (size_t)y * pitch))[x];
                memcpy(&got, raw + bl_offset(x * 4, y, bl_stride, log2_gobs), 4);
                bad += got != want;
            }
        CHECK(bad == 0, "layout is DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D's (%zu pixels wrong)", bad);
    }

    /* A sub-rectangle at an offset: from (128 bytes, row 10). */
    const uint32_t sub_w = row - 128 < 2000 ? row - 128 : 2000;
    const uint32_t sub_h = h - 10 < 100 ? h - 10 : 100;
    memset(d, 0, (size_t)pitch * h);
    if (nvchan_copy_2d(ch, &pd, 64, 5, &bs, 128, 10, sub_w, sub_h) != 0) {
        CHECK(false, "sub-rectangle rejected");
    } else if (finish(ch, "sub-rectangle") == 0) {
        size_t bad = 0;
        for (uint32_t y = 0; y < sub_h; y++)
            bad += memcmp(s + (size_t)(y + 10) * pitch + 128,
                          d + (size_t)(y + 5) * pitch + 64, sub_w) != 0;
        CHECK(bad == 0, "sub-rectangle from block-linear at an offset (%zu rows wrong)", bad);
    }

    /* Out of bounds must be refused before it reaches the GPU. */
    CHECK(nvchan_copy_2d(ch, &pd, 0, 0, &bs, 64, 0, row, h) == -EINVAL,
          "rectangle past the surface is rejected");
out:
    nvchan_buffer_destroy(src);
    nvchan_buffer_destroy(dst);
    nvchan_buffer_destroy(bl);
}

static void test_throughput(struct nvchan_device *dev, struct nvchan_channel *ch)
{
    printf("throughput\n");
    const uint64_t size = 256 << 20;
    struct nvchan_buffer *a = buffer(dev, size, NVCHAN_VIDMEM, NVCHAN_PITCH);
    struct nvchan_buffer *b = buffer(dev, size, NVCHAN_VIDMEM, NVCHAN_PITCH);
    struct nvchan_buffer *s = buffer(dev, size, NVCHAN_SYSMEM, NVCHAN_PITCH);
    if (a && b && s) {
        nvchan_copy(ch, b, 0, a, 0, size);   /* warm up */
        finish(ch, "warm up");
        double t = now(), c = cpu_now();
        for (int i = 0; i < 4; i++)
            nvchan_copy(ch, b, 0, a, 0, size);
        if (finish(ch, "vid->vid") == 0) {
            double wall = now() - t, cpu = cpu_now() - c;
            printf("  info  video -> video: %.1f GB/s\n", 4 * size / wall / 1e9);
            printf("  info  waiting: %.1f ms wall, %.1f ms CPU (%.0f %%)\n",
                   wall * 1e3, cpu * 1e3, 100 * cpu / wall);
            if (nvchan_channel_has_events(ch))
                CHECK(cpu < wall * 0.5, "finish() sleeps instead of spinning");
        }
        t = now();
        nvchan_copy(ch, a, 0, s, 0, size);
        if (finish(ch, "sys->vid") == 0)
            printf("  info  system -> video: %.1f GB/s\n", size / (now() - t) / 1e9);
        t = now();
        nvchan_copy(ch, s, 0, a, 0, size);
        if (finish(ch, "vid->sys") == 0)
            printf("  info  video -> system: %.1f GB/s\n", size / (now() - t) / 1e9);
    }
    nvchan_buffer_destroy(a);
    nvchan_buffer_destroy(b);
    nvchan_buffer_destroy(s);
}

static void test_latency(struct nvchan_device *dev, struct nvchan_channel *ch)
{
    printf("small operations\n");
    struct nvchan_buffer *b = buffer(dev, 65536, NVCHAN_VIDMEM, NVCHAN_PITCH);
    if (b == NULL)
        return;
    const int n = 2000;
    double t = now(), c = cpu_now();
    int ret = 0;
    for (int i = 0; i < n && ret == 0; i++) {
        nvchan_fill32(ch, b, 0, 4096, (uint32_t)i);
        ret = nvchan_channel_finish(ch, 5000);
    }
    double wall = now() - t, cpu = cpu_now() - c;
    CHECK(ret == 0, "%d fill + finish rounds", n);
    printf("  info  %.1f us per round, %.0f %% CPU\n", wall * 1e6 / n, 100 * cpu / wall);
    nvchan_buffer_destroy(b);
}

int main(int argc, char **argv)
{
    int minor = argc > 1 ? atoi(argv[1]) : 0;
    char err[256] = "";

    struct nvchan_device *dev = nvchan_device_open(minor, err, sizeof(err));
    if (dev == NULL) {
        fprintf(stderr, "device: %s\n", err);
        return 1;
    }
    struct nvchan_channel *ch = nvchan_channel_create_copy(dev, err, sizeof(err));
    if (ch == NULL) {
        fprintf(stderr, "copy channel: %s\n", err);
        nvchan_device_close(dev);
        return 1;
    }
    printf("%s, copy class 0x%04x, completion: %s\n", nvchan_device_name(dev),
           nvchan_device_copy_class(dev),
           nvchan_channel_has_events(ch) ? "interrupt events" : "polling");

    printf("argument checks\n");
    {
        struct nvchan_buffer *b = buffer(dev, 65536, NVCHAN_SYSMEM, NVCHAN_PITCH);
        if (b) {
            CHECK(nvchan_fill32(ch, b, 65536 - 4, 8, 0) == -EINVAL, "fill past the end rejected");
            CHECK(nvchan_copy(ch, b, 0, b, 1, 65536) == -EINVAL, "copy past the end rejected");
            CHECK(nvchan_fill32(ch, b, 0, 6, 0) == -EINVAL, "fill size not a multiple of 4 rejected");
            nvchan_buffer_destroy(b);
        }
    }

    test_fill(dev, ch);
    test_roundtrip(dev, ch);
    test_block_linear(dev, ch, 1001, 333, 4);
    test_block_linear(dev, ch, 1920, 1080, 4);
    test_block_linear(dev, ch, 300, 200, 0);
    test_block_linear(dev, ch, 300, 200, 5);
    test_throughput(dev, ch);
    test_latency(dev, ch);

    printf("\n%d failure(s)\n", failures);
    nvchan_channel_destroy(ch);
    nvchan_device_close(dev);
    return failures != 0;
}
