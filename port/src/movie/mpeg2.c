/*
 * A software MPEG-2 video decoder, for the movies (grx_movie.h). It does
 * what Sony's libmpeg did with the IPU's help: the headers (the C part of
 * libmpeg, MSSG mpeg2decode), the macroblock layer, VLC decoding, inverse
 * quantisation and IDCT (the IPU's BDEC and VDEC commands), motion
 * compensation (libmpeg's C and scratchpad DMA) and the colour conversion
 * (the IPU's CSC command).
 *
 * Written from ISO/IEC 13818-2. The IDCT is the integer Chen-Wang one
 * that mpeg2decode uses (IEEE 1180 accurate), under MSSG's notice
 * (THIRD_PARTY.md).
 *
 * Plain C, no OS calls, no game names.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "grx_movie.h"

/* ------------------------------------------------------------------------ */
/* VLC tables (Annex B), built at first use from the code strings           */

typedef struct {
    int16_t val;
    uint8_t len;   /* 0: no such code */
} Vlc;

typedef struct {
    const char *code;
    int val;
} VlcDef;

/* Fills tab[1 << bits] from codes of at most `bits` bits */
static void vlc_build(Vlc *tab, int bits, const VlcDef *d, int n) {
    int i;

    memset(tab, 0, sizeof(Vlc) << bits);
    for (i = 0; i < n; i++) {
        uint32_t c = 0, k, lo, cnt;
        int len = 0;
        const char *p;
        for (p = d[i].code; *p; p++) {
            if (*p == '0' || *p == '1') {
                c = (c << 1) | (uint32_t)(*p - '0');
                len++;
            }
        }
        lo = c << (bits - len);
        cnt = 1u << (bits - len);
        for (k = 0; k < cnt; k++) {
            tab[lo + k].val = (int16_t)d[i].val;
            tab[lo + k].len = (uint8_t)len;
        }
    }
}

/* B.1 macroblock_address_increment (the escape and stuffing are handled
   before the table) */
static const VlcDef mba_def[] = {
    {"1", 1}, {"011", 2}, {"010", 3}, {"0011", 4}, {"0010", 5}, {"00011", 6}, {"00010", 7},
    {"0000111", 8}, {"0000110", 9}, {"00001011", 10}, {"00001010", 11}, {"00001001", 12},
    {"00001000", 13}, {"00000111", 14}, {"00000110", 15}, {"0000010111", 16},
    {"0000010110", 17}, {"0000010101", 18}, {"0000010100", 19}, {"0000010011", 20},
    {"0000010010", 21}, {"00000100011", 22}, {"00000100010", 23}, {"00000100001", 24},
    {"00000100000", 25}, {"00000011111", 26}, {"00000011110", 27}, {"00000011101", 28},
    {"00000011100", 29}, {"00000011011", 30}, {"00000011010", 31}, {"00000011001", 32},
    {"00000011000", 33},
};

/* macroblock_type flags */
#define MB_QUANT 1
#define MB_FWD 2
#define MB_BWD 4
#define MB_PATTERN 8
#define MB_INTRA 16

static const VlcDef mbtype_i_def[] = {{"1", MB_INTRA}, {"01", MB_INTRA | MB_QUANT}};
static const VlcDef mbtype_p_def[] = {
    {"1", MB_FWD | MB_PATTERN}, {"01", MB_PATTERN}, {"001", MB_FWD}, {"00011", MB_INTRA},
    {"00010", MB_QUANT | MB_FWD | MB_PATTERN}, {"00001", MB_QUANT | MB_PATTERN},
    {"000001", MB_QUANT | MB_INTRA},
};
static const VlcDef mbtype_b_def[] = {
    {"10", MB_FWD | MB_BWD}, {"11", MB_FWD | MB_BWD | MB_PATTERN}, {"010", MB_BWD},
    {"011", MB_BWD | MB_PATTERN}, {"0010", MB_FWD}, {"0011", MB_FWD | MB_PATTERN},
    {"00011", MB_INTRA}, {"00010", MB_QUANT | MB_FWD | MB_BWD | MB_PATTERN},
    {"000011", MB_QUANT | MB_FWD | MB_PATTERN}, {"000010", MB_QUANT | MB_BWD | MB_PATTERN},
    {"000001", MB_QUANT | MB_INTRA},
};

/* B.9 coded_block_pattern */
static const VlcDef cbp_def[] = {
    {"111", 60}, {"1101", 4}, {"1100", 8}, {"1011", 16}, {"1010", 32}, {"10011", 12},
    {"10010", 48}, {"10001", 20}, {"10000", 40}, {"01111", 28}, {"01110", 44}, {"01101", 52},
    {"01100", 56}, {"01011", 1}, {"01010", 61}, {"01001", 2}, {"01000", 62}, {"001111", 24},
    {"001110", 36}, {"001101", 3}, {"001100", 63}, {"0010111", 5}, {"0010110", 9},
    {"0010101", 17}, {"0010100", 33}, {"0010011", 6}, {"0010010", 10}, {"0010001", 18},
    {"0010000", 34}, {"00011111", 7}, {"00011110", 11}, {"00011101", 19}, {"00011100", 35},
    {"00011011", 13}, {"00011010", 49}, {"00011001", 21}, {"00011000", 41}, {"00010111", 14},
    {"00010110", 50}, {"00010101", 22}, {"00010100", 42}, {"00010011", 15}, {"00010010", 51},
    {"00010001", 23}, {"00010000", 43}, {"00001111", 25}, {"00001110", 37}, {"00001101", 26},
    {"00001100", 38}, {"00001011", 29}, {"00001010", 45}, {"00001001", 53}, {"00001000", 57},
    {"00000111", 30}, {"00000110", 46}, {"00000101", 54}, {"00000100", 58}, {"000000111", 31},
    {"000000110", 47}, {"000000101", 55}, {"000000100", 59}, {"000000011", 27},
    {"000000010", 39}, {"000000001", 0},
};

/* B.10 motion_code, magnitude (the sign bit follows a nonzero code) */
static const VlcDef motion_def[] = {
    {"1", 0}, {"01", 1}, {"001", 2}, {"0001", 3}, {"000011", 4}, {"0000101", 5},
    {"0000100", 6}, {"0000011", 7}, {"000001011", 8}, {"000001010", 9}, {"000001001", 10},
    {"0000010001", 11}, {"0000010000", 12}, {"0000001111", 13}, {"0000001110", 14},
    {"0000001101", 15}, {"0000001100", 16},
};

