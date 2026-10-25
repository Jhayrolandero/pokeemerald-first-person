#include "global.h"
#include "first_person.h"

#if OW_FIRST_PERSON

#include "decompress.h"
#include "constants/event_object_movement.h"
#include "fieldmap.h"
#include "event_object_movement.h"
#include "field_message_box.h"
#include "field_player_avatar.h"
#include "field_control_avatar.h"
#include "gpu_regs.h"
#include "main.h"
#include "sprite.h"
#include "map_name_popup.h"
#include "malloc.h"
#include "metatile_behavior.h"
#include "overworld.h"
#include "palette.h"
#include "script.h"
#include "text.h"
#include "trig.h"
#include "trainer_see.h"
#include "constants/global.h"

/*
 * First-person (dungeon crawler) overworld view: textured raycaster in BG
 * Mode 4 (240x160, 8bpp paletted, two pages).
 *
 * Rendering
 *  - 120 ray columns (each drawn 2 px wide with one halfword store) x 160 rows.
 *  - Walls: the ray marches through the real map grid (collision / door
 *    behaviour). A hit cell is drawn as a prism whose facade is the real
 *    metatile art of the contiguous column of solid cells (up to 3 metatiles
 *    high for secondary-tileset buildings, 2 for primary-tileset trees/signs),
 *    so windows, doors and roofs come from the tileset. Metatiles are composed
 *    to 16x16 8bpp textures from the decompressed tileset tiles (ROM) and the
 *    tileset metatile definitions.
 *  - Floor: perspective floor casting, sampling the metatile under each pixel.
 *  - Sky: banded gradient tinted by the current time-of-day palette ratio.
 *  - Distance fog / side-face darkening are done by a (pixel, level) -> slot
 *    LUT. Mode 4 only has 255 usable colours, so the frame builds its own
 *    palette on the fly (4-4-4 bit dedupe, nearest-colour fallback when full).
 *  - Redraw only on toggle / step / turn. The back page is rendered, then the
 *    page flip and the new palette are applied together in VBlank.
 *
 * VRAM: Mode 4 pages 0 (0x06000000-0x060095FF) and 1 (0x0600A000-0x060135FF)
 *   are used. This overwrites the field's BG tiles/tilemaps and OBJ tiles in
 *   VRAM and all 256 BG palette entries, which is why leaving the view runs
 *   the regular "return to field" reload (CB2_ReturnToField).
 * Heap (one block, ~55 KB, freed when leaving; InitHeap resets the pointer):
 *   32 KB decompressed tileset tiles, 12 KB texture cache (48 metatiles),
 *   4 KB colour dedupe table, 2 KB shade LUT, palettes, per-column state.
 * OBJs: in bitmap modes OBJ tile numbers below 512 are not drawn, and field
 *   sprites all sit below 512, so no OBJ (player, NPCs, follower) is visible;
 *   OBJ is also switched off explicitly. Slice 3 plan: draw NPC/follower
 *   billboards in software into the columns (scaled sprite sheet sampling,
 *   z-tested against the per-column wall distance), no hardware OBJ needed.
 * Limitations: BG0 text boxes / menus cannot show in Mode 4, so A/B/Start/
 *   Select/R/L leave the view first (reload) and the input is swallowed.
 */

#define FP_COLS         120
#define FP_ROWS         160
#define FP_FOCAL        100
#define FP_HORIZON      80
#define FP_EYE          128           // eye height, Q8 cells
#define FP_MAX_Z        (6 * 256)
#define FP_STEP         64
#define FP_TEX_N        40
#define FP_LEVELS       8             // 4 fog levels x {front, side}
#define FP_PAGE_SIZE    0xA000
#define FP_DECOMP_HEADROOM 14000   // the tANS decoder allocates ~11 KB of scratch on top
#define FP_DECOMP_SLACK 128        // the smol decoder can write a little past the nominal output size
#define FP_WIN          17            // cached cell window around the player (+-8)
#define FP_IWRAM        ARM_FUNC __attribute__((section(".iwram.code"))) __attribute__((noinline))

struct FpCol
{
    u16 z;                            // wall distance (Q8 cells), 0xFFFF = none: billboard depth test
    s16 ytop, ybot;
    u16 zd;                           // z * 256 / FOCAL
    u8 lvl, tx, height, pad;
    const u8 *tex[3];
};

struct FpFace
{
    u8 h;
    u8 ok;
    const u8 *tex[3];                 // top metatile first
};

struct FpMem
{
    u8 tex[FP_TEX_N][256];
    u16 texId[FP_TEX_N];
    u8 texPin[FP_TEX_N];
    u8 texNext;
    u8 numSlots;
    u8 slotQ[4096];
    u8 lut[FP_LEVELS][512];
    u16 pal[256];                     // palette uploaded in VBlank
    u16 srcPal[512];                  // BG (0-255) and OBJ (256-511) palette snapshot
    u8 haze[3], skyTop[3];
    u8 hazeSlot, skySlot[FP_HORIZON];
    u8 solid[FP_WIN * FP_WIN];        // per-redraw cache: 0 unknown, 1 empty, 2 solid
    const u8 *cellTex[FP_WIN * FP_WIN];
    struct FpCol cols[FP_COLS];
    struct FpFace faces[FP_WIN * FP_WIN];
    u16 ceilHW[FP_HORIZON], floorHW[FP_ROWS - FP_HORIZON];   // smooth renderer: per-row colour (2 px)
    u16 floorBase;
    u32 tiles[];                      // decompressed primary + secondary tileset tiles (size depends on the map)
};

static struct
{
    bool8 active;      // user wants the view
    bool8 shown;       // Mode 4 is on screen
    bool8 pendingFlip;
    bool8 pendingEnter;
    u8 page;           // page being displayed
    s16 lastX, lastY;
    u8 lastDir;
    u32 lastSig;
    u8 settle;         // frames until a follow-up redraw once an object moved (so it is drawn in its resting pose)
    u8 heading;        // view direction (independent of the player's facing: stepping back does not turn)
    bool8 headingValid;
    bool8 interactPending;  // after the reload, feed one A press to the normal field code
    bool8 reshowMessage;    // a script's text box was dropped by the reload: draw it again from gStringVar4
    s8 turnQueue;      // -1 left, +1 right, applied when the player is on a tile centre
    u16 mapKey;
    bool8 dirty;
    bool8 vramLock;    // tileset animation uploads must not touch the bitmap pages
    u32 tileBytes;
    // smooth renderer (OW_FIRST_PERSON_SMOOTH)
    bool8 smooth;          // per-frame renderer: camera glides between tiles, turns are animated
    bool8 smoothReady;     // palette/caches initialised for the smooth renderer
    s32 angleQ4, targetQ4; // view angle in 1/4096 turn (north = 0, east = 1024), animated towards the target
    s32 camX, camY;        // camera position, Q8 cells
    s32 lastCamX, lastCamY, lastAngle;
    bool8 free;            // free movement (OW_FIRST_PERSON_FREE): the player has a sub-tile position and a free angle
    bool8 freeValid;
    s32 freeX, freeY;      // free position, Q8 cells (absolute tile coordinates, like the object event coords)
    s16 winX, winY;        // centre tile of the cached cell window
    s16 restX[OBJECT_EVENTS_COUNT], restY[OBJECT_EVENTS_COUNT];   // per object: sprite position minus tile*16 when standing still
    s16 lastSprX[OBJECT_EVENTS_COUNT], lastSprY[OBJECT_EVENTS_COUNT];
    u8 *sheetBuf[3];       // decompressed sprite sheets kept between frames (compressed follower / OW mon graphics)
    const void *sheetKey[3];
    u8 sheetNext;
    struct FpMem *m;
} sFp;

#define M (sFp.m)

static const u16 sFog[4] = { 0, 50, 112, 172 };

// ---------------------------------------------------------------- colours

static u8 GetSlot(u16 c)
{
    u32 q = ((c >> 1) & 15) | (((c >> 6) & 15) << 4) | (((c >> 11) & 15) << 8);
    u32 s = M->slotQ[q];

    if (s)
        return s;
    if (M->numSlots < 255)
    {
        s = ++M->numSlots;
        M->pal[s] = c;
    }
    else
    {
        s32 i, best = 0x7FFFFFFF;
        s32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

        for (i = 1; i <= 255; i++)
        {
            u16 p = M->pal[i];
            s32 dr = r - (p & 31), dg = g - ((p >> 5) & 31), db = b - ((p >> 10) & 31);
            s32 d = dr * dr + dg * dg + db * db;
            if (d < best)
            {
                best = d;
                s = i;
            }
        }
    }
    M->slotQ[q] = s;
    return s;
}

static u16 ShadeColor(u16 c, u32 lvl)
{
    u32 fog = sFog[lvl & 3];
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

    if (lvl & 4)
    {
        r = r * 190 >> 8;
        g = g * 190 >> 8;
        b = b * 190 >> 8;
    }
    r = (r * (256 - fog) + M->haze[0] * fog) >> 8;
    g = (g * (256 - fog) + M->haze[1] * fog) >> 8;
    b = (b * (256 - fog) + M->haze[2] * fog) >> 8;
    return r | (g << 5) | (b << 10);
}

