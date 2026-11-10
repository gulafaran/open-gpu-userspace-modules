/*
 * SPDX-License-Identifier: MIT
 *
 * Maximum clocks: the clock range of the fastest P-state. Supported clocks:
 * per P-state (fastest first) its memory clock and the graphics clock
 * frequency points within its graphics clock range, leaving out those the
 * clock generator (NAFLL) can't reliably target.
 *
 * Like the thermal controls, these GSP perf controls have no open
 * parameter definitions; the layouts were read off the closed library's
 * calls (620.70.06) and only the fields it relies on are used:
 *
 * CLK_DOMAINS_GET_INFO (12336 bytes): u32 domain mask at 4; per domain
 *   (0x180 bytes from 0x30) the NV2080_CTRL_CLK_DOMAIN_* bit at +4.
 *
 * PSTATES_GET_INFO (17676 bytes): u32 P-state mask at 4; per P-state
 *   (0x228 bytes from 0x0c) its P-state as a bit (BIT(n) = Pn) at +4 and,
 *   from +0x14, one 0x1c-byte entry per clock domain (clock domain order)
 *   with the domain's minimum at +4 and maximum at +0xc, in kHz.
 *
 * CLK_DOMAIN_FREQS_ENUM (2052 bytes): u8 clock domain index at 0 in; u16
 *   count at 2 and the domain's frequency points (MHz, ascending) as u32
 *   from 4 out.
 *
 * CLK_NAFLL_DEVICES_GET_INFO (1056 bytes): u16 at 8 is the lowest
 *   frequency (MHz) every NAFLL can target; 0 if there's no limit.
 *
 * PSTATES_GET_STATUS (32908 bytes, P-state mask at 4 in): per P-state
 *   (0x404 bytes from 0x0c) one 0x20-byte entry per clock domain from +8,
 *   in clock domain order, holding the domain's limits in kHz; the
 *   maximum is at +0xc. The closed library reads it on every call (the
 *   limits follow clock offsets), ~3 ms each. The minimum is at +4.
 *
 * CLK_CLIENT_CLK_DOMAINS_GET_INFO (908 bytes): the clock domains whose
 *   frequency clients may offset (overclocking). u32 domain mask at 4; per
 *   domain (0x1c bytes from 0x0c) the NV2080_CTRL_CLK_DOMAIN_* bit at +4
 *   and the offset range in kHz, maximum at +0x10 and minimum at +0x14.
 *   NVML reports the memory range doubled (transfer rate, like its
 *   memTransferRate).
 *
 * CLK_CLIENT_CLK_DOMAINS_GET_CONTROL (1544 bytes, domain mask at 4 in): per
 *   domain (0x30 bytes from 8, same indexes) the offset in effect, kHz, at
 *   +8. Also doubled for memory.
 *
 * PERF_GET_PERFMON_SAMPLE (0x97, 1168 bytes): u32 entry count at 0; per
 *   entry (0x10 bytes from 4) a clock domain bit in and at +4 that
 *   domain's utilization in percent out.
 *
 * PERF_GET_ADAPTIVE_CLOCKING_INFO (8 bytes): status at 4.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvml-priv.h"

#include "nvstatus.h"

#define NV2080_CTRL_CMD_CLK_DOMAINS_GET_INFO    0x20809019U
#define NV2080_CTRL_CMD_PERF_PSTATES_GET_INFO   0x2080a0a7U
#define NV2080_CTRL_CMD_PERF_PSTATES_GET_STATUS 0x2080a0a8U
#define NV2080_CTRL_CMD_CLK_DOMAIN_FREQS_ENUM   0x2080901aU
#define NV2080_CTRL_CMD_CLK_NAFLL_DEVICES_GET_INFO 0x208090b0U
#define NV2080_CTRL_CMD_CLK_CLIENT_CLK_DOMAINS_GET_INFO 0x20809029U
#define NV2080_CTRL_CMD_CLK_CLIENT_CLK_DOMAINS_GET_CONTROL 0x20809030U
#define NV2080_CTRL_CMD_PERF_GET_PERFMON_SAMPLE 0x2080a097U
#define NV2080_CTRL_CMD_PERF_GET_ADAPTIVE_CLOCKING_INFO 0x2080a054U

#define CLK_DOMAINS_INFO_SIZE   12336
#define CLK_DOMAINS_MASK        4
#define CLK_DOMAINS_FIRST       0x30
#define CLK_DOMAINS_STRIDE      0x180
#define CLK_DOMAIN_API          4
#define CLK_DOMAINS_MAX         ((CLK_DOMAINS_INFO_SIZE - CLK_DOMAINS_FIRST) / CLK_DOMAINS_STRIDE)

#define PSTATES_INFO_SIZE       17676
#define PSTATES_STATUS_SIZE     32908
#define PSTATES_MASK            4
#define PSTATES_INFO_FIRST      0x0c
#define PSTATES_INFO_STRIDE     0x228
#define PSTATES_INFO_ID         4
#define PSTATES_INFO_CLKS       0x14
#define PSTATES_INFO_CLK_SIZE   0x1c
#define PSTATES_INFO_CLK_MIN    0x04
#define PSTATES_INFO_CLK_MAX    0x0c
#define PSTATES_STATUS_FIRST    0x0c
#define PSTATES_STATUS_STRIDE   0x404
#define PSTATES_STATUS_CLKS     8
#define PSTATES_STATUS_CLK_SIZE 0x20
#define PSTATES_STATUS_CLK_MIN  0x04
#define PSTATES_STATUS_CLK_MAX  0x0c
#define PSTATES_MAX             ((PSTATES_INFO_SIZE - PSTATES_INFO_FIRST) / PSTATES_INFO_STRIDE)

#define CLK_FREQS_SIZE          2052
#define CLK_FREQS_COUNT         2
#define CLK_FREQS_FIRST         4
#define CLK_FREQS_MAX           ((CLK_FREQS_SIZE - CLK_FREQS_FIRST) / 4)

#define NAFLL_INFO_SIZE         1056
#define NAFLL_INFO_MIN_TARGET   8

#define CLIENT_CLK_INFO_SIZE    908
#define CLIENT_CLK_MASK         4
#define CLIENT_CLK_FIRST        0x0c
#define CLIENT_CLK_STRIDE       0x1c
#define CLIENT_CLK_DOMAIN       0x04
#define CLIENT_CLK_OFFSET_MAX   0x10
#define CLIENT_CLK_OFFSET_MIN   0x14
#define CLIENT_CLK_MAX          ((CLIENT_CLK_INFO_SIZE - CLIENT_CLK_FIRST) / CLIENT_CLK_STRIDE)

#define CLIENT_CLK_CONTROL_SIZE 1544
#define CLIENT_CLK_CONTROL_FIRST 8
#define CLIENT_CLK_CONTROL_STRIDE 0x30
#define CLIENT_CLK_CONTROL_OFFSET 8

#define PERFMON_SAMPLE_SIZE     1168
#define PERFMON_SAMPLE_FIRST    4
#define PERFMON_SAMPLE_STRIDE   0x10
#define PERFMON_SAMPLE_UTIL     4

/* NV2080_CTRL_CLK_DOMAIN_* */
#define CLK_DOMAIN_GPCCLK       0x00000001U
#define CLK_DOMAIN_MCLK         0x00000010U
#define CLK_DOMAIN_HOSTCLK      0x00000020U
#define CLK_DOMAIN_NVDCLK       0x00100000U

