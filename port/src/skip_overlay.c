/* The skip prompt's picture (grx_skip.h): "Hold [START] to skip" or
   "Skipping" on a dark rounded panel, with a progress bar under the text.

   The text is a small stroke font made for these words: each letter is
   lines and elliptical arcs on a grid (capital height 10 units, x-height
   7, descender 3), drawn as round-ended strokes. The picture is rendered
   from their distances, so it is smooth at any size; the presenter
   renders it again only when the size or the words change.

   Channels: R the text and the button's outline, G the panel, B the bar's
   track; the presenter's shader gives them their colours and fills the
   bar. Rows are top to bottom. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grx_skip.h"

#define MAX_SEGS 4000
#define PI 3.14159265358979f

typedef struct {
    float x0, y0, x1, y1, hw; /* units, y up; half the stroke's width */
    int ch;                   /* 0 text, 2 bar */
} Seg;

static Seg segs[MAX_SEGS];
static int nsegs;

/* Where and how big the letters go now */
static float pen_x, pen_y, pen_scale, pen_hw;
static int pen_ch; /* 0 text (white), 2 the bar channel (gold in the menu) */

static void seg(float x0, float y0, float x1, float y1) {
    if (nsegs == MAX_SEGS) {
        return;
    }
    segs[nsegs].x0 = pen_x + x0 * pen_scale;
    segs[nsegs].y0 = pen_y + y0 * pen_scale;
    segs[nsegs].x1 = pen_x + x1 * pen_scale;
    segs[nsegs].y1 = pen_y + y1 * pen_scale;
    segs[nsegs].hw = pen_hw;
    segs[nsegs].ch = pen_ch;
    nsegs++;
}

/* An elliptical arc from angle a0 to a1 (degrees, counter-clockwise
   positive; a1 < a0 goes clockwise) */
static void arc(float cx, float cy, float rx, float ry, float a0, float a1) {
    int n = (int)(fabsf(a1 - a0) / 10.0f) + 2, i;
    float px = cx + rx * cosf(a0 * PI / 180), py = cy + ry * sinf(a0 * PI / 180);
    for (i = 1; i <= n; i++) {
        float a = (a0 + (a1 - a0) * (float)i / (float)n) * PI / 180;
        float x = cx + rx * cosf(a), y = cy + ry * sinf(a);
        seg(px, py, x, y);
        px = x;
        py = y;
    }
}

static void dot(float x, float y) {
    seg(x, y, x, y);
}

