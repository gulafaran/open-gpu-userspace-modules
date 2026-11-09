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
 *   limits follow clock offsets), ~3 ms each.
 */

#include <stdlib.h>
#include <string.h>

#include "nvml-priv.h"

#include "nvstatus.h"

#define NV2080_CTRL_CMD_CLK_DOMAINS_GET_INFO    0x20809019U
#define NV2080_CTRL_CMD_PERF_PSTATES_GET_INFO   0x2080a0a7U
#define NV2080_CTRL_CMD_PERF_PSTATES_GET_STATUS 0x2080a0a8U
#define NV2080_CTRL_CMD_CLK_DOMAIN_FREQS_ENUM   0x2080901aU
#define NV2080_CTRL_CMD_CLK_NAFLL_DEVICES_GET_INFO 0x208090b0U

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
#define PSTATES_STATUS_CLK_MAX  0x0c
#define PSTATES_MAX             ((PSTATES_INFO_SIZE - PSTATES_INFO_FIRST) / PSTATES_INFO_STRIDE)

#define CLK_FREQS_SIZE          2052
#define CLK_FREQS_COUNT         2
#define CLK_FREQS_FIRST         4
#define CLK_FREQS_MAX           ((CLK_FREQS_SIZE - CLK_FREQS_FIRST) / 4)

#define NAFLL_INFO_SIZE         1056
#define NAFLL_INFO_MIN_TARGET   8

/* NV2080_CTRL_CLK_DOMAIN_* */
#define CLK_DOMAIN_GPCCLK       0x00000001U
#define CLK_DOMAIN_MCLK         0x00000010U
#define CLK_DOMAIN_NVDCLK       0x00100000U

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
    unsigned best = 32;
    for (unsigned i = 0; i < PSTATES_MAX; i++) {
        if (!(dev->pstate_mask & (1U << i)))
            continue;
        size_t base = PSTATES_INFO_FIRST + i * PSTATES_INFO_STRIDE;
        NvU32 id = rd32(p, base + PSTATES_INFO_ID);
        if (id == 0)
            continue;
        unsigned n = (unsigned)__builtin_ctz(id);    /* BIT(n) = Pn */
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
