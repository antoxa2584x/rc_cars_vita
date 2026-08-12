/*
 * audio.h -- the Vita audio backend: the output thread, the MP3 streamer and
 * the playlist. Everything that decides how a sound SOUNDS is in mix.c; this
 * file is only what has to touch psp2 and the filesystem.
 *
 * Two threads run behind this interface:
 *
 *   the output thread   blocks in sceAudioOutOutput, and between blocks calls
 *                       mix_render() with the voice lock held
 *   the decoder thread  keeps the music ring topped up from an MP3 on disk
 *
 * so the game thread never blocks on audio and a slow read off the memory card
 * cannot glitch the SFX. Anything that touches the mixer from the game thread
 * has to go through audio_lock()/audio_unlock(); sfx.c does that for you and is
 * the interface the game should actually use.
 *
 * THE PLAYLIST is the game's own. pack_snd.py copies Autoexec.gm's TrackMP
 * lines into assets/music/music.idx, keeping the `cycle` group:
 *
 *     cycle 1   the 7 long tracks   -- AUDIO_MUSIC_RACE
 *     cycle 0   the 11 short tracks -- AUDIO_MUSIC_MENU
 *
 * A group plays through in order and wraps, advancing by itself at end of file.
 */

#ifndef AUDIO_H
#define AUDIO_H

#include "mix.h"

#define AUDIO_MUSIC_MENU 0
#define AUDIO_MUSIC_RACE 1

/* bank_path: "app0:assets/sound.sbk". music_dir: "app0:assets/music".
   Returns 0 on success. A failure here is not fatal to the game -- every entry
   point below is safe to call on a failed init and simply does nothing, so a
   missing sound.sbk costs you audio and nothing else. */
int  audio_init(const char *bank_path, const char *music_dir);
void audio_shutdown(void);

int  audio_ok(void);

/* The mixer, and the lock guarding it against the output thread. */
mix_t *audio_mix(void);
void audio_lock(void);
void audio_unlock(void);

void audio_master(float sfx, float music);

/* Start a group. Re-selecting the group already playing is a no-op, so this is
   safe to call every frame. -1 stops the music. */
void audio_music_group(int group);
void audio_music_next(void);
const char *audio_music_title(void);
int  audio_music_count(int group);

/* On the host build there is no output thread; the test harness calls this to
   pump the decoder by hand. On the Vita it is a no-op. */
void audio_pump_host(void);

#endif /* AUDIO_H */
