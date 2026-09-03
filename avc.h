/*
 * avc.h -- the Vita's hardware H.264 decoder, as much of it as the launch
 * movie needs.
 *
 * WHY THIS EXISTS AT ALL. The game's intro is `Tracks/Intro.dat`, an MPEG-1
 * program stream, and the Vita has no MPEG-1 decoder anywhere -- sceAvcdec is
 * H.264 and that is the only video decoder in the OS. So the movie is
 * transcoded offline by rccars_re/pack_vid.py into H.264 Annex-B with one
 * access unit per frame, and this file feeds those units in and hands back a
 * GL texture.
 *
 * ONE AU IN, ONE PICTURE OUT, and that is a property of the encode rather than
 * of this file: pack_vid.py passes `-bf 0`, so there are no B-frames, decode
 * order is display order, and nothing here has to hold a reorder buffer.
 *
 * THE FRAME IS NEVER COPIED. The decoder is asked for RGBA8888 straight into a
 * physically-contiguous block that is also mapped into the GPU's address space,
 * and the GL texture's own GXM descriptor is pointed at that block -- so the
 * bytes the decoder writes are the bytes the GPU samples. The alternative was a
 * 1.2 MB memcpy every tick out of uncached memory, which on this machine reads
 * at a few hundred MB/s; see docs/vita-port.md on the 69% memcpy.
 *
 * That is why the buffers are UNCACHED (PHYCONT_NC_RW): the CPU never reads a
 * decoded frame, so there is nothing to gain from a cached mapping and there is
 * no cache line the hardware decoder could leave stale under it.
 *
 * TWO PICTURE BUFFERS, alternating. vglSwapBuffers waits for the GPU, so one
 * would very nearly do -- but "very nearly" over a buffer the display is
 * reading is a tearing bug that only shows on hardware, and the second buffer
 * costs 1.2 MB for the length of the intro and nothing after it.
 *
 * EVERYTHING IS RECLAIMED by avc_close(): the memory block is unmapped and
 * freed, the decoder is deleted and the library terminated. The intro runs
 * before the first track is loaded and must not still be holding 6 MB of
 * physically-contiguous memory when it is.
 */

#ifndef AVC_H
#define AVC_H

/* Bring the decoder up for a stream of `w` x `h` (both multiples of 16) whose
 * largest access unit is `max_au` bytes. Returns 0 on success and a negative
 * value on failure, in which case nothing was allocated and avc_close() need
 * not be called -- though calling it is safe.
 *
 * On the host build this always fails: there is no decoder, and the harness
 * exercises the sequencing rather than the pictures. */
int avc_open(int w, int h, int max_au);

void avc_close(void);

/* Feed one access unit. -> 1 if a picture came out and avc_tex() now names it,
 * 0 if the decoder swallowed the unit without producing one, negative on error.
 *
 * `size` must be <= the max_au avc_open was given; a larger unit is refused
 * rather than truncated, because half an access unit is not a decodable thing
 * and the decoder's own error would be about the wrong subject. */
int avc_decode(const void *au, int size);

/* The texture holding the most recent picture, 0 before there is one. Its
 * dimensions are avc_open's w and h; sample it over the whole [0,1] range. */
unsigned int avc_tex(void);

/* How many pictures have come out since avc_open. The intro's clock is the
 * audio, not this, but a decode that produces nothing is worth being able to
 * see from the log. */
int avc_frames(void);

#endif /* AVC_H */
