/*
 * ime.h -- THE VITA'S OWN KEYBOARD, behind four calls.
 *
 * The Select player page needs a name typed, and the original's dialog is typed
 * into on a PC keyboard this machine has not got. `sceImeDialog` is the machine's
 * answer: the system's own on-screen keyboard, drawn by the compositor over
 * whatever the app is showing, with the player's own language and their own
 * enter-button assignment.
 *
 * IT IS BEHIND THIS FILE AND NOT IN mainmenu.c, for the reason every other Vita
 * dependency in this port is: `mainmenu.c` is host-testable and `mainmenu_test`
 * links it on a machine with no `psp2/` headers at all. On anything that is not
 * the Vita every call below is a stub, `ime_available()` answers 0, and the page
 * falls back to the on-screen grid it already had -- which is also what happens
 * on the Vita if the dialog refuses to open, so the fallback is one failed
 * `sceImeDialogInit` away rather than dead code.
 *
 * TWO THINGS THE CALLER MUST DO, and neither is optional:
 *
 *   - `ime_init()` once, after the GL context exists. It sets the COMMON DIALOG
 *     config -- without it `sceImeDialogInit` returns NOT_CONFIGURED and nothing
 *     says why.
 *   - while `ime_active()`, swap with `vglSwapBuffers(GL_TRUE)`. That is what
 *     runs `sceCommonDialogUpdate`, and it is what puts the keyboard on screen;
 *     with GL_FALSE the dialog is running and invisible, and the app looks hung.
 *
 * ASCII ONLY, deliberately. The type is BASIC_LATIN and `maxTextLength` is the
 * caller's, so what comes back fits the engine's own 16-byte name field without
 * a multi-byte name having to be refused after the fact. Anything outside
 * 0x20..0x7e that arrives anyway is dropped on the way out.
 */
#ifndef IME_H
#define IME_H

/* 1 where the dialog exists at all. Compile-time, not a probe. */
int  ime_available(void);

/* Configure the common dialog. Returns 1 if the keyboard can be expected to
   open. Safe to call twice; safe to skip on the host. */
int  ime_init(void);

/* Open the keyboard on `initial`, at most `maxlen` characters (not counting the
   NUL). Returns 1 if it opened. `title` is the prompt above the field. */
int  ime_open(const char *title, const char *initial, int maxlen);

/* Whether a dialog is up -- the swap flag, and the caller's own "eat every
   input" test. */
int  ime_active(void);

/* One poll, once a frame while active:
      0  still up
      1  the player accepted; `out` holds the name, NUL-terminated
     -1  the player cancelled, or the dialog failed
   The dialog is torn down on 1 and on -1, so ime_active() is 0 afterwards. */
int  ime_poll(char *out, int n);

/* Take the dialog down without an answer. A no-op when nothing is up. */
void ime_abort(void);

#endif /* IME_H */