static inline u32 PixSlot(u32 lvl, u32 pix)
{
    u32 s = M->lut[lvl][pix];

    if (!s)
        s = M->lut[lvl][pix] = GetSlot(ShadeColor(M->srcPal[pix], lvl));
    return s;
}

static u32 FogLevel(s32 z)
{
    return z < 512 ? 0 : z < 896 ? 1 : z < 1280 ? 2 : 3;
}

// ---------------------------------------------------------------- textures

static const u16 *MetatileData(u32 id)
{
    const struct MapLayout *l = gMapHeader.mapLayout;
    u32 nPri = GetNumMetatilesInPrimary(l);

    if (id < nPri)
        return l->primaryTileset->metatiles + id * NUM_TILES_PER_METATILE;
    return l->secondaryTileset->metatiles + (id - nPri) * NUM_TILES_PER_METATILE;
}

static void ComposeMetatile(u32 id, u8 *out)
{
    const u16 *mt = MetatileData(id);
    const u32 *tiles = M->tiles;
    u32 q, ty, tx;

    for (q = 0; q < 4; q++)
    {
        u32 eB = mt[q], eT = mt[4 + q];
        u32 palB = (eB >> 12) << 4, palT = (eT >> 12) << 4;
        u8 *dst = out + (q >> 1) * 8 * 16 + (q & 1) * 8;

        for (ty = 0; ty < 8; ty++)
        {
            u32 rowB = tiles[(eB & 0x3FF) * 8 + ((eB & 0x800) ? 7 - ty : ty)];
            u32 rowT = tiles[(eT & 0x3FF) * 8 + ((eT & 0x800) ? 7 - ty : ty)];

            for (tx = 0; tx < 8; tx++)
            {
                u32 sB = ((eB & 0x400) ? 7 - tx : tx) * 4, sT = ((eT & 0x400) ? 7 - tx : tx) * 4;
                u32 cT = (rowT >> sT) & 15, cB = (rowB >> sB) & 15;

                dst[tx] = cT ? palT + cT : (cB ? palB + cB : 0);
            }
            dst += 16;
        }
    }
}

static const u8 *GetTex(u32 id, bool32 pin)
{
    u32 i, n;

    for (i = 0; i < FP_TEX_N; i++)
    {
        if (M->texId[i] == id + 1)
        {
            if (pin)
                M->texPin[i] = 1;
            return M->tex[i];
        }
    }
    // replace round-robin, skipping pinned wall textures
    for (n = 0; n < FP_TEX_N; n++)
    {
        i = M->texNext;
        M->texNext = (M->texNext + 1) % FP_TEX_N;
        if (!M->texPin[i])
            break;
    }
    M->texId[i] = id + 1;
    M->texPin[i] = pin;
    ComposeMetatile(id, M->tex[i]);
    return M->tex[i];
}

// ---------------------------------------------------------------- map

static bool32 CellSolid(s32 x, s32 y)
{
    u32 mb = MapGridGetMetatileBehaviorAt(x, y);

    if (MetatileBehavior_IsDoor(mb))
        return TRUE;
    if (MetatileBehavior_IsSurfableWaterOrUnderwater(mb))
        return FALSE;
    return MapGridGetCollisionAt(x, y) != 0;
}

static bool32 SolidAt(s32 px, s32 py, s32 cx, s32 cy)
{
    s32 dx = cx - px + 8, dy = cy - py + 8;
    u8 *e;

    if (dx < 0 || dy < 0 || dx >= FP_WIN || dy >= FP_WIN)
        return CellSolid(cx, cy);
    e = &M->solid[dy * FP_WIN + dx];
    if (!*e)
        *e = CellSolid(cx, cy) ? 2 : 1;
    return *e == 2;
}

static void ComputeColumn(struct FpCol *col, s32 a, s32 px, s32 py, s32 fx, s32 fy, s32 rx, s32 ry)
{
    s32 z, pcx = px, pcy = py;

    col->z = 0xFFFF;
    col->ytop = col->ybot = FP_HORIZON;
    col->height = 0;

    for (z = FP_STEP; z <= FP_MAX_Z; z += FP_STEP)
    {
        s32 u = (a * z * 655) >> 16;
        s32 cx = px + ((FP_EYE + fx * z + rx * u) >> 8);
        s32 cy = py + ((FP_EYE + fy * z + ry * u) >> 8);

        if (cx == pcx && cy == pcy)
            continue;
        if (SolidAt(px, py, cx, cy))
        {
            s32 lo = z - FP_STEP, hi = z, i, hz, wx, wy, base, h, cap, frac, sgn;
            bool32 axisX, side;
            u32 nPri = GetNumMetatilesInPrimary(gMapHeader.mapLayout);

            for (i = 0; i < 5; i++)
            {
                s32 mid = (lo + hi) / 2;
                s32 mu = (a * mid * 655) >> 16;
                s32 mx = px + ((FP_EYE + fx * mid + rx * mu) >> 8);
                s32 my = py + ((FP_EYE + fy * mid + ry * mu) >> 8);
                if (mx == cx && my == cy)
                    hi = mid;
                else
                    lo = mid;
            }
            hz = hi;
            u = (a * hz * 655) >> 16;
            wx = FP_EYE + fx * hz + rx * u;
            wy = FP_EYE + fy * hz + ry * u;

            // face normal axis: the one whose cell index changed
            if (cx != pcx && cy == pcy)
                axisX = TRUE;
            else if (cy != pcy && cx == pcx)
                axisX = FALSE;
            else
                axisX = (fx != 0);
            // the face runs along the axis the normal is not on
            side = axisX ? (fx == 0) : (fy == 0);   // normal not along the view direction: darker
            frac = ((axisX ? wy : wx) & 255) >> 4;
            if (!side)
            {
                sgn = axisX ? ry : rx;              // run axis is parallel to the right vector
            }
            else
            {
                s32 onRight = ((cx - px) * rx + (cy - py) * ry) > 0;
                s32 sx = onRight ? -fx : fx, sy = onRight ? -fy : fy;
                sgn = axisX ? sy : sx;              // run axis is parallel to the view direction
            }
            col->tx = sgn > 0 ? frac : 15 - frac;

            // facade: south-most solid cell of this column, then up to `cap` cells north
            base = cy;
            for (i = 0; i < 4 && SolidAt(px, py, cx, base + 1); i++)
                base++;
            cap = (MapGridGetMetatileIdAt(cx, base) < nPri) ? 2 : 3;
            h = 1;
            while (h < cap && SolidAt(px, py, cx, base - h))
                h++;
            for (i = 0; i < h; i++)
                col->tex[i] = GetTex(MapGridGetMetatileIdAt(cx, base - i), TRUE);
            col->height = h;
            col->z = hz;
            col->zd = hz * 256 / FP_FOCAL;
            col->lvl = FogLevel(hz) + (side ? 4 : 0);
            col->ytop = FP_HORIZON - ((h * 256 - FP_EYE) * FP_FOCAL / hz);
            col->ybot = FP_HORIZON + FP_EYE * FP_FOCAL / hz;
            if (col->ytop < 0)
                col->ytop = 0;
            if (col->ybot > FP_ROWS)
                col->ybot = FP_ROWS;
            return;
        }
        pcx = cx;
        pcy = cy;
    }
}

// ---------------------------------------------------------------- render

static const u8 *FloorTex(s32 cx, s32 cy)
{
    return GetTex(MapGridGetMetatileIdAt(cx, cy), FALSE);
}

FP_IWRAM static void RenderRows(u8 *vram, s32 px, s32 py, s32 fx, s32 fy, s32 rx, s32 ry)
{
    struct FpMem *m = M;
    s32 c, y;

    for (y = 0; y < FP_ROWS; y++)
    {
        u16 *row = (u16 *)(vram + y * 240);
        s32 fz = 0, wx = 0, wy = 0, dwx = 0, dwy = 0;
        u32 floorLvl = 0;
        bool32 haze = FALSE;

        if (y >= FP_HORIZON)
        {
            fz = 25600 / (2 * (y - FP_HORIZON) + 1);
            if (fz > FP_MAX_Z)
            {
                haze = TRUE;
            }
            else
            {
                s32 du = 2 * fz * 655 >> 16, u0 = -119 * fz * 655 >> 16;

                floorLvl = FogLevel(fz);
                wx = FP_EYE + fx * fz + rx * u0;
                wy = FP_EYE + fy * fz + ry * u0;
                dwx = rx * du;
                dwy = ry * du;
            }
        }

        for (c = 0; c < FP_COLS; c++)
        {
            const struct FpCol *col = &m->cols[c];
            u32 slot;

            if (y < col->ytop)
            {
                slot = m->skySlot[y];
            }
            else if (y < col->ybot)
            {
                s32 hq = FP_EYE + (((FP_HORIZON - y) * col->zd) >> 8);
                u32 s;

                if (hq < 0)
                    hq = 0;
                if (hq > col->height * 256 - 1)
                    hq = col->height * 256 - 1;
                s = hq >> 8;
                {
                    u32 pix = col->tex[s][(15 - ((hq >> 4) & 15)) * 16 + col->tx];
                    u32 ls = m->lut[col->lvl][pix];

                    slot = ls ? ls : PixSlot(col->lvl, pix);
                }
            }
            else if (haze)
            {
                slot = m->hazeSlot;
            }
            else
            {
                s32 dx = (wx >> 8) + 8, dy = (wy >> 8) + 8;
                const u8 *t;
                u32 pix, ls;

                if ((u32)dx >= FP_WIN || (u32)dy >= FP_WIN)
                {
                    t = FloorTex(px + dx - 8, py + dy - 8);
                }
                else
                {
                    t = m->cellTex[dy * FP_WIN + dx];
                    if (!t)
                        t = m->cellTex[dy * FP_WIN + dx] = FloorTex(px + dx - 8, py + dy - 8);
                }
                pix = t[(((wy & 255) >> 4) << 4) | ((wx & 255) >> 4)];
                ls = m->lut[floorLvl][pix];
                slot = ls ? ls : PixSlot(floorLvl, pix);
            }
            row[c] = slot | (slot << 8);
            wx += dwx;
            wy += dwy;
        }
    }
}

