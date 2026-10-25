#include "global.h"
#include "first_person.h"

#if OW_FIRST_PERSON

#include "decompress.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
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
#include "tilesets.h"
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
#define FP_TEX_N        56
#define FP_SMOOTH_COLS_MAX 60
#define FP_SKY_W        256           // sky panorama columns per full turn
#define FP_FLOOR_MID    56            // floor row pairs from here: 4x4 blocks (z < ~1.5 cells)
#define FP_FLOOR_NEAR   64            // floor row pairs from here: 8x4 blocks again (a texel is wider than 6 px)
#define FP_SKY_R        40            // sky rows (2 px each) above the horizon
#define FP_DARK_MAX     232           // shading that needs extra palette colours stops adding them above this many slots
#define FP_LEVELS       16            // 4 fog levels x {front, side} x {normal, wall base (darker, contact shadow)}
#define FP_PAGE_SIZE    0xA000
#define FP_DECOMP_HEADROOM 14000   // the tANS decoder allocates ~11 KB of scratch on top
#define FP_DECOMP_SLACK 128        // the smol decoder can write a little past the nominal output size
#define FP_TRAIL_MAX    4
#define FP_BILL_NEAR    320            // billboards closer than this (Q8 cells) are drawn at this distance
#define FP_FWIN         21            // floor texture cache window (+-10 cells)
#define FP_WIN          17            // cached cell window around the player (+-8)
#define FP_IWRAM        ARM_FUNC __attribute__((section(".iwram.code"))) __attribute__((noinline))

struct FpCol
{
    u16 z;                            // wall distance (Q8 cells), 0xFFFF = none: billboard depth test
    s16 ytop, ybot;
    u16 zd;                           // z * 256 / FOCAL
    u8 lvl, tx, height, pad;
    u16 zs;                           // smooth renderer: scenery deeper than this is hidden behind the first scenery cell of the column
    const u8 *tex[3];
};

struct FpFace
{
    u8 h;
    u8 ok;
    const u8 *tex[3];                 // top metatile first
};

#define FP_SCENERY      0xFF           // FpBill.objId of a scenery billboard (tree, bush, fence) instead of an object event
#define FP_BILLS_MAX    40
#define FP_SCENERY_DEPTH 320           // scenery this much (Q8 cells) behind the first scenery cell on a screen column is not drawn there
#define FP_SCENERY_WALL 768            // scenery beyond this depth (Q8 cells) is drawn as a wall block, nearer as billboards
#define FP_SCENERY_R    5              // scenery is collected within this many cells of the camera

struct FpBill
{
    u8 objId;
    u8 hc;         // scenery: height in cells (1 or 2)
    s16 u;         // lateral offset, Q8 cells (+ = right)
    u16 z;         // depth, Q8 cells
    const u8 *tex[2];   // scenery: metatile textures, top first (16x16 8-bit, 0 = transparent)
};

struct FpScen
{
    s16 x, y;      // cell of the southmost scenery cell of the stack
    u8 hc;
    const u8 *tex[2];
};

#define FP_SCEN_MAX     96

struct FpMem
{
    u8 tex[FP_TEX_N][256];
    u16 texId[FP_TEX_N];
    u8 texPin[FP_TEX_N];
    u8 texNext;
    u8 texIdx[1024];                  // metatile id -> slot + 1 in tex[] (validated against texId)
    u8 numSlots;
    u8 slotQ[4096];
    u8 lut[FP_LEVELS][512];
    u16 pal[256];                     // palette uploaded in VBlank
    u16 srcPal[512];                  // BG (0-255) and OBJ (256-511) palette snapshot
    u8 haze[3], skyTop[3];
    u8 hazeSlot, skySlot[FP_HORIZON];
    u8 solid[1024];                   // cell cache indexed by (y & 31, x & 31): 0 unknown, 1 empty, 2 solid
    const u8 *cellTex[1024];          // floor texture per cell; the smooth renderer indexes it by (y & 31, x & 31) and slides it with the window
    struct FpCol cols[FP_COLS];
    struct FpBill bills[FP_BILLS_MAX];    // billboards of one frame, far to near (object events and scenery)
    struct FpFace faces[FP_WIN * FP_WIN];
    struct FpScen scen[FP_SCEN_MAX];      // scenery stacks around the cached window (rebuilt when the window moves)
    u32 scenN;
    // smooth renderer
    u8 skyTab[FP_SKY_R / 2][6];           // per 2 px sky row: palette slot of {sky, cloud edge, cloud core, far ridge, near trees}
    u8 skyPan[FP_SKY_W * (FP_SKY_R / 4)];   // sky panorama, one nibble per (column, 4 px row), generated in code
    u8 skyBuilt;
    u16 recipPerp[2560];              // (6553600 / perp) >> 2: wall height in px per cell (Q8), no per-column division
    u16 recipRay[512];                // 65536 / |ray component| (Q8), capped at 65535
    struct FpEdge { u8 kind, p, y0, y1; } colEdge[FP_SMOOTH_COLS_MAX];   // per smooth column: 0 no wall, 1 front face, 2 side face; depth (Q4 cells); wall rows
    u8 darkMap[2][256];                      // slot -> darker slot (wall base / corner shading), 0 = not built yet
    u8 colYe[FP_SMOOTH_COLS_MAX];            // per smooth column: first row pair below the wall (floor starts here)
    u8 floorLvl[FP_ROWS / 2 - FP_SKY_R];    // fog level per floor row pair (4 = haze)
    u16 floorZ[FP_ROWS / 2 - FP_SKY_R];     // floor row depth, Q8 cells
    u32 tiles[];                      // decompressed primary + secondary tileset tiles (size depends on the map)
};

static EWRAM_DATA struct
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
    u8 ledgeDir;            // free mode: walked into a ledge in the view direction: PlayerStep gets this direction (real ledge jump)
    bool8 emulated;         // free mode: this tick's finished-step state was faked in FreeStep (cleared again in EndOfInput)
    u8 trailN;              // free mode: tiles the player has left that the follower still has to walk to (oldest first)
    s16 trailX[FP_TRAIL_MAX], trailY[FP_TRAIL_MAX];
    bool8 facedTrainer;     // this trainer approach already turned the player and the view
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
} sFp = {0};

#define M (sFp.m)

// fog blend towards the haze colour (of 256) per band: near (full palette contrast), two middle bands, far (silhouettes still readable)
static const u16 sFog[4] = { 0, 36, 128, 128 };   // three bands (level 3 unused: fewer palette colours)
static const u16 sFogOld[4] = { 0, 50, 112, 172 };
EWRAM_DATA u16 gFpPolishOff = 0;   // debug: the test harness writes 1 to get the previous look (fog bands, side shade, no wall base / corner shading), 2 = 1 plus one redraw

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
    u32 fog = gFpPolishOff ? sFogOld[lvl & 3] : sFog[lvl & 3];
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

    if ((lvl & 8) && !gFpPolishOff)
    {
        r = r * 184 >> 8;
        g = g * 184 >> 8;
        b = b * 184 >> 8;
    }
    if ((lvl & 4) && gFpPolishOff)
    {
        r = r * 190 >> 8;
        g = g * 190 >> 8;
        b = b * 190 >> 8;
    }
    else if (lvl & 4)
    {
        // side faces (the other wall axis): darker with a slightly cool shadow, so facades and side walls read as different planes
        r = r * 156 >> 8;
        g = g * 164 >> 8;
        b = b * 184 >> 8;
    }
    r = (r * (256 - fog) + M->haze[0] * fog) >> 8;
    g = (g * (256 - fog) + M->haze[1] * fog) >> 8;
    b = (b * (256 - fog) + M->haze[2] * fog) >> 8;
    return r | (g << 5) | (b << 10);
}

// (colour 0 of a General-tileset pixel in the smooth renderer is the matted-out part of a far scenery wall block: dark tree green, palette entry 36)
// The slow path is out of line: PixSlot is inlined into the IWRAM wall / floor loops.
static inline u32 PixSlot(u32 lvl, u32 pix);
static __attribute__((noinline)) u32 PixSlotSlow(u32 lvl, u32 pix)
{
    u32 src = pix;

    if (!pix && sFp.smooth && gMapHeader.mapLayout->primaryTileset == &gTileset_General)
        src = 36;
    if ((lvl & 8) && M->numSlots >= FP_DARK_MAX)
        return M->lut[lvl][pix] = PixSlot(lvl & 7, pix);   // palette nearly full: no contact shadow rather than nearest-colour errors
    return M->lut[lvl][pix] = GetSlot(ShadeColor(M->srcPal[src], lvl));
}

static inline u32 PixSlot(u32 lvl, u32 pix)
{
    u32 s = M->lut[lvl][pix];

    if (!s)
        s = PixSlotSlow(lvl, pix);
    return s;
}

