/*
 * rlog.c -- see rlog.h.
 */

#include "rlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>            /* SCE_KERNEL_CPU_MASK_USER_* */
#include <psp2/kernel/threadmgr.h>
#define RLOG_DIR  "ux0:data/rccars"
#define RLOG_FILE "ux0:data/rccars/rccars.log"
#else
#define RLOG_DIR  "."
#define RLOG_FILE "rccars.log"
#endif

/* The ring, in bytes, and a POWER OF TWO: the index is masked and the counters
   are monotonic, so `wr - rd` is the fill level with no wrap case to get wrong.
   32 KB is about forty of main.c's once-a-second blocks -- far more slack than a
   card that has gone away for a moment can use up, and it is the same size
   audio.c gives its music ring for the same reason. */
#define RLOG_RING       32768u
#define RLOG_RING_MASK  (RLOG_RING - 1u)

static FILE *L;
static int inited;
static unsigned written;
static char path_shown[64];

/* Producer writes `ring_wr`, consumer writes `ring_rd`; each only reads the
   other's. See the barrier note in rlog.h. */
static char ring[RLOG_RING];
static unsigned ring_wr, ring_rd;
static unsigned dropped;                /* bytes the ring had no room for */
static int drain_inline;                /* no drain thread: write from rlog() */

#ifdef __vita__
static SceUID th_log = -1;
static volatile int running;
#endif

/*
 * Producer side. Appends and returns; never touches L, which is what lets the
 * file go without a lock.
 */
static void ring_put(const char *p, unsigned n)
{
    unsigned rd, room, first;

    if (!n)
        return;
    /* Acquire on the consumer's counter, so the room seen here never overstates
       what the drain has actually finished writing out. */
    rd = __atomic_load_n(&ring_rd, __ATOMIC_ACQUIRE);
    room = RLOG_RING - (ring_wr - rd);
    if (n > room) {
        dropped += n;                   /* reported by rlog(), never silent */
        return;
    }
    first = RLOG_RING - (ring_wr & RLOG_RING_MASK);
    if (first > n)
        first = n;
    memcpy(ring + (ring_wr & RLOG_RING_MASK), p, first);
    if (n > first)
        memcpy(ring, p + first, n - first);
    /* Release, pairing with the drain's acquire: the bytes above must be visible
       to the other core before the counter that advertises them. */
    __atomic_store_n(&ring_wr, ring_wr + n, __ATOMIC_RELEASE);
}

/*
 * Consumer side, and the ONLY writer to L.
 */
static void drain(void)
{
    unsigned wr, rd;

    if (!L)
        return;
    wr = __atomic_load_n(&ring_wr, __ATOMIC_ACQUIRE);
    rd = ring_rd;
    if (rd == wr)
        return;                         /* nothing to do, and no flush either */
    while (rd != wr) {
        unsigned n = wr - rd;
        unsigned first = RLOG_RING - (rd & RLOG_RING_MASK);
        if (n > first)
            n = first;                  /* stop at the wrap; the loop takes the rest */
        fwrite(ring + (rd & RLOG_RING_MASK), 1, n, L);
        rd += n;
        __atomic_store_n(&ring_rd, rd, __ATOMIC_RELEASE);
    }
    /* ONE flush per drain rather than one per line. That is the entire fix: the
       count of card writes per second drops from seven to one, and the one is not
       on the thread that has a frame to finish. */
    fflush(L);
}

#ifdef __vita__
static int log_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
        drain();
        sceKernelDelayThread(RLOG_DRAIN_MS * 1000u);
    }
    drain();                            /* whatever arrived while we were stopped */
    return 0;
}
#endif