// ---------------------------------------------------------------- billboards

struct FpBill
{
    u8 objId;
    s16 u;         // lateral offset, Q8 cells (+ = right)
    u16 z;         // depth, Q8 cells
};

// sprite shape/size -> size in tiles
static const u8 sTilesW[3][4] = { { 1, 2, 4, 8 }, { 2, 4, 4, 8 }, { 1, 1, 2, 4 } };
static const u8 sTilesH[3][4] = { { 1, 2, 4, 8 }, { 1, 1, 2, 4 }, { 2, 4, 4, 8 } };

// one screen column of a billboard (hot loop: IWRAM, ARM)
static void BillColumn(u16 *dst, const u8 *col, u32 odd, s32 syFix, s32 hInv, s32 count, s32 wt, s32 h, const u8 *lutRow, u32 palBase, u32 lvl, s32 cs, s32 rs)
{
    s32 rowStride = wt * 32;

    while (count-- > 0)
    {
        s32 sy = syFix >> 16;
        u32 nib;

        if (sy >= h)
            sy = h - 1;
        nib = col[(sy >> 3) * rowStride + (sy & 7) * 4];
        nib = odd ? nib >> 4 : nib & 15;
        if (nib)
        {
            u32 slot = lutRow[palBase + nib];

            if (!slot)
                slot = PixSlot(lvl, palBase + nib);
            if (cs == 2)
            {
                *(u32 *)dst = slot * 0x01010101;
                if (rs == 2)
                    *(u32 *)(dst + 120) = slot * 0x01010101;
            }
            else
            {
                *dst = slot | (slot << 8);
                if (rs == 2)
                    dst[120] = slot | (slot << 8);
            }
        }
        dst += 120 * rs;
        syFix += hInv * rs;
    }
}

static void DrawBillboard(u8 *vram, const struct FpBill *b, s32 cs, s32 rs)
{
    const struct ObjectEvent *oe = &gObjectEvents[b->objId];
    const struct Sprite *spr = &gSprites[oe->spriteId];
    const struct ObjectEventGraphicsInfo *info = GetObjectEventGraphicsInfo(oe->graphicsId);
    u32 shape = info->oam->shape, size = info->oam->size;
    s32 wt, ht, w, h, sw, sh, left, top, bottom, c, wInv, hInv, c0, c1;
    s32 imageValue = 0, tileOffset;
    const u8 *frame;
    u32 palBase, lvl, hflip;

    if (shape > 2)
        return;
    wt = sTilesW[shape][size];
    ht = sTilesH[shape][size];
    w = wt * 8;
    h = ht * 8;
    if (spr->anims && spr->anims[spr->animNum][spr->animCmdIndex].type >= 0)
        imageValue = spr->anims[spr->animNum][spr->animCmdIndex].frame.imageValue;
    hflip = spr->oam.matrixNum & ST_OAM_HFLIP;

    if (spr->usingSheet)
    {
        const u8 *base = info->images->data;

        // In VRAM the sheet is loaded behind one blank frame (hence the +1 in the sprite code), the source has none.
        tileOffset = imageValue;
        if (OW_GFX_COMPRESS && spr->sheetSpan)
            tileOffset = imageValue << spr->sheetSpan;
        if (tileOffset < 0)
            tileOffset = 0;
        if (IsCompressedData(info->images->data))
        {
            u32 k;

            for (k = 0; k < 3; k++)
                if (sFp.sheetBuf[k] && sFp.sheetKey[k] == info->images->data)
                    break;
            if (k == 3)
            {
                u32 total = GetDecompressedDataSize(info->images->data);

                k = sFp.sheetNext;
                sFp.sheetNext = (k + 1) % 3;
                if (sFp.sheetBuf[k])
                {
                    Free(sFp.sheetBuf[k]);
                    sFp.sheetBuf[k] = NULL;
                }
                if (total > 16384 || GetLargestFreeHeapBlock() < total + FP_DECOMP_HEADROOM || !(sFp.sheetBuf[k] = AllocUnchecked(total + FP_DECOMP_SLACK)))
                    return;
                DecompressDataWithHeaderWram(info->images->data, sFp.sheetBuf[k]);
                sFp.sheetKey[k] = info->images->data;
            }
            base = sFp.sheetBuf[k];
        }
        frame = base + tileOffset * 32;
    }
    else
    {
        frame = info->images[imageValue].data;
    }

    sw = w * 1600 / b->z;
    sh = h * 1600 / b->z;
    left = 120 + b->u * FP_FOCAL / b->z - sw / 2;
    bottom = FP_HORIZON + FP_EYE * FP_FOCAL / b->z;
    top = bottom - sh;
    palBase = 256 + spr->oam.paletteNum * 16;
    lvl = FogLevel(b->z);
    if (sw < 1 || sh < 1)
        goto done;
    wInv = (w << 16) / sw;
    hInv = (h << 16) / sh;
    c0 = left / 2;
    if (c0 < 0)
        c0 = 0;
    c0 &= ~(cs - 1);
    c1 = (left + sw) / 2 + 1;
    if (c1 > FP_COLS)
        c1 = FP_COLS;
    for (c = c0; c < c1; c += cs)
    {
        s32 sx = ((c * 2 + cs - left) * wInv) >> 16;
        s32 y0 = top < 0 ? 0 : top, y1 = bottom > FP_ROWS ? FP_ROWS : bottom;

        if (sx < 0 || sx >= w || b->z >= M->cols[c].z)
            continue;
        if (hflip)
            sx = w - 1 - sx;
        y0 &= ~(rs - 1);
        BillColumn((u16 *)(vram + y0 * 240 + c * 2), frame + (sx >> 3) * 32 + ((sx & 7) >> 1), sx & 1, (y0 - top) * hInv + (rs - 1) * hInv / 2, hInv, (y1 - y0 + rs - 1) / rs, wt, h, M->lut[lvl], palBase, lvl, cs, rs);
    }
done:
    return;
}

static u32 CollectBillboards(struct FpBill *out, s32 px, s32 py, s32 fx, s32 fy, s32 rx, s32 ry)
{
    u32 i, n = 0;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *oe = &gObjectEvents[i];
        s32 dx, dy, z, u, j;

        if (!oe->active || oe->isPlayer || oe->invisible || oe->spriteId >= MAX_SPRITES)
            continue;
        dx = (oe->currentCoords.x - px) * 256;
        dy = (oe->currentCoords.y - py) * 256;
        z = dx * fx + dy * fy;
        u = dx * rx + dy * ry;
        if (z < 128 || z > FP_MAX_Z + 128 || n >= 12)
            continue;
        // insertion sort, far to near
        for (j = n; j > 0 && out[j - 1].z < z; j--)
            out[j] = out[j - 1];
        out[j].objId = i;
        out[j].u = u;
        out[j].z = z;
        n++;
    }
    return n;
}

// cheap change signature of everything the billboards depend on (tile, facing, graphics)
static u32 ObjectSignature(void)
{
    u32 i, sig = 0;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *oe = &gObjectEvents[i];

        if (!oe->active || oe->isPlayer || oe->invisible)
            continue;
        sig += ((oe->currentCoords.x * 31 + oe->currentCoords.y) * 7 + oe->facingDirection) * 3
             + oe->graphicsId * 5 + (i + 1) * 0x10001;
    }
    return sig;
}