/* dev->clk_offset[] */
#define OFFSET_GPC              0
#define OFFSET_MEM              1

#define DEVICE(handle, dev)                                   \
    struct nvmlDevice_st *dev;                                \
    do {                                                      \
        nvmlReturn_t r_ = nvml_device((handle), &dev);        \
        if (r_ != NVML_SUCCESS)                               \
            return r_;                                        \
    } while (0)

static NvU32 rd32(const NvU8 *p, size_t off)
{
    NvU32 v;
    memcpy(&v, p + off, sizeof(v));
    return v;
}

static NvU32 lookup_pstates(struct nvmlDevice_st *dev)
{
    static const NvU32 api[NVML_CLOCK_COUNT] = {
        [NVML_CLOCK_GRAPHICS] = CLK_DOMAIN_GPCCLK,
        [NVML_CLOCK_SM] = CLK_DOMAIN_GPCCLK,      /* one clock since Volta */
        [NVML_CLOCK_MEM] = CLK_DOMAIN_MCLK,
        [NVML_CLOCK_VIDEO] = CLK_DOMAIN_NVDCLK,
    };
    NvU8 *p = calloc(1, PSTATES_INFO_SIZE > CLK_DOMAINS_INFO_SIZE ?
                        PSTATES_INFO_SIZE : CLK_DOMAINS_INFO_SIZE);
    if (p == NULL)
        return NV_ERR_NO_MEMORY;

    NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_CLK_DOMAINS_GET_INFO, p, CLK_DOMAINS_INFO_SIZE);
    if (status != NV_OK)
        goto out;
    NvU32 mask = rd32(p, CLK_DOMAINS_MASK);
    for (unsigned t = 0; t < NVML_CLOCK_COUNT; t++) {
        dev->clk_index[t] = -1;
        for (unsigned i = 0; i < CLK_DOMAINS_MAX; i++)
            if ((mask & (1U << i)) &&
                rd32(p, CLK_DOMAINS_FIRST + i * CLK_DOMAINS_STRIDE + CLK_DOMAIN_API) == api[t]) {
                dev->clk_index[t] = (int)i;
                break;
            }
    }

    memset(p, 0, PSTATES_INFO_SIZE);
    status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_PERF_PSTATES_GET_INFO, p, PSTATES_INFO_SIZE);
    if (status != NV_OK)
        goto out;
    dev->pstate_mask = rd32(p, PSTATES_MASK);
    dev->top_pstate = -1;
    dev->num_pclks = 0;
    memset(dev->pstate_obj, -1, sizeof(dev->pstate_obj));
    unsigned best = 32;
    for (unsigned i = 0; i < PSTATES_MAX; i++) {
        if (!(dev->pstate_mask & (1U << i)))
            continue;
        size_t base = PSTATES_INFO_FIRST + i * PSTATES_INFO_STRIDE;
        NvU32 id = rd32(p, base + PSTATES_INFO_ID);
        if (id == 0)
            continue;
        unsigned n = (unsigned)__builtin_ctz(id);    /* BIT(n) = Pn */
        if (n < 16)
            dev->pstate_obj[n] = (signed char)i;
        int gpc = dev->clk_index[NVML_CLOCK_GRAPHICS], mem = dev->clk_index[NVML_CLOCK_MEM];
        if (gpc >= 0 && mem >= 0 &&
            PSTATES_INFO_CLKS + (size_t)((gpc > mem ? gpc : mem) + 1) * PSTATES_INFO_CLK_SIZE <= PSTATES_INFO_STRIDE &&
            dev->num_pclks < 32) {
            size_t g = base + PSTATES_INFO_CLKS + (size_t)gpc * PSTATES_INFO_CLK_SIZE;
            size_t m = base + PSTATES_INFO_CLKS + (size_t)mem * PSTATES_INFO_CLK_SIZE;
            /* one memory clock per P-state: its maximum */
            unsigned k = dev->num_pclks++;
            while (k > 0 && dev->pclk[k - 1].pstate > n) {   /* keep Pn order */
                dev->pclk[k] = dev->pclk[k - 1];
                k--;
            }
            dev->pclk[k].pstate = (NvU8)n;
            dev->pclk[k].mem = rd32(p, m + PSTATES_INFO_CLK_MAX);
            dev->pclk[k].gpc_min = rd32(p, g + PSTATES_INFO_CLK_MIN);
            dev->pclk[k].gpc_max = rd32(p, g + PSTATES_INFO_CLK_MAX);
        }
        if (n < best) {
            best = n;
            dev->top_pstate = (int)i;
        }
    }