/* One letter at the pen; returns its width (units, unscaled) */
static float glyph(char c) {
    switch (c) {
    case 'A':
        seg(0, 0, 3.2f, 10);
        seg(3.2f, 10, 6.4f, 0);
        seg(1.05f, 3.3f, 5.35f, 3.3f);
        return 6.4f;
    case 'E':
        seg(5.6f, 10, 0, 10);
        seg(0, 10, 0, 0);
        seg(0, 0, 5.6f, 0);
        seg(0, 5, 5, 5);
        return 5.6f;
    case 'H':
        seg(0, 0, 0, 10);
        seg(6.2f, 0, 6.2f, 10);
        seg(0, 5, 6.2f, 5);
        return 6.2f;
    case 'R':
        seg(0, 0, 0, 10);
        seg(0, 10, 3.3f, 10);
        arc(3.3f, 7.5f, 2.6f, 2.5f, 90, -90);
        seg(3.3f, 5, 0, 5);
        seg(3.1f, 5, 6, 0);
        return 6.0f;
    case 'S':
        arc(3, 7.5f, 3, 2.5f, 25, 270);
        arc(3, 2.5f, 3, 2.5f, 90, -155);
        return 6.0f;
    case 'T':
        seg(0, 10, 6, 10);
        seg(3, 10, 3, 0);
        return 6.0f;
    case '>':
        seg(0, 9, 5, 5);
        seg(5, 5, 0, 1);
        return 5.0f;
    case '2':
        arc(3, 7.2f, 2.9f, 2.8f, 160, -30);
        seg(5.51f, 5.8f, 0, 0);
        seg(0, 0, 6, 0);
        return 6.0f;
    case '4':
        seg(4.5f, 0, 4.5f, 10);
        seg(4.5f, 10, 0, 3);
        seg(0, 3, 6.2f, 3);
        return 6.2f;
    case 'C':
        arc(3.6f, 5, 3.6f, 5, 42, 318);
        return 6.6f;
    case 'D':
        seg(0, 0, 0, 10);
        seg(0, 10, 2.4f, 10);
        seg(0, 0, 2.4f, 0);
        arc(2.4f, 5, 3.6f, 5, 90, -90);
        return 6.0f;
    case 'F':
        seg(0, 0, 0, 10);
        seg(0, 10, 5.4f, 10);
        seg(0, 5, 4.6f, 5);
        return 5.4f;
    case 'b':
        seg(0, 0, 0, 10.5f);
        arc(2.9f, 3.5f, 2.9f, 3.5f, 0, 360);
        return 5.8f;
    case 'c':
        arc(2.9f, 3.5f, 2.9f, 3.5f, 45, 315);
        return 5.4f;
    case 'f':
        seg(1.4f, 0, 1.4f, 8.4f);
        arc(3.0f, 8.4f, 1.6f, 1.6f, 180, 30);
        seg(0, 7, 3.4f, 7);
        return 4.2f;
    case 'm':
        seg(0, 0, 0, 7);
        arc(1.9f, 4.8f, 1.9f, 2.2f, 180, 0);
        seg(3.8f, 4.8f, 3.8f, 0);
        arc(5.7f, 4.8f, 1.9f, 2.2f, 180, 0);
        seg(7.6f, 4.8f, 7.6f, 0);
        return 7.6f;
    case 'u':
        seg(0, 7, 0, 2.8f);
        arc(2.8f, 2.8f, 2.8f, 2.8f, 180, 360);
        seg(5.6f, 7, 5.6f, 0);
        return 5.6f;
    case 'x':
        seg(0, 0, 5.4f, 7);
        seg(0, 7, 5.4f, 0);
        return 5.4f;
    case 'y':
        seg(0, 7, 2.9f, 0);
        seg(5.8f, 7, 1.6f, -3);
        return 5.8f;
    case '0':
        arc(2.8f, 5, 2.8f, 5, 0, 360);
        return 5.6f;
    case '1':
        seg(2.6f, 0, 2.6f, 10);
        seg(2.6f, 10, 0.4f, 8);
        return 3.6f;
    case '3':
        arc(2.8f, 7.5f, 2.6f, 2.5f, 150, -90);
        arc(2.8f, 2.5f, 2.8f, 2.5f, 90, -150);
        return 5.6f;
    case '5':
        seg(5.2f, 10, 0.6f, 10);
        seg(0.6f, 10, 0.4f, 5.4f);
        arc(2.8f, 3.2f, 2.8f, 3.2f, 135, -160);
        return 5.6f;
    case '6':
        arc(2.8f, 3.2f, 2.8f, 3.2f, 0, 360);
        seg(0.1f, 3.6f, 3.4f, 10);
        return 5.6f;
    case '7':
        seg(0, 10, 5.6f, 10);
        seg(5.6f, 10, 1.8f, 0);
        return 5.6f;
    case '8':
        arc(2.8f, 7.6f, 2.4f, 2.4f, 0, 360);
        arc(2.8f, 2.6f, 2.8f, 2.6f, 0, 360);
        return 5.6f;
    case '9':
        arc(2.8f, 6.8f, 2.8f, 3.2f, 0, 360);
        seg(5.5f, 6.4f, 2.2f, 0);
        return 5.6f;
    case ':':
        dot(0, 1);
        dot(0, 6);
        return 0.0f;
    case '<':
        seg(5, 9, 0, 5);
        seg(0, 5, 5, 1);
        return 5.0f;
    case 'a':
        arc(2.9f, 3.5f, 2.9f, 3.5f, 0, 360);
        seg(5.8f, 0, 5.8f, 7);
        return 5.8f;
    case 'B':
        seg(0, 0, 0, 10);
        seg(0, 10, 2.6f, 10);
        seg(0, 5, 2.8f, 5);
        seg(0, 0, 2.9f, 0);
        arc(2.6f, 7.5f, 2.5f, 2.5f, 90, -90);
        arc(2.9f, 2.5f, 2.5f, 2.5f, 90, -90);
        return 5.4f;
    case 'G':
        arc(3.6f, 5, 3.6f, 5, 42, 360);
        seg(7.2f, 5, 4.2f, 5);
        return 7.2f;
    case 'I':
        seg(0, 0, 0, 10);
        return 0.0f;
    case 'J':
        seg(4, 10, 4, 2.6f);
        arc(2, 2.6f, 2, 2.6f, 180, 360);
        return 4.0f;
    case 'K':
        seg(0, 0, 0, 10);
        seg(5.4f, 10, 0, 4);
        seg(1.8f, 5.8f, 5.6f, 0);
        return 5.6f;
    case 'L':
        seg(0, 10, 0, 0);
        seg(0, 0, 5, 0);
        return 5.0f;
    case 'O':
        arc(3.8f, 5, 3.8f, 5, 0, 360);
        return 7.6f;
    case 'Q':
        arc(3.8f, 5, 3.8f, 5, 0, 360);
        seg(4.4f, 2.2f, 7.4f, -0.6f);
        return 7.6f;
    case 'V':
        seg(0, 10, 3, 0);
        seg(3, 0, 6, 10);
        return 6.0f;
    case 'W':
        seg(0, 10, 2, 0);
        seg(2, 0, 4.2f, 7);
        seg(4.2f, 7, 6.4f, 0);
        seg(6.4f, 0, 8.4f, 10);
        return 8.4f;
    case 'X':
        seg(0, 0, 6, 10);
        seg(0, 10, 6, 0);
        return 6.0f;
    case 'Y':
        seg(0, 10, 3, 5);
        seg(6, 10, 3, 5);
        seg(3, 5, 3, 0);
        return 6.0f;
    case 'Z':
        seg(0, 10, 6, 10);
        seg(6, 10, 0, 0);
        seg(0, 0, 6, 0);
        return 6.0f;
    case 'z':
        seg(0, 7, 5, 7);
        seg(5, 7, 0, 0);
        seg(0, 0, 5, 0);
        return 5.0f;
    case 'j':
        seg(2.2f, 7, 2.2f, -1.2f);
        arc(0.6f, -1.2f, 1.6f, 1.8f, 0, -150);
        dot(2.2f, 9.6f);
        return 2.2f;
    case 'q':
        arc(2.9f, 3.5f, 2.9f, 3.5f, 0, 360);
        seg(5.8f, 7, 5.8f, -3);
        return 5.8f;
    case '-':
        seg(0.4f, 4, 4, 4);
        return 4.4f;
    case 'M':
        seg(0, 0, 0, 10);
        seg(0, 10, 3.4f, 3);
        seg(3.4f, 3, 6.8f, 10);
        seg(6.8f, 10, 6.8f, 0);
        return 6.8f;
    case 'N':
        seg(0, 0, 0, 10);
        seg(0, 10, 5.8f, 0);
        seg(5.8f, 0, 5.8f, 10);
        return 5.8f;
    case 'P':
        seg(0, 0, 0, 10);
        seg(0, 10, 2.6f, 10);
        seg(0, 4.8f, 2.6f, 4.8f);
        arc(2.6f, 7.4f, 2.6f, 2.6f, 90, -90);
        return 5.2f;
    case 'U':
        seg(0, 10, 0, 3);
        arc(2.9f, 3, 2.9f, 3, 180, 360);
        seg(5.8f, 3, 5.8f, 10);
        return 5.8f;
    case 'h':
        seg(0, 0, 0, 10.5f);
        arc(2.8f, 4.2f, 2.8f, 2.8f, 180, 0);
        seg(5.6f, 4.2f, 5.6f, 0);
        return 5.6f;
    case 'w':
        seg(0, 7, 1.9f, 0);
        seg(1.9f, 0, 3.8f, 5);
        seg(3.8f, 5, 5.7f, 0);
        seg(5.7f, 0, 7.6f, 7);
        return 7.6f;
    case 'v':
        seg(0, 7, 2.8f, 0);
        seg(2.8f, 0, 5.6f, 7);
        return 5.6f;
    case 'd':
        arc(2.9f, 3.5f, 2.9f, 3.5f, 0, 360);
        seg(5.8f, 0, 5.8f, 10.5f);
        return 5.8f;
    case 'e':
        seg(0.1f, 3.6f, 5.8f, 3.6f);
        arc(2.9f, 3.5f, 2.9f, 3.5f, 2, 318);
        return 5.8f;
    case 'g':
        arc(2.9f, 3.5f, 2.9f, 3.5f, 0, 360);
        seg(5.8f, 7, 5.8f, -0.8f);
        arc(2.9f, -0.8f, 2.9f, 2.2f, 0, -150);
        return 5.8f;
    case 'i':
        seg(0, 0, 0, 7);
        dot(0, 9.6f);
        return 0.0f;
    case 'k':
        seg(0, 0, 0, 10.5f);
        seg(4.8f, 7, 0, 2.6f);
        seg(1.7f, 4.1f, 5, 0);
        return 5.0f;
    case 'l':
        seg(0, 0, 0, 10.5f);
        return 0.0f;
    case 'n':
        seg(0, 0, 0, 7);
        arc(2.8f, 4.2f, 2.8f, 2.8f, 180, 0);
        seg(5.6f, 4.2f, 5.6f, 0);
        return 5.6f;
    case 'o':
        arc(2.9f, 3.5f, 2.9f, 3.5f, 0, 360);
        return 5.8f;
    case 'p':
        arc(2.9f, 3.5f, 2.9f, 3.5f, 0, 360);
        seg(0, 7, 0, -3);
        return 5.8f;
    case 'r':
        seg(0, 0, 0, 7);
        arc(3, 4, 3, 3, 180, 75);
        return 3.9f;
    case 's':
        arc(2.5f, 5.25f, 2.5f, 1.75f, 25, 270);
        arc(2.5f, 1.75f, 2.5f, 1.75f, 90, -155);
        return 5.0f;
    case 't':
        seg(1.2f, 9.3f, 1.2f, 1.4f);
        arc(2.8f, 1.4f, 1.6f, 1.4f, 180, 300);
        seg(0, 7, 3.4f, 7);
        return 3.8f;
    }
    return 0.0f;
}

