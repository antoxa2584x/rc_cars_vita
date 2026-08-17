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
 * ux0:data/rccars/. The file is truncated at startup so a run's log is that run's.
 *
 * THE CARD WRITE IS NOT ON THE GAME THREAD, and that is the whole design.
 *
 * This file used to fflush() after every line, straight from the caller, and the
 * comment defending it said "nothing here may be called per frame; the existing
 * callers are per second or per load". Both clauses are true and together they are
 * not enough: the per-second caller is main.c's frame report, which emits SEVEN
 * lines in one burst, so once every 60 frames the game thread took seven
 * memory-card writes back to back. Measured off a 1,217 s hardware log that burst
 * cost 51.4 ms per block (median 48.8, and 18x the standard error of the estimate,
 * so it is not rounding) -- and ALL TEN of that session's "past the catch-up cap"
 * markers are the frame immediately after a burst. 10 of 10, no exceptions.
 * rbcar.c:418 does not merely cap an over-long frame, it DISCARDS the surplus
 * (`k->acc = 0.f`), so the world lost time and the car broke stride once per
 * block: at 60 fps that is a stutter once a second, in lockstep with the frame
 * counter rather than with anything in the world.
 *
 * IT HID BECAUSE main.c's OWN FRAME REPORT CANNOT SEE IT. `sim`, `draw` and `swap`
 * span `tf` to `t_swap1`, and the report runs after that -- between `t_swap1` and
 * the next iteration's `tf`, the one gap nothing accumulates. Only `dt` covers it,
 * and `dt` is what the physics runs on. A subsystem timed by the thing it delays
 * reports itself as free.
 *
 * So rlog() now formats into a RAM ring and returns; a drain thread owns the file.
 * The caller pays a memcpy. The ring is single-producer -- every caller in main.c,
 * ai.c, scene.c and prop.c is on the game thread, and audio.c and mix.c do not log
 * -- and single-consumer, with the release/acquire pairing mix.c's music ring
 * documents: on ARM the bytes and the counter can become visible to the other core
 * in either order, so a consumer that took the counter without a barrier could
 * read the ring's previous contents.
 *
 * WHAT IT COSTS is the tail on a hard crash. The old promise was "a crash must not
 * eat the line that says why", and up to RLOG_DRAIN_MS of lines can now be in RAM
 * when the machine goes down. Three things make that the right trade: the debug
 * channel is still written synchronously, per line, so on Vita3K and a devkit
 * nothing is lost at all; the drain flushes on every pass, so the window is a
 * bounded 50 ms rather than "whenever stdio feels like it"; and rlog_flush()
 * exists for a path that must not lose what it just said. A guaranteed 50 ms stall
 * every second is a worse bug than a 50 ms window on an event that may never
 * happen.
 *
 * Off the Vita there is no thread: rlog() drains inline before returning, so the
 * host harnesses link this unchanged and still see a line in the file the moment
 * they log it. That also means the ring itself is exercised by every harness run
 * rather than only on hardware.
 */

#ifndef RLOG_H
#define RLOG_H

/* Open (and truncate) the log, and start the drain thread. Safe to call more than
   once; the first wins. Everything logged before this still reaches the debug
   channel. If the thread cannot be created or started, rlog() falls back to
   draining inline -- a machine short of threads still gets its log, at the old
   cost. */
void rlog_init(void);

/* printf into both sinks. Callers supply their own trailing newline, as they
   did with sceClibPrintf. */
void rlog(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Block until everything logged so far has reached the card. For a path that must
   not lose its tail; NOT for per-frame or per-second use, since it waits on
   exactly the write this file exists to keep off the game thread. Bounded -- it
   gives up rather than hanging, the way audio_shutdown's waits do. */
void rlog_flush(void);

void rlog_shutdown(void);

/* Where the file went, for the one line that says so on screen. "" if none. */
const char *rlog_path(void);

/* Bytes written so far, and the cap. A session left running must not fill the
   card: past RLOG_MAX_BYTES the file stops growing and the debug channel
   carries on. */
#define RLOG_MAX_BYTES (4u * 1024u * 1024u)
unsigned rlog_bytes(void);

/* How long a line may sit in RAM before it reaches the card, and therefore how
   much of the tail a hard crash can eat. See the note above. */
#define RLOG_DRAIN_MS 50u

#endif