out:
    free(p);
    return status;
}

/* Looks the P-state and clock domain information up once (dev->lock held). */
static NvU32 ensure_pstates(struct nvmlDevice_st *dev)
{
    if (dev->pstates_known)
        return NV_OK;
    NvU32 status = lookup_pstates(dev);
    if (status != NV_OK && status != NV_ERR_NOT_SUPPORTED)
        return status;
    if (status != NV_OK) {
        dev->top_pstate = -1;
        dev->num_pclks = 0;
        memset(dev->pstate_obj, -1, sizeof(dev->pstate_obj));
        for (unsigned t = 0; t < NVML_CLOCK_COUNT; t++)
            dev->clk_index[t] = -1;
    }
    dev->pstates_known = true;
    return NV_OK;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMaxClockInfo(nvmlDevice_t device, nvmlClockType_t type,
                                                   unsigned int *clock)
{
    DEVICE(device, dev);
    if (clock == NULL || (unsigned)type >= NVML_CLOCK_COUNT)
        return NVML_ERROR_INVALID_ARGUMENT;

    pthread_mutex_lock(&dev->lock);
    NvU32 status = ensure_pstates(dev);
    int pstate = dev->top_pstate, clk = dev->clk_index[type];
    NvU32 mask = dev->pstate_mask;
    pthread_mutex_unlock(&dev->lock);
    if (status != NV_OK)
        return nvml_from_rm(status);
    if (pstate < 0 || clk < 0)
        return NVML_ERROR_NOT_SUPPORTED;

    size_t off = PSTATES_STATUS_FIRST + (size_t)pstate * PSTATES_STATUS_STRIDE +
                 PSTATES_STATUS_CLKS + (size_t)clk * PSTATES_STATUS_CLK_SIZE + PSTATES_STATUS_CLK_MAX;
    if (off + sizeof(NvU32) > PSTATES_STATUS_SIZE ||
        PSTATES_STATUS_CLKS + (size_t)(clk + 1) * PSTATES_STATUS_CLK_SIZE > PSTATES_STATUS_STRIDE)
        return NVML_ERROR_NOT_SUPPORTED;

    NvU8 *p = calloc(1, PSTATES_STATUS_SIZE);
    if (p == NULL)
        return NVML_ERROR_MEMORY;
    memcpy(p + PSTATES_MASK, &mask, sizeof(mask));
    status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_PERF_PSTATES_GET_STATUS, p, PSTATES_STATUS_SIZE);
    NvU32 khz = rd32(p, off);
    free(p);
    if (status != NV_OK)
        return nvml_from_rm(status);
    if (khz == 0)
        return NVML_ERROR_NOT_SUPPORTED;
    *clock = khz / 1000;
    return NVML_SUCCESS;
}

/* ---- supported clocks -------------------------------------------------------- */