#define LETTER_GAP 2.6f
#define WORD_GAP 6.0f

/* Text at the pen (scale 1: capitals 10 units high); returns its width */
static float text(const char *s, int draw) {
    float x0 = pen_x, w = 0;
    int first = 1;
    for (; *s; s++) {
        if (*s == ' ') {
            w += WORD_GAP;
            first = 1;
            continue;
        }
        if (!first) {
            w += LETTER_GAP;
        }
        first = 0;
        pen_x = x0 + w * pen_scale;
        if (draw) {
            w += glyph(*s);
        } else {
            int keep = nsegs;
            w += glyph(*s);
            nsegs = keep;
        }
    }
    pen_x = x0;
    return w * pen_scale;
}

/* A rounded rectangle's outline, (x, y) its bottom left */
static void round_rect(float x, float y, float w, float h, float r) {
    float s = pen_scale, ox = pen_x, oy = pen_y;
    pen_scale = 1;
    pen_x = 0;
    pen_y = 0;
    seg(x + r, y, x + w - r, y);
    seg(x + r, y + h, x + w - r, y + h);
    seg(x, y + r, x, y + h - r);
    seg(x + w, y + r, x + w, y + h - r);
    arc(x + w - r, y + h - r, r, r, 0, 90);
    arc(x + r, y + h - r, r, r, 90, 180);
    arc(x + r, y + r, r, r, 180, 270);
    arc(x + w - r, y + r, r, r, 270, 360);
    pen_scale = s;
    pen_x = ox;
    pen_y = oy;
}

