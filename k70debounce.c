/*
 * k70debounce - anti-chatter filter based on key hold duration.
 *
 * Drops key presses that are too short to be human. It catches both
 * switch chatter (the same key firing twice) and accidental grazing
 * (isolated presses of different keys), which a classic press->press
 * debounce cannot filter.
 *
 * Principle
 * ---------
 * Every event is delayed by HOLD ms. If the RELEASE of the key that was
 * just pressed arrives within that window, the press+release pair is
 * cancelled and never reaches the system. Delaying every event uniformly
 * guarantees that ordering is preserved.
 *
 * Designed for an Interception Tools pipeline:
 *     interception -g $DEVNODE | k70debounce --hold 26 | uinput -d $DEVNODE
 *
 * Safety choices
 * --------------
 *   - no allocation after initialisation (static ring buffer)
 *   - queue overflow -> flush in pass-through, never lose keystrokes
 *   - write error -> fail-open: pass everything through unfiltered
 *   - RT priority below the kernel IRQ threads (50)
 *   - silent fallback when RT privileges cannot be obtained
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define PROG            "k70debounce"
#define VERSION         "1.0"

#define QUEUE_CAP       256u        /* power of two: index = seq & MASK */
#define QUEUE_MASK      (QUEUE_CAP - 1u)
#define MAX_KEYCODE     (KEY_MAX + 1)
#define STACK_PREFAULT  (256u * 1024u)

#define DEFAULT_HOLD_MS 26
#define DEFAULT_RT_PRIO 20          /* well below the IRQ threads (50) */

/* ------------------------------------------------------------------ state */

struct slot {
    struct input_event ev;
    uint64_t           deadline_ns;
    uint64_t           seq;
    bool               alive;
};

static struct slot   queue[QUEUE_CAP];
static uint64_t      seq_head;                  /* next seq to hand out    */
static uint64_t      seq_tail;                  /* oldest not yet emitted  */

/* pending[code] = seq+1 of the PRESS still queued, 0 if none */
static uint64_t      pending[MAX_KEYCODE];
static bool          ignored[MAX_KEYCODE];

/* keys already delivered and still held, to be released on exit */
static bool          key_down[MAX_KEYCODE];

static uint64_t      hold_ns;
static bool          verbose;
static bool          fail_open;                 /* filtering disabled */

static volatile sig_atomic_t stop_requested;

/* statistics */
static uint64_t stat_in, stat_out, stat_dropped, stat_overflow;

/* defined further down, already needed here in the notify block */
static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* last dropped event, for the status report */
static uint16_t last_drop_code;
static double   last_drop_hold_ms;
static uint64_t last_drop_ns;
static bool     have_drop;

/* --------------------------------------------------- common key names */

/*
 * Compact table for codes 0..127: covers the whole normal typing area.
 * Its only purpose is to make log and status messages readable.
 */
static const char *const keyname_tbl[] = {
    [KEY_ESC]="ESC", [KEY_1]="1", [KEY_2]="2", [KEY_3]="3", [KEY_4]="4",
    [KEY_5]="5", [KEY_6]="6", [KEY_7]="7", [KEY_8]="8", [KEY_9]="9",
    [KEY_0]="0", [KEY_MINUS]="-", [KEY_EQUAL]="=", [KEY_BACKSPACE]="BKSP",
    [KEY_TAB]="TAB", [KEY_Q]="Q", [KEY_W]="W", [KEY_E]="E", [KEY_R]="R",
    [KEY_T]="T", [KEY_Y]="Y", [KEY_U]="U", [KEY_I]="I", [KEY_O]="O",
    [KEY_P]="P", [KEY_LEFTBRACE]="[", [KEY_RIGHTBRACE]="]", [KEY_ENTER]="ENTER",
    [KEY_LEFTCTRL]="LCTRL", [KEY_A]="A", [KEY_S]="S", [KEY_D]="D", [KEY_F]="F",
    [KEY_G]="G", [KEY_H]="H", [KEY_J]="J", [KEY_K]="K", [KEY_L]="L",
    [KEY_SEMICOLON]=";", [KEY_APOSTROPHE]="'", [KEY_GRAVE]="`",
    [KEY_LEFTSHIFT]="LSHIFT", [KEY_BACKSLASH]="\\", [KEY_Z]="Z", [KEY_X]="X",
    [KEY_C]="C", [KEY_V]="V", [KEY_B]="B", [KEY_N]="N", [KEY_M]="M",
    [KEY_COMMA]=",", [KEY_DOT]=".", [KEY_SLASH]="/", [KEY_RIGHTSHIFT]="RSHIFT",
    [KEY_LEFTALT]="LALT", [KEY_SPACE]="SPACE", [KEY_CAPSLOCK]="CAPS",
    [KEY_F1]="F1", [KEY_F2]="F2", [KEY_F3]="F3", [KEY_F4]="F4", [KEY_F5]="F5",
    [KEY_F6]="F6", [KEY_F7]="F7", [KEY_F8]="F8", [KEY_F9]="F9", [KEY_F10]="F10",
    [KEY_F11]="F11", [KEY_F12]="F12",
    /* media controls: also produced by the volume wheel (rotary encoder) */
    [KEY_MUTE]="MUTE", [KEY_VOLUMEDOWN]="VOL-", [KEY_VOLUMEUP]="VOL+",
    [KEY_NEXTSONG]="NEXT", [KEY_PLAYPAUSE]="PLAY", [KEY_PREVIOUSSONG]="PREV",
    [KEY_STOPCD]="STOP",
};

