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
#define RLOG_DIR  "ux0:data/rccars"
#define RLOG_FILE "ux0:data/rccars/rccars.log"
#else
#define RLOG_DIR  "."
#define RLOG_FILE "rccars.log"
#endif

static FILE *L;
static int inited;
static unsigned written;
static char path_shown[64];

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
    L = fopen(RLOG_FILE, "w");
    if (L)
        snprintf(path_shown, sizeof(path_shown), "%s", RLOG_FILE);

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

#ifdef __vita__
    sceClibPrintf("%s", buf);
#else
    fputs(buf, stdout);
#endif

    if (!L || written >= RLOG_MAX_BYTES)
        return;
    fwrite(buf, 1, (size_t)n, L);
    written += (unsigned)n;
    if (written >= RLOG_MAX_BYTES)
        fputs("[rccars] log size cap reached; file closed, debug channel continues\n", L);
    /* Flush every line. The whole point of this file is to survive whatever
       happened next. */
    fflush(L);
}

void rlog_shutdown(void)
{
    if (L) {
        fclose(L);
        L = NULL;
    }
}

const char *rlog_path(void) { return path_shown; }
unsigned rlog_bytes(void) { return written; }
