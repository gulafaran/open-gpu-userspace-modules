/* LD_PRELOAD: log NVIDIA RM escapes (control cmd, alloc class) and mmaps to stderr. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
struct os54 { uint32_t hClient, hObject, cmd, flags; uint64_t params; uint32_t size, status; };
struct os64 { uint32_t hRoot, hParent, hNew, hClass; uint64_t params, rights; uint32_t size, flags, status; };
struct xfer { uint32_t cmd, size; uint64_t ptr; };
static int (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void out(const char *fmt, ...) { char b[512]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a); write(2, b, n); }
static void decode(int fd, unsigned nr, void *arg, int ret, double us) {
    if (nr == 0x2a) { struct os54 *p = arg; out("IO ctrl 0x%08x size %u status 0x%x %.0fus\n", p->cmd, p->size, p->status, us); }
    else if (nr == 0x2b) { struct os64 *p = arg; out("IO alloc class 0x%04x status 0x%x\n", p->hClass, p->status); }
    else if (nr == 0x29) out("IO free\n");
    else out("IO esc 0x%x ret %d\n", nr, ret);
}
int ioctl(int fd, unsigned long req, ...) {
    va_list a; va_start(a, req); void *arg = va_arg(a, void *); va_end(a);
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    double t0 = now();
    int ret = real_ioctl(fd, req, arg);
    double us = (now() - t0) * 1e3;
    if (_IOC_TYPE(req) == 'F') {
        unsigned nr = _IOC_NR(req);
        if (nr == 200 + 11) { struct xfer *x = arg; decode(fd, x->cmd, (void *)(uintptr_t)x->ptr, ret, us); }
        else decode(fd, nr, arg, ret, us);
    }
    return ret;
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
    void *r = real_mmap(addr, len, prot, flags, fd, off);
    if (fd >= 0) { char p[64], l[256] = ""; snprintf(p, sizeof p, "/proc/self/fd/%d", fd); ssize_t n = readlink(p, l, sizeof l - 1); if (n > 0) l[n] = 0;
        if (strncmp(l, "/dev/nvidia", 11) == 0) out("IO mmap %s len %zu off 0x%lx\n", l, len, (long)off); }
    return r;
}
