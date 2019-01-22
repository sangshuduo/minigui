/*
 *   This file is part of MiniGUI, a mature cross-platform windowing
 *   and Graphics User Interface (GUI) support system for embedded systems
 *   and smart IoT devices.
 *
 *   Copyright (C) 2002~2018, Beijing FMSoft Technologies Co., Ltd.
 *   Copyright (C) 1998~2002, WEI Yongming
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Or,
 *
 *   As this program is a library, any link to this program must follow
 *   GNU General Public License version 3 (GPLv3). If you cannot accept
 *   GPLv3, you need to be licensed from FMSoft.
 *
 *   If you have got a commercial license of this program, please use it
 *   under the terms and conditions of the commercial license.
 *
 *   For more information about the commercial license, please refer to
 *   <http://www.minigui.com/en/about/licensing-policy/>.
 */

/*
** glyph-css.c: The implementation of APIs which conform to the specifiction
** of CSS 3
**
** Reference:
**
**  https://www.w3.org/TR/css-text-3/
**  https://www.w3.org/TR/css-writing-modes-3/
**
** Create by WEI Yongming at 2019/01/16
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "minigui.h"
#include "gdi.h"
#include "window.h"
#include "devfont.h"
#include "cliprect.h"
#include "gal.h"
#include "internals.h"
#include "ctrlclass.h"
#include "dc.h"
#include "pixel_ops.h"
#include "cursor.h"
#include "drawtext.h"
#include "fixedmath.h"
#include "glyph.h"

#ifdef _MGCHARSET_UNICODE

#define MIN_LEN_GLYPHS      4
#define INC_LEN_GLYPHS      4

static int get_next_glyph(DEVFONT* mbc_devfont, DEVFONT* sbc_devfont,
    const char* mstr, int mstr_len, Glyph32* g, Uchar32* uc)
{
    int mchar_len = 0;

    if (mbc_devfont) {
        mchar_len = mbc_devfont->charset_ops->len_first_char
            ((const unsigned char*)mstr, mstr_len);

        if (mchar_len > 0) {
            *g = mbc_devfont->charset_ops->char_glyph_value
                (NULL, 0, (Uint8*)mstr, mchar_len);
            *g = SET_MBC_GLYPH(*g);

            if (mbc_devfont->charset_ops->conv_to_uc32)
                *uc = mbc_devfont->charset_ops->conv_to_uc32(*g);
            else
                *uc = GLYPH2UCHAR(*g);
        }
    }

    if (*g == INV_GLYPH_VALUE) {
        mchar_len = sbc_devfont->charset_ops->len_first_char
            ((const unsigned char*)mstr, mstr_len);

        if (mchar_len > 0) {
            *g = sbc_devfont->charset_ops->char_glyph_value
                (NULL, 0, (Uint8*)mstr, mchar_len);
            if (sbc_devfont->charset_ops->conv_to_uc32)
                *uc = sbc_devfont->charset_ops->conv_to_uc32(*g);
            else
                *uc = GLYPH2UCHAR(*g);
        }
    }

    return mchar_len;
}

static UCharBreakType resolve_line_breaking_class(
        LanguageCode content_language, UCharScriptType writing_system,
        UCharBasicType gc, UCharBreakType bt)
{
    /*
     * TODO: according to the content language and the writing system
     * to resolve AI, CB, CJ, SA, SG, and XX into other line breaking classes.
     */

    // default handling.
    switch (bt) {
    case UCHAR_BREAK_AMBIGUOUS:
    case UCHAR_BREAK_SURROGATE:
    case UCHAR_BREAK_UNKNOWN:
        bt = UCHAR_BREAK_ALPHABETIC;
        break;

    case UCHAR_BREAK_COMPLEX_CONTEXT:
        if (gc == UCHAR_TYPE_NON_SPACING_MARK
                || gc == UCHAR_TYPE_SPACING_MARK) {
            bt = UCHAR_BREAK_COMBINING_MARK;
        }
        else {
            bt = UCHAR_BREAK_ALPHABETIC;
        }
        break;

    case UCHAR_BREAK_CONDITIONAL_JAPANESE_STARTER:
        bt = UCHAR_BREAK_NON_STARTER;
        break;

    default:
        break;
    }

    return bt;
}