/* B.12, B.13 dct_dc_size */
static const VlcDef dcl_def[] = {
    {"100", 0}, {"00", 1}, {"01", 2}, {"101", 3}, {"110", 4}, {"1110", 5}, {"11110", 6},
    {"111110", 7}, {"1111110", 8}, {"11111110", 9}, {"111111110", 10}, {"111111111", 11},
};
static const VlcDef dcc_def[] = {
    {"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"1110", 4}, {"11110", 5}, {"111110", 6},
    {"1111110", 7}, {"11111110", 8}, {"111111110", 9}, {"1111111110", 10}, {"1111111111", 11},
};

/* B.14 and B.15 DCT coefficients: run << 8 | level, sign bit not
   included. EOB and escape are special values. */
#define DCT_EOB 0x7F00
#define DCT_ESC 0x7E00
#define RL(r, l) ((r) << 8 | (l))

/* The codes the two tables share (all 12 bits and longer but those of
   (0,8)-(0,11), (1,5), (2,4) in B.14) */
#define DCT_COMMON_LONG                                                                           \
    {"000000011100", RL(3, 3)}, {"000000010010", RL(4, 3)}, {"000000011110", RL(6, 2)},           \
    {"000000010101", RL(7, 2)}, {"000000010001", RL(8, 2)}, {"000000011111", RL(17, 1)},          \
    {"000000011010", RL(18, 1)}, {"000000011001", RL(19, 1)}, {"000000010111", RL(20, 1)},        \
    {"000000010110", RL(21, 1)}, {"0000000011010", RL(0, 12)}, {"0000000011001", RL(0, 13)},      \
    {"0000000011000", RL(0, 14)}, {"0000000010111", RL(0, 15)}, {"0000000010110", RL(1, 6)},      \
    {"0000000010101", RL(1, 7)}, {"0000000010100", RL(2, 5)}, {"0000000010011", RL(3, 4)},        \
    {"0000000010010", RL(5, 3)}, {"0000000010001", RL(9, 2)}, {"0000000010000", RL(10, 2)},       \
    {"0000000011111", RL(22, 1)}, {"0000000011110", RL(23, 1)}, {"0000000011101", RL(24, 1)},     \
    {"0000000011100", RL(25, 1)}, {"0000000011011", RL(26, 1)}, {"00000000011111", RL(0, 16)},    \
    {"00000000011110", RL(0, 17)}, {"00000000011101", RL(0, 18)}, {"00000000011100", RL(0, 19)},  \
    {"00000000011011", RL(0, 20)}, {"00000000011010", RL(0, 21)}, {"00000000011001", RL(0, 22)},  \
    {"00000000011000", RL(0, 23)}, {"00000000010111", RL(0, 24)}, {"00000000010110", RL(0, 25)},  \
    {"00000000010101", RL(0, 26)}, {"00000000010100", RL(0, 27)}, {"00000000010011", RL(0, 28)},  \
    {"00000000010010", RL(0, 29)}, {"00000000010001", RL(0, 30)}, {"00000000010000", RL(0, 31)},  \
    {"000000000011000", RL(0, 32)}, {"000000000010111", RL(0, 33)},                               \
    {"000000000010110", RL(0, 34)}, {"000000000010101", RL(0, 35)},                               \
    {"000000000010100", RL(0, 36)}, {"000000000010011", RL(0, 37)},                               \
    {"000000000010010", RL(0, 38)}, {"000000000010001", RL(0, 39)},                               \
    {"000000000010000", RL(0, 40)}, {"000000000011111", RL(1, 8)},                                \
    {"000000000011110", RL(1, 9)}, {"000000000011101", RL(1, 10)},                                \
    {"000000000011100", RL(1, 11)}, {"000000000011011", RL(1, 12)},                               \
    {"000000000011010", RL(1, 13)}, {"000000000011001", RL(1, 14)},                               \
    {"0000000000010011", RL(1, 15)}, {"0000000000010010", RL(1, 16)},                             \
    {"0000000000010001", RL(1, 17)}, {"0000000000010000", RL(1, 18)},                             \
    {"0000000000010100", RL(6, 3)}, {"0000000000011010", RL(11, 2)},                              \
    {"0000000000011001", RL(12, 2)}, {"0000000000011000", RL(13, 2)},                             \
    {"0000000000010111", RL(14, 2)}, {"0000000000010110", RL(15, 2)},                             \
    {"0000000000010101", RL(16, 2)}, {"0000000000011111", RL(27, 1)},                             \
    {"0000000000011110", RL(28, 1)}, {"0000000000011101", RL(29, 1)},                             \
    {"0000000000011100", RL(30, 1)}, {"0000000000011011", RL(31, 1)}

static const VlcDef dct0_def[] = {
    {"10", DCT_EOB}, {"11", RL(0, 1)}, {"011", RL(1, 1)}, {"0100", RL(0, 2)}, {"0101", RL(2, 1)},
    {"00101", RL(0, 3)}, {"00111", RL(3, 1)}, {"00110", RL(4, 1)}, {"000110", RL(1, 2)},
    {"000111", RL(5, 1)}, {"000101", RL(6, 1)}, {"000100", RL(7, 1)}, {"0000110", RL(0, 4)},
    {"0000100", RL(2, 2)}, {"0000111", RL(8, 1)}, {"0000101", RL(9, 1)}, {"000001", DCT_ESC},
    {"00100110", RL(0, 5)}, {"00100001", RL(0, 6)}, {"00100101", RL(1, 3)}, {"00100100", RL(3, 2)},
    {"00100111", RL(10, 1)}, {"00100011", RL(11, 1)}, {"00100010", RL(12, 1)},
    {"00100000", RL(13, 1)}, {"0000001010", RL(0, 7)}, {"0000001100", RL(1, 4)},
    {"0000001011", RL(2, 3)}, {"0000001111", RL(4, 2)}, {"0000001001", RL(5, 2)},
    {"0000001110", RL(14, 1)}, {"0000001101", RL(15, 1)}, {"0000001000", RL(16, 1)},
    {"000000011101", RL(0, 8)}, {"000000011000", RL(0, 9)}, {"000000010011", RL(0, 10)},
    {"000000010000", RL(0, 11)}, {"000000011011", RL(1, 5)}, {"000000010100", RL(2, 4)},
    DCT_COMMON_LONG,
};

static const VlcDef dct1_def[] = {
    {"0110", DCT_EOB}, {"10", RL(0, 1)}, {"010", RL(1, 1)}, {"110", RL(0, 2)}, {"00101", RL(2, 1)},
    {"0111", RL(0, 3)}, {"00111", RL(3, 1)}, {"000110", RL(4, 1)}, {"00110", RL(1, 2)},
    {"000111", RL(5, 1)}, {"0000110", RL(6, 1)}, {"0000100", RL(7, 1)}, {"11100", RL(0, 4)},
    {"0000111", RL(2, 2)}, {"0000101", RL(8, 1)}, {"1111000", RL(9, 1)}, {"000001", DCT_ESC},
    {"11101", RL(0, 5)}, {"000101", RL(0, 6)}, {"1111001", RL(1, 3)}, {"00100110", RL(3, 2)},
    {"1111010", RL(10, 1)}, {"00100001", RL(11, 1)}, {"00100101", RL(12, 1)},
    {"00100100", RL(13, 1)}, {"000100", RL(0, 7)}, {"00100111", RL(1, 4)}, {"11111100", RL(2, 3)},
    {"11111101", RL(4, 2)}, {"000000100", RL(5, 2)}, {"000000101", RL(14, 1)},
    {"000000111", RL(15, 1)}, {"0000001101", RL(16, 1)}, {"1111011", RL(0, 8)},
    {"1111100", RL(0, 9)}, {"00100011", RL(0, 10)}, {"00100010", RL(0, 11)},
    {"00100000", RL(1, 5)}, {"0000001100", RL(2, 4)}, {"11111010", RL(0, 12)},
    {"11111011", RL(0, 13)}, {"11111110", RL(0, 14)}, {"11111111", RL(0, 15)},
    /* B.14's (0,12)-(0,15) codes are free in B.15; the rest is shared */
    {"000000011100", RL(3, 3)}, {"000000010010", RL(4, 3)}, {"000000011110", RL(6, 2)},
    {"000000010101", RL(7, 2)}, {"000000010001", RL(8, 2)}, {"000000011111", RL(17, 1)},
    {"000000011010", RL(18, 1)}, {"000000011001", RL(19, 1)}, {"000000010111", RL(20, 1)},
    {"000000010110", RL(21, 1)}, {"0000000010110", RL(1, 6)}, {"0000000010101", RL(1, 7)},
    {"0000000010100", RL(2, 5)}, {"0000000010011", RL(3, 4)}, {"0000000010010", RL(5, 3)},
    {"0000000010001", RL(9, 2)}, {"0000000010000", RL(10, 2)}, {"0000000011111", RL(22, 1)},
    {"0000000011110", RL(23, 1)}, {"0000000011101", RL(24, 1)}, {"0000000011100", RL(25, 1)},
    {"0000000011011", RL(26, 1)}, {"00000000011111", RL(0, 16)}, {"00000000011110", RL(0, 17)},
    {"00000000011101", RL(0, 18)}, {"00000000011100", RL(0, 19)}, {"00000000011011", RL(0, 20)},
    {"00000000011010", RL(0, 21)}, {"00000000011001", RL(0, 22)}, {"00000000011000", RL(0, 23)},
    {"00000000010111", RL(0, 24)}, {"00000000010110", RL(0, 25)}, {"00000000010101", RL(0, 26)},
    {"00000000010100", RL(0, 27)}, {"00000000010011", RL(0, 28)}, {"00000000010010", RL(0, 29)},
    {"00000000010001", RL(0, 30)}, {"00000000010000", RL(0, 31)}, {"000000000011000", RL(0, 32)},
    {"000000000010111", RL(0, 33)}, {"000000000010110", RL(0, 34)},
    {"000000000010101", RL(0, 35)}, {"000000000010100", RL(0, 36)},
    {"000000000010011", RL(0, 37)}, {"000000000010010", RL(0, 38)},
    {"000000000010001", RL(0, 39)}, {"000000000010000", RL(0, 40)},
    {"000000000011111", RL(1, 8)}, {"000000000011110", RL(1, 9)}, {"000000000011101", RL(1, 10)},
    {"000000000011100", RL(1, 11)}, {"000000000011011", RL(1, 12)},
    {"000000000011010", RL(1, 13)}, {"000000000011001", RL(1, 14)},
    {"0000000000010011", RL(1, 15)}, {"0000000000010010", RL(1, 16)},
    {"0000000000010001", RL(1, 17)}, {"0000000000010000", RL(1, 18)},
    {"0000000000010100", RL(6, 3)}, {"0000000000011010", RL(11, 2)},
    {"0000000000011001", RL(12, 2)}, {"0000000000011000", RL(13, 2)},
    {"0000000000010111", RL(14, 2)}, {"0000000000010110", RL(15, 2)},
    {"0000000000010101", RL(16, 2)}, {"0000000000011111", RL(27, 1)},
    {"0000000000011110", RL(28, 1)}, {"0000000000011101", RL(29, 1)},
    {"0000000000011100", RL(30, 1)}, {"0000000000011011", RL(31, 1)},
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static Vlc mba_tab[1 << 11];
static Vlc mbtype_tab[3][1 << 6];
static Vlc cbp_tab[1 << 9];
static Vlc motion_tab[1 << 10];
static Vlc dcl_tab[1 << 9];
static Vlc dcc_tab[1 << 10];
/* DCT: codes of up to 10 bits by the next 10 bits; the longer ones all
   start with seven zeros and go by the 9 bits after those */
static Vlc dct_short[2][1 << 10];
static Vlc dct_long[2][1 << 9];
static int tables_ready;

static void dct_build(Vlc *shrt, Vlc *lng, const VlcDef *d, int n) {
    VlcDef s[128], l[128];
    int ns = 0, nl = 0, i;

    for (i = 0; i < n; i++) {
        if (strlen(d[i].code) <= 10) {
            s[ns++] = d[i];
        } else {
            l[nl] = d[i];
            l[nl].code = d[i].code + 7;
            nl++;
        }
    }
    vlc_build(shrt, 10, s, ns);
    vlc_build(lng, 9, l, nl);
    /* the short table's entries under 0000000: len 0 = "go long" */
    for (i = 0; i < 8; i++) {
        shrt[i].len = 0;
    }
}

static void tables_init(void) {
    if (tables_ready) {
        return;
    }
    vlc_build(mba_tab, 11, mba_def, N(mba_def));
    vlc_build(mbtype_tab[0], 6, mbtype_i_def, N(mbtype_i_def));
    vlc_build(mbtype_tab[1], 6, mbtype_p_def, N(mbtype_p_def));
    vlc_build(mbtype_tab[2], 6, mbtype_b_def, N(mbtype_b_def));
    vlc_build(cbp_tab, 9, cbp_def, N(cbp_def));
    vlc_build(motion_tab, 10, motion_def, N(motion_def));
    vlc_build(dcl_tab, 9, dcl_def, N(dcl_def));
    vlc_build(dcc_tab, 10, dcc_def, N(dcc_def));
    dct_build(dct_short[0], dct_long[0], dct0_def, N(dct0_def));
    dct_build(dct_short[1], dct_long[1], dct1_def, N(dct1_def));
    tables_ready = 1;
}

static const uint8_t zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
static const uint8_t altscan[64] = {
    0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18, 3,  11, 4,  12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};
static const uint8_t default_intra_qm[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};
static const uint8_t nonlinear_qscale[32] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16,  18,  20,  22,
    24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112,
};

/* ------------------------------------------------------------------------ */
/* The decoder                                                              */

typedef struct {
    uint8_t *y, *cb, *cr;
    int type, temporal_reference;
    int64_t pts, dts;
} Pic;

struct GrxMpeg2 {
    GrxMpeg2Read read;
    void *ctx;
    void (*hook)(void *ctx, int type);
    void *hook_ctx;

    /* bitstream */
    uint8_t in[64];
    int in_pos, in_len;
    uint64_t buf;          /* left aligned */
    int bits;
    int aborted;           /* the read function said stop */

    int pending_code;      /* a start code read but not handled, or -1 */
    int ended;
    int errors;

    /* sequence */
    int have_seq;
    int hsize, vsize, mb_w, mb_h, width, height, cwidth, cheight;
    int mpeg2;
    uint8_t qm[4][64];     /* intra, non-intra, chroma intra, chroma non-intra, raster order */
    int chroma_qm_loaded[2];

    /* picture */
    int in_picture;        /* header read, slices not decoded yet */
    int ptype, temporal_reference;
    int fcode[2][2];
    int dc_precision, structure, tff, fpfd, cmv, qst, ivf, alt;
    int64_t pts, dts;
    int broken_link, have_fwd;

    /* frames: ref[0] forward (older), ref[1] backward (newer), bpic */
    uint8_t *mem;
    Pic frame[3];
    Pic *ref[2], *bpic, *cur;
    int nrefs;
    int ref_pending;       /* ref[1] decoded and not given out */

    /* macroblock layer */
    int qscale;
    int dc_pred[3];
    int pmv[2][2][2];      /* [r][s][t] */
    int16_t blk[64];
};

static int refill(GrxMpeg2 *m) {
    for (;;) {
        int n;
        if (m->aborted) {
            return 0;
        }
        n = m->read(m->ctx, m->in, (int)sizeof(m->in));
        if (n > 0) {
            m->in_pos = 0;
            m->in_len = n;
            return 1;
        }
        if (n < 0) {
            m->aborted = 1;
            return 0;
        }
    }
}

/* Makes at least n (<= 57) bits available; after an abort, zeros */
static inline void need(GrxMpeg2 *m, int n) {
    while (m->bits < n) {
        uint8_t c = 0;
        if (m->in_pos < m->in_len || refill(m)) {
            c = m->in[m->in_pos++];
        }
        m->buf |= (uint64_t)c << (56 - m->bits);
        m->bits += 8;
    }
}

static inline uint32_t peek(GrxMpeg2 *m, int n) {
    need(m, n);
    return (uint32_t)(m->buf >> (64 - n));
}

static inline void skip(GrxMpeg2 *m, int n) {
    m->buf <<= n;
    m->bits -= n;
}

static inline uint32_t get(GrxMpeg2 *m, int n) {
    uint32_t v;
    if (n == 0) {
        return 0;
    }
    v = peek(m, n);
    skip(m, n);
    return v;
}

static void align(GrxMpeg2 *m) {
    skip(m, m->bits & 7);
}

/* The next start code's value (0x100-0x1FF), from a byte boundary; -1
   if the read function aborted */
static int next_start_code(GrxMpeg2 *m) {
    align(m);
    for (;;) {
        if (m->aborted) {
            return -1;
        }
        if (peek(m, 24) == 1) {
            skip(m, 24);
            return 0x100 | (int)get(m, 8);
        }
        skip(m, 8);
    }
}

static const Vlc *vlc_get(GrxMpeg2 *m, const Vlc *tab, int bits) {
    const Vlc *v = &tab[peek(m, bits)];
    if (v->len) {
        skip(m, v->len);
        return v;
    }
    return NULL;
}

/* ---- headers ---- */

static void load_qm(GrxMpeg2 *m, uint8_t *qm) {
    int i;
    for (i = 0; i < 64; i++) {
        qm[zigzag[i]] = (uint8_t)get(m, 8);
    }
}

static int alloc_frames(GrxMpeg2 *m) {
    size_t ysz = (size_t)m->width * m->height, csz = (size_t)m->cwidth * m->cheight;
    int i;

    /* nothing may point into the old block once it is gone, even if the
       new one can't be had */
    free(m->mem);
    memset(m->frame, 0, sizeof(m->frame));
    m->ref[0] = m->ref[1] = m->bpic = m->cur = NULL;
    m->nrefs = 0;
    m->ref_pending = 0;
    m->mem = malloc((ysz + 2 * csz) * 3);
    if (m->mem == NULL) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        uint8_t *b = m->mem + (ysz + 2 * csz) * i;
        m->frame[i].y = b;
        m->frame[i].cb = b + ysz;
        m->frame[i].cr = b + ysz + csz;
        memset(b, 16, ysz);
        memset(b + ysz, 128, 2 * csz);
    }
    m->ref[0] = &m->frame[0];
    m->ref[1] = &m->frame[1];
    m->bpic = &m->frame[2];
    m->nrefs = 0;
    m->ref_pending = 0;
    return 0;
}