/*
 * Codes exempted by default: media controls.
 *
 * A keyboard volume wheel is a rotary encoder: every detent emits a
 * press+release pair about 4 ms long. That is indistinguishable from
 * chatter yet perfectly legitimate. On top of that, a bounce on these
 * keys would be harmless, since they are not part of typing.
 */
static const uint16_t default_ignored[] = {
    KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP,
    KEY_NEXTSONG, KEY_PLAYPAUSE, KEY_PREVIOUSSONG, KEY_STOPCD,
    KEY_BRIGHTNESSDOWN, KEY_BRIGHTNESSUP,
};

static const char *keyname(uint16_t code)
{
    static char fallback[16];
    if (code < sizeof(keyname_tbl) / sizeof(keyname_tbl[0]) && keyname_tbl[code])
        return keyname_tbl[code];
    snprintf(fallback, sizeof(fallback), "code%u", code);
    return fallback;
}

/* ------------------------------------------------------- sd_notify(3) */

/*
 * Minimal implementation of the sd_notify protocol, without libsystemd.
 * It is a datagram over a UNIX socket: the path comes from $NOTIFY_SOCKET
 * and may be abstract (leading '@') or an ordinary filesystem path.
 *
 * The socket is non-blocking: if systemd does not read, the message is
 * lost. In a process sitting in the input path of every keystroke,
 * losing a status line is vastly preferable to blocking.
 */
static int notify_fd = -1;
static struct sockaddr_un notify_addr;
static socklen_t notify_addrlen;
static const char *notify_path_opt;      /* from --notify-socket */

static void notify_init(void)
{
    /*
     * udevmon spawns its JOBs with execvpe and an environment built from
     * scratch (it only contains DEVNODE), so $NOTIFY_SOCKET never makes
     * it here. That is why the path can be given via --notify-socket.
     */
    const char *path = notify_path_opt ? notify_path_opt : getenv("NOTIFY_SOCKET");
    if (!path || !*path) {
        if (verbose)
            logmsg("no notification socket: pass --notify-socket "
                   "/run/systemd/notify to enable status reporting");
        return;
    }
    if (path[0] != '/' && path[0] != '@') {
        if (verbose)
            logmsg("invalid socket path: %s", path);
        return;
    }
    size_t len = strlen(path);
    if (len >= sizeof(notify_addr.sun_path))
        return;

    notify_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (notify_fd < 0)
        return;

    memset(&notify_addr, 0, sizeof(notify_addr));
    notify_addr.sun_family = AF_UNIX;
    memcpy(notify_addr.sun_path, path, len);
    if (notify_addr.sun_path[0] == '@')      /* abstract namespace */
        notify_addr.sun_path[0] = '\0';
    notify_addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len);

    if (verbose)
        logmsg("status notifications to %s", path);
}

