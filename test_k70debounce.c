/*
 * Test harness for k70debounce. No external dependencies.
 *
 * Covers: functional behaviour, rollover, regression for the prototype
 * bug, robustness, pass-through, idle latency.
 *
 * A SIGALRM watchdog tears everything down if anything stalls.
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BIN         "./k70debounce"
#define MAXEV       2048
#define WATCHDOG_S  45      /* global watchdog */

static int passed, failed;

/* ------------------------------------------------------------- helpers */

static void ok(const char *name, bool cond, const char *fmt, ...)
{
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond && fmt) {
        va_list ap;
        va_start(ap, fmt);
        printf("        ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
    }
    cond ? passed++ : failed++;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(double ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* write that does not silently ignore the return value */
static bool wr(int fd, const void *buf, size_t n)
{
    ssize_t r = write(fd, buf, n);
    return r == (ssize_t)n;
}

static struct input_event mk(uint16_t type, uint16_t code, int32_t val)
{
    struct input_event e;
    memset(&e, 0, sizeof(e));
    e.type = type; e.code = code; e.value = val;
    return e;
}

/* --------------------------------------------------------- child process */

struct child {
    pid_t pid;
    int   in;      /* write here */
    int   out;     /* read here  */
    int   err;
};

static bool spawn(struct child *c, char *const argv[])
{
    int pin[2], pout[2], perr[2];
    if (pipe(pin) || pipe(pout) || pipe(perr))
        return false;

    c->pid = fork();
    if (c->pid < 0)
        return false;

    if (c->pid == 0) {
        dup2(pin[0],  STDIN_FILENO);
        dup2(pout[1], STDOUT_FILENO);
        dup2(perr[1], STDERR_FILENO);
        close(pin[0]); close(pin[1]);
        close(pout[0]); close(pout[1]);
        close(perr[0]); close(perr[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pin[0]); close(pout[1]); close(perr[1]);
    c->in = pin[1]; c->out = pout[0]; c->err = perr[0];
    return true;
}

static int reap(struct child *c, char *errbuf, size_t errsz)
{
    if (errbuf && errsz) {
        ssize_t n = read(c->err, errbuf, errsz - 1);
        errbuf[n > 0 ? n : 0] = '\0';
    }
    close(c->err);
    int st = 0;
    waitpid(c->pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Read all output until EOF. */
static size_t drain(int fd, struct input_event *buf, size_t max)
{
    size_t n = 0;
    unsigned char raw[sizeof(struct input_event)];
    size_t have = 0;
    for (;;) {
        ssize_t r = read(fd, raw + have, sizeof(raw) - have);
        if (r <= 0)
            break;
        have += (size_t)r;
        if (have == sizeof(raw)) {
            if (n < max)
                memcpy(&buf[n], raw, sizeof(raw));
            n++;
            have = 0;
        }
    }
    return n;
}

/* Run a scenario: events with relative delays, return the sequence. */
struct step { double delay_ms; uint16_t type, code; int32_t val; };

static size_t run_scenario(const struct step *steps, size_t nsteps,
                           int hold, struct input_event *out, size_t maxout,
                           size_t chunk)
{
    char hs[16];
    snprintf(hs, sizeof(hs), "%d", hold);
    char *argv[] = { (char *)BIN, (char *)"--hold", hs,
                     (char *)"--rt", (char *)"0", NULL };
    struct child c;
    if (!spawn(&c, argv))
        return 0;

    for (size_t i = 0; i < nsteps; i++) {
        sleep_ms(steps[i].delay_ms);
        struct input_event e = mk(steps[i].type, steps[i].code, steps[i].val);
        const unsigned char *p = (const unsigned char *)&e;
        size_t left = sizeof(e);
        size_t step = chunk ? chunk : left;
        while (left) {
            size_t w = left < step ? left : step;
            if (write(c.in, p, w) != (ssize_t)w)
                break;
            p += w; left -= w;
            if (chunk) sleep_ms(1);
        }
    }
    sleep_ms((double)hold + 250.0);
    close(c.in);

    size_t n = drain(c.out, out, maxout);
    close(c.out);
    reap(&c, NULL, 0);
    return n;
}

/* Render the sequence in readable form: "AD BD AU BU" */
static char keyname(uint16_t code)
{
    switch (code) {
    case 30: return 'A';   /* KEY_A */
    case 48: return 'B';   /* KEY_B */
    case 46: return 'C';   /* KEY_C */
    case 32: return 'D';   /* KEY_D */
    case 18: return 'E';   /* KEY_E */
    default: return '?';
    }
}

static void render(const struct input_event *ev, size_t n, char *buf, size_t sz)
{
    buf[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (ev[i].type != EV_KEY || (ev[i].value != 0 && ev[i].value != 1))
            continue;
        int w = snprintf(buf + used, sz - used, "%s%c%c",
                         used ? " " : "", keyname(ev[i].code),
                         ev[i].value ? 'D' : 'U');
        if (w < 0 || (size_t)w >= sz - used) break;
        used += (size_t)w;
    }
}

static void on_alarm(int s)
{
    (void)s;
    fprintf(stderr, "\nWATCHDOG: timeout, tests aborted\n");
    _exit(2);
}

/* ------------------------------------------------------------- latency */

struct lat { double min, med, p95, p99, max; };

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static bool measure_latency(int rt, int hold, int samples, struct lat *l,
                            bool *rt_active)
{
    char hs[16], rs[16];
    snprintf(hs, sizeof(hs), "%d", hold);
    snprintf(rs, sizeof(rs), "%d", rt);
    char *argv[] = { (char *)BIN, (char *)"--hold", hs, (char *)"--rt", rs,
                     (char *)"--verbose", NULL };
    struct child c;
    if (!spawn(&c, argv))
        return false;

    double *v = malloc((size_t)samples * sizeof(double));
    if (!v) return false;
    int n = 0;

    for (int i = 0; i < samples; i++) {
        struct input_event p = mk(EV_KEY, 30, 1), r = mk(EV_KEY, 30, 0), got;
        uint64_t t0 = now_ns();
        if (write(c.in, &p, sizeof(p)) != (ssize_t)sizeof(p)) break;
        if (read(c.out, &got, sizeof(got)) != (ssize_t)sizeof(got)) break;
        double ms = (double)(now_ns() - t0) / 1e6;
        if (i >= 20) v[n++] = ms;        /* discard warm-up */
        if (write(c.in, &r, sizeof(r)) != (ssize_t)sizeof(r)) break;
        if (read(c.out, &got, sizeof(got)) != (ssize_t)sizeof(got)) break;
        sleep_ms(3);
    }
    close(c.in);
    char err[512];
    drain(c.out, NULL, 0);
    close(c.out);
    reap(&c, err, sizeof(err));
    if (rt_active)
        *rt_active = strstr(err, "SCHED_FIFO") != NULL;

    if (n < 10) { free(v); return false; }
    qsort(v, (size_t)n, sizeof(double), cmp_d);
    l->min = v[0];
    l->med = v[n / 2];
    l->p95 = v[(int)((double)n * 0.95)];
    l->p99 = v[(int)((double)n * 0.99)];
    l->max = v[n - 1];
    free(v);
    return true;
}

static void show_lat(const char *label, const struct lat *l, const char *note)
{
    printf("  %-22s min %6.2f  med %6.2f  p95 %6.2f  p99 %6.2f  max %6.2f  "
           "jitter %5.2f  %s\n",
           label, l->min, l->med, l->p95, l->p99, l->max, l->p99 - l->min,
           note ? note : "");
}

/* --------------------------------------------------------------- main */

int main(void)
{
    signal(SIGALRM, on_alarm);
    alarm(WATCHDOG_S);

    static struct input_event out[MAXEV];
    char got[256];
    size_t n;

    const int H = 26;

    printf("\n[1] Functional\n");
    {
        struct step s[] = {
            {   0, EV_KEY, 30, 1 }, { 120, EV_KEY, 30, 0 },   /* legitimate  */
            {  80, EV_KEY, 48, 1 }, {   7, EV_KEY, 48, 0 },   /* chatter     */
            {  60, EV_KEY, 46, 1 }, { 100, EV_KEY, 46, 0 },   /* legitimate  */
            {  50, EV_KEY, 30, 1 }, {  15, EV_KEY, 30, 0 },   /* grazing     */
            {  60, EV_KEY, 48, 1 }, { 200, EV_KEY, 48, 0 },   /* legitimate  */
        };
        n = run_scenario(s, 10, H, out, MAXEV, 0);
        render(out, n, got, sizeof(got));
        ok("chatter and grazing removed",
           !strcmp(got, "AD AU CD CU BD BU"), "got: %s", got);
    }

    printf("\n[2] Rollover\n");
    {
        struct step s[] = { {0,EV_KEY,30,1}, {40,EV_KEY,48,1},
                            {60,EV_KEY,30,0}, {60,EV_KEY,48,0} };
        n = run_scenario(s, 4, H, out, MAXEV, 0);
        render(out, n, got, sizeof(got));
        ok("legitimate rollover", !strcmp(got, "AD BD AU BU"), "got: %s", got);
    }
    {
        struct step s[] = { {0,EV_KEY,30,1}, {40,EV_KEY,48,1},
                            {5,EV_KEY,48,0}, {80,EV_KEY,30,0} };
        n = run_scenario(s, 4, H, out, MAXEV, 0);
        render(out, n, got, sizeof(got));
        ok("chatter inside rollover", !strcmp(got, "AD AU"), "got: %s", got);
    }
    {
        /* B pressed and released in 8 ms while A and C are already down */
        struct step s[] = { {0,EV_KEY,30,1}, {20,EV_KEY,48,1}, {4,EV_KEY,46,1},
                            {4,EV_KEY,48,0}, {80,EV_KEY,30,0}, {30,EV_KEY,46,0} };
        n = run_scenario(s, 6, H, out, MAXEV, 0);
        render(out, n, got, sizeof(got));
        ok("chatter between two held keys", !strcmp(got, "AD CD AU CU"),
           "got: %s", got);
    }
    {
        /* REGRESSION: Python prototype bug.
          * B hold = 20+8 = 28 ms > 26 -> it must SURVIVE. */
        struct step s[] = { {0,EV_KEY,30,1}, {20,EV_KEY,48,1}, {20,EV_KEY,46,1},
                            {8,EV_KEY,48,0}, {80,EV_KEY,30,0}, {30,EV_KEY,46,0} };
        n = run_scenario(s, 6, H, out, MAXEV, 0);
        render(out, n, got, sizeof(got));
        ok("28ms press above threshold NOT dropped",
           !strcmp(got, "AD BD CD BU AU CU"), "got: %s", got);
    }

    printf("\n[3] Volume wheel and media controls\n");
    {
        /* REGRESSION: the volume wheel is a rotary encoder, it emits
          * press+release pairs about 4 ms long. They must not be filtered. */
        char *argv[] = { (char *)BIN, (char *)"--hold", (char *)"26",
                         (char *)"--rt", (char *)"0", NULL };
        struct child c;
        spawn(&c, argv);
        for (int i = 0; i < 10; i++) {                 /* 10 volume detents  */
            struct input_event p = mk(EV_KEY, KEY_VOLUMEUP, 1);
            struct input_event r = mk(EV_KEY, KEY_VOLUMEUP, 0);
            (void)wr(c.in, &p, sizeof(p));
            sleep_ms(4);                                /* hold 4 ms */
            (void)wr(c.in, &r, sizeof(r));
            sleep_ms(20);
        }
        sleep_ms(300);
        close(c.in);
        n = drain(c.out, out, MAXEV);
        close(c.out);
        reap(&c, NULL, 0);
        ok("volume wheel: all 10 detents pass", n == 20,
           "expected 20 events, got %zu", n);
    }
    {
        /* with --no-default-ignore they must be filtered instead */
        char *argv[] = { (char *)BIN, (char *)"--hold", (char *)"26",
                         (char *)"--rt", (char *)"0",
                         (char *)"--no-default-ignore", NULL };
        struct child c;
        spawn(&c, argv);
        for (int i = 0; i < 10; i++) {
            struct input_event p = mk(EV_KEY, KEY_VOLUMEUP, 1);
            struct input_event r = mk(EV_KEY, KEY_VOLUMEUP, 0);
            (void)wr(c.in, &p, sizeof(p));
            sleep_ms(4);
            (void)wr(c.in, &r, sizeof(r));
            sleep_ms(20);
        }
        sleep_ms(300);
        close(c.in);
        n = drain(c.out, out, MAXEV);
        close(c.out);
        reap(&c, NULL, 0);
        ok("--no-default-ignore filters them", n == 0,
           "expected 0 events, got %zu", n);
    }
    {
        /* normal keys stay filtered even with the exemptions active */
        struct step s[] = { {0,EV_KEY,30,1}, {5,EV_KEY,30,0},
                            {60,EV_KEY,48,1}, {120,EV_KEY,48,0} };
        n = run_scenario(s, 4, H, out, MAXEV, 0);
        render(out, n, got, sizeof(got));
        ok("typing keys remain filtered", !strcmp(got, "BD BU"),
           "got: %s", got);
    }

    printf("\n[4] Clean exit: no stuck keys\n");
    {
        /* REGRESSION: stopping the pipeline with a key held meant the
          * RELEASE never reached the system -> endless autorepeat.
          * k70debounce must synthesise the releases before exiting. */
        char *argv[] = { (char *)BIN, (char *)"--hold", (char *)"26",
                         (char *)"--rt", (char *)"0", NULL };
        struct child c;
        spawn(&c, argv);
        struct input_event p = mk(EV_KEY, KEY_ENTER, 1);
        (void)wr(c.in, &p, sizeof(p));
        sleep_ms(150);                 /* key held down     */
        close(c.in);                   /* EOF: pipeline stops */
        n = drain(c.out, out, MAXEV);
        close(c.out);
        int rc = reap(&c, NULL, 0);

        bool press_ok = false, release_ok = false;
        for (size_t i = 0; i < n; i++) {
            if (out[i].type == EV_KEY && out[i].code == KEY_ENTER) {
                if (out[i].value == 1) press_ok = true;
                if (out[i].value == 0) release_ok = true;
            }
        }
        ok("ENTER held at exit gets released",
           rc == 0 && press_ok && release_ok,
           "rc=%d press=%d release=%d events=%zu", rc, press_ok, release_ok, n);
    }
    {
        /* several keys held together: all released */
        char *argv[] = { (char *)BIN, (char *)"--hold", (char *)"26",
                         (char *)"--rt", (char *)"0", NULL };
        struct child c;
        spawn(&c, argv);
        uint16_t held[] = { 30, 48, 46, KEY_LEFTSHIFT };
        for (size_t i = 0; i < 4; i++) {
            struct input_event p = mk(EV_KEY, held[i], 1);
            (void)wr(c.in, &p, sizeof(p));
            sleep_ms(40);
        }
        sleep_ms(100);
        close(c.in);
        n = drain(c.out, out, MAXEV);
        close(c.out);
        reap(&c, NULL, 0);

        int released = 0;
        for (size_t i = 0; i < n; i++)
            for (size_t k = 0; k < 4; k++)
                if (out[i].type == EV_KEY && out[i].code == held[k] &&
                    out[i].value == 0)
                    released++;
        ok("4 held keys: all released", released == 4,
           "released %d of 4", released);
    }
    {
        /* no key held: no spurious release */
        struct step s[] = { {0,EV_KEY,30,1}, {120,EV_KEY,30,0} };
        n = run_scenario(s, 2, H, out, MAXEV, 0);
        int rel = 0;
        for (size_t i = 0; i < n; i++)
            if (out[i].type == EV_KEY && out[i].value == 0)
                rel++;
        ok("no spurious release when everything is already up", rel == 1,
           "releases found: %d (expected 1)", rel);
    }

    printf("\n[5] Robustness\n");
    {
        struct step s[] = { {0,EV_KEY,30,1}, {120,EV_KEY,30,0},
                            {60,EV_KEY,48,1}, {6,EV_KEY,48,0},
                            {60,EV_KEY,46,1}, {100,EV_KEY,46,0} };
        n = run_scenario(s, 6, H, out, MAXEV, 3);   /* 3-byte writes */
        render(out, n, got, sizeof(got));
        ok("partial reads (3-byte writes)", !strcmp(got, "AD AU CD CU"),
           "got: %s", got);
    }
    {
        /* stream truncated mid-struct */
        char *argv[] = { (char *)BIN, (char *)"--hold", (char *)"26",
                         (char *)"--rt", (char *)"0", NULL };
        struct child c;
        spawn(&c, argv);
        struct input_event e = mk(EV_KEY, 30, 1);
        (void)wr(c.in, &e, sizeof(e));
        sleep_ms(100);
        (void)wr(c.in, &e, 10);                        /* half a struct */
        close(c.in);
        n = drain(c.out, out, MAXEV);
        close(c.out);
        int rc = reap(&c, NULL, 0);
        /* press + synthetic release + SYN_REPORT: the key was left down */
        bool rel = false;
        for (size_t i = 0; i < n; i++)
            if (out[i].type == EV_KEY && out[i].code == 30 && out[i].value == 0)
                rel = true;
        ok("truncated stream: no crash, press delivered and released",
           rc == 0 && n == 3 && rel, "rc=%d events=%zu release=%d", rc, n, rel);
    }
    {
        /* 400 non-cancellable events -> queue overflow (cap 256) */
        char *argv[] = { (char *)BIN, (char *)"--hold", (char *)"200",
                         (char *)"--rt", (char *)"0", (char *)"--verbose", NULL };
        struct child c;
        spawn(&c, argv);
        for (int i = 0; i < 400; i++) {
            struct input_event e = mk(EV_MSC, 4, i);
            (void)wr(c.in, &e, sizeof(e));
        }
        close(c.in);
        n = drain(c.out, out, MAXEV);
        close(c.out);
        char err[1024];
        int rc = reap(&c, err, sizeof(err));
        ok("400-event burst: nothing lost", rc == 0 && n == 400,
           "rc=%d events=%zu", rc, n);
        ok("overflow handled in pass-through", strstr(err, "queue full") != NULL,
           "stderr: %.200s", err);
    }
    {
        /* 200 complete instantaneous pairs: all below threshold */
        char *argv[] = { (char *)BIN, (char *)"--hold", (char *)"200",
                         (char *)"--rt", (char *)"0", NULL };
        struct child c;
        spawn(&c, argv);
        for (int i = 0; i < 200; i++) {
            struct input_event p = mk(EV_KEY, (uint16_t)(30 + i % 5), 1);
            struct input_event r = mk(EV_KEY, (uint16_t)(30 + i % 5), 0);
            (void)wr(c.in, &p, sizeof(p));
            (void)wr(c.in, &r, sizeof(r));
        }
        close(c.in);
        n = drain(c.out, out, MAXEV);
        close(c.out);
        int rc = reap(&c, NULL, 0);
        ok("400 events in complete pairs: all filtered", rc == 0 && n == 0,
           "rc=%d leftover=%zu", rc, n);
    }

    printf("\n[6] Pass-through of non-filterable events\n");
    {
        struct step s[] = { {0,EV_KEY,30,1}, {10,EV_SYN,0,0},
                            {300,EV_KEY,30,2}, {10,EV_KEY,30,0},
                            {20,EV_MSC,4,458792} };
        n = run_scenario(s, 5, H, out, MAXEV, 0);
        bool syn = false, rep = false, msc = false;
        for (size_t i = 0; i < n; i++) {
            if (out[i].type == EV_SYN) syn = true;
            if (out[i].type == EV_KEY && out[i].value == 2) rep = true;
            if (out[i].type == EV_MSC) msc = true;
        }
        ok("EV_SYN passes through", syn, NULL);
        ok("autorepeat passes through", rep, NULL);
        ok("EV_MSC passes through", msc, NULL);
    }

    printf("\n[7] Latency (idle)\n");
    {
        /*
         * Idle measurement only.
         *
         * A previous version also benchmarked latency under synthetic CPU
         * load, with and without SCHED_FIFO. That measurement was dropped:
         * across runs the "RT gain" swung between -12 ms and +24 ms on the
         * p99, because a p99 over ~100 samples, taken by a harness that is
         * itself not realtime, is dominated by scheduling noise. Reporting
         * numbers that unstable would be misleading.
         *
         * What is stable, and is what we assert here, is that the median
         * stays pinned at the configured threshold in every configuration.
         */
        struct lat a, b;
        bool rt_a = false, rt_b = false;

        if (measure_latency(0,  H, 120, &a, &rt_a)) show_lat("  normal", &a, "");
        if (measure_latency(20, H, 120, &b, &rt_b))
            show_lat("  SCHED_FIFO 20", &b, rt_b ? "(RT active)" : "(RT not granted)");

        ok("median ~ threshold (normal)", a.med > H - 2 && a.med < H + 2,
           "med=%.2f", a.med);
        ok("median ~ threshold (RT)", b.med > H - 2 && b.med < H + 2,
           "med=%.2f", b.med);
    }

    alarm(0);
    printf("\n%s\n", "========================================================");
    printf("passed: %d   failed: %d\n", passed, failed);
    return failed ? 1 : 0;
}
