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
    unsigned char *yuv;         /* the decoder's output: NV12 -- Y, then UV --
                                   or, once the probe below has switched, the
                                   decoder's own RGBA8888 */
    unsigned yuv_bytes;         /* what NV12 uses of it */
    unsigned pic_bytes;         /* what the LARGER of the two layouts uses */

    unsigned char *rgba;        /* the conversion's output, ordinary heap */

    /* WHICH OUTPUT FORMAT THIS MACHINE ACTUALLY IMPLEMENTS, and it is measured
       rather than assumed -- see the probe in avc_decode. */
    unsigned fmt;
    int fmt_try;                /* index into FMT_ORDER */
    int probe;                  /* 0..1 still trying, 2 settled */

    /* WHERE A PICTURE'S TIME GOES. The three are separately fixable and were
       indistinguishable from outside this file. */
    double t_dec, t_cvt, t_up;

    GLuint tex;
    int frames;                 /* pictures that came back */
    unsigned fed;               /* access units handed over */
    unsigned no_out;            /* ...that produced no picture */
} V;

/* THE ORDER THE PROBE TRIES THEM IN, AND RGBA IS FIRST ON PURPOSE.
 *
 * It is the format with no conversion at all: the decoder writes the texture's
 * own bytes and the CPU never looks at a pixel. YUV needs `nv12_to_rgba', which
 * reads all 353 KB of the picture back OUT OF UNCACHED PHYSICALLY CONTIGUOUS
 * MEMORY, three streams at a time, a byte at a time -- there is no cache line
 * to amortise the next 63 bytes against, so every read is a memory transaction.
 * The estimate in nv12_to_rgba ("~300k pixels a frame against a 40 ms budget")
 * is a CACHED-memory estimate and this buffer is not cached; avc.h said in as
 * many words that the CPU never reads a decoded frame, which was true of the
 * design the NC mapping was chosen for and false from the moment the conversion
 * was added under it.
 *
 * Vita3K does not implement RGBA -- it reports a picture and writes nothing --
 * so on the emulator this costs exactly one frame of the first logo before the
 * probe falls through to YUV, which is the path that works there. On hardware
 * it removes the whole per-pixel loop. */
static const unsigned FMT_ORDER[2] = {
    SCE_AVCDEC_PIXELFORMAT_RGBA8888,
    SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER,
};
#define FMT_N ((int)(sizeof FMT_ORDER / sizeof FMT_ORDER[0]))

static const char *fmt_name(unsigned f)
{
    return f == SCE_AVCDEC_PIXELFORMAT_RGBA8888 ? "RGBA8888"
         : f == SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER ? "YUV420_PACKED"
         : "?";
}

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
       the card and the last non-zero byte was at exactly w*h*3/2 - 1.

       SIZED FOR THE LARGER OF THE TWO LAYOUTS, though, because the probe below
       may switch this same region to the decoder's own RGBA8888 -- w*h*4, which
       is 2.7x the NV12 it starts as. The block is rounded up to a whole
       megabyte anyway, so on the shipped 640x368 movie this costs nothing. */
    V.yuv_bytes = (unsigned)w * (unsigned)h * 3u / 2u;
    V.pic_bytes = (unsigned)w * (unsigned)h * 4u;
    if (V.yuv_bytes > V.pic_bytes)
        V.pic_bytes = V.yuv_bytes;
    V.block_size = ALIGN_UP(yuv_off + ALIGN_UP(V.pic_bytes, 256u), MB);

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
    /* ZEROED, and that is not tidiness: the probe in avc_decode decides which
       output format this machine implements by asking whether the decoder wrote
       anything at all, and a block of whatever the last tenant left would answer
       yes to both. A fresh PHYCONT block is not documented to arrive clear. */
    memset(V.yuv, 0, V.pic_bytes);
    V.fmt_try = 0;
    V.fmt = FMT_ORDER[0];
    V.probe = 0;

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
    rlog("[rccars] avc: %dx%d %s, work %u KB, AU %d KB, picture %u KB, "
         "%u KB block + %u KB RGBA, tex %u\n",
         w, h, fmt_name(V.fmt), work / 1024u, max_au / 1024,
         V.pic_bytes / 1024u, V.block_size / 1024u,
         (unsigned)((size_t)w * h * 4u / 1024u), V.tex);
    return 0;
}

