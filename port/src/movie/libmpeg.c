/*
 * Sony's libmpeg (and libipu) for the port: the sceMpeg* calls the movie
 * player makes, on top of the software decoder (mpeg2.c), instead of the
 * IPU. port/tools/fixups/movie.py leaves the library's decoder out of the
 * build (src/main/4723B0.c-478FF7.c and 479208.c-4795E0.c); the demuxer
 * (sceMpegDemuxPssRing 471860 and the rest of 4715D8-471E28, with the
 * _sceMpegBs* bit reader 478FF8-4791D8) is plain C and stays. It reads
 * the stream callbacks from sceMpeg.sys as sceMpegCreate leaves them, so
 * the start of SceMpegSys (include/sdk/libmpeg.h) is kept: the callbacks,
 * the stream callback table in the work area.
 *
 * The bitstream: libmpeg read it through the IPU's input FIFO, which the
 * toIPU DMA channel (4) filled from the game's video ring (viBuf, 466458):
 * a source chain of 2 KB "ref" tags, a "refe" after the last full sector,
 * a "next" back to the start. viBuf's code keeps writing that chain and
 * D4_MADR/TADR/QWC/CHCR into the mapped registers, reads D4_MADR to see
 * how far the IPU has got (getFIFOindex 466340, viBufAddDMA 466848), and
 * IPU_BP/IPU_CTRL's FIFO counts to find the time stamp of what is being
 * decoded (viBufGetTs 467368). So the decoder reads its bytes by walking
 * that chain the way the DMAC does, writes the channel's registers back
 * as it goes (MADR/QWC/TADR, CHCR's TAG field, STR cleared at the end of
 * a refe), and reports what it has read ahead as the FIFO's contents. When
 * the chain runs dry it calls the "no data" callback, as libmpeg did: the
 * game's mpegNodata yields to the reading thread and queues new sectors
 * with viBufAddDMA.
 *
 * The output: sceMpegGetPicture leaves RGBA32 macroblocks as the IPU's
 * CSC did, column by column, which is what the movie's prebuilt image
 * transfer chains (MovSetImageTag 468278) expect.
 *
 * Callbacks the original made that this doesn't: stop/restart DMA (2, 3:
 * libmpeg stopped the bitstream DMA to feed the CSC through the same
 * channel; there's no CSC on the channel here). Background (4) is called
 * once per picture: the game's mpegBackground polls the IOP.
 *
 * Game-named functions, so no <windows.h>.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "sdk/libmpeg.h"
#include "grx_hwmem.h"
#include "grx_movie.h"
#include "grx_platform.h"

/* MovPlay's flag (include/main/n11.h): -1 while a movie plays */
extern s8 sndMovieStreaming;

int grx_movie_playing(void) {
    return sndMovieStreaming == -1;
}

/* ------------------------------------------------------------------------ */
/* The registers                                                            */

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define D4_CHCR REG(0x1000B400u)
#define D4_MADR REG(0x1000B410u)
#define D4_QWC REG(0x1000B420u)
#define D4_TADR REG(0x1000B430u)
#define D_STAT REG(0x1000E010u)
#define IPU_CTRL REG(0x10002010u)
#define IPU_BP REG(0x10002020u)

#define CHCR_STR 0x100u

/* An address in a DMA register or tag: the port's own address of the
   memory (below 0x08000000, port/docs/hardware.md (a) 3) */
static uint8_t *dma_ptr(uint32_t addr) {
    return (uint8_t *)(uintptr_t)(addr & 0x0FFFFFF0u);
}

/* The chain ended (a refe or end tag done): STR off, the channel's D_STAT
   bit, as the DMAC leaves them. Nothing waits on channel 4's interrupt. */
static void d4_finish(void) {
    D4_CHCR &= ~CHCR_STR;
    D_STAT |= 1u << 4;
}

/* Up to max bytes (a multiple of 16) of the toIPU source chain */

/* GRX_MOVIE_TRACE=1: the movie commands and the decoder's steps in the log */
static int movie_trace(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GRX_MOVIE_TRACE");
        on = e != NULL && *e != 0 && *e != '0';
    }
    return on;
}

