/*
 * ime.c -- see ime.h.
 */

#include "ime.h"
#include "rlog.h"

#include <string.h>

#ifndef __vita__

/* THE HOST. Every entry point answers "no keyboard here", which is what makes
   mainmenu.c's grid the path `mainmenu_test` walks. */
int  ime_available(void) { return 0; }
int  ime_init(void) { return 0; }
int  ime_open(const char *t, const char *i, int m)
{
    (void)t; (void)i; (void)m;
    return 0;
}
int  ime_active(void) { return 0; }
int  ime_poll(char *out, int n) { (void)out; (void)n; return -1; }
void ime_abort(void) {}

#else

#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/sysmodule.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>

/* SCE_IME_DIALOG_MAX_TEXT_LENGTH is 2048; nothing here wants a name that long,
   and the two buffers are UTF-16 so they are twice the characters. 64 is four
   times the engine's own field. */
#define IME_MAX 64

static SceWChar16 buf_in[IME_MAX + 1];
static SceWChar16 buf_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static int configured;
static int active;

int ime_available(void) { return 1; }

/* ASCII to UTF-16, bounded, NUL-terminated. */
static void to_w(const char *s, SceWChar16 *w, int max)
{
    int i = 0;
    if (s)
        for (; s[i] && i < max - 1; i++)
            w[i] = (SceWChar16)(unsigned char)s[i];
    w[i] = 0;
}

int ime_init(void)
{
    SceCommonDialogConfigParam cfg;
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    int r;

    if (configured)
        return 1;
    /* The dialog lives in a system module on some firmwares and is already
       resident on others; either answer is fine and neither is fatal. */
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);

    /* THE PLAYER'S OWN LANGUAGE AND THEIR OWN ENTER BUTTON, where they can be
       read. `sceCommonDialogConfigParamInit` leaves both at MAX_VALUE, which is
       the one value sceCommonDialogSetConfigParam refuses -- so these have to be
       filled in whatever happens, and the defaults above are what this app
       already assumes everywhere else (English, CROSS accepts). */
    {
        SceAppUtilInitParam ip;
        SceAppUtilBootParam bp;
        memset(&ip, 0, sizeof ip);
        memset(&bp, 0, sizeof bp);
        if (sceAppUtilInit(&ip, &bp) >= 0) {
            int v = 0;
            if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &v) >= 0)
                lang = v;
            if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON,
                                            &v) >= 0)
                enter = v;
        }
    }

    sceCommonDialogConfigParamInit(&cfg);
    cfg.language = (SceSystemParamLang)lang;
    cfg.enterButtonAssign = (SceSystemParamEnterButtonAssign)enter;
    r = sceCommonDialogSetConfigParam(&cfg);
    configured = r >= 0;
    rlog("[rccars] ime: config lang %d enter %d -> 0x%08x%s\n",
         lang, enter, (unsigned)r,
         configured ? "" : " -- the on-screen grid will be used instead");
    return configured;
}

int ime_open(const char *title, const char *initial, int maxlen)
{
    SceImeDialogParam p;
    int r;

    if (active)
        return 0;
    if (!configured && !ime_init())
        return 0;
    if (maxlen < 1)
        maxlen = 1;
    if (maxlen > IME_MAX)
        maxlen = IME_MAX;

    to_w(title, buf_title, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
    to_w(initial, buf_in, IME_MAX + 1);

    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0;           /* 0 is "whatever the system allows" */
    p.languagesForced = SCE_FALSE;
    /* BASIC_LATIN, so what comes back fits the engine's 16-BYTE name field
       without a multi-byte name having to be refused after it was typed. */
    p.type = SCE_IME_TYPE_BASIC_LATIN;
    p.option = 0;
    p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_DEFAULT;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = buf_title;
    p.maxTextLength = (SceUInt32)maxlen;
    p.initialText = buf_in;
    p.inputTextBuffer = buf_in;

    r = sceImeDialogInit(&p);
    if (r < 0) {
        rlog("[rccars] ime: sceImeDialogInit 0x%08x -- falling back to the "
             "on-screen grid\n", (unsigned)r);
        return 0;
    }
    active = 1;
    return 1;
}

int ime_active(void) { return active; }

int ime_poll(char *out, int n)
{
    SceImeDialogResult res;
    int i, k = 0;

    if (!active)
        return -1;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;

    memset(&res, 0, sizeof res);
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    active = 0;

    if (res.result != SCE_COMMON_DIALOG_RESULT_OK
        || res.button != SCE_IME_DIALOG_BUTTON_ENTER)
        return -1;

    /* Back to ASCII. Anything outside the printable range is DROPPED rather
       than substituted: a name is what the player typed, and a row of question
       marks is not. */
    if (out && n > 0) {
        for (i = 0; i < IME_MAX && buf_in[i] && k < n - 1; i++)
            if (buf_in[i] >= 0x20 && buf_in[i] < 0x7f)
                out[k++] = (char)buf_in[i];
        out[k] = 0;
    }
    return 1;
}

void ime_abort(void)
{
    if (!active)
        return;
    sceImeDialogAbort();
    sceImeDialogTerm();
    active = 0;
}

#endif /* __vita__ */