static void notify_send(const char *msg)
{
    if (notify_fd < 0)
        return;
    ssize_t r = sendto(notify_fd, msg, strlen(msg), MSG_NOSIGNAL,
                       (struct sockaddr *)&notify_addr, notify_addrlen);
    if (r < 0 && verbose)
        logmsg("notify sendto failed: %s", strerror(errno));
}

/* Updates the "Status:" line shown by `systemctl status`. */
static uint64_t last_status_ns;

static void notify_status(uint64_t now, bool force)
{
    if (notify_fd < 0)
        return;
    if (!force && now - last_status_ns < 2000000000ull)   /* at most 1 per 2 s */
        return;
    last_status_ns = now;

    char buf[320];
    if (have_drop) {
        double ago_s = (double)(now - last_drop_ns) / 1e9;
        snprintf(buf, sizeof(buf),
                 "STATUS=filter active | threshold %llu ms | events %llu | "
                 "dropped %llu | last: %s at %.1f ms, %.0f s ago\n",
                 (unsigned long long)(hold_ns / 1000000ull),
                 (unsigned long long)stat_in,
                 (unsigned long long)stat_dropped,
                 keyname(last_drop_code), last_drop_hold_ms, ago_s);
    } else {
        snprintf(buf, sizeof(buf),
                 "STATUS=filter active | threshold %llu ms | events %llu | "
                 "dropped 0\n",
                 (unsigned long long)(hold_ns / 1000000ull),
                 (unsigned long long)stat_in);
    }
    notify_send(buf);
}


/* --------------------------------------------------------------- helpers */

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, PROG ": ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* Full write: write() may be partial on a pipe. */
static bool write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool emit(const struct input_event *ev)
{
    if (!write_all(STDOUT_FILENO, ev, sizeof(*ev))) {
        if (!fail_open) {
            logmsg("write error (%s): switching to FAIL-OPEN, filtering disabled",
                   strerror(errno));
            fail_open = true;
        }
        return false;
    }
    /* Track which keys were actually delivered and are still held, so we
     * can release them on exit (see release_stuck_keys). */
    if (ev->type == EV_KEY && ev->code < MAX_KEYCODE) {
        if (ev->value == 1)
            key_down[ev->code] = true;
        else if (ev->value == 0)
            key_down[ev->code] = false;
    }
    stat_out++;
    return true;
}

/*
 * Release every key that is still held down.
 *
 * This matters on exit: when the pipeline is stopped, interception drops
 * the grab and the virtual device is destroyed. If a key was held at that
 * moment, its RELEASE never reaches the system, which stays convinced the
 * key is still down: endless autorepeat and an unusable keyboard.
 *
 * So we synthesise a RELEASE for every key still held, followed by a
 * SYN_REPORT so the consumer processes them.
 */
static void release_stuck_keys(void)
{
    int n = 0;
    for (unsigned c = 0; c < MAX_KEYCODE; c++) {
        if (!key_down[c])
            continue;

        struct input_event rel;
        memset(&rel, 0, sizeof(rel));
        rel.type  = EV_KEY;
        rel.code  = (uint16_t)c;
        rel.value = 0;
        if (!emit(&rel))
            return;
        n++;
    }
    if (n > 0) {
        struct input_event syn;
        memset(&syn, 0, sizeof(syn));
        syn.type = EV_SYN;          /* SYN_REPORT */
        emit(&syn);
        logmsg("exit: released %d key(s) that were still held", n);
    }
}

/* ---------------------------------------------------------------- queue */

static inline size_t queue_len(void)
{
    return (size_t)(seq_head - seq_tail);
}

/* Emit every event whose deadline has passed. */
static void flush_due(uint64_t now)
{
    while (seq_tail < seq_head) {
        struct slot *s = &queue[seq_tail & QUEUE_MASK];
        if (s->deadline_ns > now)
            break;

        /* the PRESS is leaving the queue: it can no longer be cancelled */
        if (s->ev.type == EV_KEY && s->ev.value == 1 &&
            s->ev.code < MAX_KEYCODE && pending[s->ev.code] == s->seq + 1)
            pending[s->ev.code] = 0;

        if (s->alive)
            emit(&s->ev);

        seq_tail++;
    }
}

