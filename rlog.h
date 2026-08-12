/*
 * rlog.h -- the port's log, on a real machine.
 *
 * Everything used to go to sceClibPrintf, which is a debug-channel write: it
 * lands in Vita3K's console and in a devkit's, and on retail hardware it goes
 * NOWHERE. So every diagnostic this port has -- the frame breakdown, the per-wheel
 * contact dump, the packing counts, the audio residency -- was invisible on the
 * only machine whose numbers actually matter.
 *
 * rlog() writes both: the debug channel as before, and a file under
 * ux0:data/rccars/. The file is truncated at startup so a run's log is that run's,
 * and it is FLUSHED AFTER EVERY LINE -- a crash must not eat the line that says
 * why. That costs a memory-card write per line, which is why nothing here may be
 * called per frame; the existing callers are per second or per load.
 *
 * Off the Vita this is stdout, so the host harnesses link it unchanged.
 */

#ifndef RLOG_H
#define RLOG_H

/* Open (and truncate) the log. Safe to call more than once; the first wins.
   Everything logged before this still reaches the debug channel. */
void rlog_init(void);

/* printf into both sinks. Callers supply their own trailing newline, as they
   did with sceClibPrintf. */
void rlog(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

void rlog_shutdown(void);

/* Where the file went, for the one line that says so on screen. "" if none. */
const char *rlog_path(void);

/* Bytes written so far, and the cap. A session left running must not fill the
   card: past RLOG_MAX_BYTES the file stops growing and the debug channel
   carries on. */
#define RLOG_MAX_BYTES (4u * 1024u * 1024u)
unsigned rlog_bytes(void);

#endif