/* The button's picture at x (baseline 0); returns its width */
static float button_art(int button, float x) {
    float w;
    switch (button) {
    case 2: {
        /* Xbox Menu: three lines in a circle */
        float r = 6.2f, cx = x + r, cy = 4.2f;
        int i;
        pen_x = 0;
        pen_y = 0;
        pen_scale = 1;
        pen_hw = 0.55f;
        arc(cx, cy, r, r, 0, 360);
        pen_hw = 0.5f;
        for (i = -1; i <= 1; i++) {
            seg(cx - 2.6f, cy + 2.1f * (float)i, cx + 2.6f, cy + 2.1f * (float)i);
        }
        return 2 * r;
    }
    case 1: {
        /* A key cap with "Enter" */
        pen_x = x + 2.6f;
        pen_y = 1.4f;
        pen_scale = 0.62f;
        pen_hw = 0.48f;
        w = text("Enter", 1);
        pen_hw = 0.55f;
        round_rect(x, -2.2f, w + 5.2f, 13, 2.2f);
        return w + 5.2f;
    }
    default:
        /* START in a pill, as on the pad */
        pen_x = x + 4.4f;
        pen_y = 1.35f;
        pen_scale = 0.6f;
        pen_hw = 0.5f;
        w = text("START", 1);
        pen_hw = 0.55f;
        round_rect(x, -1.5f, w + 8.8f, 11.5f, 5.75f);
        return w + 8.8f;
    }
}