/* Drain everything without waiting for deadlines (exit / overflow). */
static void flush_all(void)
{
    while (seq_tail < seq_head) {
        struct slot *s = &queue[seq_tail & QUEUE_MASK];
        if (s->ev.type == EV_KEY && s->ev.value == 1 &&
            s->ev.code < MAX_KEYCODE && pending[s->ev.code] == s->seq + 1)
            pending[s->ev.code] = 0;
        if (s->alive)
            emit(&s->ev);
        seq_tail++;
    }
}

static void enqueue(const struct input_event *ev, uint64_t now)
{
    if (queue_len() >= QUEUE_CAP) {
        /* Must not happen with a hold of a few tens of ms. If it does,
         * the safe choice is to deliver, not to discard. */
        stat_overflow++;
        if (verbose)
            logmsg("queue full: forced flush in pass-through");
        flush_all();
    }

    struct slot *s = &queue[seq_head & QUEUE_MASK];
    s->ev          = *ev;
    s->deadline_ns = now + hold_ns;
    s->seq         = seq_head;
    s->alive       = true;

    if (ev->type == EV_KEY && ev->value == 1 &&
        ev->code < MAX_KEYCODE && !ignored[ev->code])
        pending[ev->code] = seq_head + 1;

    seq_head++;
}

/*
 * Try to cancel the PRESS still queued for this key code.
 * Returns true if the press+release pair was suppressed.
 */
static bool try_cancel(const struct input_event *rel, uint64_t now)
{
    uint16_t code = rel->code;
    if (code >= MAX_KEYCODE || pending[code] == 0)
        return false;

    uint64_t seq = pending[code] - 1;
    if (seq < seq_tail || seq >= seq_head)
        return false;                       /* already left the queue */

    struct slot *s = &queue[seq & QUEUE_MASK];
    if (s->seq != seq || !s->alive || s->ev.code != code)
        return false;

    s->alive       = false;
    pending[code]  = 0;
    stat_dropped++;

    last_drop_code    = code;
    last_drop_hold_ms = (double)(now - (s->deadline_ns - hold_ns)) / 1e6;
    last_drop_ns      = now;
    have_drop         = true;

    /* One line per drop lands in the journal. At roughly one drop per
     * minute that is harmless, and stderr is non-blocking in RT mode. */
    logmsg("dropped %s (hold %.1f ms) - total %llu",
           keyname(code), last_drop_hold_ms,
           (unsigned long long)stat_dropped);

    notify_status(now, true);
    return true;
}

/* -------------------------------------------------------------- realtime */

static void setup_realtime(int prio)
{
    if (prio <= 0)
        return;

    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;

    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        /* Fallback: running at normal priority beats not running. */
        logmsg("could not obtain RT priority (%s), continuing normally",
               strerror(errno));
        return;
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        logmsg("mlockall failed (%s), page faults possible", strerror(errno));

    /*
     * Non-blocking stderr: messages reach the journal through a socket.
     * If journald is slow or its queue is full, a blocking write would
     * stall the filter and therefore the keyboard. Better to lose a log
     * line.
     */
    int fl = fcntl(STDERR_FILENO, F_GETFL);
    if (fl != -1)
        fcntl(STDERR_FILENO, F_SETFL, fl | O_NONBLOCK);

    /* pre-fault the stack: avoids page faults on the critical path */
    volatile unsigned char scratch[STACK_PREFAULT];
    memset((void *)scratch, 0, sizeof(scratch));

    if (verbose)
        logmsg("SCHED_FIFO priority %d, memory locked", prio);
}

/* --------------------------------------------------------------- runtime */