void rlog_init(void)
{
    if (inited)
        return;
    inited = 1;

#ifdef __vita__
    /* ux0:data exists on every machine; the app's own subdirectory may not, and
       sceIoMkdir on an existing one is a harmless error we ignore. */
    sceIoMkdir(RLOG_DIR, 0777);
#endif
    /*
     * REMOVED FIRST, because "w" does not reliably truncate on this platform and a
     * log that is half this run and half the last one is a diagnostic trap. A real
     * one: a 4,400-line file whose first 2,728 lines were the current session and
     * whose remainder -- after one torn line -- was an older, longer one, which
     * read as "the characters stopped loading half way through the session"
     * because the tail had no .chr lines in it. remove() then fopen() gives a
     * fresh file on any implementation; a missing file is not an error here.
     */
    remove(RLOG_FILE);
    L = fopen(RLOG_FILE, "w");
    if (L) {
        snprintf(path_shown, sizeof(path_shown), "%s", RLOG_FILE);
        /* The ring is already the buffer, so there is no reason to let stdio add
           a second one with its own flush policy on top. Big enough that a whole
           drain coalesces into one write() instead of one per BUFSIZ. Must come
           before any I/O on the stream. */
        setvbuf(L, NULL, _IOFBF, 8192);
    }

#ifdef __vita__
    if (L) {
        /* THE LOWEST PRIORITY ANYTHING IN THIS APP RUNS AT, and not on core 0.
           It has no deadline of any kind -- a log line is allowed to be
           RLOG_DRAIN_MS late -- and it spends its life either asleep or blocked
           inside one card write, so it can starve neither the frame nor the
           mixer. 0x10000100 is SCE_KERNEL_DEFAULT_PRIORITY_USER and larger
           numbers are LOWER priority here, so +32 puts it below even audio.c's
           decoder at +16. Core 0 is the render loop and the physics; the point of
           this thread is that the write happens somewhere else, so it goes on
           core 2 beside the decoder, which is likewise lazy. */
        running = 1;
        /* 0x10000 of stack, the same as audio.c's two: the drain itself needs
           almost none, but it calls into newlib's stdio and the card path, and a
           stack overflow in there would be a far nastier thing to chase than 64 KB
           is to spend. */
        th_log = sceKernelCreateThread("rccars_log", log_thread,
                                       0x10000100 + 32, 0x10000, 0,
                                       SCE_KERNEL_CPU_MASK_USER_2, NULL);
        if (th_log < 0) {
            running = 0;
            drain_inline = 1;
        } else if (sceKernelStartThread(th_log, 0, NULL) < 0) {
            running = 0;
            drain_inline = 1;
            sceKernelDeleteThread(th_log);
            th_log = -1;
        }
    }
#else
    /* No thread off the Vita: the harnesses expect the line to be in the file by
       the time rlog() returns, and a host build has no frame to protect. */
    drain_inline = 1;
#endif

    rlog("[rccars] log opened at %s\n", L ? RLOG_FILE : "(failed -- debug channel only)");
}

void rlog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;       /* truncated: log what fits */

    /* Synchronous, per line, unchanged -- this is what keeps a crash's tail on a
       devkit and in Vita3K, and it is why the ring's window is affordable. */
#ifdef __vita__
    sceClibPrintf("%s", buf);
#else
    fputs(buf, stdout);
#endif

    if (!L || written >= RLOG_MAX_BYTES)
        return;

    /* A GAP IN THE FILE MUST SAY SO. Emitted before the line that found room, so
       it marks the place the loss happened; if the notice itself does not fit,
       ring_put counts it and we try again on the next line. */
    if (dropped) {
        char note[96];
        unsigned d = dropped;
        int m = snprintf(note, sizeof(note),
                         "[rccars] log: %u byte(s) dropped, the ring overflowed\n", d);
        dropped = 0;
        if (m > 0) {
            if (m > (int)sizeof(note) - 1)
                m = (int)sizeof(note) - 1;
            ring_put(note, (unsigned)m);
            written += (unsigned)m;
        }
    }

    ring_put(buf, (unsigned)n);
    written += (unsigned)n;
    if (written >= RLOG_MAX_BYTES) {
        static const char cap[] =
            "[rccars] log size cap reached; file closed, debug channel continues\n";
        ring_put(cap, (unsigned)sizeof(cap) - 1u);
    }

    if (drain_inline)
        drain();
}

void rlog_flush(void)
{
    if (!L)
        return;
#ifdef __vita__
    if (!drain_inline) {
        /* Wait for the drain thread rather than writing from here: it is the only
           writer to L and keeping it that way is what makes the file lock-free.
           Bounded at ~1 s -- a flush must not be able to hang the caller, the
           same lesson audio_shutdown's waits carry. `ring_wr` is ours to read. */
        int guard;
        for (guard = 0; guard < 200; guard++) {
            if (__atomic_load_n(&ring_rd, __ATOMIC_ACQUIRE) == ring_wr)
                return;
            sceKernelDelayThread(5000);         /* 5 ms */
        }
        return;
    }
#endif
    drain();
}

void rlog_shutdown(void)
{
#ifdef __vita__
    if (th_log >= 0) {
        int ended;
        __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
        /* The thread is either asleep for at most RLOG_DRAIN_MS or inside one
           card write, so this normally returns at once. Bounded anyway: a
           shutdown path must not be able to hang the exit. */
        ended = sceKernelWaitThreadEnd(th_log, NULL, (SceUInt[]){ 500000 }) >= 0;
        if (!ended)
            return;     /* still in a write: do NOT also drain or fclose from
                           here. Two writers to one FILE during teardown is worse
                           than losing the tail, and the process is going away. */
        sceKernelDeleteThread(th_log);
        th_log = -1;
    }
    drain_inline = 1;
#endif
    drain();                            /* anything the thread did not reach */
    if (L) {
        fclose(L);
        L = NULL;
    }
}

const char *rlog_path(void) { return path_shown; }
unsigned rlog_bytes(void) { return written; }