static void Draw(u32 page)
{
    s32 px, py, fx, fy, rx, ry, c, y;
    struct MapPosition pos;
    enum Direction dir = sFp.heading;
    u8 *vram = (u8 *)VRAM + page * FP_PAGE_SIZE;

    GetPlayerPosition(&pos);
    px = pos.x;
    py = pos.y;
    switch (dir)
    {
    case DIR_NORTH: fx = 0;  fy = -1; break;
    case DIR_EAST:  fx = 1;  fy = 0;  break;
    case DIR_WEST:  fx = -1; fy = 0;  break;
    default:        fx = 0;  fy = 1;  break;
    }
    rx = -fy;
    ry = fx;
    sFp.lastX = px;
    sFp.lastY = py;
    sFp.lastDir = dir;

    // fresh palette each frame
    memset(M->slotQ, 0, sizeof(M->slotQ));
    memset(M->lut, 0, sizeof(M->lut));
    memset(M->texPin, 0, sizeof(M->texPin));
    memset(M->solid, 0, sizeof(M->solid));
    memset(M->cellTex, 0, sizeof(M->cellTex));
    M->numSlots = 0;
    M->pal[0] = 0;
    M->hazeSlot = GetSlot(M->haze[0] | (M->haze[1] << 5) | (M->haze[2] << 10));
    for (y = 0; y < FP_HORIZON; y++)
    {
        s32 t = (y / 10) * 32 + 16;   // 8 bands, top .. horizon
        u32 r = (M->skyTop[0] * (256 - t) + M->haze[0] * t) >> 8;
        u32 g = (M->skyTop[1] * (256 - t) + M->haze[1] * t) >> 8;
        u32 b = (M->skyTop[2] * (256 - t) + M->haze[2] * t) >> 8;
        M->skySlot[y] = GetSlot(r | (g << 5) | (b << 10));
    }

    for (c = 0; c < FP_COLS; c++)
        ComputeColumn(&M->cols[c], c * 2 + 1 - 120, px, py, fx, fy, rx, ry);

    RenderRows(vram, px, py, fx, fy, rx, ry);

    CpuCopy16(&gPlttBufferFaded[256], &M->srcPal[256], 256 * 2);
    {
        struct FpBill bills[12];
        u32 n = CollectBillboards(bills, px, py, fx, fy, rx, ry), i;

        for (i = 0; i < n; i++)
            DrawBillboard(vram, &bills[i], 1, 1);
    }
    sFp.lastSig = ObjectSignature();
}

// ---------------------------------------------------------------- install

static void ScaleTint(u8 *rgb, const s32 *ratio)
{
    s32 i;

    for (i = 0; i < 3; i++)
    {
        s32 v = rgb[i] * ratio[i] >> 8;
        rgb[i] = v > 31 ? 31 : v;
    }
}

static bool32 Setup(void)
{
    const struct MapLayout *l = gMapHeader.mapLayout;
    u32 nPri = GetNumTilesInPrimary(l);
    u32 secBytes = l->secondaryTileset->isCompressed ? GetDecompressedDataSize(l->secondaryTileset->tiles) : (NUM_TILES_TOTAL - nPri) * 32;
    u32 tileBytes = (nPri * 32 + secBytes + FP_DECOMP_SLACK + 3) & ~3;
    s32 i, ratio[3], sumU[3] = {0, 0, 0}, sumF[3] = {0, 0, 0};

    if (M && tileBytes > sFp.tileBytes)
    {
        Free(M);   // a bigger tileset than the block was made for (warp): reallocate
        M = NULL;
    }
    if (!M)
    {
        if (GetLargestFreeHeapBlock() < sizeof(struct FpMem) + tileBytes + FP_DECOMP_HEADROOM + 4096)
            return FALSE;   // not enough heap: stay in the normal view instead of crashing
        M = AllocZeroedUnchecked(sizeof(struct FpMem) + tileBytes);
        if (!M)
            return FALSE;
        sFp.tileBytes = tileBytes;
    }
    else
    {
        memset(M->texId, 0, sizeof(M->texId));
        if (GetLargestFreeHeapBlock() < FP_DECOMP_HEADROOM)
            return FALSE;
    }

    if (l->primaryTileset->isCompressed)
        DecompressDataWithHeaderWram(l->primaryTileset->tiles, M->tiles);
    else
        CpuCopy32(l->primaryTileset->tiles, M->tiles, nPri * 32);
    if (l->secondaryTileset->isCompressed)
        DecompressDataWithHeaderWram(l->secondaryTileset->tiles, &M->tiles[nPri * 8]);
    else
        CpuCopy32(l->secondaryTileset->tiles, &M->tiles[nPri * 8], secBytes);

    CpuCopy16(gPlttBufferFaded, M->srcPal, 256 * 2);

    // time-of-day / weather tint = faded / unfaded over the primary palettes
    for (i = 0; i < 96; i++)
    {
        u16 u = gPlttBufferUnfaded[i], f = gPlttBufferFaded[i];
        sumU[0] += u & 31; sumU[1] += (u >> 5) & 31; sumU[2] += (u >> 10) & 31;
        sumF[0] += f & 31; sumF[1] += (f >> 5) & 31; sumF[2] += (f >> 10) & 31;
    }
    for (i = 0; i < 3; i++)
        ratio[i] = sumU[i] ? sumF[i] * 256 / sumU[i] : 256;
    M->haze[0] = 23; M->haze[1] = 27; M->haze[2] = 31;
    M->skyTop[0] = 7; M->skyTop[1] = 15; M->skyTop[2] = 30;
    ScaleTint(M->haze, ratio);
    ScaleTint(M->skyTop, ratio);
    return TRUE;
}

static void FreeSheets(void)
{
    u32 k;

    for (k = 0; k < 3; k++)
    {
        if (sFp.sheetBuf[k])
            Free(sFp.sheetBuf[k]);
        sFp.sheetBuf[k] = NULL;
    }
}

static void Release(void)
{
    FreeSheets();
    if (M)
        Free(M);
    M = NULL;
    sFp.shown = FALSE;
    sFp.vramLock = FALSE;
    sFp.pendingFlip = FALSE;
    sFp.pendingEnter = FALSE;
}

void FirstPerson_HeapReset(void)
{
    M = NULL;
    memset(sFp.sheetBuf, 0, sizeof(sFp.sheetBuf));
    sFp.shown = FALSE;
    sFp.vramLock = FALSE;
    sFp.pendingFlip = FALSE;
    sFp.pendingEnter = FALSE;
}

bool8 FirstPerson_VramLocked(void)
{
    return sFp.vramLock;
}

bool8 FirstPerson_IsActive(void)
{
    return sFp.active;
}

// ---------------------------------------------------------------- smooth renderer
/*
 * OW_FIRST_PERSON_SMOOTH: per-frame renderer (Wolf3D style) on top of the same Mode 4 double buffer and real tile movement.
 *  - The camera position is the player's real sub-tile position (sprite pixel offset while a step is in progress), so it glides
 *    between tiles; the view angle is animated ~10 ticks per 90 degrees.
 *  - 120 columns (2 px wide), DDA ray cast against the cached cell window, one wall texture column per ray (facade strips taken
 *    from the metatile cache), flat ceiling / floor with per-row fog bands (no per-pixel floor casting).
 *  - The palette is persistent (colours are only ever added) so both pages can share it; fog is quantised to 4 levels.
 *  - Billboards use the same renderer as the stepping mode.
 * Frame time is measured in gFpStats (scanlines per render, 1232 cycles each).
 */
#define FP_SMOOTH_COLW  4              // wall strip width in pixels (2 = 120 columns, 4 = 60 columns)
#define FP_SMOOTH_COLS  (240 / FP_SMOOTH_COLW)
#define FP_SMOOTH_ROWH  2              // wall rows drawn per texture sample (1 = full height detail, 2 = 80 vertical samples)
#define FP_ANGLE_STEP   102            // Q4 angle units per tick: 1024 / 102 = ~10 ticks per 90 degrees
#define FP_PLANE        307            // tan(half FOV) in Q8 (1.2 = focal length 100 px, half screen 120 px)

EWRAM_DATA u32 gFpStats[4] = {0};      // 0: last render (scanlines), 1: renders, 2: scanlines sum, 3: max scanlines

static inline s32 SinB(s32 a) { return gSineTable[a & 255]; }
static inline s32 CosB(s32 a) { return gSineTable[(a + 64) & 255]; }

static s32 HeadingBrads(enum Direction d)
{
    switch (d)
    {
    case DIR_EAST:  return 64;
    case DIR_SOUTH: return 128;
    case DIR_WEST:  return 192;
    default:        return 0;
    }
}

// Q8 cell position of an object event: its tile centre plus the sprite's sub-tile pixel offset
static void SmoothObjPos(u32 i, s32 *x, s32 *y)
{
    const struct ObjectEvent *oe = &gObjectEvents[i];
    const struct Sprite *spr = &gSprites[oe->spriteId];
    s32 tx = oe->currentCoords.x, ty = oe->currentCoords.y;

    *x = tx * 256 + 128 + (spr->x + spr->x2 - tx * 16 - sFp.restX[i]) * 16;
    *y = ty * 256 + 128 + (spr->y + spr->y2 - ty * 16 - sFp.restY[i]) * 16;
}

// keep each object's "standing still" sprite offset up to date (the offset while moving is relative to it)
static void SmoothTrackObjects(void)
{
    u32 i;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *oe = &gObjectEvents[i];
        const struct Sprite *spr;
        s32 sx, sy;

        if (!oe->active || oe->spriteId >= MAX_SPRITES)
            continue;
        spr = &gSprites[oe->spriteId];
        sx = spr->x + spr->x2;
        sy = spr->y + spr->y2;
        if (sx == sFp.lastSprX[i] && sy == sFp.lastSprY[i])
        {
            sFp.restX[i] = sx - oe->currentCoords.x * 16;
            sFp.restY[i] = sy - oe->currentCoords.y * 16;
        }
        sFp.lastSprX[i] = sx;
        sFp.lastSprY[i] = sy;
    }
}