static int run_realtime(void)
{
    unsigned char buf[sizeof(struct input_event) * 8];
    size_t        have = 0;

    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };

    notify_init();
    logmsg("started: threshold %llu ms, RT priority requested",
           (unsigned long long)(hold_ns / 1000000ull));
    notify_send("READY=1\n");
    notify_status(now_ns(), true);

    while (!stop_requested) {
        uint64_t now = now_ns();
        flush_due(now);
        notify_status(now, false);

        struct timespec  to;
        struct timespec *pto = NULL;
        /* Even with an empty queue we wake up every 5 s to keep the
         * status line shown by `systemctl status` fresh. Negligible
         * cost, and only when someone is listening. */
        uint64_t delta = (notify_fd >= 0) ? 5000000000ull : 0;
        if (seq_tail < seq_head) {
            uint64_t d = queue[seq_tail & QUEUE_MASK].deadline_ns;
            uint64_t q = (d > now) ? (d - now) : 0;
            delta = (delta == 0 || q < delta) ? q : delta;
        }
        if (delta > 0 || seq_tail < seq_head) {
            to.tv_sec  = (time_t)(delta / 1000000000ull);
            to.tv_nsec = (long)(delta % 1000000000ull);
            pto = &to;
        }

        int r = ppoll(&pfd, 1, pto, NULL);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            logmsg("ppoll: %s", strerror(errno));
            break;
        }
        if (r == 0)
            continue;                       /* timeout only: loop and flush */

        ssize_t n = read(STDIN_FILENO, buf + have, sizeof(buf) - have);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            logmsg("read: %s", strerror(errno));
            break;
        }
        if (n == 0)
            break;                          /* EOF */

        have += (size_t)n;
        now   = now_ns();

        /* Consume whole structs only: a read on a pipe may be partial. */
        size_t off = 0;
        while (have - off >= sizeof(struct input_event)) {
            struct input_event ev;
            memcpy(&ev, buf + off, sizeof(ev));
            off += sizeof(ev);
            stat_in++;

            if (fail_open) {
                emit(&ev);
                continue;
            }
            if (ev.type == EV_KEY && ev.value == 0 && try_cancel(&ev, now))
                continue;                   /* drop the release as well */

            enqueue(&ev, now);
        }

        /* Keep the partial remainder for the next read. */
        if (off > 0 && off < have)
            memmove(buf, buf + off, have - off);
        have -= off;

        flush_due(now_ns());
    }

    flush_all();
    release_stuck_keys();

    notify_send("STOPPING=1\n");
    logmsg("exit: events %llu, dropped %llu, overflow %llu",
           (unsigned long long)stat_in,
           (unsigned long long)stat_dropped,
           (unsigned long long)stat_overflow);
    return 0;
}

/* --------------------------------------------------------------- offline */

/*
 * Deterministic mode: decides purely from the event timestamps, with no
 * waiting. Used to replay a capture and verify that the filter drops
 * exactly the expected events.
 * malloc is fine here: we are not in the input path.
 */
static int run_offline(void)
{
    size_t cap = 4096, n = 0;
    struct input_event *ev = malloc(cap * sizeof(*ev));
    if (!ev) {
        logmsg("out of memory");
        return 1;
    }

    for (;;) {
        if (n == cap) {
            cap *= 2;
            struct input_event *t = realloc(ev, cap * sizeof(*ev));
            if (!t) { free(ev); logmsg("out of memory"); return 1; }
            ev = t;
        }
        ssize_t r = read(STDIN_FILENO, &ev[n], sizeof(ev[n]));
        if (r == 0)
            break;
        if (r < 0) {
            if (errno == EINTR) continue;
            logmsg("read: %s", strerror(errno));
            free(ev);
            return 1;
        }
        if (r != (ssize_t)sizeof(ev[n]))
            break;                          /* struct truncated at end of stream */
        n++;
    }

    bool *drop = calloc(n ? n : 1, sizeof(*drop));
    if (!drop) { free(ev); logmsg("out of memory"); return 1; }

    /* index of the last PRESS not yet closed, per key code */
    static size_t press_idx[MAX_KEYCODE];
    static bool   press_has[MAX_KEYCODE];
    memset(press_has, 0, sizeof(press_has));

    uint64_t hold_us = hold_ns / 1000;

    for (size_t i = 0; i < n; i++) {
        if (ev[i].type != EV_KEY || ev[i].code >= MAX_KEYCODE)
            continue;
        uint16_t c = ev[i].code;

        if (ev[i].value == 1) {
            press_idx[c] = i;
            press_has[c] = true;
        } else if (ev[i].value == 0 && press_has[c]) {
            size_t p = press_idx[c];
            press_has[c] = false;
            if (ignored[c])
                continue;

            uint64_t t0 = (uint64_t)ev[p].input_event_sec * 1000000ull
                        + (uint64_t)ev[p].input_event_usec;
            uint64_t t1 = (uint64_t)ev[i].input_event_sec * 1000000ull
                        + (uint64_t)ev[i].input_event_usec;
            if (t1 < t0)
                continue;

            if (t1 - t0 < hold_us) {
                drop[p] = drop[i] = true;
                stat_dropped++;
                if (verbose)
                    logmsg("dropped %s (hold %.1f ms)", keyname(c),
                           (double)(t1 - t0) / 1000.0);
            }
        }
    }

    for (size_t i = 0; i < n; i++) {
        stat_in++;
        if (!drop[i])
            emit(&ev[i]);
    }

    if (verbose)
        logmsg("offline: in=%llu out=%llu dropped=%llu",
               (unsigned long long)stat_in, (unsigned long long)stat_out,
               (unsigned long long)stat_dropped);

    free(drop);
    free(ev);
    return 0;
}