static int sequence_header(GrxMpeg2 *m) {
    int h = (int)get(m, 12), v = (int)get(m, 12);

    get(m, 4);                 /* aspect_ratio_information */
    get(m, 4);                 /* frame_rate_code */
    get(m, 18);                /* bit_rate_value */
    get(m, 1);                 /* marker */
    get(m, 10);                /* vbv_buffer_size_value */
    get(m, 1);                 /* constrained_parameters_flag */
    if (get(m, 1)) {
        load_qm(m, m->qm[0]);
    } else {
        memcpy(m->qm[0], default_intra_qm, 64);
    }
    if (get(m, 1)) {
        load_qm(m, m->qm[1]);
    } else {
        memset(m->qm[1], 16, 64);
    }
    memcpy(m->qm[2], m->qm[0], 64);
    memcpy(m->qm[3], m->qm[1], 64);
    m->chroma_qm_loaded[0] = m->chroma_qm_loaded[1] = 0;
    m->mpeg2 = 0;
    if (!m->have_seq || h != m->hsize || v != m->vsize) {
        m->hsize = h;
        m->vsize = v;
        m->mb_w = (h + 15) / 16;
        m->mb_h = (v + 15) / 16;
        m->width = m->mb_w * 16;
        m->height = m->mb_h * 16;
        m->cwidth = m->width / 2;
        m->cheight = m->height / 2;
        if (h == 0 || v == 0 || alloc_frames(m) != 0) {
            m->have_seq = 0;
            return -1;
        }
        m->have_seq = 1;
    }
    return 0;
}