void avc_close(void)
{
    if (!V.ok && !V.block && !V.tex)
        return;
    free_all();
    rlog("[rccars] avc: closed after %d picture(s) from %u access unit(s), "
         "%u of them silent, on %s\n", V.frames, V.fed, V.no_out,
         fmt_name(V.fmt));
    if (V.fed)
        rlog("[rccars] avc: per access unit -- decode %.1f ms, convert %.1f ms,"
             " upload %.1f ms (%.1f ms all in, %.1f fps of decoder)\n",
             V.t_dec / V.fed, V.t_cvt / V.fed, V.t_up / V.fed,
             (V.t_dec + V.t_cvt + V.t_up) / V.fed,
             (V.t_dec + V.t_cvt + V.t_up) > 0.0
                 ? 1000.0 * V.fed / (V.t_dec + V.t_cvt + V.t_up) : 0.0);
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

int avc_decode(const void *au, int size, int present)
{
    SceAvcdecAu a;
    double t0;
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
    pic.frame.pixelType = V.fmt;
    pic.frame.framePitch = (unsigned)V.w;
    pic.frame.frameWidth = (unsigned)V.w;
    pic.frame.frameHeight = (unsigned)V.h;
    pic.frame.pPicture[0] = V.yuv;
    if (V.fmt == SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER) {
        /* PACKED, i.e. Y then the interleaved chroma back to back in the one
           buffer at pPicture[0] -- which is what the emulator's YUV path reads,
           and it reads pPicture[0] alone. pPicture[1] is handed the SAME
           layout's second plane rather than left NULL, because it costs
           nothing and a decoder that wants the two planes named separately
           gets the answer it would have computed itself. */
        pic.frame.pPicture[1] = V.yuv + (size_t)V.w * V.h;
    } else {
        /* AND THE ALPHA IS NOT OPTIONAL on the RGBA path: the decoder writes
           exactly the byte it is given into every texel's fourth channel, so a
           zeroed option block is a fully transparent picture. */
        pic.frame.opt.rgba.alpha = 0xFF;
    }

    pics[0] = &pic;
    memset(&arr, 0, sizeof arr);
    arr.numOfElm = 1;
    arr.pPicture = pics;

    t0 = rlog_now_ms();
    r = sceAvcdecDecode(&V.ctrl, &a, &arr);
    V.t_dec += rlog_now_ms() - t0;
    V.fed++;
    if (r < 0) {
        rlog("[rccars] avc: sceAvcdecDecode 0x%08X on AU %u (%d B, %s)\n",
             r, V.fed, size, fmt_name(V.fmt));
        return r;
    }
    if (arr.numOfOutput == 0) {
        /* SAID ONCE, AND ONLY ONCE. A decoder that takes every access unit and
           reports no picture is a black screen with no error in it, which is
           the exact failure this file has already been rewritten for -- and it
           was invisible for as long as nothing printed what came back. */
        if (++V.no_out == 30u)
            rlog("[rccars] avc: 30 access units in, no picture out -- the "
                 "stream decodes into nothing\n");
        return 0;
    }

    /* THE FIRST PICTURE IS REPORTED, all of it, plus one sample out of each
       plane. Everything here is something the decoder decided rather than
       something this file asked for -- and the black screen this path replaced
       was invisible for exactly as long as nothing printed what came back. */
    if (V.frames == 0) {
        const unsigned char *Yp = V.yuv;
        const unsigned char *UV = V.yuv + (size_t)V.w * V.h;
        rlog("[rccars] avc: first picture (%s): type %u, %ux%u pitch %u, crop "
             "%u/%u/%u/%u;  [0] %u  mid %u  chroma %u,%u\n",
             fmt_name(V.fmt),
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

    /* ------------------------------------------------- WHICH FORMAT IS REAL
     *
     * `numOfOutput' 1 means the decoder DECODED a picture. It does not mean it
     * wrote one where it was asked to: Vita3K answers a request for RGBA8888
     * with `Avcdec rgba output is not implemented' in its own log, returns
     * success, reports a picture and leaves the buffer untouched -- 2611 frames
     * decoded, sound playing, black screen, and nothing in this app's log to
     * say why. That is a whole class of failure and the emulator is not the only
     * place it can happen, so the format is MEASURED here instead of assumed:
     * the region was zeroed at avc_open, and a decoder that wrote a picture into
     * it cannot have left every byte at zero.
     *
     * A GENUINELY BLACK FRAME DOES NOT FALSE-TRIGGER THIS, which is the whole
     * reason the test is "any non-zero byte" over the WHOLE picture rather than
     * "the luma is dark": black in NV12 is Y=16 (or 0) with the chroma at 128,
     * and black in RGBA8888 carries the alpha byte this file sets to 0xFF. Only
     * an untouched buffer is all zero.
     *
     * The fallback is the other format, once, and then the answer stands. */
    if (V.probe < 2) {
        const unsigned used = (V.fmt == SCE_AVCDEC_PIXELFORMAT_RGBA8888)
                            ? (unsigned)V.w * (unsigned)V.h * 4u : V.yuv_bytes;
        unsigned i;
        int touched = 0;
        for (i = 0; i < used; i++)
            if (V.yuv[i]) { touched = 1; break; }
        if (touched) {
            V.probe = 2;
            rlog("[rccars] avc: %s is what this decoder writes%s\n",
                 fmt_name(V.fmt),
                 V.fmt == SCE_AVCDEC_PIXELFORMAT_RGBA8888
                     ? " -- no conversion needed"
                     : " -- every picture costs a BT.601 pass over uncached "
                       "memory");
        } else if (V.fmt_try + 1 < FMT_N) {
            V.fmt_try++;
            rlog("[rccars] avc: %s reported a picture and wrote NOTHING -- "
                 "this decoder does not implement it; trying %s\n",
                 fmt_name(V.fmt), fmt_name(FMT_ORDER[V.fmt_try]));
            V.fmt = FMT_ORDER[V.fmt_try];
            memset(V.yuv, 0, V.pic_bytes);
            return 0;               /* one frame, on the first IDR, is nothing */
        } else {
            rlog("[rccars] avc: NO output format this build knows is written "
                 "by this decoder -- the movie will play silent-black\n");
            V.probe = 2;
            return 0;
        }
    }

    /* ONLY THE PICTURE THAT WILL BE SEEN IS PUT ON THE TEXTURE.
     *
     * A tick may feed up to FEED_PER_TICK access units and it draws once, so
     * with `present' 0 for all but the last one the decoder still runs (it must
     * -- the frames are references for the ones after them) and the conversion
     * and the upload do not. On the hardware log the intro was falling 27 frames
     * behind every 50 and resyncing forwards over and over, which is the shape
     * of catching up costing MORE than keeping up: four conversions per drawn
     * frame instead of one. */
    if (!present) {
        V.frames++;
        return 1;
    }

    t0 = rlog_now_ms();
    if (V.fmt == SCE_AVCDEC_PIXELFORMAT_RGBA8888) {
        /* Nothing to convert: the decoder wrote the texture's own format. */
        glBindTexture(GL_TEXTURE_2D, V.tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, V.w, V.h, GL_RGBA,
                        GL_UNSIGNED_BYTE, V.yuv);
        V.t_up += rlog_now_ms() - t0;
    } else {
        nv12_to_rgba(V.yuv, V.w, V.h, V.rgba);
        V.t_cvt += rlog_now_ms() - t0;
        t0 = rlog_now_ms();
        glBindTexture(GL_TEXTURE_2D, V.tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, V.w, V.h, GL_RGBA,
                        GL_UNSIGNED_BYTE, V.rgba);
        V.t_up += rlog_now_ms() - t0;
    }

    /* DOES THE PICTURE HAVE PIXELS IN IT. One frame, once, from a frame that is
       known not to be black -- 100 frames in is four seconds, which on the
       shipped movie is the middle of the 1C logo's green countdown, so a
       plausible answer here is a dark desaturated green and 0,0,0,0 is the
       failure this whole file was rewritten to get past. */
    if (V.frames == 100) {
        const unsigned char *px = (V.fmt == SCE_AVCDEC_PIXELFORMAT_RGBA8888)
                                ? V.yuv : V.rgba;
        const unsigned char *c = px
                               + ((size_t)(V.h / 2) * V.w + V.w / 2) * 4;
        rlog("[rccars] avc: frame 100 centre RGBA %u,%u,%u,%u (%s)\n",
             c[0], c[1], c[2], c[3], fmt_name(V.fmt));
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

int avc_decode(const void *au, int size, int present)
{
    (void)au; (void)size; (void)present;
    return -1;
}

unsigned int avc_tex(void) { return 0u; }
int avc_frames(void) { return 0; }

#endif