/* ------------------------------------------------------------------ main */

static void usage(FILE *f)
{
    fprintf(f,
"usage: " PROG " [options]\n"
"\n"
"Filters out key presses shorter than a threshold, removing switch\n"
"chatter and accidental key grazing.\n"
"Reads struct input_event from stdin and writes them to stdout.\n"
"\n"
"options:\n"
"  --hold <ms>          minimum hold duration            (default %d)\n"
"  --rt <prio>          SCHED_FIFO priority, 0 = none    (default %d)\n"
"  --ignore-code <n>    key code to never filter         (repeatable)\n"
"  --no-default-ignore  also filter the media controls\n"
"                       (by default volume, play, next/prev and\n"
"                        brightness are exempt, because a volume wheel is\n"
"                        a rotary encoder and emits legitimate pulses only\n"
"                        a few ms long)\n"
"  --notify-socket <p>  sd_notify socket for status reporting to systemd\n"
"                       (udevmon does not pass $NOTIFY_SOCKET to its JOBs)\n"
"  --offline            deterministic mode driven by event timestamps\n"
"  --verbose            diagnostics on stderr\n"
"  --version            print the version\n"
"  --help               this message\n"
"\n"
"example:\n"
"  interception -g $DEVNODE | " PROG " --hold 26 | uinput -d $DEVNODE\n",
        DEFAULT_HOLD_MS, DEFAULT_RT_PRIO);
}

int main(int argc, char **argv)
{
    long hold_ms = DEFAULT_HOLD_MS;
    long rt_prio = DEFAULT_RT_PRIO;
    bool offline = false;
    bool use_default_ignore = true;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(a, "--version")) {
            printf(PROG " " VERSION "\n");
            return 0;
        }
        if (!strcmp(a, "--verbose")) { verbose = true; continue; }
        if (!strcmp(a, "--offline")) { offline = true; continue; }
        if (!strcmp(a, "--no-default-ignore")) { use_default_ignore = false; continue; }

        if (!strcmp(a, "--hold") && i + 1 < argc) {
            char *end;
            hold_ms = strtol(argv[++i], &end, 10);
            if (*end != '\0' || hold_ms < 0 || hold_ms > 1000) {
                logmsg("invalid --hold: %s (expected 0..1000)", argv[i]);
                return 2;
            }
            continue;
        }
        if (!strcmp(a, "--rt") && i + 1 < argc) {
            char *end;
            rt_prio = strtol(argv[++i], &end, 10);
            if (*end != '\0' || rt_prio < 0 || rt_prio > 49) {
                logmsg("invalid --rt: %s (expected 0..49, below the IRQ threads)",
                       argv[i]);
                return 2;
            }
            continue;
        }
        if (!strcmp(a, "--notify-socket") && i + 1 < argc) {
            notify_path_opt = argv[++i];
            continue;
        }
        if (!strcmp(a, "--ignore-code") && i + 1 < argc) {
            char *end;
            long c = strtol(argv[++i], &end, 10);
            if (*end != '\0' || c < 0 || c >= MAX_KEYCODE) {
                logmsg("invalid --ignore-code: %s", argv[i]);
                return 2;
            }
            ignored[c] = true;
            continue;
        }

        logmsg("unknown option: %s", a);
        usage(stderr);
        return 2;
    }

    hold_ns = (uint64_t)hold_ms * 1000000ull;

    if (use_default_ignore)
        for (size_t k = 0; k < sizeof(default_ignored) / sizeof(default_ignored[0]); k++)
            ignored[default_ignored[k]] = true;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (offline)
        return run_offline();

    setup_realtime((int)rt_prio);
    return run_realtime();
}