static void SmoothCamera(void)
{
    SmoothTrackObjects();
    if (sFp.free && sFp.freeValid)
    {
        sFp.camX = sFp.freeX;
        sFp.camY = sFp.freeY;
    }
    else
        SmoothObjPos(gPlayerAvatar.objectEventId, &sFp.camX, &sFp.camY);
}

// change signature of the objects that can be seen from the current camera (in front, in range, inside the field of view)
static u32 SmoothObjectSignature(void)
{
    u32 i, sig = 0;
    s32 a = (sFp.angleQ4 >> 4) & 255;
    s32 dirX = SinB(a), dirY = -CosB(a), rightX = CosB(a), rightY = SinB(a);

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *oe = &gObjectEvents[i];
        const struct Sprite *spr;
        s32 ox, oy, dx, dy, z, u;

        if (!oe->active || oe->isPlayer || oe->invisible || oe->spriteId >= MAX_SPRITES)
            continue;
        SmoothObjPos(i, &ox, &oy);
        dx = ox - sFp.camX;
        dy = oy - sFp.camY;
        z = (dx * dirX + dy * dirY) >> 8;
        u = (dx * rightX + dy * rightY) >> 8;
        if (z < 128 || z > FP_MAX_Z + 128 || (u < 0 ? -u : u) > z * 2)
            continue;
        spr = &gSprites[oe->spriteId];
        sig += (spr->x + spr->x2) * 31 + (spr->y + spr->y2) * 7 + oe->facingDirection + oe->graphicsId * 5 + (i + 1) * 0x10001
             + spr->animCmdIndex * 13 + spr->animNum * 3;
    }
    return sig;
}

static const struct FpFace *GetFace(s32 cx, s32 cy)
{
    static struct FpFace sTemp;
    s32 dx = cx - sFp.winX + 8, dy = cy - sFp.winY + 8;
    struct FpFace *f = &sTemp;
    s32 base = cy, h, i, cap;
    u32 nPri = GetNumMetatilesInPrimary(gMapHeader.mapLayout);
    const u8 *tex[3];

    if (dx >= 0 && dy >= 0 && dx < FP_WIN && dy < FP_WIN)
    {
        f = &M->faces[dy * FP_WIN + dx];
        if (f->ok)
            return f;
    }
    // facade = south-most solid cell of this column, then up to `cap` cells north
    for (i = 0; i < 4 && SolidAt(sFp.winX, sFp.winY, cx, base + 1); i++)
        base++;
    cap = (MapGridGetMetatileIdAt(cx, base) < nPri) ? 2 : 3;
    h = 1;
    while (h < cap && SolidAt(sFp.winX, sFp.winY, cx, base - h))
        h++;
    for (i = 0; i < h; i++)
        tex[i] = GetTex(MapGridGetMetatileIdAt(cx, base - i), TRUE);
    for (i = 0; i < h; i++)
        f->tex[i] = tex[h - 1 - i];
    f->h = h;
    f->ok = TRUE;
    return f;
}

static u16 AverageFloorColor(s32 tx, s32 ty)
{
    const u8 *t = GetTex(MapGridGetMetatileIdAt(tx, ty), FALSE);
    u32 i, n = 0, r = 0, g = 0, b = 0;

    for (i = 0; i < 256; i += 3)
    {
        u16 c;

        if (!t[i])
            continue;
        c = M->srcPal[t[i]];
        r += c & 31;
        g += (c >> 5) & 31;
        b += (c >> 10) & 31;
        n++;
    }
    if (!n)
        return 0x2A52;
    return (r / n) | ((g / n) << 5) | ((b / n) << 10);
}

// ceiling / floor colours per row (fog bands)
static void SmoothBuildRows(void)
{
    s32 y;
    u32 fr = M->floorBase & 31, fg = (M->floorBase >> 5) & 31, fb = (M->floorBase >> 10) & 31;

    for (y = 0; y < FP_HORIZON; y++)
    {
        s32 t = (y / 10) * 32 + 16;
        u32 r = (M->skyTop[0] * (256 - t) + M->haze[0] * t) >> 8;
        u32 g = (M->skyTop[1] * (256 - t) + M->haze[1] * t) >> 8;
        u32 b = (M->skyTop[2] * (256 - t) + M->haze[2] * t) >> 8;
        u32 s = GetSlot(r | (g << 5) | (b << 10));

        M->ceilHW[y] = s | (s << 8);
    }
    for (y = FP_HORIZON; y < FP_ROWS; y++)
    {
        s32 band = (y - FP_HORIZON) / 6, yc = FP_HORIZON + band * 6 + 3;
        s32 fz = 25600 / (2 * (yc - FP_HORIZON) + 1);
        u32 f = fz > FP_MAX_Z ? 230 : fz * 230 / FP_MAX_Z;
        u32 r = (fr * (256 - f) + M->haze[0] * f) >> 8;
        u32 g = (fg * (256 - f) + M->haze[1] * f) >> 8;
        u32 b = (fb * (256 - f) + M->haze[2] * f) >> 8;
        u32 s = GetSlot(r | (g << 5) | (b << 10));

        M->floorHW[y - FP_HORIZON] = s | (s << 8);
    }
}

static void SmoothResetWindow(s32 tx, s32 ty)
{
    memset(M->solid, 0, sizeof(M->solid));
    memset(M->faces, 0, sizeof(M->faces));
    memset(M->texPin, 0, sizeof(M->texPin));
    sFp.winX = tx;
    sFp.winY = ty;
    M->floorBase = AverageFloorColor(tx, ty);
    SmoothBuildRows();
}

static void SmoothInit(void)
{
    u32 i;

    memset(M->slotQ, 0, sizeof(M->slotQ));
    memset(M->lut, 0, sizeof(M->lut));
    memset(M->texId, 0, sizeof(M->texId));
    M->numSlots = 0;
    M->pal[0] = 0;
    CpuCopy16(&gPlttBufferFaded[256], &M->srcPal[256], 256 * 2);
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *oe = &gObjectEvents[i];

        sFp.lastSprX[i] = sFp.lastSprY[i] = -32768;
        if (oe->active && oe->spriteId < MAX_SPRITES)
        {
            const struct Sprite *spr = &gSprites[oe->spriteId];

            sFp.restX[i] = spr->x + spr->x2 - oe->currentCoords.x * 16;
            sFp.restY[i] = spr->y + spr->y2 - oe->currentCoords.y * 16;
        }
    }
    sFp.winX = -1000;
    sFp.smoothReady = TRUE;
}

// Background: ceiling / floor rows are identical across the screen, so they are filled row-run by row-run (BIOS fill, ~25 calls)
static void FillBackground(u8 *vram)
{
    s32 y = 0;

    while (y < FP_ROWS)
    {
        u16 v = y < FP_HORIZON ? M->ceilHW[y] : M->floorHW[y - FP_HORIZON];
        s32 n = 1;

        while (y + n < FP_ROWS && (y + n < FP_HORIZON ? M->ceilHW[y + n] : M->floorHW[y + n - FP_HORIZON]) == v && y + n != FP_HORIZON)
            n++;
        CpuFastFill(v | (v << 16), vram + y * 240, n * 240);
        y += n;
    }
}

// One wall strip (FP_SMOOTH_COLW pixels wide), rows y0..y1-1: the hot loop, kept in IWRAM.
FP_IWRAM static void WallStrip(u8 *dst, s32 count, const struct FpFace *face, s32 tx, s32 pos, s32 step, const u8 *lutRow, u32 lvl)
{
    s32 tmax = face->h * 16 - 1;

    while (count-- > 0)
    {
        s32 t = pos >> 16;
        u32 pix, slot;

        if (t > tmax)
            t = tmax;
        pix = face->tex[t >> 4][((t & 15) << 4) + tx];
        slot = lutRow[pix];
        if (!slot)
            slot = PixSlot(lvl, pix);
#if FP_SMOOTH_COLW == 4
        *(u32 *)dst = slot * 0x01010101;
#if FP_SMOOTH_ROWH == 2
        *(u32 *)(dst + 240) = slot * 0x01010101;
#endif
#else
        *(u16 *)dst = slot * 0x0101;
#if FP_SMOOTH_ROWH == 2
        *(u16 *)(dst + 240) = slot * 0x0101;
#endif
#endif
        dst += 240 * FP_SMOOTH_ROWH;
        pos += step * FP_SMOOTH_ROWH;
    }
}