static int extension(GrxMpeg2 *m) {
    int id = (int)get(m, 4);

    switch (id) {
    case 1:                    /* sequence extension */
        get(m, 8);             /* profile_and_level_indication */
        get(m, 1);             /* progressive_sequence */
        if (get(m, 2) != 1) {  /* chroma_format: 4:2:0 only */
            return -1;
        }
        if (get(m, 2) != 0 || get(m, 2) != 0) {  /* size extensions */
            return -1;
        }
        m->mpeg2 = 1;
        break;
    case 3:                    /* quant matrix extension */
        if (get(m, 1)) {
            load_qm(m, m->qm[0]);
            if (!m->chroma_qm_loaded[0]) {
                memcpy(m->qm[2], m->qm[0], 64);
            }
        }
        if (get(m, 1)) {
            load_qm(m, m->qm[1]);
            if (!m->chroma_qm_loaded[1]) {
                memcpy(m->qm[3], m->qm[1], 64);
            }
        }
        if (get(m, 1)) {
            load_qm(m, m->qm[2]);
            m->chroma_qm_loaded[0] = 1;
        }
        if (get(m, 1)) {
            load_qm(m, m->qm[3]);
            m->chroma_qm_loaded[1] = 1;
        }
        break;
    case 8:                    /* picture coding extension */
        m->fcode[0][0] = (int)get(m, 4);
        m->fcode[0][1] = (int)get(m, 4);
        m->fcode[1][0] = (int)get(m, 4);
        m->fcode[1][1] = (int)get(m, 4);
        m->dc_precision = (int)get(m, 2);
        m->structure = (int)get(m, 2);
        m->tff = (int)get(m, 1);
        m->fpfd = (int)get(m, 1);
        m->cmv = (int)get(m, 1);
        m->qst = (int)get(m, 1);
        m->ivf = (int)get(m, 1);
        m->alt = (int)get(m, 1);
        break;
    default:                   /* display, scalable, copyright, ...: nothing needed */
        break;
    }
    return 0;
}

/* ---- motion compensation ---- */

/* One prediction block: w x h from src (stride sstride) at the integer
   position, half-pel flags hx/hy, into dst (stride dstride); avg: the
   average with what dst holds (the second direction of a B macroblock) */
static void pred_block(uint8_t *dst, int dstride, const uint8_t *src, int sstride, int hx, int hy, int w,
                       int h, int avg) {
    int x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *s = src + y * sstride;
        uint8_t *d = dst + y * dstride;
        if (!hx && !hy) {
            if (avg) {
                for (x = 0; x < w; x++) {
                    d[x] = (uint8_t)((d[x] + s[x] + 1) >> 1);
                }
            } else {
                memcpy(d, s, (size_t)w);
            }
        } else if (hx && !hy) {
            for (x = 0; x < w; x++) {
                int p = (s[x] + s[x + 1] + 1) >> 1;
                d[x] = (uint8_t)(avg ? (d[x] + p + 1) >> 1 : p);
            }
        } else if (!hx && hy) {
            for (x = 0; x < w; x++) {
                int p = (s[x] + s[x + sstride] + 1) >> 1;
                d[x] = (uint8_t)(avg ? (d[x] + p + 1) >> 1 : p);
            }
        } else {
            for (x = 0; x < w; x++) {
                int p = (s[x] + s[x + 1] + s[x + sstride] + s[x + sstride + 1] + 2) >> 2;
                d[x] = (uint8_t)(avg ? (d[x] + p + 1) >> 1 : p);
            }
        }
    }
}