static int d4_fetch(uint8_t *dst, int max) {
    int got = 0, tags = 0;

    while (got + 16 <= max) {
        uint32_t chcr = D4_CHCR, qwc = D4_QWC;

        if (!(chcr & CHCR_STR)) {
            break;
        }
        if (qwc != 0) {
            uint32_t n = (uint32_t)(max - got) / 16;
            if (n > qwc) {
                n = qwc;
            }
            memcpy(dst + got, dma_ptr(D4_MADR), n * 16);
            D4_MADR += n * 16;
            D4_QWC = qwc - n;
            got += (int)n * 16;
            if (qwc == n) {
                uint32_t id = (chcr >> 28) & 7;
                if (id == 0 || id == 7) {
                    d4_finish();
                }
            }
            continue;
        }
        /* the next tag */
        {
            uint32_t tadr = D4_TADR;
            const uint32_t *tag = (const uint32_t *)dma_ptr(tadr);
            uint32_t lo = tag[0], hi = tag[1], id = (lo >> 28) & 7, n = lo & 0xFFFF;

            if (++tags > 4096) {
                grx_log("movie: the toIPU chain loops without data (TADR 0x%08X)", tadr);
                d4_finish();
                break;
            }
            D4_CHCR = (chcr & 0xFFFFu) | (lo & 0xFFFF0000u);
            switch (id) {
            case 0: /* refe */
            case 3: /* ref */
            case 4: /* refs */
                D4_MADR = hi;
                D4_TADR = tadr + 16;
                break;
            case 1: /* cnt */
                D4_MADR = tadr + 16;
                D4_TADR = tadr + 16 + n * 16;
                break;
            case 2: /* next */
                D4_MADR = tadr + 16;
                D4_TADR = hi;
                break;
            case 7: /* end */
                D4_MADR = tadr + 16;
                break;
            default: /* call, ret: viBuf doesn't use them */
                grx_log("movie: toIPU tag id %u not supported", id);
                d4_finish();
                return got;
            }
            D4_QWC = n;
            if (n == 0 && (id == 0 || id == 7)) {
                d4_finish();
            }
        }
    }
    return got;
}

/* ------------------------------------------------------------------------ */
/* The decoder behind a sceMpeg                                             */

typedef struct {
    sceMpeg *mp;
    GrxMpeg2 *dec;
} Priv;

/* The handle of the last create or picture call, for _sceMpegError */
static sceMpeg *cur_mp;

/* sceMpeg.sys->frameBuf[0] holds it: libmpeg's frame buffers aren't used */
static Priv *priv(sceMpeg *mp) {
    return mp != NULL && mp->sys != NULL ? (Priv *)mp->sys->frameBuf[0] : NULL;
}

typedef struct {
    s32 type;
    s32 pad;
    s64 pts;
    s64 dts;
} CbTimeStamp;

typedef struct {
    s32 type;
    const char *message;
} CbError;

static s32 callback(sceMpeg *mp, int type, void *data) {
    SceMpegCallback *cb = &mp->sys->callbacks[type];
    if (cb->func == NULL) {
        return -1;
    }
    *(s32 *)data = type;
    return cb->func(mp, data, cb->data);
}

/* The decoder's read function: the chain, else the "no data" callback */
static int read_bits_(void *ctx, uint8_t *dst, int max);

static int read_bits(void *ctx, uint8_t *dst, int max) {
    int n = read_bits_(ctx, dst, max);
    if (movie_trace()) {
        grx_log("movie: read_bits(%d) = %d", max, n);
    }
    return n;
}

static int read_bits_(void *ctx, uint8_t *dst, int max) {
    Priv *p = ctx;
    int n = d4_fetch(dst, max & ~15);
    s32 cbd[2];

    if (n > 0) {
        return n;
    }
    if (p->mp->sys->callbacks[1].func == NULL) {
        return -1;     /* nothing will ever come */
    }
    return callback(p->mp, 1, cbd) == 0 ? -1 : 0;
}

/* A picture header has been read: its time stamps, from the time stamp
   callback, which looks them up by where the DMA is minus what the FIFO
   holds (viBufGetTs) */
static void picture_hook(void *ctx, int type) {
    Priv *p = ctx;
    CbTimeStamp ts;
    int qw = grx_mpeg2_buffered(p->dec) / 16;

    (void)type;
    if (qw > 15) {
        qw = 15;
    }
    IPU_BP = (uint32_t)qw << 8;   /* IFC; FP and BP 0 */
    IPU_CTRL = (uint32_t)qw;
    ts.pts = -1;
    ts.dts = -1;
    if (callback(p->mp, 5, &ts) < 0) {
        ts.pts = ts.dts = -1;
    }
    grx_mpeg2_set_pts(p->dec, ts.pts, ts.dts);
    IPU_BP = 0;
    IPU_CTRL = 0;
}

/* ------------------------------------------------------------------------ */
/* The calls                                                                */

void _sceMpegError(s32 message) {
    sceMpeg *mp = cur_mp;
    CbError e;

    if (mp != NULL && mp->sys != NULL) {
        e.message = (const char *)(uintptr_t)(uint32_t)message;
        callback(mp, 0, &e);
    }
}

void sceIpuInit(void) {
}

u32 sceIpuSync(s32 mode, s32 timeout) {
    (void)mode;
    (void)timeout;
    return 0;
}