static void DrawSmooth(u32 page)
{
    u8 *vram = (u8 *)VRAM + page * FP_PAGE_SIZE;
    s32 a = (sFp.angleQ4 >> 4) & 255;
    s32 dirX = SinB(a), dirY = -CosB(a), rightX = CosB(a), rightY = SinB(a);
    s32 planeX = rightX * FP_PLANE >> 8, planeY = rightY * FP_PLANE >> 8;
    s32 camX = sFp.camX, camY = sFp.camY;
    s32 fracX = camX & 255, fracY = camY & 255;
    s32 c;
    struct MapPosition pos;

    if (!sFp.smoothReady)
        SmoothInit();
    GetPlayerPosition(&pos);
    if (pos.x != sFp.winX || pos.y != sFp.winY)
        SmoothResetWindow(pos.x, pos.y);

    FillBackground(vram);
    for (c = 0; c < FP_SMOOTH_COLS; c++)
    {
        s32 camx = (((c * FP_SMOOTH_COLW + FP_SMOOTH_COLW / 2 - 120) * 546) >> 8);
        s32 rayX = dirX + (planeX * camx >> 8), rayY = dirY + (planeY * camx >> 8);
        s32 mapX = camX >> 8, mapY = camY >> 8;
        s32 deltaX = rayX ? 65536 / (rayX < 0 ? -rayX : rayX) : 0x3FFFFFFF;
        s32 deltaY = rayY ? 65536 / (rayY < 0 ? -rayY : rayY) : 0x3FFFFFFF;
        s32 stepX = rayX < 0 ? -1 : 1, stepY = rayY < 0 ? -1 : 1;
        s32 sideX = rayX < 0 ? (fracX * deltaX) >> 8 : ((256 - fracX) * deltaX) >> 8;
        s32 sideY = rayY < 0 ? (fracY * deltaY) >> 8 : ((256 - fracY) * deltaY) >> 8;
        s32 side = 0, i, perp = 0;
        bool32 hit = FALSE;
        u16 zFar = 0xFFFF;

        for (i = 0; i < 9; i++)
        {
            if (sideX < sideY)
            {
                sideX += deltaX;
                mapX += stepX;
                side = 0;
            }
            else
            {
                sideY += deltaY;
                mapY += stepY;
                side = 1;
            }
            if (SolidAt(sFp.winX, sFp.winY, mapX, mapY))
            {
                hit = TRUE;
                break;
            }
        }
        if (hit)
        {
            const struct FpFace *face = GetFace(mapX, mapY);
            s32 u, tx, ytopQ8, ybotQ8, step, p0, lvl;

            perp = side == 0 ? sideX - deltaX : sideY - deltaY;
            if (perp < 40)
                perp = 40;
            if (side == 0)
            {
                tx = ((camY + (perp * rayY >> 8)) & 255) >> 4;
                if (rayX < 0)
                    tx = 15 - tx;
            }
            else
            {
                tx = ((camX + (perp * rayX >> 8)) & 255) >> 4;
                if (rayY > 0)
                    tx = 15 - tx;
            }
            u = 6553600 / perp;                                   // pixels per cell, Q8
            ybotQ8 = FP_HORIZON * 256 + u / 2;
            ytopQ8 = FP_HORIZON * 256 - u * (2 * face->h - 1) / 2;
            step = 268435456 / u;                                 // texels per pixel, Q16
            {
                s32 y0 = ytopQ8 < 0 ? 0 : ytopQ8 >> 8;

                y0 &= ~(FP_SMOOTH_ROWH - 1);
                p0 = (s32)(((s64)((y0 * 256 + 128 * FP_SMOOTH_ROWH) - ytopQ8) * step) >> 8);
                if (p0 < 0)
                    p0 = 0;
            }
            lvl = FogLevel(perp) + (side ? 4 : 0);
            zFar = perp;
            {
                s32 y0 = ytopQ8 >> 8, y1 = ybotQ8 >> 8;

                if (y0 < 0)
                    y0 = 0;
                y0 &= ~(FP_SMOOTH_ROWH - 1);
                if (y1 > FP_ROWS)
                    y1 = FP_ROWS;
                if (y1 > y0)
                    WallStrip(vram + y0 * 240 + c * FP_SMOOTH_COLW, (y1 - y0 + FP_SMOOTH_ROWH - 1) / FP_SMOOTH_ROWH, face, tx, p0, step, M->lut[lvl], lvl);
            }
        }
        for (i = 0; i < FP_SMOOTH_COLW / 2; i++)
            M->cols[c * (FP_SMOOTH_COLW / 2) + i].z = zFar;
    }

    // billboards (same renderer as the stepping view)
    {
        struct FpBill bills[12];
        u32 n = 0, k, j;

        for (k = 0; k < OBJECT_EVENTS_COUNT; k++)
        {
            const struct ObjectEvent *oe = &gObjectEvents[k];
            s32 ox, oy, dx, dy, z, u;

            if (!oe->active || oe->isPlayer || oe->invisible || oe->spriteId >= MAX_SPRITES || n >= 12)
                continue;
            SmoothObjPos(k, &ox, &oy);
            dx = ox - camX;
            dy = oy - camY;
            z = (dx * dirX + dy * dirY) >> 8;
            u = (dx * rightX + dy * rightY) >> 8;
            if (z < 128 || z > FP_MAX_Z + 128)
                continue;
            for (j = n; j > 0 && bills[j - 1].z < z; j--)
                bills[j] = bills[j - 1];
            bills[j].objId = k;
            bills[j].u = u;
            bills[j].z = z;
            n++;
        }
        CpuCopy16(&gPlttBufferFaded[256], &M->srcPal[256], 256 * 2);
        for (k = 0; k < n; k++)
            DrawBillboard(vram, &bills[k], FP_SMOOTH_COLW / 2, FP_SMOOTH_ROWH);
    }
    sFp.lastCamX = camX;
    sFp.lastCamY = camY;
    sFp.lastAngle = sFp.angleQ4;
    sFp.lastSig = SmoothObjectSignature();
}

static u16 DirToKey(enum Direction d);
static bool32 LeaveView(bool32 keepWanted, bool32 continueScript);

// ---------------------------------------------------------------- free movement
/*
 * OW_FIRST_PERSON_FREE: the player has a sub-tile position (sFp.freeX/Y, a static, no SaveBlock data) and a free angle.
 *  - Collision: a small box (radius FP_FREE_RADIUS) against the real map. Every tile the box would newly touch is asked the game's
 *    own GetCollisionAtCoords() (impassable / water / elevation / ledges / other object events), per axis so walls are slid along.
 *  - Triggers: whenever the position enters a new tile the real player object event is moved there (MoveObjectEventToMapCoords) and
 *    for that tick the avatar state is set to "finished a step" (T_TILE_CENTER + MOVING), so the normal FieldGetPlayerInput /
 *    ProcessPlayerFieldInput path does the step counter, coord events, warps, wild encounters, trainer sight and so on.
 *    Holding Up also passes the facing direction key, which the door / arrow warp and sign checks need; PlayerStep itself never
 *    sees it (FirstPerson_FreeMovement), so the avatar is never walked by the normal movement code.
 */
#define FP_FREE_RADIUS  64             // Q8 cells (0.25)
#define FP_FREE_WALK    30             // Q8 cells per tick (~3.5 tiles/s at 30 ticks/s)
#define FP_FREE_RUN     56
#define FP_FREE_TURN    80             // Q4 angle units per tick (5 brads: a full turn in ~1.7 s)

static bool32 FreeBoxBlocked(s32 x, s32 y, enum Direction dir)
{
    struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
    s32 cx0 = (x - FP_FREE_RADIUS) >> 8, cx1 = (x + FP_FREE_RADIUS - 1) >> 8;
    s32 cy0 = (y - FP_FREE_RADIUS) >> 8, cy1 = (y + FP_FREE_RADIUS - 1) >> 8;
    s32 cx, cy;

    for (cy = cy0; cy <= cy1; cy++)
    {
        for (cx = cx0; cx <= cx1; cx++)
        {
            if (cx == pl->currentCoords.x && cy == pl->currentCoords.y)
                continue;
            if (GetCollisionAtCoords(pl, cx, cy, dir) != COLLISION_NONE)
                return TRUE;
        }
    }
    return FALSE;
}

static void FreeSyncToPlayer(void)
{
    const struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];

    if (!sFp.freeValid || (sFp.freeX >> 8) != pl->currentCoords.x || (sFp.freeY >> 8) != pl->currentCoords.y)
    {
        // first use, or the player was moved by something else (warp, map connection, script): restart at the tile centre
        sFp.freeX = pl->currentCoords.x * 256 + 128;
        sFp.freeY = pl->currentCoords.y * 256 + 128;
        sFp.freeValid = TRUE;
    }
}

