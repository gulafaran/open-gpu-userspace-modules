/*
 * SPDX-License-Identifier: MIT
 *
 * Robustness checks for nvchan:
 *  - leaks:   create/use/destroy cycles keep the number of open files;
 *  - wrap:    many queued operations wrap the push buffer before finish();
 *  - threads: concurrent channels and buffer churn on one device;
 *  - fork:    a child can't use or free the parent's objects, can open its
 *             own device, and the parent's channel keeps working;
 *  - fault:   (only with --fault) a deliberately faulting copy makes
 *             finish() return -EIO and a new channel works. This logs an
 *             Xid in the kernel log.
 *
 *   build/nvchan/nvchan-stress [--fault]
 */

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nvchan-priv.h"   /* the fault test builds a buffer that isn't mapped */

static int failures;
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;

#define CHECK(cond, ...) do {                       \
        pthread_mutex_lock(&print_lock);            \
        printf((cond) ? "  ok    " : "  FAIL  ");   \
        if (!(cond))                                \
            failures++;                             \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
        pthread_mutex_unlock(&print_lock);          \
    } while (0)

static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    int n = 0;

    while (d && readdir(d))
        n++;
    if (d)
        closedir(d);
    return n;
}

static uint32_t pattern(uint32_t seed, size_t i)
{
    uint64_t x = (seed + i) * 0x9e3779b97f4a7c15ull;
    return (uint32_t)(x >> 29);
}

/* sysmem -> vidmem -> sysmem of size bytes, verified. */
static bool round_trip(struct nvchan_device *dev, struct nvchan_channel *ch,
                       uint64_t size, uint32_t seed)
{
    struct nvchan_buffer *s = nvchan_buffer_create(dev, size, NVCHAN_SYSMEM, NVCHAN_PITCH, NULL, 0);
    struct nvchan_buffer *v = nvchan_buffer_create(dev, size, NVCHAN_VIDMEM, NVCHAN_PITCH, NULL, 0);
    struct nvchan_buffer *d = nvchan_buffer_create(dev, size, NVCHAN_SYSMEM, NVCHAN_PITCH, NULL, 0);
    bool ok = s && v && d;

    if (ok) {
        uint32_t *sp = nvchan_buffer_map(s), *dp = nvchan_buffer_map(d);
        for (size_t i = 0; i < size / 4; i++)
            sp[i] = pattern(seed, i);
        memset(dp, 0, size);
        ok = nvchan_copy(ch, v, 0, s, 0, size) == 0 &&
             nvchan_copy(ch, d, 0, v, 0, size) == 0 &&
             nvchan_channel_finish(ch, 5000) == 0 &&
             memcmp(sp, dp, size) == 0;
    }
    nvchan_buffer_destroy(s);
    nvchan_buffer_destroy(v);
    nvchan_buffer_destroy(d);
    return ok;
}

static void test_leaks(struct nvchan_device *dev)
{
    printf("leaks\n");
    /* One cycle first: lazily opened files are not leaks. */
    struct nvchan_channel *ch = nvchan_channel_create_copy(dev, NULL, 0);
    if (ch == NULL) {
        CHECK(false, "channel");
        return;
    }
    round_trip(dev, ch, 1 << 20, 0);
    nvchan_channel_destroy(ch);

    const int fds = count_fds();
    int bad = 0;
    for (int i = 0; i < 100; i++) {
        ch = nvchan_channel_create_copy(dev, NULL, 0);
        if (ch == NULL || !round_trip(dev, ch, 1 << 20, (uint32_t)i))
            bad++;
        nvchan_channel_destroy(ch);
    }
    CHECK(bad == 0, "100 channel + buffer cycles (%d failed)", bad);
    CHECK(count_fds() == fds, "open files unchanged (%d before, %d after)", fds, count_fds());
}

static void test_wrap(struct nvchan_device *dev)
{
    printf("push buffer wrap\n");
    const int n = 20000;
    const uint64_t slot = 4096, size = (uint64_t)n * slot;
    struct nvchan_channel *ch = nvchan_channel_create_copy(dev, NULL, 0);
    struct nvchan_buffer *v = nvchan_buffer_create(dev, size, NVCHAN_VIDMEM, NVCHAN_PITCH, NULL, 0);
    struct nvchan_buffer *s = nvchan_buffer_create(dev, size, NVCHAN_SYSMEM, NVCHAN_PITCH, NULL, 0);

    if (ch && v && s) {
        int queued = 0;
        for (int i = 0; i < n; i++)
            queued += nvchan_fill32(ch, v, (uint64_t)i * slot, slot, 0x10000000u + (uint32_t)i) == 0;
        nvchan_copy(ch, s, 0, v, 0, size);
        int ret = nvchan_channel_finish(ch, 10000);
        size_t bad = 0;
        const uint32_t *p = nvchan_buffer_map(s);
        for (int i = 0; i < n && ret == 0; i++)
            for (uint64_t w = 0; w < slot / 4; w += 256)
                bad += p[i * slot / 4 + w] != 0x10000000u + (uint32_t)i;
        CHECK(queued == n && ret == 0 && bad == 0,
              "%d fills queued without finish(), all landed (finish %d, %zu wrong)", n, ret, bad);
    } else {
        CHECK(false, "setup");
    }
    nvchan_buffer_destroy(s);
    nvchan_buffer_destroy(v);
    nvchan_channel_destroy(ch);
}

struct thread_arg {
    struct nvchan_device *dev;
    int id, rounds, bad;
};