/* The graphics clock frequency points and the NAFLL's lowest targetable
 * frequency, looked up once (dev->lock held). Without discrete frequency
 * points (NOT_SUPPORTED) a P-state offers its graphics range's ends. */
static NvU32 ensure_clock_table(struct nvmlDevice_st *dev)
{
    NvU32 status = ensure_pstates(dev);
    if (status != NV_OK || dev->clocks_known)
        return status;
    NvU8 *p = calloc(1, CLK_FREQS_SIZE > NAFLL_INFO_SIZE ? CLK_FREQS_SIZE : NAFLL_INFO_SIZE);
    if (p == NULL)
        return NV_ERR_NO_MEMORY;

    dev->num_gpc_freqs = 0;
    if (dev->clk_index[NVML_CLOCK_GRAPHICS] >= 0) {
        p[0] = (NvU8)dev->clk_index[NVML_CLOCK_GRAPHICS];
        status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_CLK_DOMAIN_FREQS_ENUM, p, CLK_FREQS_SIZE);
        if (status == NV_OK) {
            NvU16 c;
            memcpy(&c, p + CLK_FREQS_COUNT, sizeof(c));
            dev->num_gpc_freqs = c < CLK_FREQS_MAX ? c : CLK_FREQS_MAX;
            for (unsigned i = 0; i < dev->num_gpc_freqs; i++)
                dev->gpc_freqs[i] = (NvU16)rd32(p, CLK_FREQS_FIRST + 4 * i);
        } else if (status != NV_ERR_NOT_SUPPORTED) {
            goto out;
        }
    }

    memset(p, 0, NAFLL_INFO_SIZE);
    status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_CLK_NAFLL_DEVICES_GET_INFO, p, NAFLL_INFO_SIZE);
    if (status != NV_OK && status != NV_ERR_NOT_SUPPORTED)
        goto out;
    NvU16 min = 0;
    if (status == NV_OK)
        memcpy(&min, p + NAFLL_INFO_MIN_TARGET, sizeof(min));
    dev->min_target_mhz = min;

    /* Drop the frequency points below it (they're ascending). */
    unsigned first = 0;
    while (first < dev->num_gpc_freqs && dev->gpc_freqs[first] < dev->min_target_mhz)
        first++;
    memmove(dev->gpc_freqs, dev->gpc_freqs + first, (dev->num_gpc_freqs - first) * sizeof(dev->gpc_freqs[0]));
    dev->num_gpc_freqs -= first;
    dev->clocks_known = true;
    status = NV_OK;
out:
    free(p);
    return status;
}