/* Prediction from a plane (pw x ph, stride ps: for field prediction
   the field's own height and stride) at (x + mvx/2, y + mvy/2), clamped
   to the plane where the vector points outside it */
static void pred_plane(uint8_t *dst, int dstride, const uint8_t *plane, int ps, int pw, int ph, int x, int y,
                       int mvx, int mvy, int w, int h, int avg) {
    int ix = x + (mvx >> 1), iy = y + (mvy >> 1), hx = mvx & 1, hy = mvy & 1;

    if (ix >= 0 && iy >= 0 && ix + w + hx <= pw && iy + h + hy <= ph) {
        pred_block(dst, dstride, plane + iy * ps + ix, ps, hx, hy, w, h, avg);
    } else {
        uint8_t tmp[17 * 17];
        int i, j;
        for (j = 0; j <= h; j++) {
            int sy = iy + j < 0 ? 0 : iy + j >= ph ? ph - 1 : iy + j;
            for (i = 0; i <= w; i++) {
                int sx = ix + i < 0 ? 0 : ix + i >= pw ? pw - 1 : ix + i;
                tmp[j * 17 + i] = plane[sy * ps + sx];
            }
        }
        pred_block(dst, dstride, tmp, 17, hx, hy, w, h, avg);
    }
}

/* Chroma vector of a luma one (4:2:0): halved, rounded toward zero */
static inline int cvec(int v) {
    return v / 2;
}

/* A macroblock's prediction from ref in direction s */
static void predict(GrxMpeg2 *m, const Pic *ref, int mbx, int mby, int motion_type, int s, int avg,
                    int mv[2][2][2], int fsel[2][2]) {
    Pic *cur = m->cur;
    int W = m->width, H = m->height, CW = m->cwidth, CH = m->cheight;
    int x = mbx * 16, y = mby * 16;

    if (motion_type == 2) {    /* frame */
        int mvx = mv[0][s][0], mvy = mv[0][s][1];
        pred_plane(cur->y + y * W + x, W, ref->y, W, W, H, x, y, mvx, mvy, 16, 16, avg);
        pred_plane(cur->cb + (y / 2) * CW + x / 2, CW, ref->cb, CW, CW, CH, x / 2, y / 2, cvec(mvx), cvec(mvy),
                   8, 8, avg);
        pred_plane(cur->cr + (y / 2) * CW + x / 2, CW, ref->cr, CW, CW, CH, x / 2, y / 2, cvec(mvx), cvec(mvy),
                   8, 8, avg);
    } else {                   /* field: each field of the macroblock from a field of ref */
        int r;
        for (r = 0; r < 2; r++) {
            int mvx = mv[r][s][0], mvy = mv[r][s][1], f = fsel[r][s];
            pred_plane(cur->y + (y + r) * W + x, 2 * W, ref->y + f * W, 2 * W, W, H / 2, x, y / 2, mvx, mvy, 16, 8,
                       avg);
            pred_plane(cur->cb + (y / 2 + r) * CW + x / 2, 2 * CW, ref->cb + f * CW, 2 * CW, CW, CH / 2, x / 2,
                       y / 4, cvec(mvx), cvec(mvy), 8, 4, avg);
            pred_plane(cur->cr + (y / 2 + r) * CW + x / 2, 2 * CW, ref->cr + f * CW, 2 * CW, CW, CH / 2, x / 2,
                       y / 4, cvec(mvx), cvec(mvy), 8, 4, avg);
        }
    }
}

/* ---- IDCT (mpeg2decode's idct.c, Chen-Wang) ---- */

#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

static inline int iclip(int v) {
    return v < -256 ? -256 : v > 255 ? 255 : v;
}

static void idct_row(int16_t *blk) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;

    if (!((x1 = blk[4] << 11) | (x2 = blk[6]) | (x3 = blk[2]) | (x4 = blk[1]) | (x5 = blk[7]) | (x6 = blk[5]) |
          (x7 = blk[3]))) {
        blk[0] = blk[1] = blk[2] = blk[3] = blk[4] = blk[5] = blk[6] = blk[7] = (int16_t)(blk[0] << 3);
        return;
    }
    x0 = (blk[0] << 11) + 128;
    x8 = W7 * (x4 + x5);
    x4 = x8 + (W1 - W7) * x4;
    x5 = x8 - (W1 + W7) * x5;
    x8 = W3 * (x6 + x7);
    x6 = x8 - (W3 - W5) * x6;
    x7 = x8 - (W3 + W5) * x7;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2);
    x2 = x1 - (W2 + W6) * x2;
    x3 = x1 + (W2 - W6) * x3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    blk[0] = (int16_t)((x7 + x1) >> 8);
    blk[1] = (int16_t)((x3 + x2) >> 8);
    blk[2] = (int16_t)((x0 + x4) >> 8);
    blk[3] = (int16_t)((x8 + x6) >> 8);
    blk[4] = (int16_t)((x8 - x6) >> 8);
    blk[5] = (int16_t)((x0 - x4) >> 8);
    blk[6] = (int16_t)((x3 - x2) >> 8);
    blk[7] = (int16_t)((x7 - x1) >> 8);
}

static void idct_col(int16_t *blk) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;

    if (!((x1 = (blk[8 * 4] << 8)) | (x2 = blk[8 * 6]) | (x3 = blk[8 * 2]) | (x4 = blk[8 * 1]) |
          (x5 = blk[8 * 7]) | (x6 = blk[8 * 5]) | (x7 = blk[8 * 3]))) {
        int16_t v = (int16_t)iclip((blk[8 * 0] + 32) >> 6);
        blk[8 * 0] = blk[8 * 1] = blk[8 * 2] = blk[8 * 3] = blk[8 * 4] = blk[8 * 5] = blk[8 * 6] = blk[8 * 7] = v;
        return;
    }
    x0 = (blk[8 * 0] << 8) + 8192;
    x8 = W7 * (x4 + x5) + 4;
    x4 = (x8 + (W1 - W7) * x4) >> 3;
    x5 = (x8 - (W1 + W7) * x5) >> 3;
    x8 = W3 * (x6 + x7) + 4;
    x6 = (x8 - (W3 - W5) * x6) >> 3;
    x7 = (x8 - (W3 + W5) * x7) >> 3;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2) + 4;
    x2 = (x1 - (W2 + W6) * x2) >> 3;
    x3 = (x1 + (W2 - W6) * x3) >> 3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    blk[8 * 0] = (int16_t)iclip((x7 + x1) >> 14);
    blk[8 * 1] = (int16_t)iclip((x3 + x2) >> 14);
    blk[8 * 2] = (int16_t)iclip((x0 + x4) >> 14);
    blk[8 * 3] = (int16_t)iclip((x8 + x6) >> 14);
    blk[8 * 4] = (int16_t)iclip((x8 - x6) >> 14);
    blk[8 * 5] = (int16_t)iclip((x0 - x4) >> 14);
    blk[8 * 6] = (int16_t)iclip((x3 - x2) >> 14);
    blk[8 * 7] = (int16_t)iclip((x7 - x1) >> 14);
}

static void idct(int16_t *blk) {
    int i;
    for (i = 0; i < 8; i++) {
        idct_row(blk + 8 * i);
    }
    for (i = 0; i < 8; i++) {
        idct_col(blk + i);
    }
}

