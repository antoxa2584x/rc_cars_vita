/*
 * avc.c -- see avc.h.
 */

#include "avc.h"
#include "rlog.h"

#include <stdlib.h>
#include <string.h>

#ifdef __vita__

#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <vitaGL.h>

/* The number of reference frames to size the decoder for. pack_vid.py encodes
   baseline with no B-frames, so x264 uses at most 3 -- ask for 3 rather than
   trusting the stream, since the cost is decoder work memory the intro gives
   back immediately. */
#define AVC_REFS 3

/* One block holds the work memory, the access-unit buffer and the YUV frame,
   because a PHYCONT allocation is rounded up to a whole megabyte and three of
   them would waste two. */
#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))
#define MB (1024u * 1024u)

static struct {
    int ok;
    int w, h, max_au;

    SceAvcdecCtrl ctrl;
    SceUID block;
    void *base;
    unsigned block_size;

    unsigned char *es;          /* the access-unit staging buffer */
    unsigned char *yuv;         /* the decoder's output: NV12 -- Y, then UV */
    unsigned yuv_bytes;

    unsigned char *rgba;        /* the conversion's output, ordinary heap */

    GLuint tex;
    int frames;
} V;

static void free_all(void)
{
    if (V.tex) {
        glDeleteTextures(1, &V.tex);
        V.tex = 0;
    }
    if (V.ctrl.frameBuf.pBuf) {
        sceAvcdecDeleteDecoder(&V.ctrl);
        memset(&V.ctrl, 0, sizeof V.ctrl);
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    }
    if (V.block > 0) {
        sceKernelFreeMemBlock(V.block);
        V.block = 0;
    }
    V.base = NULL;
    V.es = NULL;
    V.yuv = NULL;
    free(V.rgba);
    V.rgba = NULL;
}