static void *thread_main(void *p)
{
    struct thread_arg *a = p;
    struct nvchan_channel *ch = nvchan_channel_create_copy(a->dev, NULL, 0);

    if (ch == NULL) {
        a->bad = a->rounds;
        return NULL;
    }
    for (int r = 0; r < a->rounds; r++) {
        uint64_t size = (uint64_t)(64 + (r * 37 + a->id * 11) % 512) << 10;
        if (!round_trip(a->dev, ch, size, (uint32_t)(a->id << 16 | r)))
            a->bad++;
    }
    nvchan_channel_destroy(ch);
    return NULL;
}

static void test_threads(struct nvchan_device *dev)
{
    printf("threads\n");
    enum { N = 4 };
    pthread_t t[N];
    struct thread_arg a[N];
    int bad = 0;

    for (int i = 0; i < N; i++) {
        a[i] = (struct thread_arg){ dev, i, 50, 0 };
        pthread_create(&t[i], NULL, thread_main, &a[i]);
    }
    for (int i = 0; i < N; i++) {
        pthread_join(t[i], NULL);
        bad += a[i].bad;
    }
    CHECK(bad == 0, "%d threads x 50 rounds, own channel each, buffers churned (%d failed)", N, bad);
}

static void test_fork(struct nvchan_device *dev)
{
    printf("fork\n");
    struct nvchan_channel *ch = nvchan_channel_create_copy(dev, NULL, 0);
    struct nvchan_buffer *b = nvchan_buffer_create(dev, 1 << 20, NVCHAN_SYSMEM, NVCHAN_PITCH, NULL, 0);
    if (ch == NULL || b == NULL) {
        CHECK(false, "setup");
        return;
    }
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: exit status bit per failed check. */
        int st = 0;
        char err[128] = "";
        if (nvchan_device_is_usable(dev))
            st |= 1;
        if (nvchan_channel_create_copy(dev, err, sizeof(err)) != NULL)
            st |= 2;
        if (nvchan_buffer_create(dev, 65536, NVCHAN_SYSMEM, NVCHAN_PITCH, NULL, 0) != NULL)
            st |= 4;
        nvchan_fill32(ch, b, 0, 4096, 1);
        if (nvchan_channel_finish(ch, 1000) != -EPERM)
            st |= 8;
        /* Must only drop the child's copies. */
        nvchan_buffer_destroy(b);
        nvchan_channel_destroy(ch);
        nvchan_device_close(dev);

        struct nvchan_device *own = nvchan_device_open(0, NULL, 0);
        struct nvchan_channel *och = own ? nvchan_channel_create_copy(own, NULL, 0) : NULL;
        if (och == NULL || !round_trip(own, och, 1 << 20, 99))
            st |= 16;
        nvchan_channel_destroy(och);
        nvchan_device_close(own);
        _exit(st);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int st = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
    CHECK(!(st & 1), "child sees the inherited device as unusable");
    CHECK(!(st & 6), "child can't create channels or buffers on it");
    CHECK(!(st & 8), "child's finish() on the parent's channel is refused");
    CHECK(!(st & 16), "child's own device works");
    CHECK(st != 255, "child exited normally");

    /* The child destroyed its copies; the parent's objects must be intact. */
    uint32_t *p = nvchan_buffer_map(b);
    memset(p, 0, 4096);
    int ret = nvchan_fill32(ch, b, 0, 4096, 0xabcdef01);
    ret = ret ? ret : nvchan_channel_finish(ch, 5000);
    CHECK(ret == 0 && p[0] == 0xabcdef01 && p[1023] == 0xabcdef01,
          "parent's channel and buffer still work");
    nvchan_buffer_destroy(b);
    nvchan_channel_destroy(ch);
}

static void test_fault(struct nvchan_device *dev)
{
    printf("fault (an Xid in the kernel log is expected)\n");
    struct nvchan_channel *ch = nvchan_channel_create_copy(dev, NULL, 0);
    struct nvchan_buffer *ok = nvchan_buffer_create(dev, 1 << 20, NVCHAN_SYSMEM, NVCHAN_PITCH, NULL, 0);
    if (ch == NULL || ok == NULL) {
        CHECK(false, "setup");
        return;
    }
    /* A buffer nothing is mapped at: the copy engine faults. */
    struct nvchan_buffer bogus = {
        .dev = dev, .size = 1 << 20, .gpu_va = 0x7f0000000000ull,
    };
    nvchan_copy(ch, ok, 0, &bogus, 0, 1 << 20);
    int ret = nvchan_channel_finish(ch, 5000);
    CHECK(ret == -EIO, "finish() reports the channel error (%d)", ret);
    nvchan_channel_destroy(ch);

    ch = nvchan_channel_create_copy(dev, NULL, 0);
    CHECK(ch != NULL && round_trip(dev, ch, 1 << 20, 7), "a new channel works afterwards");
    nvchan_channel_destroy(ch);
    nvchan_buffer_destroy(ok);
}

int main(int argc, char **argv)
{
    bool fault = argc > 1 && strcmp(argv[1], "--fault") == 0;
    char err[256] = "";
    struct nvchan_device *dev = nvchan_device_open(0, err, sizeof(err));

    if (dev == NULL) {
        fprintf(stderr, "device: %s\n", err);
        return 1;
    }
    printf("%s\n", nvchan_device_name(dev));

    test_leaks(dev);
    test_wrap(dev);
    test_threads(dev);
    test_fork(dev);
    if (fault)
        test_fault(dev);

    printf("\n%d failure(s)\n", failures);
    nvchan_device_close(dev);
    return failures != 0;
}
