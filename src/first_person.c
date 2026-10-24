#include "global.h"
#include "first_person.h"

#if OW_FIRST_PERSON

#include "decompress.h"
#include "fieldmap.h"
#include "event_object_movement.h"
#include "field_message_box.h"
#include "field_player_avatar.h"
#include "field_control_avatar.h"
#include "gpu_regs.h"
#include "main.h"
#include "map_name_popup.h"
#include "malloc.h"
#include "metatile_behavior.h"
#include "overworld.h"
#include "palette.h"
#include "script.h"
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
#define FP_TEX_N        48
#define FP_LEVELS       8             // 4 fog levels x {front, side}
#define FP_PAGE_SIZE    0xA000
#define FP_WIN          17            // cached cell window around the player (+-8)
#define FP_IWRAM        ARM_FUNC __attribute__((section(".iwram.code"))) __attribute__((noinline))

struct FpCol
{
    s16 ytop, ybot;
    u16 zd;                           // z * 256 / FOCAL
    u8 lvl, tx, height, pad;
    const u8 *tex[3];
};

struct FpMem
{
    u32 tiles[NUM_TILES_TOTAL * 8];
    u8 tex[FP_TEX_N][256];
    u16 texId[FP_TEX_N];
    u8 texPin[FP_TEX_N];
    u8 texNext;
    u8 numSlots;
    u8 slotQ[4096];
    u8 lut[FP_LEVELS][256];
    u16 pal[256];                     // palette uploaded in VBlank
    u16 srcPal[256];                  // field palette snapshot
    u8 haze[3], skyTop[3];
    u8 hazeSlot, skySlot[FP_HORIZON];
    u8 solid[FP_WIN * FP_WIN];        // per-redraw cache: 0 unknown, 1 empty, 2 solid
    const u8 *cellTex[FP_WIN * FP_WIN];
    struct FpCol cols[FP_COLS];
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
    u8 heading;        // view direction (independent of the player's facing: stepping back does not turn)
    bool8 headingValid;
    bool8 interactPending;  // after the reload, feed one A press to the normal field code
    s8 turnQueue;      // -1 left, +1 right, applied when the player is on a tile centre
    u16 mapKey;
    bool8 dirty;
    bool8 vramLock;    // tileset animation uploads must not touch the bitmap pages
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

FP_IWRAM static void ComposeMetatile(u32 id, u8 *out)
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
    s32 i, ratio[3], sumU[3] = {0, 0, 0}, sumF[3] = {0, 0, 0};

    if (!M)
    {
        M = AllocZeroed(sizeof(struct FpMem));
        if (!M)
            return FALSE;
    }
    else
    {
        memset(M->texId, 0, sizeof(M->texId));
    }

    if (l->primaryTileset->isCompressed)
        DecompressDataWithHeaderWram(l->primaryTileset->tiles, M->tiles);
    else
        CpuCopy32(l->primaryTileset->tiles, M->tiles, nPri * 32);
    if (l->secondaryTileset->isCompressed)
        DecompressDataWithHeaderWram(l->secondaryTileset->tiles, &M->tiles[nPri * 8]);
    else
        CpuCopy32(l->secondaryTileset->tiles, &M->tiles[nPri * 8], (NUM_TILES_TOTAL - nPri) * 32);

    CpuCopy16(gPlttBufferFaded, M->srcPal, sizeof(M->srcPal));

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

static void Release(void)
{
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

static void RenderBackPage(bool32 entering)
{
    u32 page = entering ? 0 : sFp.page ^ 1;

    if (entering)
    {
        sFp.vramLock = TRUE;
        REG_DISPCNT |= DISPCNT_FORCED_BLANK;   // the field BGs would show garbage while VRAM is overwritten
    }
    Draw(page);
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
static bool32 LeaveView(bool32 keepWanted)
{
    bool32 wasShown = sFp.shown || sFp.pendingFlip;

    sFp.active = keepWanted;
    if (!keepWanted)
        sFp.headingValid = FALSE;
    sFp.turnQueue = 0;
    Release();
    if (!wasShown)
        return FALSE;
    gFieldCallback = NULL;
    SetMainCallback2(CB2_ReturnToField);
    return TRUE;
}

bool32 FirstPerson_Update(u16 *newKeys, u16 *heldKeys)
{
    struct MapPosition pos;
    u8 trans = gPlayerAvatar.tileTransitionState;
    bool32 atCenter = (trans == T_TILE_CENTER || trans == T_NOT_MOVING);
    bool32 canAct = !ArePlayerFieldControlsLocked() && atCenter;
    u16 nk = *newKeys, hk = *heldKeys;

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
    }
    else if (canAct && (nk & L_BUTTON))
    {
        return LeaveView(FALSE);
    }
    else if (sFp.shown && canAct && (nk & (START_BUTTON | SELECT_BUTTON | R_BUTTON)))
    {
        // menus need BG0, which Mode 4 does not have: leave, swallow the press
        return LeaveView(FALSE);
    }
    else if (sFp.shown && canAct && (nk & A_BUTTON))
    {
        // talk / read: face the view direction, and if something is there leave, run the interaction, come back
        ObjectEventTurn(&gObjectEvents[gPlayerAvatar.objectEventId], sFp.heading);
        *newKeys &= ~A_BUTTON;
        if (HasInteractionAheadOfPlayer())
        {
            sFp.interactPending = TRUE;
            if (!LeaveView(TRUE))
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

    if (sFp.shown && !IsFieldMessageBoxHidden())
    {
        // a script started a text box on its own (trainer, step trigger...): BG0 is not available, so drop to the
        // normal view for it and stay there (press L to come back)
        return LeaveView(FALSE);
    }

    if (sFp.shown)
    {
        // Up = step along the view direction, Down = step back, Left/Right = turn the view only
        u16 dpad = DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT;

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
        if (sFp.turnQueue && canAct && !(hk & (DPAD_UP | DPAD_DOWN)))
        {
            sFp.heading = RotateHeading(sFp.heading, sFp.turnQueue);
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
        HideMapNamePopUpWindow();   // its tilemap/tile uploads would land in the bitmap pages
        RenderBackPage(TRUE);
        return FALSE;
    }

    GetPlayerPosition(&pos);
    if (atCenter && (sFp.dirty || pos.x != sFp.lastX || pos.y != sFp.lastY || sFp.heading != sFp.lastDir))
        RenderBackPage(FALSE);
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