int avc_open(int w, int h, int max_au)
{
    SceVideodecQueryInitInfoHwAvcdec init;
    SceAvcdecQueryDecoderInfo q;
    SceAvcdecDecoderInfo di;
    unsigned work, es_off, yuv_off;
    int r;

    memset(&V, 0, sizeof V);
    if (w <= 0 || h <= 0 || max_au <= 0)
        return -1;
    /* The decoder rejects anything not on a macroblock with
       SCE_AVCDEC_ERROR_UNSUPPORT_IMAGE_SIZE, and pack_vid.py already only
       emits such sizes -- so this is about a hand-made .vid, not about the
       shipped one. */
    if ((w & 15) || (h & 15)) {
        rlog("[rccars] avc: %dx%d is not on a macroblock\n", w, h);
        return -2;
    }

    V.w = w;
    V.h = h;
    V.max_au = max_au;

    if (sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC) < 0) {
        rlog("[rccars] avc: SCE_SYSMODULE_AVCDEC would not load\n");
        return -3;
    }

    memset(&init, 0, sizeof init);
    init.size = sizeof init;
    init.horizontal = (unsigned)w;
    init.vertical = (unsigned)h;
    init.numOfRefFrames = AVC_REFS;
    init.numOfStreams = 1;
    r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (r < 0) {
        rlog("[rccars] avc: sceVideodecInitLibrary 0x%08X\n", r);
        return -4;
    }

    memset(&q, 0, sizeof q);
    q.horizontal = (unsigned)w;
    q.vertical = (unsigned)h;
    q.numOfRefFrames = AVC_REFS;
    memset(&di, 0, sizeof di);
    r = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &di);
    if (r < 0) {
        rlog("[rccars] avc: sceAvcdecQueryDecoderMemSize 0x%08X\n", r);
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        return -5;
    }

    /* One block: the decoder's work memory, then the ES staging buffer, then
       the YUV frame. 256 between regions is more than any of the three needs
       and makes the layout readable in a log line. */
    work        = ALIGN_UP(di.frameMemSize, 256u);
    es_off      = work;
    yuv_off     = ALIGN_UP(es_off + ALIGN_UP((unsigned)max_au, 256u), 4096u);
    /* Y plane, then the interleaved chroma plane at half height: w*h + w*h/2.
       MEASURED, not assumed -- the buffer was zeroed, decoded into, dumped off
       the card and the last non-zero byte was at exactly w*h*3/2 - 1. */
    V.yuv_bytes = (unsigned)w * (unsigned)h * 3u / 2u;
    V.block_size = ALIGN_UP(yuv_off + ALIGN_UP(V.yuv_bytes, 256u), MB);

    V.block = sceKernelAllocMemBlock("rccars_avc",
                                     SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                     (SceSize)V.block_size, NULL);
    if (V.block <= 0) {
        rlog("[rccars] avc: no %u KB of phycont (0x%08X)\n",
             V.block_size / 1024u, V.block);
        V.block = 0;
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        return -6;
    }
    if (sceKernelGetMemBlockBase(V.block, &V.base) < 0) {
        rlog("[rccars] avc: sceKernelGetMemBlockBase failed\n");
        free_all();
        return -7;
    }
    V.es  = (unsigned char *)V.base + es_off;
    V.yuv = (unsigned char *)V.base + yuv_off;

    V.rgba = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    if (!V.rgba) {
        rlog("[rccars] avc: no %u KB for the RGBA frame\n",
             (unsigned)((size_t)w * h * 4u / 1024u));
        free_all();
        return -8;
    }
    memset(V.rgba, 0, (size_t)w * (size_t)h * 4u);

    V.ctrl.frameBuf.pBuf = V.base;
    V.ctrl.frameBuf.size = work;
    r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &V.ctrl, &q);
    if (r < 0) {
        rlog("[rccars] avc: sceAvcdecCreateDecoder 0x%08X\n", r);
        V.ctrl.frameBuf.pBuf = NULL;    /* nothing to delete */
        free_all();
        return -9;
    }

    /* Uploaded from the zeroed RGBA buffer rather than from NULL, so a frame
       drawn before the first decode is black and not whatever the texture pool
       last held. */
    glGenTextures(1, &V.tex);
    glBindTexture(GL_TEXTURE_2D, V.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, V.rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    V.ok = 1;
    rlog("[rccars] avc: %dx%d NV12, work %u KB, AU %d KB, YUV %u KB, "
         "%u KB block + %u KB RGBA, tex %u\n",
         w, h, work / 1024u, max_au / 1024, V.yuv_bytes / 1024u,
         V.block_size / 1024u, (unsigned)((size_t)w * h * 4u / 1024u), V.tex);
    return 0;
}

void avc_close(void)
{
    if (!V.ok && !V.block && !V.tex)
        return;
    free_all();
    rlog("[rccars] avc: closed after %d frames\n", V.frames);
    memset(&V, 0, sizeof V);
}

/* NV12 -> RGBA8888, BT.601, in the usual integer form.
 *
 * WHY THERE IS A CONVERSION HERE AT ALL, when the decoder can do it. The first
 * build asked for SCE_AVCDEC_PIXELFORMAT_RGBA8888 straight into a physically
 * contiguous block that the GL texture's own GXM descriptor was then pointed at,
 * so the bytes the decoder wrote were the bytes the GPU sampled and nothing was
 * copied at all. Vita3K -- which is this project's whole test loop -- answers
 * that request with `Avcdec rgba output is not implemented' in its OWN log,
 * returns success, reports a picture, and leaves the buffer untouched: 2611
 * frames decoded, sound playing, black screen, and nothing in the app's log to
 * say why. YUV is the path both the emulator and the hardware implement.
 *
 * AND `YUV420_PACKED_RASTER' IS NV12, NOT THREE PLANES. The name and the fact
 * that SceAvcdecFrame carries two plane pointers both suggest Y, U, V back to
 * back, and reading it that way costs you the CHROMA ONLY: the luma is perfect,
 * so the picture is recognisable, correctly lit, correctly timed -- and the
 * colours are wrong in a way that reads as corruption rather than as a wrong
 * offset. What settled it was not reasoning: the buffer was zeroed, decoded
 * into and dumped off the card, the SAME access units were decoded with ffmpeg
 * on the host for a reference, and six candidate layouts were scored against
 * it. Interleaved UV at `yuv + w*h`, stride w, U at the even byte, matched the
 * reference at a mean absolute difference of 0.00 on all three planes; tight
 * planar scored 11.0 and 7.8. See traps.md.
 *
 * Two rows and two columns at a time, so each chroma pair is read once and
 * spent on the four pixels that share it; the clamp is a table because there
 * are twelve of them per 2x2 block. It is ~300k pixels a frame at 25 Hz against
 * a 40 ms budget with nothing else on the screen, which is why a plain C loop
 * is left to the auto-vectoriser rather than hand-written in NEON. If it ever
 * needs to be faster, the RGBA path above is the real answer and it is a
 * pixelType away.
 */
