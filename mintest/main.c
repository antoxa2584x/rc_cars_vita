#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <vitaGL.h>

/* vitaGL apps must reserve a large newlib heap; the default is far too small */
unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;

int main(void) {
    sceClibPrintf("[min] start\n");
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    GLboolean ok = vglInitExtended(0, 960, 544, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
    sceClibPrintf("[min] vglInitExtended -> %d\n", (int)ok);
    glClearColor(0.9f, 0.3f, 0.1f, 1.f);
    SceCtrlData pad;
    for (int i = 0; i < 600; i++) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_FALSE);
        if (i == 30) sceClibPrintf("[min] 30 frames ok\n");
    }
    sceClibPrintf("[min] done\n");
    sceKernelExitProcess(0);
    return 0;
}