void sceIpuStopDMA(sceIpuDmaEnv *env) {
    (void)env;
}

void sceIpuRestartDMA(sceIpuDmaEnv *env) {
    (void)env;
}

s32 sceMpegInit(void) {
    IPU_CTRL = 0;
    IPU_BP = 0;
    return 0;
}

s32 sceMpegCreate(sceMpeg *mp, u32 work, u32 size) {
    if (movie_trace()) {
        grx_log("movie: sceMpegCreate(work 0x%08X, size 0x%X)", work, size);
    }
    SceMpegSys *s = (SceMpegSys *)(uintptr_t)((work + 3) & ~3u);
    Priv *p;

    size -= (u32)(uintptr_t)s - work;
    if (size < sizeof(SceMpegSys) + 0x10 + 0x600) {
        grx_log("sceMpegCreate: work area too small (0x%X)", size);
        return 0;
    }
    memset(s, 0, sizeof(*s));
    memset(mp, 0, sizeof(*mp));
    mp->sys = s;
    mp->pts = mp->dts = -1;
    mp->pts2nd = mp->dts2nd = -1;
    /* the stream callbacks, where the demuxer (sceMpegAddStrCallback,
       sceMpegDemuxPssRing) looks for them: after the heap header */
    s->strCallbacks = (SceMpegStrCallback *)((uint8_t *)s + sizeof(SceMpegSys) + 0x10);
    s->strCallbackCount = 0;
    s->nextPts = -1;
    s->lastPts = -1;
    s->decodeModeI = s->decodeModeP = s->decodeModeB = -1;
    s->outputCsc = 1;
    /* Its address goes into a 32-bit field of the game's struct (frameBuf[0]),
       so it must be below 2 GB: from the low pool, zero-filled */
    p = grx_hwmem_alloc_low(sizeof(*p));
    if (p == NULL) {
        return 0;
    }
    p->mp = mp;
    p->dec = grx_mpeg2_create(read_bits, p);
    if (p->dec == NULL) {
        grx_hwmem_free_low(p);
        return 0;
    }
    grx_mpeg2_set_picture_hook(p->dec, picture_hook, p);
    s->frameBuf[0] = p;
    cur_mp = mp;
    return 1;
}

s32 sceMpegDelete(sceMpeg *mp) {
    Priv *p = priv(mp);

    if (p != NULL) {
        grx_mpeg2_destroy(p->dec);
        grx_hwmem_free_low(p);
        mp->sys->frameBuf[0] = NULL;
    }
    if (cur_mp == mp) {
        cur_mp = NULL;
    }
    return 1;
}

void *sceMpegAddCallback(sceMpeg *mp, s32 type, void *func, void *data) {
    SceMpegCallback *cb;
    void *old;

    if (type < 0 || type > 6) {
        return NULL;
    }
    cb = &mp->sys->callbacks[type];
    old = (void *)cb->func;
    cb->func = (s32(*)(void *, void *, void *))func;
    cb->data = data;
    return old;
}

/* One picture, in display order, as RGBA32 macroblocks at rgb32 (at most
   mbcount of them). 0, or -1 when the stream can't be decoded. At the
   end of the sequence it gives the last picture and sceMpegIsEnd turns
   1; called after that, it leaves the buffer alone. */
s32 sceMpegGetPicture(sceMpeg *mp, s32 rgb32, s32 mbcount) {
    if (movie_trace()) {
        grx_log("movie: sceMpegGetPicture(0x%08X, %d)", rgb32, mbcount);
    }
    Priv *p = priv(mp);
    GrxMpeg2Frame f;
    int r;
    s32 cbd[2];

    if (p == NULL) {
        return -1;
    }
    cur_mp = mp;
    r = grx_mpeg2_decode(p->dec, &f);
    if (r == 1) {
        grx_mpeg2_csc_mb(&f, (uint8_t *)(uintptr_t)((uint32_t)rgb32 & 0x0FFFFFFFu), mbcount);
        mp->width = f.display_width;
        mp->height = f.display_height;
        mp->frameCount++;
        mp->pts = f.pts;
        mp->dts = f.dts;
        mp->sys->displayWidth = f.display_width;
        mp->sys->displayHeight = f.display_height;
        mp->sys->lastPts = (s32)f.pts;
    }
    if (mp->sys->callbacks[4].func != NULL) {
        callback(mp, 4, cbd);
    }
    if (r >= 0) {
        mp->sys->isEnd = grx_mpeg2_ended(p->dec);
        return 0;
    }
    if (r == -2) {
        _sceMpegError((s32)(uintptr_t)"sceMpegGetPicture: a stream the decoder doesn't support");
    }
    return -1;
}