static unsigned char CLAMP[1024];
static int clamp_ready;

static void build_clamp(void)
{
    int i;
    for (i = 0; i < 1024; i++)
        CLAMP[i] = (unsigned char)(i < 256 ? 0 : (i > 511 ? 255 : i - 256));
    clamp_ready = 1;
}
#define CL(v) CLAMP[(unsigned)((v) + 256) & 1023u]

static void nv12_to_rgba(const unsigned char *yuv, int w, int h,
                         unsigned char *out)
{
    const unsigned char *Y = yuv;
    const unsigned char *UV = yuv + (size_t)w * h;
    const int cw = w / 2;
    int y;

    if (!clamp_ready)
        build_clamp();

    for (y = 0; y < h; y += 2) {
        const unsigned char *y0 = Y + (size_t)y * w;
        const unsigned char *y1 = y0 + w;
        /* One interleaved row per two luma rows, w bytes of it: U at the even
           byte and V at the odd one. */
        const unsigned char *uv = UV + (size_t)(y / 2) * w;
        unsigned char *o0 = out + ((size_t)y * w) * 4;
        unsigned char *o1 = o0 + (size_t)w * 4;
        int x;

        for (x = 0; x < cw; x++) {
            const int d = (int)uv[x * 2 + 0] - 128;
            const int e = (int)uv[x * 2 + 1] - 128;
            const int rr =  409 * e + 128;
            const int gg = -100 * d - 208 * e + 128;
            const int bb =  516 * d + 128;
            int k;

            for (k = 0; k < 2; k++) {
                int c = 298 * ((int)y0[x * 2 + k] - 16);
                o0[0] = CL((c + rr) >> 8);
                o0[1] = CL((c + gg) >> 8);
                o0[2] = CL((c + bb) >> 8);
                o0[3] = 255;
                o0 += 4;
                c = 298 * ((int)y1[x * 2 + k] - 16);
                o1[0] = CL((c + rr) >> 8);
                o1[1] = CL((c + gg) >> 8);
                o1[2] = CL((c + bb) >> 8);
                o1[3] = 255;
                o1 += 4;
            }
        }
    }
}