static float seg_dist(const Seg *s, float x, float y) {
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0, px = x - s->x0, py = y - s->y0;
    float l = dx * dx + dy * dy, t = l > 0 ? (px * dx + py * dy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    px -= t * dx;
    py -= t * dy;
    return sqrtf(px * px + py * py);
}

static unsigned char cover(float d) {
    float c = 0.5f - d;
    c = c < 0 ? 0 : c > 1 ? 1 : c;
    return (unsigned char)(c * 255 + 0.5f);
}

/* Panel layout, units: text from 0 (baseline) to 10, the bar below */
#define PAD_X 7.0f
#define TOP 15.0f
#define BOTTOM -11.0f
#define BAR_Y -6.8f
#define BAR_HW 0.85f
#define RADIUS 6.0f

/* The picture of the segments: R text, G a rounded panel over it all, B
   the bar segments (ch 2); `width`, `top`, `bottom` in units */
static unsigned char *raster(float width, float top, float bottom, float s, int *w, int *h) {
    /* Each pixel only against the segments near it: those of its pixel
       row, then of its 32-pixel block */
    static int row_segs[MAX_SEGS], block_segs[MAX_SEGS];
    int pw, ph, i, j, k, nrow, nblock, bx;
    unsigned char *px;

    pw = (int)ceilf(width * s);
    ph = (int)ceilf((top - bottom) * s);
    px = calloc((size_t)pw * (size_t)ph, 4);
    if (px == NULL) {
        return NULL;
    }
    for (j = 0; j < ph; j++) {
        /* Pixel centres in units */
        float uy = top - ((float)j + 0.5f) / s;
        nrow = 0;
        for (k = 0; k < nsegs; k++) {
            const Seg *g = &segs[k];
            if (uy >= fminf(g->y0, g->y1) - g->hw - 1 && uy <= fmaxf(g->y0, g->y1) + g->hw + 1) {
                row_segs[nrow++] = k;
            }
        }
        for (bx = 0; bx < pw; bx += 32) {
            float bx0 = (float)bx / s - 1, bx1 = (float)(bx + 32) / s + 1;
            nblock = 0;
            for (k = 0; k < nrow; k++) {
                const Seg *g = &segs[row_segs[k]];
                if (fmaxf(g->x0, g->x1) + g->hw >= bx0 && fminf(g->x0, g->x1) - g->hw <= bx1) {
                    block_segs[nblock++] = row_segs[k];
                }
            }
            for (i = bx; i < pw && i < bx + 32; i++) {
                float ux = ((float)i + 0.5f) / s, dt = 1e9f, db = 1e9f, qx, qy, dp;
                unsigned char *o = px + ((size_t)j * (size_t)pw + (size_t)i) * 4;
                for (k = 0; k < nblock; k++) {
                    const Seg *g = &segs[block_segs[k]];
                    float d = seg_dist(g, ux, uy) - g->hw;
                    if (g->ch == 2) {
                        db = fminf(db, d);
                    } else {
                        dt = fminf(dt, d);
                    }
                }
                /* The panel: a rounded rectangle over the whole picture */
                qx = fabsf(ux - width / 2) - (width / 2 - RADIUS);
                qy = fabsf(uy - (top + bottom) / 2) - ((top - bottom) / 2 - RADIUS);
                dp = sqrtf(fmaxf(qx, 0) * fmaxf(qx, 0) + fmaxf(qy, 0) * fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - RADIUS;
                o[0] = cover(dt * s);
                o[1] = cover(dp * s + 0.5f);
                o[2] = cover(db * s);
                o[3] = 255;
            }
        }
    }
    *w = pw;
    *h = ph;
    return px;
}

unsigned char *grx_skip_art(int text_px, int kind, int button, int *w, int *h, int *bar_x0, int *bar_x1) {
    float s = (float)text_px / 10.0f, width, x, bx0, bx1;
    unsigned char *px;

    nsegs = 0;
    pen_y = 0;
    pen_scale = 1;
    pen_hw = 0.72f;
    x = PAD_X;
    if (kind == 9) {
        pen_x = x;
        x += text("Location recorded", 1);
    } else if (kind >= 2) {
        /* Fast forward: ">> n", laid out as "Skipping" is */
        char label[8] = {'>', '>', ' ', (char)('0' + kind % 10), 0};
        pen_x = x;
        x += text(label, 1);
    } else if (kind == 1) {
        pen_x = x;
        x += text("Skipping", 1);
    } else {
        pen_x = x;
        x += text("Hold", 1) + WORD_GAP + 0.6f;
        x += button_art(button, x) + WORD_GAP + 0.6f;
        pen_x = x;
        pen_y = 0;
        pen_scale = 1;
        pen_hw = 0.72f;
        x += text("to skip", 1);
    }
    width = x + PAD_X;
    /* The bar's track */
    bx0 = PAD_X + BAR_HW;
    bx1 = width - PAD_X - BAR_HW;
    if (nsegs < MAX_SEGS) {
        Seg *b = &segs[nsegs++];
        b->x0 = bx0;
        b->x1 = bx1;
        b->y0 = b->y1 = BAR_Y;
        b->hw = BAR_HW;
        b->ch = 2;
    }

    px = raster(width, TOP, BOTTOM, s, w, h);
    if (px == NULL) {
        return NULL;
    }
    *bar_x0 = (int)((bx0 - BAR_HW) * s);
    *bar_x1 = (int)ceilf((bx1 + BAR_HW) * s);
    return px;
}

/* The port menu (grx_menu.h): tabs (the current one gold), then rows "label   value", the
   selected one in gold with arrows around its value; panel and fonts as
   the prompt's */
#define MENU_ROW 19.0f
#define MENU_VALUE_GAP 14.0f
#define MENU_TAB_GAP 16.0f

unsigned char *grx_menu_art(int text_px, const char *const *tabs, int ntabs, int tab, const char *const *labels,
                            const char *const *values, int rows, int sel, int *w, int *h) {
    float s = (float)text_px / 10.0f, label_w = 0, value_w = 0, col1_w = 0, col2_w = 0, width, top, bottom, y,
          arrow = 5.0f + WORD_GAP;
    int r;

    nsegs = 0;
    pen_scale = 1;
    pen_y = 0;
    pen_hw = 0.72f;
    pen_ch = 0;
    for (r = 0; r < rows; r++) {
        float lw, vw;
        pen_x = 0;
        lw = text(labels[r], 0);
        if (values[r] != NULL && strchr(values[r], '\t') != NULL) {
            char a[64], *b;
            snprintf(a, sizeof(a), "%s", values[r]);
            b = strchr(a, '\t');
            *b++ = 0;
            pen_x = 0;
            vw = text(a, 0);
            col1_w = vw > col1_w ? vw : col1_w;
            pen_x = 0;
            vw = text(b, 0);
            col2_w = vw > col2_w ? vw : col2_w;
            vw = 0;
        } else {
            vw = values[r] != NULL ? text(values[r], 0) : 0;
        }
        label_w = lw > label_w ? lw : label_w;
        value_w = vw > value_w ? vw : value_w;
    }
    /* "a\tb" values: two columns, a's and b's, left-aligned */
    if (col1_w > 0 || col2_w > 0) {
        float cw = col1_w + MENU_VALUE_GAP + col2_w - arrow * 2;
        value_w = cw > value_w ? cw : value_w;
    }
    width = PAD_X * 2 + label_w + MENU_VALUE_GAP + arrow * 2 + value_w;
    {
        /* The tabs side by side, as wide as they need */
        float tw = 0;
        for (r = 0; r < ntabs; r++) {
            pen_x = 0;
            tw += text(tabs[r], 0) + (r > 0 ? MENU_TAB_GAP : 0);
        }
        width = PAD_X * 2 + tw > width ? PAD_X * 2 + tw : width;
    }
    top = 15.0f;
    /* The tabs, the current one gold, a rule under them */
    pen_x = PAD_X;
    pen_y = 0;
    pen_hw = 0.8f;
    for (r = 0; r < ntabs; r++) {
        float x0 = pen_x, tw;
        pen_ch = r == tab ? 2 : 0;
        pen_x = 0;
        tw = text(tabs[r], 0);
        pen_x = x0;
        text(tabs[r], 1);
        pen_x = x0 + tw + MENU_TAB_GAP;
    }
    pen_hw = 0.45f;
    {
        float x0 = pen_x, y0 = pen_y, sc = pen_scale;
        pen_x = 0;
        pen_y = 0;
        pen_scale = 1;
        seg(PAD_X, -5.5f, width - PAD_X, -5.5f);
        pen_x = x0;
        pen_y = y0;
        pen_scale = sc;
    }
    y = -5.5f - MENU_ROW;
    for (r = 0; r < rows; r++, y -= MENU_ROW) {
        pen_ch = r == sel ? 2 : 0;
        pen_hw = 0.72f;
        pen_y = y;
        pen_x = PAD_X;
        text(labels[r], 1);
        if (values[r] != NULL && strchr(values[r], '\t') != NULL) {
            float vx = PAD_X + label_w + MENU_VALUE_GAP;
            char a[64], *b;
            snprintf(a, sizeof(a), "%s", values[r]);
            b = strchr(a, '\t');
            *b++ = 0;
            pen_y = y;
            pen_x = vx;
            text(a, 1);
            pen_x = vx + col1_w + MENU_VALUE_GAP;
            text(b, 1);
        } else if (values[r] != NULL) {
            float vx = PAD_X + label_w + MENU_VALUE_GAP, vw;
            pen_x = 0;
            vw = text(values[r], 0);
            /* Values right-aligned in their column, arrows on the selected row */
            pen_x = vx + arrow + (value_w - vw);
            pen_y = y;
            text(values[r], 1);
            if (r == sel) {
                pen_x = vx + (value_w - vw);
                text("<", 1);
                pen_x = vx + arrow + value_w + WORD_GAP;
                text(">", 1);
            }
        }
    }
    pen_ch = 0;
    bottom = y + MENU_ROW - 9.0f;
    return raster(width, top, bottom, s, w, h);
}
