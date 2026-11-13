/* The CPU at its stock maximum, for the game menu's CPU boost. Before the game starts,
 * tools/sd-session.cjs opens the console's clock service (clkrst) for the CPU, reads the CPU's rate
 * at that moment and checks that the stock range tops out at the maximum; it passes all three in
 * the launcher context. This sets the rate and puts the launch rate back, each time reading it
 * back. Nothing else changes: the GPU, memory and voltages stay with the system. */
#include "storage.h"
#include <errno.h>
#include <pthread.h>
#include <string.h>

enum { SFCI = 0x49434653, SFCO = 0x4f434653, SET_RATE = 7, GET_RATE = 8 };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int raised; /* this launch set the maximum and hasn't put the launch rate back */

static struct sd_context *clock_context(void) {
    struct sd_context *ctx = card();
    return ctx && ctx->cpu_magic == CPU_MAGIC && ctx->cpu && ctx->cpu_command && ctx->cpu_launch_hz &&
                   ctx->cpu_max_hz > ctx->cpu_launch_hz
               ? ctx
               : NULL;
}

/* One request on the CPU's clock session, in its own page (the card's requests use another, from
 * other threads). */
static long request(struct sd_context *ctx, unsigned command, const uint32_t *hz_in, uint32_t *hz_out) {
    uint32_t *p = ctx->cpu_command;
    memset(p, 0, 256);
    p[0] = 4;
    p[1] = 8 + (hz_in ? 1 : 0);
    p[4] = SFCI;
    p[6] = command;
    if (hz_in)
        p[8] = *hz_in;
    if (ctx->send(p, 4096, ctx->cpu))
        return -EIO;
    if (p[1] >> 31)
        return -EPROTO; /* these replies carry no handles */
    unsigned at = 2 + 2 * ((p[0] >> 16) & 15) + 3 * (((p[0] >> 20) & 15) + ((p[0] >> 24) & 15) + (p[0] >> 28));
    at = (at + 3) & ~3u;
    if (at + 6 > 1024 || p[at] != SFCO)
        return -EPROTO;
    if (p[at + 2] & 0x3fffff)
        return -EIO;
    if (hz_out)
        *hz_out = p[at + 4];
    return 0;
}

static long set_rate(struct sd_context *ctx, uint32_t hz) {
    uint32_t now = 0;
    long rc = request(ctx, SET_RATE, &hz, NULL);
    if (!rc)
        rc = request(ctx, GET_RATE, NULL, &now);
    return rc ? rc : now == hz ? 0 : -EAGAIN;
}

int hbxl_cpu_clock_rates(uint32_t *launch_hz, uint32_t *max_hz) {
    const struct sd_context *ctx = clock_context();
    if (!ctx)
        return 0;
    *launch_hz = ctx->cpu_launch_hz;
    *max_hz = ctx->cpu_max_hz;
    return 1;
}

/* The maximum (on) or the launch rate (off). 0 when the CPU reads back that rate. */
long hbxl_cpu_clock_boost(int on) {
    struct sd_context *ctx = clock_context();
    if (!ctx)
        return -ENODEV;
    pthread_mutex_lock(&lock);
    long rc = set_rate(ctx, on ? ctx->cpu_max_hz : ctx->cpu_launch_hz);
    if (on && !rc)
        raised = 1;
    else if (!on && !rc)
        raised = 0;
    pthread_mutex_unlock(&lock);
    return rc;
}

/* On the way out: if this launch raised the CPU and it is still at the maximum, back to the launch
 * rate. A rate the system chose since is left alone. Safe to call more than once. */
void hbxl_cpu_clock_restore(void) {
    struct sd_context *ctx = clock_context();
    if (!ctx)
        return;
    pthread_mutex_lock(&lock);
    if (raised) {
        uint32_t now = 0;
        long rc = request(ctx, GET_RATE, NULL, &now);
        if (!rc && now == ctx->cpu_max_hz) {
            rc = set_rate(ctx, ctx->cpu_launch_hz);
            hbxl_logf(rc ? "[cpu] couldn't put the CPU back to %d Hz (%d)" : "[cpu] CPU back to %d Hz",
                      (int64_t)ctx->cpu_launch_hz, (int64_t)rc);
        } else if (!rc)
            hbxl_logf("[cpu] the system moved the CPU to %d Hz; left as it is", (int64_t)now);
        raised = 0;
    }
    pthread_mutex_unlock(&lock);
}