static void FreeStep(s32 f, s32 s, bool32 run)
{
    struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
    s32 a = (sFp.angleQ4 >> 4) & 255;
    s32 fwdX = SinB(a), fwdY = -CosB(a), rightX = CosB(a), rightY = SinB(a);
    s32 sp = run ? FP_FREE_RUN : FP_FREE_WALK;
    s32 vx = fwdX * f + rightX * s, vy = fwdY * f + rightY * s;
    s32 dx, dy, nx, ny;

    if (f && s)
    {
        vx = vx * 181 >> 8;
        vy = vy * 181 >> 8;
    }
    dx = vx * sp >> 8;
    dy = vy * sp >> 8;
    if (dx)
    {
        nx = sFp.freeX + dx;
        if (!FreeBoxBlocked(nx, sFp.freeY, dx > 0 ? DIR_EAST : DIR_WEST))
            sFp.freeX = nx;
    }
    if (dy)
    {
        ny = sFp.freeY + dy;
        if (!FreeBoxBlocked(sFp.freeX, ny, dy > 0 ? DIR_SOUTH : DIR_NORTH))
            sFp.freeY = ny;
    }
    if ((sFp.freeX >> 8) != pl->currentCoords.x || (sFp.freeY >> 8) != pl->currentCoords.y)
    {
        // entered a new tile: move the real player there and let the normal "finished a step" hooks run this tick
        s32 tdx = (sFp.freeX >> 8) - pl->currentCoords.x, tdy = (sFp.freeY >> 8) - pl->currentCoords.y;

        MoveObjectEventToMapCoords(pl, sFp.freeX >> 8, sFp.freeY >> 8);
        ObjectEventUpdateElevation(pl, &gSprites[pl->spriteId]);
        // the field camera normally follows the player's sprite one tile at a time; do the same tile step by hand so that
        // gSaveBlock1Ptr->pos, map connections (route changes) and object event spawning / despawning stay in step
        if (tdx >= -1 && tdx <= 1 && tdy >= -1 && tdy <= 1)
        {
            CameraMove(tdx, tdy);
            UpdateObjectEventsForCameraUpdate(tdx, tdy);
            if (gCamera.active)
            {
                // a map connection was crossed: the new map has its own coordinate origin
                sFp.freeX += gCamera.x * 256;
                sFp.freeY += gCamera.y * 256;
            }
        }
        gPlayerAvatar.tileTransitionState = T_TILE_CENTER;
        gPlayerAvatar.runningState = MOVING;
    }
}

static enum Direction AngleToHeading(s32 angleQ4)
{
    switch ((((angleQ4 >> 4) + 32) >> 6) & 3)
    {
    case 1:  return DIR_EAST;
    case 2:  return DIR_SOUTH;
    case 3:  return DIR_WEST;
    default: return DIR_NORTH;
    }
}

// Called every tick while the free view is shown: turns, strafes and walks, and rewrites the keys for the normal field input code.
static void FreeInput(u16 *newKeys, u16 *heldKeys, u16 nk, u16 hk, bool32 locked)
{
    u16 strip = DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT | L_BUTTON | R_BUTTON;
    s32 f = (hk & DPAD_UP) ? 1 : (hk & DPAD_DOWN) ? -1 : 0;
    s32 s = (hk & R_BUTTON) ? 1 : (hk & L_BUTTON) ? -1 : 0;
    enum Direction heading;

    FreeSyncToPlayer();
    if (!locked)
    {
        if (hk & DPAD_LEFT)
            sFp.angleQ4 -= FP_FREE_TURN;
        if (hk & DPAD_RIGHT)
            sFp.angleQ4 += FP_FREE_TURN;
        sFp.angleQ4 &= 4095;
        sFp.targetQ4 = sFp.angleQ4;
        heading = AngleToHeading(sFp.angleQ4);
        if (heading != sFp.heading || GetPlayerFacingDirection() != heading)
        {
            sFp.heading = heading;
            ObjectEventTurn(&gObjectEvents[gPlayerAvatar.objectEventId], heading);
        }
        if (f || s)
            FreeStep(f, s, (hk & B_BUTTON) != 0);
    }
    *newKeys &= ~strip;
    *heldKeys &= ~strip;
    if (f > 0 && !locked)
    {
        *heldKeys |= DirToKey(sFp.heading);   // for door / arrow warps and signposts (never reaches PlayerStep)
        if (nk & DPAD_UP)
            *newKeys |= DirToKey(sFp.heading);
    }
}

bool8 FirstPerson_FreeMovement(void)
{
    return sFp.shown && sFp.free;
}

void FirstPerson_EndOfInput(void)
{
    // The step was only emulated for this tick. If the field input started a script (trainer sight, warp...) instead of calling
    // PlayerStep, the avatar would stay in MOVING and scripts waiting for IsPlayerStandingStill() would never continue.
    if (sFp.free && gPlayerAvatar.runningState == MOVING)
    {
        gPlayerAvatar.runningState = NOT_MOVING;
        gPlayerAvatar.tileTransitionState = T_NOT_MOVING;
    }
    // A script was just started by the field input (trainer sight, coord event, bg event...). It has not run yet: park it and
    // reload now. One frame later it would already have created tasks (trainer approach, movement) that the reload destroys.
    // (not for trainer sight: its approach runs on tasks and object movement that the reload would destroy; it is parked later,
    // when its first text box opens)
    if (sFp.shown && ScriptContext_IsEnabled() && gNoOfApproachingTrainers == 0)
        LeaveView(TRUE, TRUE);
}

static void RenderBackPage(bool32 entering)
{
    u32 page = entering ? 0 : sFp.page ^ 1;

    if (entering)
    {
        sFp.vramLock = TRUE;
        REG_DISPCNT |= DISPCNT_FORCED_BLANK;   // the field BGs would show garbage while VRAM is overwritten
    }
    {
        u32 t0 = gMain.vblankCounter1 * 228 + REG_VCOUNT;
        s32 lines;

        if (sFp.smooth)
            DrawSmooth(page);
        else
        {
            sFp.smoothReady = FALSE;
            Draw(page);
        }
        lines = (s32)(gMain.vblankCounter1 * 228 + REG_VCOUNT - t0);
        if (lines < 0)
            lines = 0;   // VCOUNT / frame counter read raced a VBlank
        gFpStats[0] = lines;
        gFpStats[1]++;
        gFpStats[2] += lines;
        if ((u32)lines > gFpStats[3])
            gFpStats[3] = lines;
    }
    sFp.page = page;
    sFp.pendingEnter = entering;
    sFp.pendingFlip = TRUE;
    sFp.dirty = FALSE;
}

static enum Direction RotateHeading(enum Direction d, s32 turn)
{
    // turn < 0: left (N -> W -> S -> E), turn > 0: right (N -> E -> S -> W)
    static const u8 sLeft[5] = { 0, DIR_EAST, DIR_WEST, DIR_SOUTH, DIR_NORTH };
    static const u8 sRight[5] = { 0, DIR_WEST, DIR_EAST, DIR_NORTH, DIR_SOUTH };

    return turn < 0 ? sLeft[d] : sRight[d];
}

static u16 DirToKey(enum Direction d)
{
    switch (d)
    {
    case DIR_NORTH: return DPAD_UP;
    case DIR_WEST:  return DPAD_LEFT;
    case DIR_EAST:  return DPAD_RIGHT;
    default:        return DPAD_DOWN;
    }
}

static enum Direction OppositeDir(enum Direction d)
{
    return RotateHeading(RotateHeading(d, 1), 1);
}

// Leave the view and reload the normal field. keepWanted: the view comes back by itself afterwards.
// continueScript: a script has just started; park it while the field reloads and let it run in the normal view
// (same pattern as the menus use: ScriptContext_Stop + CB2_ReturnToFieldContinueScript).
static bool32 LeaveView(bool32 keepWanted, bool32 continueScript)
{
    bool32 wasShown = sFp.shown || sFp.pendingFlip;

    sFp.active = keepWanted;
    if (!keepWanted)
        sFp.headingValid = FALSE;
    sFp.turnQueue = 0;
    Release();
    if (!wasShown)
        return FALSE;
    // CB2_ReturnToField wipes the whole heap (MoveSaveBlocks_ResetHeap in ResetMirageTowerAndSaveBlockPtrs). Anything still
    // referenced from a static must be released first: the text printer list (a script's message may have just started
    // printing) would otherwise be freed after the wipe and trip the Free() assert, hanging in CB2_ReturnToFieldLocal.
    DeactivateAllTextPrinters();
    // also free the field's BG tilemap buffers and windows, like the menus do
    CleanupOverworldWindowsAndTilemaps();
    if (continueScript)
    {
        ScriptContext_Stop();
        SetMainCallback2(CB2_ReturnToFieldContinueScript);
    }
    else
    {
        gFieldCallback = NULL;
        SetMainCallback2(CB2_ReturnToField);
    }
    return TRUE;
}