static inline uint8_t clip255(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* ---- the macroblock layer ---- */

/* One block's coefficients into m->blk (dequantised, raster order).
   cc: 0 luma, 1 Cb, 2 Cr. Returns 0, or -1 on a bad code. */
static int block(GrxMpeg2 *m, int cc, int intra) {
    int16_t *F = m->blk;
    const uint8_t *scan = m->alt ? altscan : zigzag;
    const uint8_t *qm = m->qm[(cc ? 2 : 0) + (intra ? 0 : 1)];
    int tab = intra && m->ivf;
    int n, sum, qs = m->qscale;

    memset(F, 0, sizeof(m->blk));
    if (intra) {
        const Vlc *v = cc ? vlc_get(m, dcc_tab, 10) : vlc_get(m, dcl_tab, 9);
        int size, diff = 0, dc;
        if (v == NULL) {
            return -1;
        }
        size = v->val;
        if (size) {
            diff = (int)get(m, size);
            if (!(diff & (1 << (size - 1)))) {
                diff -= (1 << size) - 1;
            }
        }
        m->dc_pred[cc] += diff;
        dc = m->dc_pred[cc] << (3 - m->dc_precision);
        F[0] = (int16_t)dc;
        sum = dc;
        n = 1;
    } else {
        sum = 0;
        n = 0;
        /* the first coefficient: "1s" is (0, 1) */
        if (peek(m, 1)) {
            int val = 1, s;
            skip(m, 1);
            s = (int)get(m, 1);
            val = ((2 * val + 1) * qm[0] * qs) / 32;
            if (val > 2047) {
                val = 2047;
            }
            F[scan[0]] = (int16_t)(s ? -val : val);
            sum += F[scan[0]];
            n = 1;
        }
    }
    for (;;) {
        const Vlc *v;
        int run, level, s, pos, val;
        uint32_t c = peek(m, 16);
        v = &dct_short[tab][c >> 6];
        if (v->len == 0) {
            v = &dct_long[tab][c & 0x1FF];
            if (v->len == 0) {
                return -1;
            }
            skip(m, 7 + v->len);
        } else {
            skip(m, v->len);
        }
        if (v->val == DCT_EOB) {
            break;
        }
        if (v->val == DCT_ESC) {
            run = (int)get(m, 6);
            level = (int)get(m, 12);
            if (level == 0 || level == 0x800) {
                return -1;
            }
            s = level >= 0x800;
            if (s) {
                level = 0x1000 - level;
            }
        } else {
            run = v->val >> 8;
            level = v->val & 0xFF;
            s = (int)get(m, 1);
        }
        n += run;
        if (n > 63) {
            return -1;
        }
        pos = scan[n];
        if (intra) {
            val = (level * qm[pos] * qs) / 16;
        } else {
            val = ((2 * level + 1) * qm[pos] * qs) / 32;
        }
        if (s) {
            val = -val;
            if (val < -2048) {
                val = -2048;
            }
        } else if (val > 2047) {
            val = 2047;
        }
        F[pos] = (int16_t)val;
        sum += val;
        n++;
    }
    /* mismatch control */
    if (!(sum & 1)) {
        F[63] ^= 1;
    }
    return 0;
}

/* Puts (intra) or adds the IDCT of m->blk at dst */
static void put_block(GrxMpeg2 *m, uint8_t *dst, int stride, int add) {
    int16_t *b = m->blk;
    int x, y;

    idct(b);
    for (y = 0; y < 8; y++) {
        uint8_t *d = dst + y * stride;
        const int16_t *s = b + y * 8;
        if (add) {
            for (x = 0; x < 8; x++) {
                d[x] = clip255(d[x] + s[x]);
            }
        } else {
            for (x = 0; x < 8; x++) {
                d[x] = clip255(s[x]);
            }
        }
    }
}

static int motion_component(GrxMpeg2 *m, int fcode, int pred) {
    const Vlc *v;
    int code, delta, r_size, f, low, high, range, val;

    /* f_code 0 is forbidden and 15 marks a direction unused: a vector
       read with either is a bad stream */
    if (fcode < 1 || fcode > 14) {
        return INT32_MIN;
    }
    r_size = fcode - 1;
    f = 1 << r_size;
    v = vlc_get(m, motion_tab, 10);
    if (v == NULL) {
        return INT32_MIN;
    }
    code = v->val;
    if (code && get(m, 1)) {
        code = -code;
    }
    if (code == 0 || f == 1) {
        delta = code;
    } else {
        int residual = (int)get(m, r_size);
        delta = ((code < 0 ? -code : code) - 1) * f + residual + 1;
        if (code < 0) {
            delta = -delta;
        }
    }
    high = 16 * f - 1;
    low = -16 * f;
    range = 32 * f;
    val = pred + delta;
    if (val < low) {
        val += range;
    } else if (val > high) {
        val -= range;
    }
    return val;
}

/* motion_vectors(s): into pmv (and the field selects) */
static int motion_vectors(GrxMpeg2 *m, int s, int motion_type, int fsel[2][2]) {
    int t, v;

    if (motion_type == 2) {
        for (t = 0; t < 2; t++) {
            v = motion_component(m, m->fcode[s][t], m->pmv[0][s][t]);
            if (v == INT32_MIN) {
                return -1;
            }
            m->pmv[0][s][t] = m->pmv[1][s][t] = v;
        }
    } else if (motion_type == 1) {
        int r;
        for (r = 0; r < 2; r++) {
            fsel[r][s] = (int)get(m, 1);
            v = motion_component(m, m->fcode[s][0], m->pmv[r][s][0]);
            if (v == INT32_MIN) {
                return -1;
            }
            m->pmv[r][s][0] = v;
            v = motion_component(m, m->fcode[s][1], m->pmv[r][s][1] >> 1);
            if (v == INT32_MIN) {
                return -1;
            }
            m->pmv[r][s][1] = v * 2;
        }
    } else {
        return -1;             /* dual prime */
    }
    return 0;
}

/* The prediction of a non-intra macroblock (also skipped ones) */
static void mb_predict(GrxMpeg2 *m, int mbx, int mby, int type, int motion_type, int fsel[2][2]) {
    int mv[2][2][2];
    int r, s;

    for (r = 0; r < 2; r++) {
        for (s = 0; s < 2; s++) {
            mv[r][s][0] = m->pmv[r][s][0];
            mv[r][s][1] = motion_type == 1 ? m->pmv[r][s][1] >> 1 : m->pmv[r][s][1];
        }
    }
    if (type & MB_FWD) {
        predict(m, m->ref[0 + (m->ptype == 3 ? 0 : 1)], mbx, mby, motion_type, 0, 0, mv, fsel);
    }
    if (type & MB_BWD) {
        predict(m, m->ref[1], mbx, mby, motion_type, 1, (type & MB_FWD) != 0, mv, fsel);
    }
}

/* Decodes one slice; the slice start code has been read. Returns 0, or
   -1 when it ended on an error (the caller resyncs at the next start
   code). */
static int slice(GrxMpeg2 *m, int row) {
    int mb_addr, first = 1, last_type = MB_FWD, last_motion = 2;
    int fsel[2][2] = {{0, 0}, {0, 0}};
    int last_fsel[2][2] = {{0, 0}, {0, 0}};
    int dcreset = 1 << (m->dc_precision + 7);
    int max_addr = m->mb_w * m->mb_h;

    if (row >= m->mb_h) {
        return -1;
    }
    m->qscale = m->qst ? nonlinear_qscale[get(m, 5)] : (int)get(m, 5) * 2;
    if (peek(m, 1)) {          /* intra_slice_flag, intra_slice, reserved, extra information */
        get(m, 1);
        get(m, 1);
        get(m, 7);
        while (get(m, 1)) {
            get(m, 8);
        }
    }
    get(m, 1);                 /* extra_bit_slice */
    m->dc_pred[0] = m->dc_pred[1] = m->dc_pred[2] = dcreset;
    memset(m->pmv, 0, sizeof(m->pmv));
    mb_addr = row * m->mb_w - 1;

    for (;;) {
        int inc = 0, type, motion_type = 2, dct_type = 0, cbp, mbx, mby, i;
        const Vlc *v;

        for (;;) {
            uint32_t c = peek(m, 11);
            if (c == 0x008) {          /* escape: +33 */
                skip(m, 11);
                inc += 33;
            } else if (c == 0x00F) {   /* stuffing */
                skip(m, 11);
            } else {
                break;
            }
        }
        v = vlc_get(m, mba_tab, 11);
        if (v == NULL) {
            return -1;
        }
        inc += v->val;
        if (first) {
            mb_addr += inc;
            first = 0;
        } else {
            /* skipped macroblocks */
            for (i = 1; i < inc; i++) {
                int a = mb_addr + i;
                if (a >= max_addr) {
                    return -1;
                }
                if (m->ptype == 2) {
                    memset(m->pmv, 0, sizeof(m->pmv));
                    mb_predict(m, a % m->mb_w, a / m->mb_w, MB_FWD, 2, fsel);
                } else if (m->ptype == 3) {
                    mb_predict(m, a % m->mb_w, a / m->mb_w, last_type, last_motion, last_fsel);
                }
                m->dc_pred[0] = m->dc_pred[1] = m->dc_pred[2] = dcreset;
            }
            mb_addr += inc;
        }
        if (mb_addr >= max_addr) {
            return -1;
        }
        mbx = mb_addr % m->mb_w;
        mby = mb_addr / m->mb_w;

        v = vlc_get(m, mbtype_tab[m->ptype - 1], 6);
        if (v == NULL) {
            return -1;
        }
        type = v->val;
        if (m->structure == 3 && !m->fpfd && (type & (MB_FWD | MB_BWD))) {
            motion_type = (int)get(m, 2);
        }
        if (m->structure == 3 && !m->fpfd && (type & (MB_INTRA | MB_PATTERN))) {
            dct_type = (int)get(m, 1);
        }
        if (type & MB_QUANT) {
            int q = (int)get(m, 5);
            m->qscale = m->qst ? nonlinear_qscale[q] : q * 2;
        }
        if ((type & MB_FWD) || ((type & MB_INTRA) && m->cmv)) {
            if (motion_vectors(m, 0, (type & MB_INTRA) ? 2 : motion_type, fsel) != 0) {
                return -1;
            }
        }
        if (type & MB_BWD) {
            if (motion_vectors(m, 1, motion_type, fsel) != 0) {
                return -1;
            }
        }
        if ((type & MB_INTRA) && m->cmv) {
            get(m, 1);
        }

        if (type & MB_INTRA) {
            cbp = 0x3F;
            if (!m->cmv) {
                memset(m->pmv, 0, sizeof(m->pmv));
            }
        } else {
            m->dc_pred[0] = m->dc_pred[1] = m->dc_pred[2] = dcreset;
            if (m->ptype == 2 && !(type & MB_FWD)) {
                /* P, no motion compensation: zero vector, frame prediction */
                memset(m->pmv, 0, sizeof(m->pmv));
                motion_type = 2;
                type |= MB_FWD;
            }
            if (type & MB_PATTERN) {
                v = vlc_get(m, cbp_tab, 9);
                if (v == NULL) {
                    return -1;
                }
                cbp = v->val;
            } else {
                cbp = 0;
            }
            mb_predict(m, mbx, mby, type, motion_type, fsel);
            last_type = type & (MB_FWD | MB_BWD);
            last_motion = motion_type;
            memcpy(last_fsel, fsel, sizeof(fsel));
        }

        for (i = 0; i < 6; i++) {
            uint8_t *dst;
            int stride;
            if (!(cbp & (1 << (5 - i)))) {
                continue;
            }
            if (block(m, i < 4 ? 0 : i - 3, (type & MB_INTRA) != 0) != 0) {
                return -1;
            }
            if (i < 4) {
                int bx = mbx * 16 + (i & 1) * 8;
                if (dct_type) {
                    dst = m->cur->y + (mby * 16 + (i >> 1)) * m->width + bx;
                    stride = 2 * m->width;
                } else {
                    dst = m->cur->y + (mby * 16 + (i >> 1) * 8) * m->width + bx;
                    stride = m->width;
                }
            } else {
                dst = (i == 4 ? m->cur->cb : m->cur->cr) + mby * 8 * m->cwidth + mbx * 8;
                stride = m->cwidth;
            }
            put_block(m, dst, stride, !(type & MB_INTRA));
        }
        if (m->ptype == 3 && (type & MB_INTRA)) {
            last_type = MB_FWD;    /* (a skipped MB can't follow an intra one) */
        }

        if (peek(m, 23) == 0 || m->aborted) {
            return 0;
        }
    }
}

/* ---- pictures ---- */

static void output(GrxMpeg2 *m, const Pic *p, GrxMpeg2Frame *out) {
    out->y = p->y;
    out->cb = p->cb;
    out->cr = p->cr;
    out->width = m->width;
    out->height = m->height;
    out->display_width = m->hsize;
    out->display_height = m->vsize;
    out->y_stride = m->width;
    out->c_stride = m->cwidth;
    out->type = p->type;
    out->temporal_reference = p->temporal_reference;
    out->pts = p->pts;
    out->dts = p->dts;
}

static int picture_header(GrxMpeg2 *m) {
    m->temporal_reference = (int)get(m, 10);
    m->ptype = (int)get(m, 3);
    get(m, 16);                /* vbv_delay */
    if (m->ptype == 2 || m->ptype == 3) {
        get(m, 4);             /* full_pel_forward_vector, forward_f_code */
    }
    if (m->ptype == 3) {
        get(m, 4);
    }
    while (get(m, 1)) {        /* extra_information_picture */
        get(m, 8);
    }
    return m->ptype >= 1 && m->ptype <= 3 ? 0 : -1;
}

/* Decodes the slices of the picture whose headers have been read, up to
   the next start code that isn't a slice (left in pending_code) */
static int picture_data(GrxMpeg2 *m) {
    int code = m->pending_code;

    m->pending_code = -1;
    for (;;) {
        if (code < 0) {
            code = next_start_code(m);
            if (code < 0) {
                return -1;
            }
        }
        if (code >= 0x101 && code <= 0x1AF) {
            if (slice(m, code - 0x101) != 0) {
                if (m->aborted) {
                    return -1;
                }
                m->errors++;
            }
            code = -1;
            continue;
        }
        if (code == 0x1B2 || code == 0x1B5) {
            /* user data or an extension between the picture's slices: skip */
            code = -1;
            continue;
        }
        m->pending_code = code;
        return 0;
    }
}

int grx_mpeg2_decode(GrxMpeg2 *m, GrxMpeg2Frame *out) {
    if (m->ended && !m->ref_pending) {
        return 0;
    }
    for (;;) {
        int code;

        if (m->in_picture) {
            /* headers read: decode the slices */
            m->in_picture = 0;
            if (picture_data(m) != 0) {
                return -1;
            }
            if (m->ptype == 3) {
                output(m, m->cur, out);
                return 1;
            }
            /* a new reference picture: the newest one */
            m->cur = m->ref[0];
            m->ref[0] = m->ref[1];
            m->ref[1] = m->cur;
            if (m->nrefs < 2) {
                m->nrefs++;
            }
            m->ref_pending = 1;
            continue;
        }

        code = m->pending_code;
        m->pending_code = -1;
        if (code < 0) {
            code = next_start_code(m);
        }
        if (code < 0) {
            return -1;
        }
        switch (code) {
        case 0x1B3:
            if (sequence_header(m) != 0) {
                return -2;
            }
            break;
        case 0x1B5:
            if (extension(m) != 0) {
                return -2;
            }
            break;
        case 0x1B8:            /* group of pictures */
            get(m, 25);        /* time code */
            get(m, 1);         /* closed_gop */
            m->broken_link = (int)get(m, 1);
            break;
        case 0x1B7:            /* sequence end */
            if (m->ref_pending) {
                m->ref_pending = 0;
                m->ended = 1;
                output(m, m->ref[1], out);
                return 1;
            }
            m->ended = 1;
            return 0;
        case 0x100: {
            Pic *p;
            int hdr_code;
            if (!m->have_seq) {
                break;         /* no sequence header yet: skip the picture */
            }
            if (picture_header(m) != 0) {
                return -2;
            }
            /* the picture's extensions and user data, up to its first slice */
            for (;;) {
                hdr_code = next_start_code(m);
                if (hdr_code == 0x1B5) {
                    if (extension(m) != 0) {
                        return -2;
                    }
                    continue;
                }
                if (hdr_code == 0x1B2) {
                    continue;
                }
                break;
            }
            if (hdr_code < 0) {
                return -1;
            }
            m->pending_code = hdr_code;
            if (!m->mpeg2) {
                return -2;     /* MPEG-1 */
            }
            if (m->structure != 3) {
                return -2;     /* field pictures */
            }
            /* a B picture needs two references, a P one one */
            if ((m->ptype == 3 && m->nrefs < 2) || (m->ptype == 2 && m->nrefs < 1) ||
                (m->ptype == 3 && m->broken_link && !m->have_fwd)) {
                /* not decodable: pass over its slices */
                int c = hdr_code;
                while (c >= 0x101 && c <= 0x1AF) {
                    c = next_start_code(m);
                }
                m->pending_code = c;
                break;
            }
            if (m->ptype != 3) {
                m->have_fwd = 1;
                m->broken_link = 0;
            }
            p = m->ptype == 3 ? m->bpic : m->ref[0];
            m->cur = p;
            p->type = m->ptype;
            p->temporal_reference = m->temporal_reference;
            m->pts = m->dts = -1;
            if (m->hook != NULL) {
                m->hook(m->hook_ctx, m->ptype);
            }
            p->pts = m->pts;
            p->dts = m->dts;
            m->in_picture = 1;
            if (m->ptype != 3 && m->ref_pending) {
                /* give out the previous reference picture before this one
                   overwrites the buffer before it (ref[0]) */
                m->ref_pending = 0;
                output(m, m->ref[1], out);
                return 1;
            }
            break;
        }
        default:
            break;             /* user data, slices without a picture, system codes */
        }
    }
}

/* ------------------------------------------------------------------------ */

GrxMpeg2 *grx_mpeg2_create(GrxMpeg2Read read, void *ctx) {
    GrxMpeg2 *m = calloc(1, sizeof(*m));

    tables_init();
    if (m == NULL) {
        return NULL;
    }
    m->read = read;
    m->ctx = ctx;
    m->pending_code = -1;
    return m;
}

void grx_mpeg2_destroy(GrxMpeg2 *m) {
    if (m != NULL) {
        free(m->mem);
        free(m);
    }
}

void grx_mpeg2_set_picture_hook(GrxMpeg2 *m, void (*hook)(void *ctx, int type), void *ctx) {
    m->hook = hook;
    m->hook_ctx = ctx;
}

void grx_mpeg2_set_pts(GrxMpeg2 *m, int64_t pts, int64_t dts) {
    m->pts = pts;
    m->dts = dts;
}

void grx_mpeg2_reset(GrxMpeg2 *m, int what) {
    if (what & GRX_MPEG2_RESET_STREAM) {
        m->in_pos = m->in_len = 0;
        m->buf = 0;
        m->bits = 0;
        m->aborted = 0;
        m->pending_code = -1;
        m->in_picture = 0;
        m->ended = 0;
    }
    if (what & GRX_MPEG2_RESET_REFS) {
        m->nrefs = 0;
        m->ref_pending = 0;
        m->have_fwd = 0;
    }
}

int grx_mpeg2_has_pending(const GrxMpeg2 *m) {
    return m->ref_pending;
}

int grx_mpeg2_ended(const GrxMpeg2 *m) {
    return m->ended && !m->ref_pending;
}

int grx_mpeg2_buffered(const GrxMpeg2 *m) {
    return (m->in_len - m->in_pos) + m->bits / 8;
}

int grx_mpeg2_errors(const GrxMpeg2 *m) {
    return m->errors;
}

/* ------------------------------------------------------------------------ */
/* CSC                                                                      */

/* The IPU's YCbCr to RGB, in its fixed point (coefficients / 128) */
static inline void csc_pixel(uint8_t *d, int y, int cb, int cr) {
    int lum = (0x95 * (y > 16 ? y - 16 : 0)) >> 6;
    int r = lum + ((0xCC * (cr - 128)) >> 6);
    int g = lum + ((-0x68 * (cr - 128)) >> 6) + ((-0x32 * (cb - 128)) >> 6);
    int b = lum + ((0x102 * (cb - 128)) >> 6);
    d[0] = clip255((r + 1) >> 1);
    d[1] = clip255((g + 1) >> 1);
    d[2] = clip255((b + 1) >> 1);
    d[3] = 0x80;
}

static void csc_mb(const GrxMpeg2Frame *f, int mbx, int mby, uint8_t *d, int pitch) {
    int x, y;
    for (y = 0; y < 16; y++) {
        const uint8_t *ys = f->y + (mby * 16 + y) * f->y_stride + mbx * 16;
        const uint8_t *cbs = f->cb + (mby * 8 + y / 2) * f->c_stride + mbx * 8;
        const uint8_t *crs = f->cr + (mby * 8 + y / 2) * f->c_stride + mbx * 8;
        uint8_t *o = d + y * pitch;
        for (x = 0; x < 16; x++) {
            csc_pixel(o + x * 4, ys[x], cbs[x / 2], crs[x / 2]);
        }
    }
}

void grx_mpeg2_csc_mb(const GrxMpeg2Frame *f, uint8_t *dst, int max_mb) {
    int mbw = f->width / 16, mbh = f->height / 16, x, y, n = 0;

    for (x = 0; x < mbw; x++) {
        for (y = 0; y < mbh; y++) {
            if (n >= max_mb) {
                return;
            }
            csc_mb(f, x, y, dst + (size_t)n * 1024, 64);
            n++;
        }
    }
}

void grx_mpeg2_csc_linear(const GrxMpeg2Frame *f, uint8_t *dst, int pitch) {
    int mbw = f->width / 16, mbh = f->height / 16, x, y;

    for (y = 0; y < mbh; y++) {
        for (x = 0; x < mbw; x++) {
            csc_mb(f, x, y, dst + (size_t)y * 16 * pitch + x * 64, pitch);
        }
    }
}