/* P-states whose graphics range reaches a targetable clock count. */
static bool pclk_usable(const struct nvmlDevice_st *dev, unsigned i)
{
    return dev->pclk[i].gpc_max / 1000 >= dev->min_target_mhz;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetSupportedMemoryClocks(nvmlDevice_t device, unsigned int *count,
                                                            unsigned int *clocksMHz)
{
    unsigned mem[32], n = 0;
    DEVICE(device, dev);
    if (count == NULL || (*count != 0 && clocksMHz == NULL))
        return NVML_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&dev->lock);
    NvU32 status = ensure_clock_table(dev);
    for (unsigned i = 0; status == NV_OK && i < dev->num_pclks; i++)
        if (pclk_usable(dev, i))
            mem[n++] = dev->pclk[i].mem / 1000;
    pthread_mutex_unlock(&dev->lock);
    if (status != NV_OK)
        return nvml_from_rm(status);
    if (n == 0)
        return NVML_ERROR_NOT_SUPPORTED;
    if (*count < n) {
        *count = n;
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    memcpy(clocksMHz, mem, n * sizeof(*mem));
    *count = n;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetSupportedGraphicsClocks(nvmlDevice_t device, unsigned int memoryClockMHz,
                                                              unsigned int *count, unsigned int *clocksMHz)
{
    unsigned out[CLK_FREQS_MAX], n = 0;
    bool found = false, any = false;
    DEVICE(device, dev);
    if (count == NULL || memoryClockMHz == 0 || (*count != 0 && clocksMHz == NULL))
        return NVML_ERROR_INVALID_ARGUMENT;

    pthread_mutex_lock(&dev->lock);
    NvU32 status = ensure_clock_table(dev);
    for (unsigned i = 0; status == NV_OK && i < dev->num_pclks; i++) {
        if (!pclk_usable(dev, i))
            continue;
        any = true;
        if (dev->pclk[i].mem / 1000 != memoryClockMHz)
            continue;
        /* The first P-state with this memory clock, fastest graphics clock first. */
        NvU32 lo = dev->pclk[i].gpc_min, hi = dev->pclk[i].gpc_max;
        if (dev->num_gpc_freqs == 0) {
            out[n++] = hi / 1000;
            if (lo / 1000 != hi / 1000)
                out[n++] = lo / 1000;
        }
        for (unsigned k = dev->num_gpc_freqs; k-- > 0;) {
            NvU32 khz = dev->gpc_freqs[k] * 1000u;
            if (khz >= lo && khz <= hi)
                out[n++] = dev->gpc_freqs[k];
        }
        found = true;
        break;
    }
    pthread_mutex_unlock(&dev->lock);
    if (status != NV_OK)
        return nvml_from_rm(status);
    if (!any)
        return NVML_ERROR_NOT_SUPPORTED;
    if (!found)
        return NVML_ERROR_NOT_FOUND;
    if (*count < n) {
        *count = n;
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    memcpy(clocksMHz, out, n * sizeof(*out));
    *count = n;
    return NVML_SUCCESS;
}

/* ---- per-P-state clocks ------------------------------------------------------ */

/* Every P-state's current clock limits (PSTATES_GET_STATUS); the caller
 * frees *out. */
static NvU32 read_pstates_status(struct nvmlDevice_st *dev, NvU32 mask, NvU8 **out)
{
    NvU8 *p = calloc(1, PSTATES_STATUS_SIZE);
    if (p == NULL)
        return NV_ERR_NO_MEMORY;
    memcpy(p + PSTATES_MASK, &mask, sizeof(mask));
    NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_PERF_PSTATES_GET_STATUS, p, PSTATES_STATUS_SIZE);
    if (status != NV_OK) {
        free(p);
        return status;
    }
    *out = p;
    return NV_OK;
}

/* A clock domain's range in a P-state object, MHz; false if out of bounds. */
static bool status_range(const NvU8 *p, int obj, int clk, unsigned *min, unsigned *max)
{
    if (obj < 0 || clk < 0 ||
        PSTATES_STATUS_CLKS + (size_t)(clk + 1) * PSTATES_STATUS_CLK_SIZE > PSTATES_STATUS_STRIDE ||
        PSTATES_STATUS_FIRST + (size_t)(obj + 1) * PSTATES_STATUS_STRIDE > PSTATES_STATUS_SIZE)
        return false;
    size_t e = PSTATES_STATUS_FIRST + (size_t)obj * PSTATES_STATUS_STRIDE +
               PSTATES_STATUS_CLKS + (size_t)clk * PSTATES_STATUS_CLK_SIZE;
    *min = rd32(p, e + PSTATES_STATUS_CLK_MIN) / 1000;
    *max = rd32(p, e + PSTATES_STATUS_CLK_MAX) / 1000;
    return true;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMinMaxClockOfPState(nvmlDevice_t device, nvmlClockType_t type,
                                                          nvmlPstates_t pstate, unsigned int *minClockMHz,
                                                          unsigned int *maxClockMHz)
{
    unsigned min, max;
    NvU8 *p;
    DEVICE(device, dev);
    if ((minClockMHz == NULL && maxClockMHz == NULL) ||
        (unsigned)type >= NVML_CLOCK_COUNT || (unsigned)pstate >= 16)
        return NVML_ERROR_INVALID_ARGUMENT;

    pthread_mutex_lock(&dev->lock);
    NvU32 status = ensure_pstates(dev);
    int obj = dev->pstate_obj[pstate], clk = dev->clk_index[type], top = dev->top_pstate;
    NvU32 mask = dev->pstate_mask;
    pthread_mutex_unlock(&dev->lock);
    if (status != NV_OK)
        return nvml_from_rm(status);
    if (top < 0 || clk < 0)
        return NVML_ERROR_NOT_SUPPORTED;
    if (obj < 0)
        return NVML_ERROR_UNKNOWN;      /* the board has no such P-state */

    if ((status = read_pstates_status(dev, mask, &p)) != NV_OK)
        return nvml_from_rm(status);
    bool ok = status_range(p, obj, clk, &min, &max);
    free(p);
    if (!ok)
        return NVML_ERROR_NOT_SUPPORTED;
    /* The closed library leaves both untouched unless both are asked for. */
    if (minClockMHz != NULL)
        *minClockMHz = min;
    if (maxClockMHz != NULL)
        *maxClockMHz = max;
    return NVML_SUCCESS;
}

/* The board's P-states, slowest first; returns their count (dev->lock held). */
static unsigned pstate_list(const struct nvmlDevice_st *dev, unsigned *list)
{
    unsigned n = 0;
    for (int ps = 15; ps >= 0; ps--)
        if (dev->pstate_obj[ps] >= 0)
            list[n++] = (unsigned)ps;
    return n;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetSupportedPerformanceStates(nvmlDevice_t device, nvmlPstates_t *pstates,
                                                                 unsigned int size)
{
    unsigned list[16];
    DEVICE(device, dev);
    if (pstates == NULL || size == 0)
        return NVML_ERROR_INVALID_ARGUMENT;

    pthread_mutex_lock(&dev->lock);
    NvU32 status = ensure_pstates(dev);
    unsigned n = status == NV_OK ? pstate_list(dev, list) : 0;
    pthread_mutex_unlock(&dev->lock);
    if (status != NV_OK)
        return nvml_from_rm(status);
    if (n == 0)
        return NVML_ERROR_NOT_SUPPORTED;

    /* size is in bytes. (The closed library writes one entry more than
     * fits, and so accepts a buffer one entry short.) */
    unsigned cap = size / (unsigned)sizeof(*pstates);
    for (unsigned i = 0; i < cap; i++)
        pstates[i] = i < n ? (nvmlPstates_t)list[i] : NVML_PSTATE_UNKNOWN;
    return cap < n ? NVML_ERROR_INSUFFICIENT_SIZE : NVML_SUCCESS;
}

/* ---- clock offsets ----------------------------------------------------------- */

/* The offset ranges of the graphics and memory clock, static, looked up
 * once (dev->lock held). */
static NvU32 ensure_offsets(struct nvmlDevice_st *dev)
{
    static const NvU32 domain[2] = { [OFFSET_GPC] = CLK_DOMAIN_GPCCLK, [OFFSET_MEM] = CLK_DOMAIN_MCLK };
    if (dev->offsets_known)
        return NV_OK;
    NvU8 *p = calloc(1, CLIENT_CLK_INFO_SIZE);
    if (p == NULL)
        return NV_ERR_NO_MEMORY;
    NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_CLK_CLIENT_CLK_DOMAINS_GET_INFO, p, CLIENT_CLK_INFO_SIZE);
    if (status != NV_OK && status != NV_ERR_NOT_SUPPORTED) {
        free(p);
        return status;
    }
    NvU32 mask = status == NV_OK ? rd32(p, CLIENT_CLK_MASK) : 0;
    for (unsigned k = 0; k < 2; k++) {
        dev->clk_offset[k].index = -1;
        for (unsigned i = 0; i < CLIENT_CLK_MAX; i++) {
            size_t e = CLIENT_CLK_FIRST + i * CLIENT_CLK_STRIDE;
            if (!(mask & (1U << i)) || rd32(p, e + CLIENT_CLK_DOMAIN) != domain[k])
                continue;
            dev->clk_offset[k].index = (int)i;
            dev->clk_offset[k].min_khz = (int)rd32(p, e + CLIENT_CLK_OFFSET_MIN);
            dev->clk_offset[k].max_khz = (int)rd32(p, e + CLIENT_CLK_OFFSET_MAX);
            break;
        }
    }
    free(p);
    dev->offsets_known = true;
    return NV_OK;
}

/* Whether clients can offset the clock: it has a non-empty offset range. */
static bool offset_editable(const struct nvmlDevice_st *dev, unsigned k)
{
    return dev->clk_offset[k].index >= 0 &&
           (dev->clk_offset[k].min_khz != 0 || dev->clk_offset[k].max_khz != 0);
}

static nvmlReturn_t clock_offset(struct nvmlDevice_st *dev, unsigned k, int *cur, int *min, int *max);

static nvmlReturn_t offset_range(nvmlDevice_t device, unsigned k, int *min, int *max)
{
    DEVICE(device, dev);
    if (min == NULL || max == NULL)     /* the closed library crashes */
        return NVML_ERROR_INVALID_ARGUMENT;
    return clock_offset(dev, k, NULL, min, max);
}

/* The range and, if cur isn't NULL, the offset in effect; MHz as NVML
 * reports them. */
static nvmlReturn_t clock_offset(struct nvmlDevice_st *dev, unsigned k, int *cur, int *min, int *max)
{
    pthread_mutex_lock(&dev->lock);
    NvU32 status = ensure_offsets(dev);
    int index = dev->clk_offset[k].index, lo = dev->clk_offset[k].min_khz, hi = dev->clk_offset[k].max_khz;
    NvU32 mask = 0;
    for (unsigned i = 0; i < 2; i++)
        if (dev->clk_offset[i].index >= 0)
            mask |= 1U << dev->clk_offset[i].index;
    pthread_mutex_unlock(&dev->lock);
    if (status != NV_OK)
        return nvml_from_rm(status);
    if (index < 0)
        return NVML_ERROR_NOT_SUPPORTED;
    int scale = k == OFFSET_MEM ? 2 : 1;
    if (cur != NULL) {
        NvU8 p[CLIENT_CLK_CONTROL_SIZE] = { 0 };
        memcpy(p + CLIENT_CLK_MASK, &mask, sizeof(mask));
        status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_CLK_CLIENT_CLK_DOMAINS_GET_CONTROL, p, sizeof(p));
        if (status != NV_OK)
            return nvml_from_rm(status);
        *cur = (int)rd32(p, CLIENT_CLK_CONTROL_FIRST + (size_t)index * CLIENT_CLK_CONTROL_STRIDE +
                            CLIENT_CLK_CONTROL_OFFSET) / 1000 * scale;
    }
    if (min != NULL)
        *min = lo / 1000 * scale;
    if (max != NULL)
        *max = hi / 1000 * scale;
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetGpcClkVfOffset(nvmlDevice_t device, int *offset)
{
    DEVICE(device, dev);
    if (offset == NULL)                 /* the closed library crashes */
        return NVML_ERROR_INVALID_ARGUMENT;
    return clock_offset(dev, OFFSET_GPC, offset, NULL, NULL);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMemClkVfOffset(nvmlDevice_t device, int *offset)
{
    DEVICE(device, dev);
    if (offset == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    return clock_offset(dev, OFFSET_MEM, offset, NULL, NULL);
}

/* One offset per clock: the P-state asked for makes no difference. */
NVML_EXPORT nvmlReturn_t nvmlDeviceGetClockOffsets(nvmlDevice_t device, nvmlClockOffset_t *info)
{
    DEVICE(device, dev);
    if (info == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (info->version != nvmlClockOffset_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    if (info->type != NVML_CLOCK_GRAPHICS && info->type != NVML_CLOCK_MEM)
        return NVML_ERROR_INVALID_ARGUMENT;
    return clock_offset(dev, info->type == NVML_CLOCK_MEM ? OFFSET_MEM : OFFSET_GPC,
                        &info->clockOffsetMHz, &info->minClockOffsetMHz, &info->maxClockOffsetMHz);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetGpcClkMinMaxVfOffset(nvmlDevice_t device, int *minOffset, int *maxOffset)
{
    return offset_range(device, OFFSET_GPC, minOffset, maxOffset);
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetMemClkMinMaxVfOffset(nvmlDevice_t device, int *minOffset, int *maxOffset)
{
    return offset_range(device, OFFSET_MEM, minOffset, maxOffset);
}

/* ---- performance mode strings ------------------------------------------------ */

struct mode_clocks {
    unsigned nv, nv_min, nv_max, mem, mem_min, mem_max;
    bool nv_editable, mem_editable;
};

/* The "name=value" list nvidia-settings-style tools parse; memTransferRate
 * is twice the memory clock. Returns the length, clamped to the space. */
static size_t format_mode(char *buf, size_t space, const struct mode_clocks *c)
{
    int n = snprintf(buf, space,
                     "nvclock=%u, nvclockmin=%u, nvclockmax=%u, nvclockeditable=%d, "
                     "memclock=%u, memclockmin=%u, memclockmax=%u, memclockeditable=%d, "
                     "memTransferRate=%u, memTransferRatemin=%u, memTransferRatemax=%u, "
                     "memTransferRateeditable=%d",
                     c->nv, c->nv_min, c->nv_max, c->nv_editable,
                     c->mem, c->mem_min, c->mem_max, c->mem_editable,
                     c->mem * 2, c->mem_min * 2, c->mem_max * 2, c->mem_editable);
    if (n < 0 || space == 0)
        return 0;
    return (size_t)n < space ? (size_t)n : space - 1;
}

/* What both strings need: the P-state objects (slowest first), the clock
 * domains, whether the clocks take offsets, and the P-states' limits. */
struct mode_state {
    unsigned n, list[16];
    int obj[16], gpc, mem, top;
    bool nv_editable, mem_editable;
    NvU8 *status;               /* PSTATES_GET_STATUS; freed by the caller */
};

static nvmlReturn_t mode_state(struct nvmlDevice_st *dev, struct mode_state *m)
{
    pthread_mutex_lock(&dev->lock);
    NvU32 status = ensure_pstates(dev);
    if (status == NV_OK)
        status = ensure_offsets(dev);
    m->n = status == NV_OK ? pstate_list(dev, m->list) : 0;
    for (unsigned i = 0; i < m->n; i++)
        m->obj[i] = dev->pstate_obj[m->list[i]];
    m->gpc = dev->clk_index[NVML_CLOCK_GRAPHICS];
    m->mem = dev->clk_index[NVML_CLOCK_MEM];
    m->top = dev->top_pstate;
    m->nv_editable = status == NV_OK && offset_editable(dev, OFFSET_GPC);
    m->mem_editable = status == NV_OK && offset_editable(dev, OFFSET_MEM);
    NvU32 mask = dev->pstate_mask;
    pthread_mutex_unlock(&dev->lock);
    if (status != NV_OK)
        return nvml_from_rm(status);
    if (m->n == 0 || m->gpc < 0 || m->mem < 0)
        return NVML_ERROR_NOT_SUPPORTED;
    return nvml_from_rm(read_pstates_status(dev, mask, &m->status));
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetPerformanceModes(nvmlDevice_t device, nvmlDevicePerfModes_t *perfModes)
{
    struct mode_state m;
    DEVICE(device, dev);
    if (perfModes == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (perfModes->version != nvmlDevicePerfModes_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    nvmlReturn_t ret = mode_state(dev, &m);
    if (ret != NVML_SUCCESS)
        return ret;

    /* One entry per P-state, slowest first, each at its lowest clocks. */
    char *s = perfModes->str;
    size_t space = sizeof(perfModes->str), len = 0;
    s[0] = '\0';
    for (unsigned i = 0; i < m.n; i++) {
        struct mode_clocks c = { .nv_editable = m.nv_editable, .mem_editable = m.mem_editable };
        if (!status_range(m.status, m.obj[i], m.gpc, &c.nv_min, &c.nv_max) ||
            !status_range(m.status, m.obj[i], m.mem, &c.mem_min, &c.mem_max))
            continue;
        c.nv = c.nv_min;
        c.mem = c.mem_min;
        int n = snprintf(s + len, space - len, "%sperf=%u, ", len ? " ; " : "", i);
        if (n < 0 || (size_t)n >= space - len)
            break;
        len += (size_t)n;
        len += format_mode(s + len, space - len, &c);
    }
    free(m.status);
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetCurrentClockFreqs(nvmlDevice_t device,
                                                        nvmlDeviceCurrentClockFreqs_t *currentClockFreqs)
{
    struct mode_state m;
    struct mode_clocks c;
    unsigned pstate;
    DEVICE(device, dev);
    if (currentClockFreqs == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    if (currentClockFreqs->version != nvmlDeviceCurrentClockFreqs_v1)
        return NVML_ERROR_ARGUMENT_VERSION_MISMATCH;
    nvmlReturn_t ret = mode_state(dev, &m);
    if (ret != NVML_SUCCESS)
        return ret;

    /* The current clocks within the current P-state's limits. */
    int obj = m.top;
    if (nvml_current_pstate(dev, &pstate))
        for (unsigned i = 0; i < m.n; i++)
            if (m.list[i] == pstate)
                obj = m.obj[i];
    c.nv_editable = m.nv_editable;
    c.mem_editable = m.mem_editable;
    bool ok = status_range(m.status, obj, m.gpc, &c.nv_min, &c.nv_max) &&
              status_range(m.status, obj, m.mem, &c.mem_min, &c.mem_max);
    free(m.status);
    if (!ok)
        return NVML_ERROR_NOT_SUPPORTED;
    if ((ret = nvmlDeviceGetClockInfo(device, NVML_CLOCK_GRAPHICS, &c.nv)) != NVML_SUCCESS ||
        (ret = nvmlDeviceGetClockInfo(device, NVML_CLOCK_MEM, &c.mem)) != NVML_SUCCESS)
        return ret;
    format_mode(currentClockFreqs->str, sizeof(currentClockFreqs->str), &c);
    return NVML_SUCCESS;
}

/* ---- utilization by clock domain, adaptive clocking -------------------------- */

NVML_EXPORT nvmlReturn_t nvmlDeviceGetDynamicPstatesInfo(nvmlDevice_t device,
                                                         nvmlGpuDynamicPstatesInfo_t *pDynamicPstatesInfo)
{
    /* In nvmlGpuUtilizationDomainId_t order: GPU, FB, video engine, bus. */
    static const NvU32 domain[] = { CLK_DOMAIN_GPCCLK, CLK_DOMAIN_MCLK, CLK_DOMAIN_NVDCLK, CLK_DOMAIN_HOSTCLK };
    enum { N = sizeof(domain) / sizeof(domain[0]) };
    NvU8 p[PERFMON_SAMPLE_SIZE] = { 0 };
    DEVICE(device, dev);
    if (pDynamicPstatesInfo == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;

    NvU32 count = N;
    memcpy(p, &count, sizeof(count));
    for (unsigned i = 0; i < N; i++)
        memcpy(p + PERFMON_SAMPLE_FIRST + i * PERFMON_SAMPLE_STRIDE, &domain[i], sizeof(domain[i]));
    NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_PERF_GET_PERFMON_SAMPLE, p, sizeof(p));
    if (status != NV_OK)
        return nvml_from_rm(status);

    memset(pDynamicPstatesInfo, 0, sizeof(*pDynamicPstatesInfo));
    for (unsigned i = 0; i < N; i++) {
        pDynamicPstatesInfo->utilization[i].bIsPresent = 1;
        pDynamicPstatesInfo->utilization[i].percentage =
            rd32(p, PERFMON_SAMPLE_FIRST + i * PERFMON_SAMPLE_STRIDE + PERFMON_SAMPLE_UTIL);
    }
    return NVML_SUCCESS;
}

NVML_EXPORT nvmlReturn_t nvmlDeviceGetAdaptiveClockInfoStatus(nvmlDevice_t device, unsigned int *adaptiveClockStatus)
{
    NvU32 p[2] = { 0 };
    DEVICE(device, dev);
    if (adaptiveClockStatus == NULL)
        return NVML_ERROR_INVALID_ARGUMENT;
    NvU32 status = nvml_ctrl_sub(dev, NV2080_CTRL_CMD_PERF_GET_ADAPTIVE_CLOCKING_INFO, p, sizeof(p));
    if (status != NV_OK)
        return nvml_from_rm(status);
    *adaptiveClockStatus = p[1];
    return NVML_SUCCESS;
}