bool32 FirstPerson_Update(u16 *newKeys, u16 *heldKeys)
{
    struct MapPosition pos;
    u8 trans = gPlayerAvatar.tileTransitionState;
    bool32 atCenter = (trans == T_TILE_CENTER || trans == T_NOT_MOVING);
    bool32 canAct = !ArePlayerFieldControlsLocked() && atCenter;
    u16 nk = *newKeys, hk = *heldKeys;

    if (sFp.reshowMessage && gMain.callback2 == CB2_Overworld && !ScriptContext_IsEnabled())
    {
        // the field is back and the parked script has not been re-enabled yet: put its text box back
        sFp.reshowMessage = FALSE;
        ShowFieldMessageFromBuffer();
    }

    if (sFp.interactPending)
    {
        // back from the reload: hand the A press to the normal field code once the screen is up
        if (gPaletteFade.active || ArePlayerFieldControlsLocked() || gMain.callback2 != CB2_Overworld)
            return FALSE;
        sFp.interactPending = FALSE;
        *newKeys |= A_BUTTON;
        return FALSE;
    }

    if (!sFp.active)
    {
        if (!(canAct && (nk & L_BUTTON)))
            return FALSE;
        sFp.active = TRUE;
        sFp.headingValid = FALSE;
        sFp.free = OW_FIRST_PERSON_FREE;
        sFp.freeValid = FALSE;
        sFp.smooth = OW_FIRST_PERSON_SMOOTH || OW_FIRST_PERSON_FREE;
    }
    else if (canAct && !sFp.free && (nk & L_BUTTON))
    {
        return LeaveView(FALSE, FALSE);
    }
    else if (sFp.shown && canAct && (nk & SELECT_BUTTON) && !(nk & (START_BUTTON | R_BUTTON)))
    {
        // runtime switch: free movement -> smooth -> stepping -> free movement
        if (sFp.free)
            sFp.free = FALSE;
        else if (sFp.smooth)
            sFp.smooth = FALSE;
        else
        {
            sFp.smooth = TRUE;
            sFp.free = OW_FIRST_PERSON_FREE;
        }
        sFp.smoothReady = FALSE;
        sFp.freeValid = FALSE;
        sFp.heading = AngleToHeading(sFp.angleQ4);
        sFp.angleQ4 = sFp.targetQ4 = HeadingBrads(sFp.heading) * 16;
        sFp.dirty = TRUE;
        *newKeys &= ~SELECT_BUTTON;
        nk &= ~SELECT_BUTTON;
    }
    else if (sFp.shown && canAct && (nk & (START_BUTTON | SELECT_BUTTON | R_BUTTON)) && !(sFp.free && !(nk & (START_BUTTON | SELECT_BUTTON))))
    {
        // menus need BG0, which Mode 4 does not have: leave, swallow the press
        return LeaveView(FALSE, FALSE);
    }
    else if (sFp.shown && canAct && (nk & A_BUTTON))
    {
        // talk / read: face the view direction, and if something is there leave, run the interaction, come back
        ObjectEventTurn(&gObjectEvents[gPlayerAvatar.objectEventId], sFp.heading);
        *newKeys &= ~A_BUTTON;
        if (HasInteractionAheadOfPlayer())
        {
            sFp.interactPending = TRUE;
            if (!LeaveView(TRUE, FALSE))
                sFp.interactPending = FALSE;
            return TRUE;
        }
    }

    if (sFp.shown && (REG_DISPCNT & 7) != 4)
    {
        // the field reinitialised the screen (battle, warp, menu): reinstall below
        sFp.vramLock = FALSE;
        sFp.shown = FALSE;
    }

    if (sFp.shown && ((ScriptContext_IsEnabled() && gNoOfApproachingTrainers == 0) || !IsFieldMessageBoxHidden()))
    {
        // A script started by itself (trainer, step trigger, map script...) or a text box opened: BG0 is not available in
        // Mode 4, so run it in the normal view and come back afterwards. Reloading mid-script and re-entering the view while
        // the trigger is still true used to loop forever (view on / reload / view on ...), hence the script is parked, not dropped.
        bool32 script = ScriptContext_IsEnabled();

        if (!IsFieldMessageBoxHidden())
            sFp.reshowMessage = TRUE;   // gStringVar4 still holds the expanded text
        return LeaveView(script, script);
    }

    if (sFp.shown && sFp.free)
        FreeInput(newKeys, heldKeys, nk, hk, !canAct && ArePlayerFieldControlsLocked());
    else if (sFp.shown)
    {
        // Up = step along the view direction, Down = step back, Left/Right = turn the view only
        u16 dpad = DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT;

        bool32 turning = FALSE;

        if (sFp.smooth)
        {
            // animate the view angle towards the heading; no stepping while it is still turning
            s32 diff = sFp.targetQ4 - sFp.angleQ4;

            if (diff)
            {
                sFp.angleQ4 += diff > FP_ANGLE_STEP ? FP_ANGLE_STEP : diff < -FP_ANGLE_STEP ? -FP_ANGLE_STEP : diff;
                if (sFp.angleQ4 == sFp.targetQ4)
                {
                    sFp.angleQ4 &= 4095;
                    sFp.targetQ4 = sFp.angleQ4;
                }
                else
                    turning = TRUE;
            }
            if (turning)
            {
                hk &= ~(DPAD_UP | DPAD_DOWN);
                nk &= ~(DPAD_UP | DPAD_DOWN);
            }
        }
        if (nk & DPAD_LEFT)
            sFp.turnQueue = -1;
        else if (nk & DPAD_RIGHT)
            sFp.turnQueue = 1;
        *newKeys &= ~dpad;
        *heldKeys &= ~dpad;
        if (hk & DPAD_UP)
            *heldKeys |= DirToKey(sFp.heading);
        else if (hk & DPAD_DOWN)
            *heldKeys |= DirToKey(OppositeDir(sFp.heading));
        if (nk & DPAD_UP)
            *newKeys |= DirToKey(sFp.heading);
        else if (nk & DPAD_DOWN)
            *newKeys |= DirToKey(OppositeDir(sFp.heading));
        if (canAct && (hk & (DPAD_UP | DPAD_DOWN)))
        {
            // the sprite is hidden, so face the move direction first: a tap then steps instead of only turning
            enum Direction moveDir = (hk & DPAD_UP) ? sFp.heading : OppositeDir(sFp.heading);

            if (GetPlayerFacingDirection() != moveDir)
                ObjectEventTurn(&gObjectEvents[gPlayerAvatar.objectEventId], moveDir);
        }
        if (sFp.turnQueue && canAct && !turning && !(hk & (DPAD_UP | DPAD_DOWN)))
        {
            sFp.heading = RotateHeading(sFp.heading, sFp.turnQueue);
            if (sFp.smooth)
                sFp.targetQ4 += sFp.turnQueue * 1024;
            sFp.turnQueue = 0;
        }
    }

    if (sFp.pendingFlip)
        return FALSE;

    if (!sFp.shown)
    {
        u16 mapKey = (gSaveBlock1Ptr->location.mapGroup << 8) | gSaveBlock1Ptr->location.mapNum;

        if (ArePlayerFieldControlsLocked() || gPaletteFade.active || gMain.callback2 != CB2_Overworld)
            return FALSE;
        if (!Setup())
        {
            sFp.active = FALSE;
            return FALSE;
        }
        if (!sFp.headingValid || mapKey != sFp.mapKey)
            sFp.heading = GetPlayerFacingDirection();
        sFp.headingValid = TRUE;
        sFp.mapKey = mapKey;
        sFp.angleQ4 = sFp.targetQ4 = HeadingBrads(sFp.heading) * 16;
        sFp.smoothReady = FALSE;
        HideMapNamePopUpWindow();   // its tilemap/tile uploads would land in the bitmap pages
        RenderBackPage(TRUE);
        return FALSE;
    }

    if (sFp.smooth)
    {
        SmoothCamera();
        if (sFp.dirty || sFp.camX != sFp.lastCamX || sFp.camY != sFp.lastCamY || sFp.angleQ4 != sFp.lastAngle || SmoothObjectSignature() != sFp.lastSig)
            RenderBackPage(FALSE);
        return FALSE;
    }

    GetPlayerPosition(&pos);
    if (sFp.settle && --sFp.settle == 0)
        sFp.dirty = TRUE;
    if (sFp.dirty || (atCenter && (pos.x != sFp.lastX || pos.y != sFp.lastY || sFp.heading != sFp.lastDir)) || ObjectSignature() != sFp.lastSig)
    {
        if (ObjectSignature() != sFp.lastSig)
            sFp.settle = 24;
        RenderBackPage(FALSE);
    }
    return FALSE;
}

void FirstPerson_VBlank(void)
{
    if (!M)
        return;
    if (sFp.pendingFlip)
    {
        CpuCopy16(M->pal, gPlttBufferFaded, sizeof(M->pal));
        CpuCopy16(M->pal, gPlttBufferUnfaded, sizeof(M->pal));
        if (sFp.pendingEnter)
        {
            REG_BG2PA = 0x100;
            REG_BG2PB = 0;
            REG_BG2PC = 0;
            REG_BG2PD = 0x100;
            REG_BG2X = 0;
            REG_BG2Y = 0;
            REG_BG2CNT = 0;
            SetGpuReg(REG_OFFSET_BG2CNT, 0);
            sFp.pendingEnter = FALSE;
        }
        REG_DISPCNT = DISPCNT_MODE_4 | DISPCNT_BG2_ON | (sFp.page ? 0x10 : 0);   // we are in VBlank: write the hardware directly
        SetGpuReg(REG_OFFSET_DISPCNT, REG_DISPCNT);   // keep the buffered copy in sync
        sFp.pendingFlip = FALSE;
        sFp.shown = TRUE;
        }
    else if (sFp.shown && !gPaletteFade.active)
    {
        // keep the weather / time-of-day palette updates from touching our palette
        CpuCopy16(M->pal, gPlttBufferFaded, sizeof(M->pal));
    }
}

#endif // OW_FIRST_PERSON