static u32 FogLevel(s32 z)
{
    if (gFpPolishOff)
        return z < 512 ? 0 : z < 896 ? 1 : z < 1280 ? 2 : 3;
    return z < 640 ? 0 : z < 1152 ? 1 : 2;
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

// ---- scenery: solid metatiles of the General primary tileset that are drawn as standing billboards instead of wall blocks (smooth/free
// renderer only): trees (22/23, 198/199/206, 468-471, 476/477, 484-487) and the grey fence pieces (306-308, 312/313, 320-322, 328-330).
// Their art sits on a grass background; MatteTex() cuts that grass out (colour 0 = transparent).
static bool32 SceneryId(u32 id)
{
    if (!sFp.smooth || id >= 512 || gMapHeader.mapLayout->primaryTileset != &gTileset_General)
        return FALSE;
    return id == 22 || id == 23 || id == 198 || id == 199 || id == 206
        || (id >= 468 && id <= 471) || id == 476 || id == 477 || (id >= 484 && id <= 487)
        || (id >= 306 && id <= 308) || id == 312 || id == 313 || (id >= 320 && id <= 322) || (id >= 328 && id <= 330);
}

// grass background colours (palette 2, entries 12-14) that touch the metatile border (or already transparent pixels) become transparent
static void MatteTex(u8 *t)
{
    bool32 changed = TRUE;
    s32 x, y;

    while (changed)
    {
        changed = FALSE;
        for (y = 0; y < 16; y++)
        {
            for (x = 0; x < 16; x++)
            {
                u8 *p = &t[y * 16 + x];

                if (*p - 44u >= 3)
                    continue;
                if (x == 0 || y == 0 || x == 15 || y == 15 || !p[-1] || !p[1] || !p[-16] || !p[16])
                {
                    *p = 0;
                    changed = TRUE;
                }
            }
        }
    }
}

static const u8 *GetTex(u32 id, bool32 pin)
{
    u32 i, n;

    i = M->texIdx[id & 1023];
    if (i && M->texId[i - 1] == id + 1)
    {
        if (pin)
            M->texPin[i - 1] = 1;
        return M->tex[i - 1];
    }
    // replace round-robin, skipping pinned wall textures
    for (n = 0; n < FP_TEX_N; n++)
    {
        i = M->texNext;
        M->texNext = (M->texNext + 1) % FP_TEX_N;
        if (!M->texPin[i])
            break;
    }
    if (M->texId[i])
    {
        for (n = 0; n < 1024; n++)
            if (M->cellTex[n] == M->tex[i])
                M->cellTex[n] = NULL;   // a cached floor pointer would show the new metatile
    }
    M->texId[i] = id + 1;
    M->texIdx[id & 1023] = i + 1;
    M->texPin[i] = pin;
    ComposeMetatile(id, M->tex[i]);
    if (SceneryId(id))
        MatteTex(M->tex[i]);
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

// cell kind: 1 empty, 2 solid wall, 3 solid scenery (a billboard in the smooth/free renderer; a wall in the stepping one)
static __attribute__((noinline)) u32 CellKindSlow(s32 px, s32 py, s32 cx, s32 cy)
{
    s32 dx = cx - px + 8, dy = cy - py + 8;
    u32 k;

    k = !CellSolid(cx, cy) ? 1 : SceneryId(MapGridGetMetatileIdAt(cx, cy)) ? 3 : 2;
    if (dx >= 0 && dy >= 0 && dx < FP_WIN && dy < FP_WIN)
        M->solid[((cy & 31) << 5) | (cx & 31)] = k;
    return k;
}

static inline u32 CellKind(s32 px, s32 py, s32 cx, s32 cy)
{
    s32 dx = cx - px + 8, dy = cy - py + 8;

    if ((u32)dx < FP_WIN && (u32)dy < FP_WIN)
    {
        u32 k = M->solid[((cy & 31) << 5) | (cx & 31)];

        if (k)
            return k;
    }
    return CellKindSlow(px, py, cx, cy);
}

static bool32 SolidAt(s32 px, s32 py, s32 cx, s32 cy)
{
    return CellKind(px, py, cx, cy) >= 2;
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

__attribute__((noinline)) static const u8 *FloorTex(s32 cx, s32 cy)
{
    u32 id = MapGridGetMetatileIdAt(cx, cy);

    if (SceneryId(id))
        id = 1;   // plain grass under a tree / fence billboard (its own texture has transparent holes)
    return GetTex(id, FALSE);
}

static void RenderRows(u8 *vram, s32 px, s32 py, s32 fx, s32 fy, s32 rx, s32 ry)
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
                s32 dx = (wx >> 8) + 10, dy = (wy >> 8) + 10;
                const u8 *t;
                u32 pix, ls;

                if ((u32)dx >= FP_FWIN || (u32)dy >= FP_FWIN)
                {
                    t = FloorTex(px + dx - 10, py + dy - 10);
                }
                else
                {
                    t = m->cellTex[dy * FP_FWIN + dx];
                    if (!t)
                        t = m->cellTex[dy * FP_FWIN + dx] = FloorTex(px + dx - 10, py + dy - 10);
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

// Scenery billboard: same geometry, near plane, fog and wall depth test as DrawBillboard, but a metatile stack (16 texels per cell) as the picture.
// Always the smooth renderer's 4 x 2 px blocks (a strip is 2 columns). Drawn texel row by texel row: per billboard the texel rows that cover at
// least one block row are listed once (sSceneryRows); per strip each listed row costs one texture + one palette lookup, then its blocks are stored.
struct FpSceneryRow
{
    const u8 *p;     // 16 texels of this row
    s16 a, b;        // block rows (2 px) [a, b) it covers on screen
};

static EWRAM_DATA struct FpSceneryRow sSceneryRows[32];

static __attribute__((noinline)) u32 SlotSlow(u32 lvl, u32 pix)
{
    return PixSlot(lvl, pix);
}

// one strip
static void SceneryStrip(u32 *dst, const struct FpSceneryRow *r, const struct FpSceneryRow *rEnd, s32 sx, const u8 *lutRow, u32 lvl)
{
    for (; r < rEnd; r++)
    {
        u32 pix = r->p[sx];

        if (pix)
        {
            u32 slot = lutRow[pix];
            u32 *d = dst + r->a * 120;
            s32 n = r->b - r->a;

            if (!slot)
                slot = SlotSlow(lvl, pix);
            slot *= 0x01010101;
            do
            {
                d[0] = slot;
                d[60] = slot;
                d += 120;
            } while (--n);
        }
    }
}

// the same, 8 px wide (two strips with the same texel column): near billboards, where a texel is 4+ px wide
static void SceneryStripWide(u32 *dst, const struct FpSceneryRow *r, const struct FpSceneryRow *rEnd, s32 sx, const u8 *lutRow, u32 lvl)
{
    for (; r < rEnd; r++)
    {
        u32 pix = r->p[sx];

        if (pix)
        {
            u32 slot = lutRow[pix];
            u32 *d = dst + r->a * 120;
            s32 n = r->b - r->a;

            if (!slot)
                slot = SlotSlow(lvl, pix);
            slot *= 0x01010101;
            do
            {
                d[0] = slot;
                d[1] = slot;
                d[60] = slot;
                d[61] = slot;
                d += 120;
            } while (--n);
        }
    }
}

static void DrawScenery(u8 *vram, const struct FpBill *b)
{
    s32 h = b->hc * 16, sw, sh, left, top, bottom, c, wInv, c0, c1, zD, t, acc, rowLen, y, from, ts, j, m = 0;
    bool32 coarse;
    s32 cellPx;
    u32 lvl;
    const u8 *lutRow;

    // no divisions (the BIOS divide is slow): cellPx = pixels per cell at this depth, Q8, from the wall height table
    zD = b->z < FP_BILL_NEAR ? FP_BILL_NEAR : b->z;
    cellPx = M->recipPerp[zD] << 2;
    sw = cellPx >> 8;
    sh = (cellPx * b->hc) >> 8;
    left = 120 + ((b->u * cellPx) >> 16) - (sw >> 1);
    bottom = FP_HORIZON + (cellPx >> 9);
    top = bottom - sh;
    lvl = FogLevel(zD);
    if (sw < 1 || sh < 1)
        return;
    wInv = (zD * 10486) >> 8;                // texels per pixel, Q16
    c0 = left >> 1;
    if (c0 < 0)
        c0 = 0;
    c0 &= ~1;
    c1 = ((left + sw) >> 1) + 1;
    if (c1 > FP_COLS)
        c1 = FP_COLS;
    rowLen = cellPx * 16;                     // screen rows per texel row, Q16
    from = (top + 1) >> 1;
    if (from < 0)
        from = 0;
    // far away a texel row is thinner than a block row: sample every 2nd / 4th texel row (halves / quarters the work per strip)
    ts = (cellPx >> 12) >= 3 ? 1 : (cellPx >> 12) >= 2 ? 2 : 4;
    rowLen *= ts;
    for (t = 0, acc = 0; t < h; t += ts)
    {
        s32 to, tr = t + (ts >> 1);

        acc += rowLen;
        y = t + ts >= h ? bottom : top + (acc >> 16);
        to = (y + 1) >> 1;
        if (to > FP_ROWS / 2)
            to = FP_ROWS / 2;
        if (to > from)
        {
            sSceneryRows[m].p = b->tex[tr >> 4] + ((tr & 15) << 4);
            sSceneryRows[m].a = from;
            sSceneryRows[m].b = to;
            m++;
            from = to;
        }
    }
    lutRow = M->lut[lvl];
    coarse = (cellPx >> 12) >= 4;
    c0 &= coarse ? ~3 : ~1;
    for (c = c0; c < c1; c += coarse ? 4 : 2)
    {
        s32 sx = ((c * 2 + (coarse ? 4 : 2) - left) * wInv) >> 16;
        bool32 ok0 = b->z < M->cols[c].z && b->z <= M->cols[c].zs;

        if (coarse)
        {
            bool32 ok1 = b->z < M->cols[c + 2].z && b->z <= M->cols[c + 2].zs;

            if (ok0 && ok1 && sx >= 0 && sx < 16)
            {
                SceneryStripWide((u32 *)(vram + c * 2), sSceneryRows, sSceneryRows + m, sx, lutRow, lvl);
                continue;
            }
            // an edge (a wall in front of one half): the two strips one by one
            for (j = 0; j < 2; j++)
            {
                s32 cc = c + 2 * j, sxx = ((cc * 2 + 2 - left) * wInv) >> 16;

                if (sxx >= 0 && sxx < 16 && b->z < M->cols[cc].z && b->z <= M->cols[cc].zs)
                    SceneryStrip((u32 *)(vram + cc * 2), sSceneryRows, sSceneryRows + m, sxx, lutRow, lvl);
            }
            continue;
        }
        if (sx < 0 || sx >= 16 || !ok0)
            continue;
        SceneryStrip((u32 *)(vram + c * 2), sSceneryRows, sSceneryRows + m, sx, lutRow, lvl);
    }
}

static EWRAM_DATA s32 sViewDirX = 0, sViewDirY = 0, sViewRightX = 0, sViewRightY = 0;   // camera forward / right (Q8), set before the billboards are drawn

// Is this one of the animation tables laid out like the standard one (face S/N/W/E, go S/N/W/E, ...: direction = animNum & 3)?
static bool32 StandardAnimLayout(const struct ObjectEvent *oe, const struct ObjectEventGraphicsInfo *info)
{
    if (info->inanimate)
        return FALSE;
    if (oe->graphicsId & OBJ_EVENT_MON)
        return TRUE;   // OW mons / followers use the "following" table, same layout
    return info->anims == GetObjectEventGraphicsInfo(OBJ_EVENT_GFX_BOY_1)->anims
        || info->anims == GetObjectEventGraphicsInfo(OBJ_EVENT_GFX_BRENDAN_NORMAL)->anims
        || info->anims == GetObjectEventGraphicsInfo(OBJ_EVENT_GFX_MAY_NORMAL)->anims;
}

// test hook (tools/fp_test/regress_billboard.sh): the last 8 billboards drawn: graphicsId, facing, sprite animNum, chosen animNum, image, hflip, z, u.
// Set FP_TEST_HOOKS to 0 to drop it (260 B of EWRAM) in a release build; the billboard regression then cannot run.
#ifndef FP_TEST_HOOKS
#define FP_TEST_HOOKS 1
#endif
#if FP_TEST_HOOKS
EWRAM_DATA u32 gFpBillLog[64] = {0};
EWRAM_DATA u32 gFpBillLogN = 0;
#endif
static void DrawBillboard(u8 *vram, const struct FpBill *b, s32 cs, s32 rs)
{
    const struct ObjectEvent *oe;
    const struct Sprite *spr;
    const struct ObjectEventGraphicsInfo *info;
    u32 shape, size;
    s32 wt, ht, w, h, sw, sh, left, top, bottom, c, wInv, hInv, c0, c1;
    s32 imageValue = 0, tileOffset, zD;
    const u8 *frame;
    u32 palBase, lvl, hflip;

    if (b->objId == FP_SCENERY)
    {
        if (cs == 2 && rs == 2)   // scenery only exists in the smooth renderer
            DrawScenery(vram, b);
        return;
    }
    oe = &gObjectEvents[b->objId];
    spr = &gSprites[oe->spriteId];
    info = GetObjectEventGraphicsInfo(oe->graphicsId);
    shape = info->oam->shape;
    size = info->oam->size;
    if (shape > 2)
        return;
    wt = sTilesW[shape][size];
    ht = sTilesH[shape][size];
    w = wt * 8;
    h = ht * 8;
    {
        // Frame choice: keep the current animation group (standing / walking, so the walk cycle runs), but pick the direction
        // from the angle between the camera and the way the object faces: toward us = front, away = back, sideways = left/right
        // (right is the left frame flipped by the animation command itself).
        u32 animNum = spr->animNum;
        const union AnimCmd *cmd;

        if (spr->anims && animNum < 12 && oe->facingDirection >= DIR_SOUTH && oe->facingDirection <= DIR_EAST && StandardAnimLayout(oe, info))
        {
            s32 ox = oe->facingDirection == DIR_EAST ? 256 : oe->facingDirection == DIR_WEST ? -256 : 0;
            s32 oy = oe->facingDirection == DIR_SOUTH ? 256 : oe->facingDirection == DIR_NORTH ? -256 : 0;
            s32 dotF = (ox * sViewDirX + oy * sViewDirY) >> 8, dotR = (ox * sViewRightX + oy * sViewRightY) >> 8;
            u32 idx;

            if ((dotF < 0 ? -dotF : dotF) >= (dotR < 0 ? -dotR : dotR))
                idx = dotF < 0 ? 0 : 1;      // facing the camera: front (south frames); facing away: back (north frames)
            else
                idx = dotR > 0 ? 3 : 2;      // facing the camera's right: east frames; left: west frames
            animNum = (animNum & ~3u) | idx;
        }
        cmd = spr->anims ? &spr->anims[animNum][spr->animCmdIndex] : NULL;
        if (cmd && cmd->type < 0)
            cmd = &spr->anims[animNum][0];
        if (cmd && cmd->type >= 0)
        {
            imageValue = cmd->frame.imageValue;
            hflip = cmd->frame.hFlip;
#if FP_TEST_HOOKS
            { u32 *d = &gFpBillLog[(gFpBillLogN++ & 7) * 8]; d[0] = oe->graphicsId; d[1] = oe->facingDirection; d[2] = spr->animNum; d[3] = animNum; d[4] = imageValue; d[5] = hflip; d[6] = b->z; d[7] = (u16)b->u; }
#endif
        }
        else
        {
            hflip = spr->oam.matrixNum & ST_OAM_HFLIP;
        }
    }
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

    // Near plane: a sprite closer than 1.25 cells is drawn as if it were 1.25 cells away. It keeps a sane size (and a bounded
    // cost) instead of becoming a giant block; its depth test still uses the real distance.
    zD = b->z < FP_BILL_NEAR ? FP_BILL_NEAR : b->z;
    sw = w * 1600 / zD;
    sh = h * 1600 / zD;
    left = 120 + b->u * FP_FOCAL / zD - sw / 2;
    bottom = FP_HORIZON + FP_EYE * FP_FOCAL / zD;
    top = bottom - sh;
    palBase = 256 + spr->oam.paletteNum * 16;
    lvl = FogLevel(zD);
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

// Scenery billboards for the smooth/free renderer: every solid scenery cell within FP_SCENERY_R of the camera that is in front of it and in view.
// A column of scenery cells is split into 1-2 cell stacks from its south end; each stack is one billboard at its southmost cell.
static void BuildScenery(void)
{
    s32 cx, cy;
    u32 n = 0;

    for (cy = sFp.winY - FP_SCENERY_R - 1; cy <= sFp.winY + FP_SCENERY_R + 1; cy++)
    {
        for (cx = sFp.winX - FP_SCENERY_R - 1; cx <= sFp.winX + FP_SCENERY_R + 1; cx++)
        {
            struct FpScen *e;
            s32 d;
            bool32 two;

            if (CellKind(sFp.winX, sFp.winY, cx, cy) != 3)
                continue;
            for (d = 0; d < 4 && CellKind(sFp.winX, sFp.winY, cx, cy + 1 + d) == 3; d++)
                ;
            if ((d & 1) || n >= FP_SCEN_MAX)
                continue;
            two = CellKind(sFp.winX, sFp.winY, cx, cy - 1) == 3;
            e = &M->scen[n++];
            e->x = cx;
            e->y = cy;
            e->hc = two ? 2 : 1;
            if (two)
            {
                e->tex[0] = GetTex(MapGridGetMetatileIdAt(cx, cy - 1), TRUE);
                e->tex[1] = GetTex(MapGridGetMetatileIdAt(cx, cy), TRUE);
            }
            else
            {
                e->tex[0] = GetTex(MapGridGetMetatileIdAt(cx, cy), TRUE);
                e->tex[1] = NULL;
            }
        }
    }
    M->scenN = n;
}

// per frame: the cached scenery stacks that are in front of the camera and in view, merged far-to-near into the billboard list
static u32 CollectScenery(struct FpBill *out, u32 n, s32 camX, s32 camY, s32 dirX, s32 dirY, s32 rightX, s32 rightY)
{
    u32 i;

    for (i = 0; i < M->scenN; i++)
    {
        const struct FpScen *e = &M->scen[i];
        s32 dx = e->x * 256 + 128 - camX, dy = e->y * 256 + 128 - camY;
        s32 z = (dx * dirX + dy * dirY) >> 8;
        s32 u, au, j;

        if (z < 128 || z > FP_SCENERY_WALL + 192)
            continue;
        u = (dx * rightX + dy * rightY) >> 8;
        au = u < 0 ? -u : u;
        if (au - 128 > ((z * 307) >> 8))
            continue;
        if (n >= FP_BILLS_MAX)
        {
            if (z >= out[0].z)
                continue;
            memmove(&out[0], &out[1], (n - 1) * sizeof(out[0]));
            n--;
        }
        for (j = n; j > 0 && out[j - 1].z < z; j--)
            out[j] = out[j - 1];
        out[j].objId = FP_SCENERY;
        out[j].hc = e->hc;
        out[j].u = u;
        out[j].z = z;
        out[j].tex[0] = e->tex[0];
        out[j].tex[1] = e->tex[1];
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
    memset(M->darkMap, 0, sizeof(M->darkMap));
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
        struct FpBill *bills = M->bills;
        u32 n = CollectBillboards(bills, px, py, fx, fy, rx, ry), i;

        sViewDirX = fx * 256;
        sViewDirY = fy * 256;
        sViewRightX = rx * 256;
        sViewRightY = ry * 256;
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
    sFp.trailN = 0;
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

EWRAM_DATA u8 gFpSlots[2] = {0};      // palette slots used by the last render / the most used by any render (255 = full: nearest-colour fallback)
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

// ---- sky: gradient + clouds + distant ridges, generated in code into a wrapping panorama (one nibble per 4 px x 2 px block)

static u32 SkyHash(u32 x)
{
    x ^= x >> 15;
    x *= 0x2C1B3C6D;
    x ^= x >> 12;
    x *= 0x297A2D39;
    x ^= x >> 15;
    return x;
}

// value noise, 0..255; x wraps every gw cells of width cw
static s32 SkyNoise(s32 x, s32 y, s32 cw, s32 ch, s32 gw, u32 seed)
{
    s32 ix = x / cw, iy = y / ch, fx = (x % cw) * 256 / cw, fy = (y % ch) * 256 / ch;
    s32 ix1 = (ix + 1) % gw;
    s32 v00 = SkyHash(ix * 7919 + iy * 104729 + seed) & 255, v10 = SkyHash(ix1 * 7919 + iy * 104729 + seed) & 255;
    s32 v01 = SkyHash(ix * 7919 + (iy + 1) * 104729 + seed) & 255, v11 = SkyHash(ix1 * 7919 + (iy + 1) * 104729 + seed) & 255;
    s32 a, b;

    fx = (fx * fx * (768 - 2 * fx)) >> 16;
    fy = (fy * fy * (768 - 2 * fy)) >> 16;
    a = v00 + (((v10 - v00) * fx) >> 8);
    b = v01 + (((v11 - v01) * fx) >> 8);
    return a + (((b - a) * fy) >> 8);
}

static void SkyBuildPanorama(void)
{
    s32 x, r;

    for (x = 0; x < FP_SKY_W; x++)
    {
        s32 hf = 2 + ((3 * SinB(x * 2) + 2 * SinB(x * 5 + 60) + SinB(x * 11 + 13)) >> 9);
        s32 hn = 1 + ((SinB(x * 7) + SinB(x * 19 + 5)) >> 9) + (SkyHash(x + 77) & 1);

        if (hf < 1)
            hf = 1;
        if (hn < 1)
            hn = 1;
        for (r = 0; r < FP_SKY_R / 2; r++)
        {
            s32 d = (SkyNoise(x, r, 32, 7, 8, 11) * 5 + SkyNoise(x, r, 16, 4, 16, 523) * 3) >> 3;
            u32 v = 0;

            if (r > 13)
                d -= (r - 13) * 24;
            if (r < 2)
                d -= (2 - r) * 40;
            if (d > 150)
                v = 2;
            else if (d > 122)
                v = 1;
            if (r >= FP_SKY_R / 2 - hf)
                v = 3;
            if (r >= FP_SKY_R / 2 - hn)
                v = 4;
            if (r & 1)
                M->skyPan[x * (FP_SKY_R / 4) + (r >> 1)] |= v << 4;
            else
                M->skyPan[x * (FP_SKY_R / 4) + (r >> 1)] = v;
        }
    }
}

static u32 Mix3(const u8 *a, const u8 *b, u32 t)
{
    u32 r = (a[0] * (256 - t) + b[0] * t) >> 8, g = (a[1] * (256 - t) + b[1] * t) >> 8, bl = (a[2] * (256 - t) + b[2] * t) >> 8;

    return GetSlot(r | (g << 5) | (bl << 10));
}

// sky colour table per row, floor row depths and fog levels
static void SmoothBuildRows(void)
{
    s32 r;
    u8 white[3], far[3], near[3];

    for (r = 0; r < 3; r++)
    {
        u32 w = M->haze[r] * 5 / 4 + 2;

        white[r] = w > 31 ? 31 : w;
    }
    far[0] = M->haze[0] * 165 >> 8;  far[1] = M->haze[1] * 185 >> 8;  far[2] = M->haze[2] * 175 >> 8;
    near[0] = M->haze[0] * 100 >> 8; near[1] = M->haze[1] * 125 >> 8; near[2] = M->haze[2] * 105 >> 8;
    M->hazeSlot = GetSlot(M->haze[0] | (M->haze[1] << 5) | (M->haze[2] << 10));
    if (!M->skyBuilt)
    {
        SkyBuildPanorama();
        M->skyBuilt = TRUE;
    }
    for (r = 0; r < FP_SKY_R / 2; r++)
    {
        u8 base[3];
        u32 t = 16 + r * 12, i;

        for (i = 0; i < 3; i++)
            base[i] = (M->skyTop[i] * (256 - t) + M->haze[i] * t) >> 8;
        M->skyTab[r][0] = Mix3(base, base, 0);
        M->skyTab[r][1] = Mix3(base, white, 90);
        M->skyTab[r][2] = Mix3(base, white, 180);
        M->skyTab[r][3] = Mix3(far, far, 0);
        M->skyTab[r][4] = Mix3(near, near, 0);
    }
    for (r = 0; r < FP_ROWS / 2 - FP_SKY_R; r++)
    {
        s32 z = 12800 / (2 * (FP_SKY_R + r) - 79);

        M->floorZ[r] = z > 65535 ? 65535 : z;
        M->floorLvl[r] = z > FP_MAX_Z ? 4 : FogLevel(z);
    }
}

// Slide the cell caches (solid / floor texture, indexed mod 32) with the window: only the cells that entered the +-10 window are cleared.
static void SmoothResetWindow(s32 tx, s32 ty)
{
    s32 ox = sFp.winX, oy = sFp.winY, x, y;

    memset(M->faces, 0, sizeof(M->faces));
    memset(M->texPin, 0, sizeof(M->texPin));
    if (ox < tx - 2 || ox > tx + 2 || oy < ty - 2 || oy > ty + 2)
    {
        memset(M->solid, 0, sizeof(M->solid));
        memset(M->cellTex, 0, sizeof(M->cellTex));
    }
    else
    {
        for (x = tx - 10; x <= tx + 10; x++)
        {
            for (y = ty - 10; y <= ty + 10; y++)
            {
                if (x < ox - 10 || x > ox + 10 || y < oy - 10 || y > oy + 10)
                {
                    M->solid[((y & 31) << 5) | (x & 31)] = 0;
                    M->cellTex[((y & 31) << 5) | (x & 31)] = NULL;
                }
            }
        }
    }
    sFp.winX = tx;
    sFp.winY = ty;
    BuildScenery();
}

static void SmoothInit(void)
{
    u32 i;

    memset(M->slotQ, 0, sizeof(M->slotQ));
    memset(M->lut, 0, sizeof(M->lut));
    memset(M->darkMap, 0, sizeof(M->darkMap));
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
    memset(M->solid, 0, sizeof(M->solid));
    memset(M->cellTex, 0, sizeof(M->cellTex));
    SmoothBuildRows();
    if (!M->recipPerp[40])
    {
        for (i = 1; i < 2560; i++)
            M->recipPerp[i] = (6553600 / (i < 40 ? 40 : i)) >> 2;
        for (i = 1; i < 512; i++)
            M->recipRay[i] = 65536 / i > 65535 ? 65535 : 65536 / i;
    }
    sFp.winX = -1000;
    sFp.smoothReady = TRUE;
}

// Sky strip: `count` rows of 2 px (drawn as 4 px blocks, a last odd row as 2 px) of one 4 px column from the panorama column `pan`
static void SkyStrip(u8 *dst, s32 count, const u8 *pan, const u8 (*tab)[6])
{
    s32 r;

    for (r = 0; r < (count >> 1); r++)
    {
        u32 slot = tab[r][(pan[r >> 1] >> ((r & 1) << 2)) & 15] * 0x01010101;

        *(u32 *)dst = slot;
        *(u32 *)(dst + 240) = slot;
        *(u32 *)(dst + 480) = slot;
        *(u32 *)(dst + 720) = slot;
        dst += 960;
    }
    if (count & 1)
    {
        u32 slot = tab[r][(pan[r >> 1] >> ((r & 1) << 2)) & 15] * 0x01010101;

        *(u32 *)dst = slot;
        *(u32 *)(dst + 240) = slot;
    }
}

static const u8 *FloorCell(s32 cx, s32 cy)
{
    s32 dx = cx - sFp.winX + 10, dy = cy - sFp.winY + 10;
    const u8 *t;

    if ((u32)dx >= FP_FWIN || (u32)dy >= FP_FWIN)
        return FloorTex(cx, cy);
    t = M->cellTex[((cy & 31) << 5) | (cx & 31)];
    if (!t)
        t = M->cellTex[((cy & 31) << 5) | (cx & 31)] = FloorTex(cx, cy);
    return t;
}

// Floor: perspective floor casting over the lower half below each column's wall (colYe), textured with the metatile under each block.
// Rows are cast row by row so the world position only needs adding per block (Q16 cells). Block size follows the texel size on screen:
// 8x2 haze rows, then 8x4 (far), 4x4 (middle) and 8x4 (nearest rows, where a texel is wider than 6 px). Hot loop in IWRAM; the metatile
// is only looked up again when the position enters another cell.
FP_IWRAM static void FloorRows(u8 *vram, s32 camX, s32 camY, s32 dirX, s32 dirY, s32 planeX, s32 planeY)
{
    const u8 *lut = &M->lut[0][0];
    const u8 *ye = M->colYe;
    s32 k, nk;

    for (k = 0; k < FP_ROWS / 2 - FP_SKY_R; k += nk)
    {
        s32 r = FP_SKY_R + k, c;
        u32 *dst = (u32 *)(vram + r * 480);
        u32 lvl = M->floorLvl[k];

        if (lvl == 4)
        {
            u32 slot = M->hazeSlot * 0x01010101;

            nk = 1;
            for (c = 0; c < FP_SMOOTH_COLS; c++)
                if (r >= ye[c])
                    dst[c] = dst[c + 60] = slot;
        }
        else
        {
            s32 z = M->floorZ[k + 1];   // middle of the two row pairs
            s32 px = planeX * z, py = planeY * z;
            s32 cstep = (r < FP_FLOOR_MID || r >= FP_FLOOR_NEAR) ? 2 : 1;
            s32 sx = ((px * 2184) >> 16) * cstep, sy = ((py * 2184) >> 16) * cstep;
            s32 wx = camX * 256 + dirX * z + (((px >> 6) * -64428) >> 10);
            s32 wy = camY * 256 + dirY * z + (((py >> 6) * -64428) >> 10);
            const u8 *lutRow = lut + lvl * 512;
            const u8 *t = NULL;
            s32 lastCx = -1000, lastCy = -1000;

            nk = 2;
            for (c = 0; c < FP_SMOOTH_COLS; c += cstep)
            {
                s32 ye0 = ye[c], ye1 = cstep == 2 ? ye[c + 1] : 0;

                // blocks that lie entirely above the wall bottom are covered by the wall: skip them (the position still advances)
                if (r + 1 >= ye0 || (cstep == 2 && r + 1 >= ye1))
                {
                    s32 cx = wx >> 16, cy = wy >> 16;
                    u32 pix, slot;

                    if (cx != lastCx || cy != lastCy)
                    {
                        t = FloorCell(cx, cy);
                        lastCx = cx;
                        lastCy = cy;
                    }
                    pix = t[(((wy >> 12) & 15) << 4) | ((wx >> 12) & 15)];
                    slot = lutRow[pix];
                    if (!slot)
                        slot = PixSlot(lvl, pix);
                    slot *= 0x01010101;
                    if (r >= ye0)
                        dst[c] = dst[c + 60] = slot;
                    if (r + 1 >= ye0)
                        dst[c + 120] = dst[c + 180] = slot;
                    if (cstep == 2)
                    {
                        if (r >= ye1)
                            dst[c + 1] = dst[c + 61] = slot;
                        if (r + 1 >= ye1)
                            dst[c + 121] = dst[c + 181] = slot;
                    }
                }
                wx += sx;
                wy += sy;
            }
        }
    }
}

// One wall strip (FP_SMOOTH_COLW pixels wide), rows y0..y1-1, two rows per sample: the hot loop, kept in IWRAM.
// wide = 4 px; otherwise 2 px (dst points at the 2 px half of a near-field column).
FP_IWRAM static void WallStrip(u8 *dst, s32 count, const struct FpFace *face, s32 tx, s32 pos, s32 step, const u8 *lutRow, u32 lvl, bool32 wide)
{
    s32 tmax = face->h * 16 - 1;
    u32 *d = (u32 *)dst;

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
        if (wide)
        {
            slot *= 0x01010101;
            d[0] = slot;
            d[60] = slot;
        }
        else
        {
            slot *= 0x0101;
            *(u16 *)d = slot;
            *(u16 *)((u8 *)d + 240) = slot;
        }
        d += 120;
        pos += step * 2;
    }
}

// Mid/near-field detail: a 4 px column whose centre ray hits a wall at FP_NEAR_LO <= perp < FP_NEAR_HI (Q8 cells) is cast again as two
// 2 px columns with 1-row blocks. A texel is 1600/perp px wide: below ~4 px (perp >= 400) the 4x2 blocks drop texture detail; closer than
// FP_NEAR_LO the texels are wider than the block anyway, and the wall is tall (expensive), so it stays cheap.
#define FP_NEAR_LO      400
#define FP_NEAR_HI      500
EWRAM_DATA u16 gFpNearOff = 0;   // debug: the test harness writes 1 to see the view without the near-field columns, 2 = 1 plus one redraw

struct FpCast
{
    s32 camX, camY, dirX, dirY, planeX, planeY;
};

struct FpWall
{
    const struct FpFace *face;
    s32 tx, y0, y1, p0, step, lvl, ytopQ8, mapX, mapY, perp;
    u16 zFar, zScen;
    bool8 hit, side, flip, scen;   // scen: the wall is a far scenery block (tree, fence), not a building
};

// Casts the ray through the screen offset px2 (half pixels from the centre) and fills
// the wall strip parameters. rh = rows per sample (1 or 2): y0 is aligned down to it, y1 is the exact bottom (clamped).
static void CastCol(const struct FpCast *cs, s32 px2, s32 rh, struct FpWall *w)
{
    s32 camx = (px2 * 273) >> 8;                       // (px2 / 2) * 546 >> 8
    s32 rayX = cs->dirX + (cs->planeX * camx >> 8), rayY = cs->dirY + (cs->planeY * camx >> 8);
    s32 mapX = cs->camX >> 8, mapY = cs->camY >> 8;
    s32 fracX = cs->camX & 255, fracY = cs->camY & 255;
    s32 aX = rayX < 0 ? -rayX : rayX, aY = rayY < 0 ? -rayY : rayY;
    s32 deltaX = aX ? M->recipRay[aX > 511 ? 511 : aX] : 0x3FFFFFFF;
    s32 deltaY = aY ? M->recipRay[aY > 511 ? 511 : aY] : 0x3FFFFFFF;
    s32 stepX = rayX < 0 ? -1 : 1, stepY = rayY < 0 ? -1 : 1;
    s32 sideX = rayX < 0 ? (fracX * deltaX) >> 8 : ((256 - fracX) * deltaX) >> 8;
    s32 sideY = rayY < 0 ? (fracY * deltaY) >> 8 : ((256 - fracY) * deltaY) >> 8;
    s32 side = 0, i, perp = 0;
    u32 k;

    w->hit = FALSE;
    w->scen = FALSE;
    w->zFar = 0xFFFF;
    w->zScen = 0xFFFF;
    w->y0 = w->y1 = FP_HORIZON;
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
        k = CellKind(sFp.winX, sFp.winY, mapX, mapY);
        if (k == 2)
        {
            w->hit = TRUE;
            break;
        }
        if (k == 3)
        {
            s32 zNow = side == 0 ? sideX - deltaX : sideY - deltaY;

            if (zNow >= FP_SCENERY_WALL)
            {
                w->hit = TRUE;
                w->scen = TRUE;   // far scenery is a cheap wall block (the billboards would cost too much, and the trees are small there)
                break;
            }
            if (w->zScen == 0xFFFF)
                w->zScen = zNow + FP_SCENERY_DEPTH;
        }
    }
    if (!w->hit)
        return;
    {
        const struct FpFace *face = GetFace(mapX, mapY);
        s32 u, tx, ytopQ8, ybotQ8, y0, y1;

        perp = side == 0 ? sideX - deltaX : sideY - deltaY;
        if (perp < 40)
            perp = 40;
        if (side == 0)
        {
            tx = ((cs->camY + (perp * rayY >> 8)) & 255) >> 4;
            if (rayX < 0)
                tx = 15 - tx;
        }
        else
        {
            tx = ((cs->camX + (perp * rayX >> 8)) & 255) >> 4;
            if (rayY > 0)
                tx = 15 - tx;
        }
        u = M->recipPerp[perp > 2559 ? 2559 : perp] << 2;     // pixels per cell, Q8
        ybotQ8 = FP_HORIZON * 256 + u / 2;
        ytopQ8 = FP_HORIZON * 256 - u * (2 * face->h - 1) / 2;
        w->step = (perp * 10486) >> 8;                        // texels per pixel, Q16 (2^28 / u)
        y0 = ytopQ8 < 0 ? 0 : ytopQ8 >> 8;
        y0 &= ~(rh - 1);
        w->p0 = (s32)(((s64)((y0 * 256 + 128 * rh) - ytopQ8) * w->step) >> 8);
        if (w->p0 < 0)
            w->p0 = 0;
        y1 = ybotQ8 >> 8;
        if (y1 > FP_ROWS)
            y1 = FP_ROWS;
        w->face = face;
        w->ytopQ8 = ytopQ8;
        w->mapX = mapX;
        w->mapY = mapY;
        w->side = side;
        w->perp = perp;
        w->flip = side == 0 ? rayX < 0 : rayY > 0;
        w->tx = tx;
        w->y0 = y0;
        w->y1 = y1;
        w->lvl = FogLevel(perp) + (side ? 4 : 0);
        w->zFar = perp;
    }
}

// A sub column of a column whose centre ray already hit a wall: the depth (perp) of a flat wall is the same for every ray, so when the
// hit point of the sub ray is still inside the same wall face only the texture column changes. Falls back to a full cast otherwise.
static void SubCol(const struct FpCast *cs, s32 px2, s32 rh, const struct FpWall *cw, struct FpWall *w)
{
    s32 camx = (px2 * 273) >> 8;
    s32 rayX = cs->dirX + (cs->planeX * camx >> 8), rayY = cs->dirY + (cs->planeY * camx >> 8);
    s32 perp = cw->perp, tx, h, y0;

    if (cw->side == 0)
    {
        h = cs->camY + (perp * rayY >> 8);
        if ((h >> 8) != cw->mapY || (rayX < 0) != cw->flip)
            goto full;
        tx = (h & 255) >> 4;
        if (rayX < 0)
            tx = 15 - tx;
    }
    else
    {
        h = cs->camX + (perp * rayX >> 8);
        if ((h >> 8) != cw->mapX || (rayY > 0) != cw->flip)
            goto full;
        tx = (h & 255) >> 4;
        if (rayY > 0)
            tx = 15 - tx;
    }
    *w = *cw;
    w->tx = tx;
    y0 = cw->ytopQ8 < 0 ? 0 : cw->ytopQ8 >> 8;
    y0 &= ~(rh - 1);
    w->y0 = y0;
    w->p0 = (s32)(((s64)((y0 * 256 + 128 * rh) - cw->ytopQ8) * cw->step) >> 8);
    if (w->p0 < 0)
        w->p0 = 0;
    return;
full:
    CastCol(cs, px2, rh, w);
}

// Wall shading pass (ROM, after the floor): a soft contact shadow where a wall meets the floor (two bands) and a dark line on the
// side face next to a vertical corner or a depth jump. Pixels are remapped through slot -> darker slot maps (built lazily),
// whole words are read and written because VRAM does not take byte writes.
static __attribute__((noinline)) u32 DarkSlow(u32 s, u32 m)
{
    u16 c = M->pal[s];
    u32 f = 184;   // same factor as the wall-base fog level (lvl & 8), so both land on the same palette slots

    if (M->numSlots >= FP_DARK_MAX)
        return M->darkMap[m][s] = s;
    return M->darkMap[m][s] = GetSlot((((c & 31) * f >> 8)) | ((((c >> 5) & 31) * f >> 8) << 5) | ((((c >> 10) & 31) * f >> 8) << 10));
}

static inline u32 DarkPix(u32 s, const u8 *map, u32 m)
{
    u32 d = map[s];

    return d ? d : (s ? DarkSlow(s, m) : 0);
}

static u32 DarkWord(u32 v, u32 m)
{
    const u8 *map = M->darkMap[m];

    if (v == (v & 255) * 0x01010101)
        return DarkPix(v & 255, map, m) * 0x01010101;   // a flat 4 px block: one lookup

    return DarkPix(v & 255, map, m) | (DarkPix((v >> 8) & 255, map, m) << 8) | (DarkPix((v >> 16) & 255, map, m) << 16) | (DarkPix(v >> 24, map, m) << 24);
}

static void EdgeLine(u8 *vram, s32 c, s32 off, const struct FpEdge *e)
{
    s32 y;
    const u8 *map = M->darkMap[0];

    for (y = e->y0; y < e->y1; y++)
    {
        u16 *q = (u16 *)(vram + y * 240 + c * 4 + off);
        u32 v = *q;

        *q = DarkPix(v & 255, map, 0) | (DarkPix(v >> 8, map, 0) << 8);
    }
}

static void WallShading(u8 *vram)
{
    s32 c;

    for (c = 0; c < FP_SMOOTH_COLS; c++)
    {
        const struct FpEdge *e = &M->colEdge[c];
        u32 *p;
        s32 ye;

        if (!e->kind || (e->kind & 4) || e->p > 64)      // far walls (> 5 cells): the contact band would be a pixel or two
            continue;
        ye = e->y1;
        if (ye + 2 <= FP_ROWS && ye >= FP_HORIZON)
        {
            // wall base (last two rows) and the floor under it (two bands)
            p = (u32 *)(vram + ye * 240 + c * 4);
            {
                u32 v0 = p[0], v1 = p[60], d0 = DarkWord(v0, 0);

                p[0] = d0;
                p[60] = v1 == v0 ? d0 : DarkWord(v1, 0);   // the two rows of a floor block are the same word
            }
        }
        if (c > 0 && M->colEdge[c - 1].kind)
        {
            const struct FpEdge *l = &M->colEdge[c - 1];
            // corner (the wall changes between front and side face): dark 2 px line on the side face
            if (!((e->kind | l->kind) & 4) && e->kind != l->kind)
                EdgeLine(vram, (e->kind & 3) == 2 ? c : c - 1, (e->kind & 3) == 2 ? 0 : 2, (e->kind & 3) == 2 ? e : l);
        }
    }
}

static void DrawSmooth(u32 page)
{
    u8 *vram = (u8 *)VRAM + page * FP_PAGE_SIZE;
    s32 a = (sFp.angleQ4 >> 4) & 255;
    s32 dirX = SinB(a), dirY = -CosB(a), rightX = CosB(a), rightY = SinB(a);
    s32 planeX = rightX * FP_PLANE >> 8, planeY = rightY * FP_PLANE >> 8;
    s32 camX = sFp.camX, camY = sFp.camY;
    s32 c;
    struct MapPosition pos;
    struct FpCast cs;
    struct FpWall w[2];

    if (!sFp.smoothReady)
        SmoothInit();
    GetPlayerPosition(&pos);
    if (pos.x != sFp.winX || pos.y != sFp.winY)
        SmoothResetWindow(pos.x, pos.y);
    cs.camX = camX;
    cs.camY = camY;
    cs.dirX = dirX;
    cs.dirY = dirY;
    cs.planeX = planeX;
    cs.planeY = planeY;

    for (c = 0; c < FP_SMOOTH_COLS; c++)
    {
        s32 px2 = c * 8 + 4 - 240;      // column centre in half pixels from the screen centre
        s32 n = 1, j, ys, ye;

        CastCol(&cs, px2, FP_SMOOTH_ROWH, &w[0]);
        M->colEdge[c].kind = w[0].hit && w[0].y1 > w[0].y0 ? 1 + w[0].side + (w[0].scen ? 4 : 0) : 0;
        M->colEdge[c].p = w[0].perp >> 4 > 255 ? 255 : w[0].perp >> 4;
        M->colEdge[c].y0 = w[0].y0;
        M->colEdge[c].y1 = w[0].y1;
        if (w[0].hit && w[0].zFar >= FP_NEAR_LO && w[0].zFar < FP_NEAR_HI && !gFpNearOff)
        {
            // near wall: two 2 px columns (rays through the pixel centres at 1 and 3 px), 1-row blocks
            struct FpWall cw = w[0];

            SubCol(&cs, px2 - 2, 2, &cw, &w[0]);
            SubCol(&cs, px2 + 2, 2, &cw, &w[1]);
            n = 2;
        }
        if (n == 1)
        {
            ys = ye = FP_HORIZON;
            if (w[0].hit && w[0].y1 > w[0].y0)
            {
                s32 cnt = (w[0].y1 - w[0].y0 + FP_SMOOTH_ROWH - 1) / FP_SMOOTH_ROWH;

                ys = w[0].y0;
                ye = ys + cnt * FP_SMOOTH_ROWH;
            }
            SkyStrip(vram + c * FP_SMOOTH_COLW, ys >> 1, M->skyPan + ((((sFp.angleQ4 >> 4) + ((c * 1213 >> 10) - 35)) & (FP_SKY_W - 1)) * (FP_SKY_R / 4)), M->skyTab);
            if (ye > ys)
            {
                s32 cnt = (ye - ys) / FP_SMOOTH_ROWH;

                if (cnt > 1 && w[0].y1 < FP_ROWS && w[0].perp < 1024 && !w[0].scen && !gFpPolishOff)
                {
                    // last sample row pair darker: contact shadow at the wall base (separate fog level, no extra per pixel work)
                    WallStrip(vram + ys * 240 + c * FP_SMOOTH_COLW, cnt - 1, w[0].face, w[0].tx, w[0].p0, w[0].step, M->lut[w[0].lvl], w[0].lvl, TRUE);
                    WallStrip(vram + (ye - FP_SMOOTH_ROWH) * 240 + c * FP_SMOOTH_COLW, 1, w[0].face, w[0].tx, w[0].p0 + (cnt - 1) * w[0].step * FP_SMOOTH_ROWH, w[0].step, M->lut[w[0].lvl + 8], w[0].lvl + 8, TRUE);
                }
                else
                    WallStrip(vram + ys * 240 + c * FP_SMOOTH_COLW, cnt, w[0].face, w[0].tx, w[0].p0, w[0].step, M->lut[w[0].lvl], w[0].lvl, TRUE);
            }
            M->colYe[c] = ye >> 1;
            M->cols[c * 2].z = M->cols[c * 2 + 1].z = w[0].zFar;
            M->cols[c * 2].zs = M->cols[c * 2 + 1].zs = w[0].zScen;
        }
        else
        {
            // common sky bottom (the lower of the two wall tops, even) and common floor start (the lower of the two bottoms, rounded up to even)
            s32 y0m = 0, y1m = 0;

            for (j = 0; j < 2; j++)
            {
                s32 t0 = w[j].hit ? w[j].y0 : FP_HORIZON, t1 = w[j].hit ? w[j].y1 : FP_HORIZON;

                if (t0 > y0m)
                    y0m = t0;
                if (t1 > y1m)
                    y1m = t1;
            }
            ys = (y0m + 1) & ~1;
            ye = (y1m + 1) & ~1;
            SkyStrip(vram + c * FP_SMOOTH_COLW, ys >> 1, M->skyPan + ((((sFp.angleQ4 >> 4) + ((c * 1213 >> 10) - 35)) & (FP_SKY_W - 1)) * (FP_SKY_R / 4)), M->skyTab);
            for (j = 0; j < 2; j++)
            {
                if (w[j].hit)
                {
                    s32 y0 = w[j].y0;   // drawn over the sky strip (which reaches down to the lower of the two tops)

                    if (ye > y0)
                    {
                        s32 cnt = (ye - y0) / 2;

                        if (cnt > 1 && w[j].y1 < FP_ROWS && w[j].perp < 1024 && !w[j].scen && !gFpPolishOff)
                        {
                            WallStrip(vram + y0 * 240 + c * FP_SMOOTH_COLW + j * 2, cnt - 1, w[j].face, w[j].tx, w[j].p0, w[j].step, M->lut[w[j].lvl], w[j].lvl, FALSE);
                            WallStrip(vram + (ye - 2) * 240 + c * FP_SMOOTH_COLW + j * 2, 1, w[j].face, w[j].tx, w[j].p0 + (cnt - 1) * w[j].step * 2, w[j].step, M->lut[w[j].lvl + 8], w[j].lvl + 8, FALSE);
                        }
                        else
                            WallStrip(vram + y0 * 240 + c * FP_SMOOTH_COLW + j * 2, cnt, w[j].face, w[j].tx, w[j].p0, w[j].step, M->lut[w[j].lvl], w[j].lvl, FALSE);
                    }
                }
                M->cols[c * 2 + j].z = w[j].zFar;
                M->cols[c * 2 + j].zs = w[j].zScen;
            }
            M->colYe[c] = ye >> 1;
        }
    }
    FloorRows(vram, camX, camY, dirX, dirY, planeX, planeY);
    if (!gFpPolishOff)
        WallShading(vram);

    // billboards (same renderer as the stepping view)
    {
        struct FpBill *bills = M->bills;
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
        n = CollectScenery(bills, n, camX, camY, dirX, dirY, rightX, rightY);
        CpuCopy16(&gPlttBufferFaded[256], &M->srcPal[256], 256 * 2);
        sViewDirX = dirX;
        sViewDirY = dirY;
        sViewRightX = rightX;
        sViewRightY = rightY;
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

// The free movement is designed for 30 ticks per second (one tick = one 2-frame redraw). A redraw that takes 3 or more frames (busy scenes,
// scenery) would slow walking and turning down: the speed per tick is scaled by the frames the tick took, never below 1x (so a 30 fps scene
// behaves exactly as before). Q1: 2 = 1x.
static EWRAM_DATA u32 sFreeLastV = 0;
static EWRAM_DATA s32 sFreeScale = 0;   // 0 until the first tick

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

static void TrailPush(s32 x, s32 y)
{
    u32 i;

    if (sFp.trailN == FP_TRAIL_MAX)
    {
        for (i = 1; i < FP_TRAIL_MAX; i++)
        {
            sFp.trailX[i - 1] = sFp.trailX[i];
            sFp.trailY[i - 1] = sFp.trailY[i];
        }
        sFp.trailN--;
    }
    sFp.trailX[sFp.trailN] = x;
    sFp.trailY[sFp.trailN] = y;
    sFp.trailN++;
}

// Pushing straight into a ledge in the direction it jumps: ask the normal field code for the jump (PlayerStep gets the direction,
// CheckForPlayerAvatarCollision reports COLLISION_LEDGE_JUMP and PlayerJumpLedge runs, exactly as for a d-pad press).
static void LedgeCheck(enum Direction dir, s32 f, s32 s)
{
    const struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
    s16 x = pl->currentCoords.x, y = pl->currentCoords.y;

    if (f <= 0 || s != 0 || dir != sFp.heading)
        return;
    MoveCoords(dir, &x, &y);
    if (GetLedgeJumpDirection(x, y, dir) != DIR_NONE)
        sFp.ledgeDir = dir;
}

static void FreeStep(s32 f, s32 s, bool32 run)
{
    struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
    s32 a = (sFp.angleQ4 >> 4) & 255;
    s32 fwdX = SinB(a), fwdY = -CosB(a), rightX = CosB(a), rightY = SinB(a);
    s32 sp = ((run ? FP_FREE_RUN : FP_FREE_WALK) * (sFreeScale < 2 ? 2 : sFreeScale)) >> 1;
    s32 vx = fwdX * f + rightX * s, vy = fwdY * f + rightY * s;
    s32 dx, dy, nx, ny, i;

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
        else
            LedgeCheck(dx > 0 ? DIR_EAST : DIR_WEST, f, s);
    }
    if (dy)
    {
        ny = sFp.freeY + dy;
        if (!FreeBoxBlocked(sFp.freeX, ny, dy > 0 ? DIR_SOUTH : DIR_NORTH))
            sFp.freeY = ny;
        else
            LedgeCheck(dy > 0 ? DIR_SOUTH : DIR_NORTH, f, s);
    }
    if ((sFp.freeX >> 8) != pl->currentCoords.x || (sFp.freeY >> 8) != pl->currentCoords.y)
    {
        // entered a new tile: move the real player there and let the normal "finished a step" hooks run this tick
        s32 tdx = (sFp.freeX >> 8) - pl->currentCoords.x, tdy = (sFp.freeY >> 8) - pl->currentCoords.y;

        s16 fromX = pl->currentCoords.x, fromY = pl->currentCoords.y;

        MoveObjectEventToMapCoords(pl, sFp.freeX >> 8, sFp.freeY >> 8);
        ObjectEventUpdateElevation(pl, &gSprites[pl->spriteId]);
        // Follower: remember the tile we left; FollowerTrail() feeds it to the real follower logic one tile at a time
        TrailPush(fromX, fromY);
        if (tdx && tdy)
            TrailPush(fromX + tdx, fromY);   // diagonal: pass the corner so every hop is one tile
        pl->movementDirection = tdx > 0 ? DIR_EAST : tdx < 0 ? DIR_WEST : tdy > 0 ? DIR_SOUTH : DIR_NORTH;
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
                for (i = 0; i < sFp.trailN; i++)
                {
                    sFp.trailX[i] += gCamera.x;
                    sFp.trailY[i] += gCamera.y;
                }
            }
        }
        gPlayerAvatar.tileTransitionState = T_TILE_CENTER;
        gPlayerAvatar.runningState = MOVING;
        sFp.emulated = TRUE;
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

    if (!locked && (gPlayerAvatar.runningState == MOVING || gPlayerAvatar.tileTransitionState == T_TILE_TRANSITION))
    {
        // a real movement is running (ledge jump): glide the view to where the avatar is going, no input
        const struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
        s32 gx = pl->currentCoords.x * 256 + 128 - sFp.freeX, gy = pl->currentCoords.y * 256 + 128 - sFp.freeY;

        sFp.freeX += gx > 16 ? 16 : gx < -16 ? -16 : gx;
        sFp.freeY += gy > 16 ? 16 : gy < -16 ? -16 : gy;
        sFp.targetQ4 = sFp.angleQ4;
        *newKeys &= ~strip;
        *heldKeys &= ~strip;
        return;
    }
    FreeSyncToPlayer();
    sFreeScale = (s32)(gMain.vblankCounter1 - sFreeLastV);
    sFreeLastV = gMain.vblankCounter1;
    if (sFreeScale < 2 || sFreeScale > 8)
        sFreeScale = 2;   // a pause (battle, menu, text, first tick), not a slow redraw
    else if (sFreeScale > 5)
        sFreeScale = 5;
    if (!locked)
    {
        if (hk & DPAD_LEFT)
            sFp.angleQ4 -= (FP_FREE_TURN * sFreeScale) >> 1;
        if (hk & DPAD_RIGHT)
            sFp.angleQ4 += (FP_FREE_TURN * sFreeScale) >> 1;
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

u8 FirstPerson_LedgeDir(void)
{
    u8 d = sFp.ledgeDir;

    sFp.ledgeDir = DIR_NONE;
    return d;
}

bool8 FirstPerson_FreeMovement(void)
{
    return sFp.shown && sFp.free;
}

// Free mode teleports the player from tile to tile, so the game's own follower logic (which walks the follower to the player's
// previousCoords while the player's copyable movement is WALK) never sees a step. Feed it the trail of tiles the player left, one
// at a time, after the field input has run: the follower then walks and animates through the normal code path.
static void FollowerTrail(void)
{
    struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
    struct ObjectEvent *fo;

    if (!sFp.free || !sFp.trailN)
        return;
    fo = GetFollowerObject();
    if (fo == NULL || !IsFollowerVisible())
    {
        sFp.trailN = 0;
        return;
    }
    // pop the tiles the follower has reached (a hidden follower is only shadowing the player: it has not come out yet)
    while (sFp.trailN && !fo->invisible && fo->currentCoords.x == sFp.trailX[0] && fo->currentCoords.y == sFp.trailY[0])
    {
        u32 i;

        for (i = 1; i < sFp.trailN; i++)
        {
            sFp.trailX[i - 1] = sFp.trailX[i];
            sFp.trailY[i - 1] = sFp.trailY[i];
        }
        sFp.trailN--;
    }
    if (!sFp.trailN)
        return;
    if (!fo->invisible && !fo->singleMovementActive)
    {
        s32 d = fo->currentCoords.x - sFp.trailX[0], e = fo->currentCoords.y - sFp.trailY[0];

        if ((d < 0 ? -d : d) + (e < 0 ? -e : e) > 1)
            MoveObjectEventToMapCoords(fo, sFp.trailX[0], sFp.trailY[0]);   // fell too far behind: put it on the trail
    }
    pl->previousCoords.x = sFp.trailX[0];
    pl->previousCoords.y = sFp.trailY[0];
    pl->playerCopyableMovement = COPY_MOVE_WALK;
    // The follower's own callback runs after the player's, whose held "face" action collapses previousCoords again before the
    // follower looks at it. So run the follower's active-state logic here, in the same state it would be called in.
    if (gSprites[fo->spriteId].data[1] == 1)   // sTypeFuncId 1 = active (0 = shadowing, 2 = executing a step)
        MovementType_FollowPlayer_Active(fo, &gSprites[fo->spriteId]);
}

void FirstPerson_EndOfInput(void)
{
    // The step was only emulated for this tick. If the field input started a script (trainer sight, warp...) instead of calling
    // PlayerStep, the avatar would stay in MOVING and scripts waiting for IsPlayerStandingStill() would never continue.
    if (sFp.emulated)
    {
        sFp.emulated = FALSE;
        gPlayerAvatar.runningState = NOT_MOVING;
        gPlayerAvatar.tileTransitionState = T_NOT_MOVING;
    }
    FollowerTrail();
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
        REG_TM3CNT_H = 0;
        REG_TM3CNT_L = 0;
        REG_TM3CNT_H = TIMER_ENABLE | TIMER_64CLK;   // render time probe: 64 cycles per tick
        s32 lines;

        if (sFp.smooth)
            DrawSmooth(page);
        else
        {
            sFp.smoothReady = FALSE;
            Draw(page);
        }
        lines = (s32)(REG_TM3CNT_L * 5 / 96);   // scanlines (1232 cycles)
        REG_TM3CNT_H = 0;
        gFpStats[0] = lines;
        gFpSlots[0] = M->numSlots;
        if (M->numSlots > gFpSlots[1])
            gFpSlots[1] = M->numSlots;
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
    sFp.trailN = 0;
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

// A trainer spotted the player: turn the player object and the view towards the approaching trainer (the real game keeps the
// player facing wherever it was; with the camera glued to the facing direction the approach would play out behind our back).
static void FaceTrainer(void)
{
    u8 id = GetCurrentApproachingTrainerObjectEventId();
    struct ObjectEvent *pl = &gObjectEvents[gPlayerAvatar.objectEventId];
    s32 diff;

    if (id < OBJECT_EVENTS_COUNT && !sFp.facedTrainer)
    {
        const struct ObjectEvent *tr = &gObjectEvents[id];
        s32 dx = tr->currentCoords.x - pl->currentCoords.x, dy = tr->currentCoords.y - pl->currentCoords.y;
        s32 ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;

        if (ax || ay)
        {
            sFp.heading = ax >= ay ? (dx > 0 ? DIR_EAST : DIR_WEST) : (dy > 0 ? DIR_SOUTH : DIR_NORTH);
            sFp.headingValid = TRUE;
            sFp.targetQ4 = HeadingBrads(sFp.heading) * 16;
            ObjectEventTurn(pl, sFp.heading);
        }
        sFp.facedTrainer = TRUE;
    }
    // glide the view round the short way
    diff = (((sFp.targetQ4 - sFp.angleQ4 + 2048) & 4095) - 2048);
    if (diff)
    {
        s32 step = diff > FP_FREE_TURN * 2 ? FP_FREE_TURN * 2 : diff < -FP_FREE_TURN * 2 ? -FP_FREE_TURN * 2 : diff;

        sFp.angleQ4 = (sFp.angleQ4 + step) & 4095;
        sFp.dirty = TRUE;
    }
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
        if (M)
            memset(M->texId, 0, sizeof(M->texId));   // scenery textures are matted in smooth mode only
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

    if (sFp.shown && gNoOfApproachingTrainers != 0)
        FaceTrainer();
    else
        sFp.facedTrainer = FALSE;

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
        sFp.trailN = 0;
        HideMapNamePopUpWindow();   // its tilemap/tile uploads would land in the bitmap pages
        RenderBackPage(TRUE);
        return FALSE;
    }

    if (sFp.smooth)
    {
        SmoothCamera();
        if (sFp.dirty || gFpNearOff == 2 || gFpPolishOff == 2 || sFp.camX != sFp.lastCamX || sFp.camY != sFp.lastCamY || sFp.angleQ4 != sFp.lastAngle || SmoothObjectSignature() != sFp.lastSig)
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
