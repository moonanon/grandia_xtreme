/* Port-only C for FldFlagListUpdate (0x00657F20), which the decompilation still has in assembly. */
#include "common.h"
#include "protos/field.h"
#include "gph.h"
#include "grxsys.h"
#include "pad.h"
#include "save.h"
#include "field/n18.h"
#include "field/n25.h"
#include "grx_mdt.h"

/* Same layout as src/field/data/670470.c */
typedef struct FldFlagListEntry {
    u16 id;
    u16 value;
} FldFlagListEntry;

extern FldFlagListEntry fldFlagListEntries[0x26];
extern const s8 fldFlagListShadowOfs[4][2];

void FldFlagListUpdate(void) {
    s8 ofs[4][2];
    u8 right;
    u8 left;
    s32 n;
    s32 i;
    s32 j;
    s32 k;
    s32 col;
    s32 row;
    s32 w;
    s32 x;
    s32 y;
    s32 top;
    s32 shR, shG, shB, b;
    u8 *lens;
    u16 **strs;
    u16 *str;
    GphBatchQuad *q;
    TxtMsgHdr *msg;
    FldFlagListEntry *e;
    s32 idx;
    s32 len;

    __builtin_memcpy(ofs, fldFlagListShadowOfs, sizeof(ofs));
    if (fldFlagListWork->open == 0) {
        return;
    }
    if (EvtScnrIsRunning() == 1) {
        return;
    }
    FldMenuCursorSetAt(2, 48.0f, (f32)(fldFlagListWork->cursor * 26) + 16.0f);
    n = fldFlagListWork->count - fldFlagListWork->unk6 * 6;
    if (n >= 6) {
        n = 6;
    }

    if (fldFlagListWork->pages >= 2 && (padPort0[0].trigger & 4)) {
        /* L1: previous page */
        if (fldFlagListWork->page != 0) {
            fldFlagListWork->page--;
        } else {
            fldFlagListWork->page = fldFlagListWork->pages - 1;
        }
        fldFlagListWork->cursor = 0;
        MenuLrArrowsFlash(1);
        SndSePlay(0x7D1, 0, 0, 0x1E, 0);
    } else if (fldFlagListWork->pages >= 2 && (padPort0[0].trigger & 8)) {
        /* R1: next page */
        if (fldFlagListWork->page < fldFlagListWork->pages - 1) {
            fldFlagListWork->page++;
        } else {
            fldFlagListWork->page = 0;
        }
        fldFlagListWork->cursor = 0;
        MenuLrArrowsFlash(0);
        SndSePlay(0x7D1, 0, 0, 0x1E, 0);
    } else if (padPort0[0].repeat & 0x1000) {
        if (fldFlagListWork->cursor != 0) {
            fldFlagListWork->cursor--;
        } else {
            fldFlagListWork->cursor = n - 1;
        }
        SndSePlay(0x7D1, 0, 0, 0x1E, 0);
    } else if (padPort0[0].repeat & 0x4000) {
        if (fldFlagListWork->cursor < n - 1) {
            fldFlagListWork->cursor++;
        } else {
            fldFlagListWork->cursor = 0;
        }
        SndSePlay(0x7D1, 0, 0, 0x1E, 0);
    } else if (padPort0[0].repeat & 0x40) {
        FldMenuWinClose(2, 0, 3);
        FldMenuCursorReset();
        fldFlagListWork->open = 0;
        SndSePlay(0x7D3, 0, 0, 0x1E, 0);
        return;
    } else if (padPort0[0].repeat & 0x20) {
        FldMenuWinClose(2, 0, 3);
        FldMenuCursorReset();
        fldFlagListWork->open = 0;
        idx = fldFlagListWork->entries[fldFlagListWork->unk6 * 6 + fldFlagListWork->cursor];
        /* SysSaveData.unk1E40. Not through sysSaveData: the bss defines
           evtMainFlag as its own object, which the port lays out on its own.
           Entries past the game's 0x26 are mods/flaglist.txt's (grx_mdt.h). */
        if (idx >= 0x26) {
            (&evtMainFlag)[2] = grx_flaglist_value(idx - 0x26) + 1;
        } else {
            e = &fldFlagListEntries[idx];
            (&evtMainFlag)[2] = e->value + 1;
        }
        EvtScnrCall(fldFlagListWork->event);
        SndSePlay(0x7D2, 0, 0, 0x1E, 0);
        return;
    }

    /* Draw the page: each glyph gets four shadow copies on the cursor line */
    y = 0;
    q = (GphBatchQuad *)fldFlagListWork->winSprites;
    msg = (TxtMsgHdr *)sysGrxsysPac->msg->tbl[1];
    lens = (u8 *)msg + msg->lenOfs;
    strs = (u16 **)((u8 *)msg + msg->strOfs);
    if (fldFlagListWork->page != fldFlagListWork->unk6) {
        fldFlagListWork->unk6 = fldFlagListWork->page;
        FldFlagListSetPage(fldFlagListWork->page);
    }
    for (i = 0; i < n; i++) {
        idx = fldFlagListWork->entries[fldFlagListWork->unk6 * 6 + i];
        if (idx >= 0x26) {
            str = grx_flaglist_glyphs(idx - 0x26, &len);
        } else {
            str = strs[idx];
            len = lens[idx];
        }
        if (i == fldFlagListWork->cursor) {
            b = 0x70;
            shR = 0x70;
            shG = 0x70;
            shB = 0;
        } else {
            b = 0x80;
            shR = 0x20;
            shG = 0x40;
            shB = 0x80;
        }
        x = 0x30;
        for (j = 0; j < len; j++) {
            if (j < 28) {
                col = j;
                row = 0;
            } else {
                col = j - 28;
                row = 1;
            }
            TxtGlyphGetMetrics(str[j], &right, &left, sysGrxsysPac->glyphTable);
            w = (s32)((f32)(right - left) * 1.125f);
            top = (i * 2 + row) * 18;
            if (i == fldFlagListWork->cursor) {
                for (k = 0; k < 4; k++) {
                    q->type = 1;
                    q->v[0].r = shR;
                    q->v[0].g = shG;
                    q->v[0].a = 0x40;
                    q->v[0].b = shB;
                    q->v[0].x = ofs[k][0] + x;
                    q->v[0].y = y + (ofs[k][1] + 6);
                    q->v[1].x = ofs[k][0] + x + w;
                    q->z = 0;
                    q->v[1].y = y + 0x16 + (ofs[k][1] + 6);
                    q->v[0].v = top;
                    q->v[0].u = left + col * 18;
                    q->v[1].v = top + 16;
                    q->v[1].u = right + col * 18;
                    q++;
                }
            }
            q->v[1].y = y + 0x1C;
            q->v[0].x = x;
            q->v[0].r = 0x80;
            q->v[1].x = x + w;
            q->type = 1;
            q->v[0].g = 0x80;
            q->v[0].a = 0x80;
            q->v[0].b = b;
            q->z = 0;
            q->v[0].y = y + 6;
            q->v[0].v = top;
            q->v[0].u = left + col * 18;
            q->v[1].v = top + 16;
            q->v[1].u = right + col * 18;
            q++;
            x += 2 + w;
        }
        y += 0x16 + 4;
    }
    q->type = 0;
}