static int collapse_space(DEVFONT* mbc_devfont, DEVFONT* sbc_devfont,
    const char* mstr, int mstr_len)
{
    UCharBreakType bt;
    int cosumed = 0;

    do {
        int mchar_len;
        Glyph32 g;
        Uchar32 uc;

        mchar_len = get_next_glyph(mbc_devfont, sbc_devfont, mstr, mstr_len,
                        &g, &uc);
        if (mchar_len == 0)
            break;

        mstr += mchar_len;
        mstr_len -= mchar_len;
        cosumed += mchar_len;

        bt = UCharGetBreak(uc);
    } while (bt == UCHAR_BREAK_SPACE);

    return cosumed;
}

int GUIAPI GetGlyphsByRules(LOGFONT* logfont, const char* mstr, int mstr_len,
            LanguageCode content_language, UCharScriptType writing_system,
            Uint32 space_rule, Uint32 trans_rule,
            Glyph32** glyphs, Uint8** break_oppos, int* nr_glyphs)
{
    Glyph32* gs = NULL;
    Uint8* bs = NULL;
    int len_buff;
    int n = 0;
    int cosumed = 0;

    DEVFONT* sbc_devfont  = logfont->sbc_devfont;
    DEVFONT* mbc_devfont = logfont->mbc_devfont;

    *glyphs = NULL;
    *break_oppos = NULL;
    *nr_glyphs = 0;
    if (mstr_len == 0)
        return 0;

    // pre-allocate buffers
    len_buff = mstr_len >> 1;
    if (len_buff < MIN_LEN_GLYPHS)
        len_buff = MIN_LEN_GLYPHS;

    gs = (Glyph32*)malloc(sizeof(Glyph32) * len_buff);
    bs = (Uint8*)malloc(sizeof(Uint8) * len_buff);
    if (gs == NULL || bs == NULL)
        goto error;

    while (mstr_len > 0 && *mstr != '\0') {
        Glyph32 g = INV_GLYPH_VALUE;
        Uchar32 uc = 0;
        int mchar_len;
        UCharBasicType gc;
        UCharBreakType bt;

        mchar_len = get_next_glyph(mbc_devfont, sbc_devfont, mstr, mstr_len,
                        &g, &uc);
        if (mchar_len == 0)
            break;

        gc = UCharGetType(uc);
        bt = UCharGetBreak(uc);

        // CSS: collapses space acoording to space rule
        if ((space_rule == WSR_NORMAL || space_rule == WSR_NOWRAP)
                && bt == UCHAR_BREAK_SPACE) {
            mchar_len += collapse_space(mbc_devfont, sbc_devfont, mstr,
                mstr_len);
        }

        mstr_len -= mchar_len;
        mstr += mchar_len;
        cosumed += mchar_len;

        gs[n] = g;
        bs[n] = BOV_UNKNOWN;

        if (space_rule == WSR_PRE || space_rule == WSR_NOWRAP) {
            // only break at forced line breaks.
        }
        else {
            /* Complete Line Breaking Algorithm goes here */

            // LB1: Resolve line breaking classes
            bt = resolve_line_breaking_class(content_language, writing_system,
                    gc, bt);

            // mark all breaking opportunities
        }

        /* check and realloc buffers */
        n++;
        if (n == len_buff) {
            len_buff += INC_LEN_GLYPHS;
            gs = (Glyph32*)realloc(gs, sizeof(Glyph32) * len_buff);
            bs = (Uint8*)realloc(bs, sizeof(Uint8) * len_buff);
            if (gs == NULL || bs == NULL)
                goto error;
        }

    }

    if (n == 0) {
        if (gs) free(gs);
        if (bs) free(bs);
    }
    else {
        *glyphs = gs;
        *break_oppos = bs;
        *nr_glyphs = n;
    }

    return cosumed;

error:
    if (gs) free(gs);
    if (bs) free(bs);
    return 0;
}

PLOGFONT GUIAPI GetGlyphsExtentPointEx (LOGFONT* logfont, int x, int y,
            const Glyph32* glyphs, const Uint8* break_oppos, int nr_glyphs,
            Uint32 reander_flags, Uint32 space_rule,
            int letter_spacing, int word_spacing, int tab_size, int max_extent,
            SIZE* line_size, GLYPHEXTINFO* glyph_ext_info, GLYPHPOSORT* pos_orts,
            int* nr_to_fit)
{
    return NULL;
}

#endif /*  _MGCHARSET_UNICODE */

BOOL GUIAPI DrawGlyphStringEx (HDC hdc, const Glyph32* glyphs, int nr_glyphs,
        const GLYPHPOSORT* pos_orts, PLOGFONT logfont_sideways)
{
    return TRUE;
}