/* The RAW8 forms (Y, Cb, Cr per macroblock: the IPU's output before CSC);
   the game doesn't use them */
static s32 get_raw8(sceMpeg *mp, uint8_t *dst, s32 mbcount) {
    Priv *p = priv(mp);
    GrxMpeg2Frame f;
    int r, x, y, i, n = 0;

    if (p == NULL) {
        return -1;
    }
    r = grx_mpeg2_decode(p->dec, &f);
    if (r != 1) {
        mp->sys->isEnd = grx_mpeg2_ended(p->dec);
        return r == 0 ? 0 : -1;
    }
    for (x = 0; x < f.width / 16; x++) {
        for (y = 0; y < f.height / 16 && n < mbcount; y++, n++) {
            uint8_t *o = dst + (size_t)n * 384;
            for (i = 0; i < 16; i++) {
                memcpy(o + i * 16, f.y + (y * 16 + i) * f.y_stride + x * 16, 16);
            }
            for (i = 0; i < 8; i++) {
                memcpy(o + 256 + i * 8, f.cb + (y * 8 + i) * f.c_stride + x * 8, 8);
                memcpy(o + 320 + i * 8, f.cr + (y * 8 + i) * f.c_stride + x * 8, 8);
            }
        }
    }
    mp->width = f.display_width;
    mp->height = f.display_height;
    mp->frameCount++;
    mp->pts = f.pts;
    mp->dts = f.dts;
    mp->sys->isEnd = grx_mpeg2_ended(p->dec);
    return 0;
}

s32 sceMpegGetPictureRAW8(sceMpeg *mp, s32 raw8, s32 mbcount) {
    return get_raw8(mp, (uint8_t *)(uintptr_t)((uint32_t)raw8 & 0x0FFFFFFFu), mbcount);
}

s32 sceMpegGetPictureRAW8xy(sceMpeg *mp, s32 raw8, s32 mbx, s32 mby) {
    return get_raw8(mp, (uint8_t *)(uintptr_t)((uint32_t)raw8 & 0x0FFFFFFFu), mbx * mby);
}

void sceMpegSetDecodeMode(sceMpeg *mp, s32 ni, s32 np, s32 nb) {
    mp->sys->decodeModeI = ni;
    mp->sys->decodeModeP = np;
    mp->sys->decodeModeB = nb;
}

void sceMpegGetDecodeMode(sceMpeg *mp, s32 *ni, s32 *np, s32 *nb) {
    *ni = mp->sys->decodeModeI;
    *np = mp->sys->decodeModeP;
    *nb = mp->sys->decodeModeB;
}

s32 sceMpegIsEnd(sceMpeg *mp) {
    Priv *p = priv(mp);
    return p != NULL && grx_mpeg2_ended(p->dec);
}

s32 sceMpegIsRefBuffEmpty(sceMpeg *mp) {
    Priv *p = priv(mp);
    return p == NULL || !grx_mpeg2_has_pending(p->dec);
}

void sceMpegReset(sceMpeg *mp) {
    Priv *p = priv(mp);
    if (p != NULL) {
        grx_mpeg2_reset(p->dec, GRX_MPEG2_RESET_STREAM);
        mp->sys->isEnd = 0;
    }
}

s32 sceMpegClearRefBuff(sceMpeg *mp) {
    Priv *p = priv(mp);
    if (p != NULL) {
        grx_mpeg2_reset(p->dec, GRX_MPEG2_RESET_REFS);
    }
    return 1;
}

/* The input buffer form (the game feeds the chain instead) */
void sceMpegAddBs(sceMpeg *mp, s32 addr, s32 size) {
    (void)mp;
    grx_log("sceMpegAddBs(0x%08X, 0x%X): not supported (the port reads the toIPU chain)", addr, size);
}

void sceMpegSetDefaultPtsGap(sceMpeg *mp, s64 gap) {
    mp->sys->ptsGapSet = 1;
    mp->sys->ptsGap = gap;
}

void sceMpegResetDefaultPtsGap(sceMpeg *mp) {
    mp->sys->ptsGapSet = 0;
}

s32 sceMpegDispWidth(sceMpeg *mp) {
    return mp->sys->displayWidth;
}

s32 sceMpegDispHeight(sceMpeg *mp) {
    return mp->sys->displayHeight;
}

s32 sceMpegDispCenterOffX(sceMpeg *mp) {
    (void)mp;
    return 0;
}

s32 sceMpegDispCenterOffY(sceMpeg *mp) {
    (void)mp;
    return 0;
}

/* Save states (iop_movie.c's part): a decoder exists while a movie plays;
   it is this run's, so no state then */
int grx_mpeg_active(void) {
    return cur_mp != NULL;
}