int avc_decode(const void *au, int size)
{
    SceAvcdecAu a;
    SceAvcdecPicture pic;
    SceAvcdecPicture *pics[1];
    SceAvcdecArrayPicture arr;
    int r;

    if (!V.ok || !au || size <= 0)
        return -1;
    if (size > V.max_au) {
        rlog("[rccars] avc: a %d byte access unit over a %d byte buffer\n",
             size, V.max_au);
        return -2;
    }

    /* Into the decoder's own buffer: the ES has to be physically contiguous
       (SCE_AVCDEC_ERROR_NOT_PHY_CONTINUOUS_MEMORY) and the caller's copy came
       off the read-ahead window, which is ordinary heap. */
    memcpy(V.es, au, (size_t)size);

    memset(&a, 0, sizeof a);
    /* The header says so in as many words: "be initialized timestamps with
       0xFFFFFFFF". The intro's clock is the audio and every AU is one frame in
       order, so there is no timestamp here worth carrying. */
    a.pts.upper = a.pts.lower = 0xFFFFFFFFu;
    a.dts.upper = a.dts.lower = 0xFFFFFFFFu;
    a.es.pBuf = V.es;
    a.es.size = (unsigned)size;

    memset(&pic, 0, sizeof pic);
    pic.size = sizeof pic;
    /* PACKED, i.e. Y then U then V back to back in the one buffer at
       pPicture[0]. The struct carries only two plane pointers, and the
       emulator's YUV path reads pPicture[0] alone. */
    pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER;
    pic.frame.framePitch = (unsigned)V.w;
    pic.frame.frameWidth = (unsigned)V.w;
    pic.frame.frameHeight = (unsigned)V.h;
    pic.frame.pPicture[0] = V.yuv;

    pics[0] = &pic;
    memset(&arr, 0, sizeof arr);
    arr.numOfElm = 1;
    arr.pPicture = pics;

    r = sceAvcdecDecode(&V.ctrl, &a, &arr);
    if (r < 0)
        return r;
    if (arr.numOfOutput == 0)
        return 0;

    /* THE FIRST PICTURE IS REPORTED, all of it, plus one sample out of each
       plane. Everything here is something the decoder decided rather than
       something this file asked for -- and the black screen this path replaced
       was invisible for exactly as long as nothing printed what came back. */
    if (V.frames == 0) {
        const unsigned char *Yp = V.yuv;
        const unsigned char *UV = V.yuv + (size_t)V.w * V.h;
        rlog("[rccars] avc: first picture: type %u, %ux%u pitch %u, crop "
             "%u/%u/%u/%u;  Y[0] %u  Y mid %u  UV mid %u,%u\n",
             pic.frame.pixelType, pic.frame.frameWidth, pic.frame.frameHeight,
             pic.frame.framePitch, pic.frame.frameCropLeftOffset,
             pic.frame.frameCropRightOffset, pic.frame.frameCropTopOffset,
             pic.frame.frameCropBottomOffset,
             Yp[0], Yp[(size_t)(V.h / 2) * V.w + V.w / 2],
             UV[(size_t)(V.h / 4) * V.w + (V.w / 2 & ~1)],
             UV[(size_t)(V.h / 4) * V.w + (V.w / 2 & ~1) + 1]);
        if (pic.frame.frameWidth != (unsigned)V.w
            || pic.frame.frameHeight != (unsigned)V.h)
            rlog("[rccars] avc: the stream is %ux%u, not the .vid's %dx%d -- "
                 "the picture will be wrong\n", pic.frame.frameWidth,
                 pic.frame.frameHeight, V.w, V.h);
    }

    nv12_to_rgba(V.yuv, V.w, V.h, V.rgba);
    glBindTexture(GL_TEXTURE_2D, V.tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, V.w, V.h, GL_RGBA,
                    GL_UNSIGNED_BYTE, V.rgba);

    /* DOES THE PICTURE HAVE PIXELS IN IT. One frame, once, from a frame that is
       known not to be black -- 100 frames in is four seconds, which on the
       shipped movie is the middle of the 1C logo's green countdown, so a
       plausible answer here is a dark desaturated green and 0,0,0,0 is the
       failure this whole file was rewritten to get past. */
    if (V.frames == 100) {
        const unsigned char *c = V.rgba
                               + ((size_t)(V.h / 2) * V.w + V.w / 2) * 4;
        rlog("[rccars] avc: frame 100 centre RGBA %u,%u,%u,%u\n",
             c[0], c[1], c[2], c[3]);
    }

    V.frames++;
    return 1;
}

unsigned int avc_tex(void) { return V.ok ? (unsigned int)V.tex : 0u; }
int avc_frames(void) { return V.frames; }

#else /* !__vita__ */

/* The host build has no video decoder. intro.c is written so that this is a
 * supported state rather than a broken one: with no decoder there are no
 * pictures, the part clock still runs off the audio, and the harness checks the
 * sequencing -- which is the half that was transcribed from the engine and the
 * half that can be wrong. */

int avc_open(int w, int h, int max_au)
{
    (void)w; (void)h; (void)max_au;
    return -1;
}

void avc_close(void) { }

int avc_decode(const void *au, int size)
{
    (void)au; (void)size;
    return -1;
}

unsigned int avc_tex(void) { return 0u; }
int avc_frames(void) { return 0; }

#endif
