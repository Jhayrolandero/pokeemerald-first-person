// Headless mGBA harness. Usage: fp_harness <rom> <script>
// Script commands (one per line, '#' comments):
//   run N                 run N frames, no keys
//   press KEYS N          hold keys (A,B,SELECT,START,RIGHT,LEFT,UP,DOWN,R,L joined by '+') for N frames, then release 8 frames
//   shot FILE.ppm         write the 240x160 frame as PPM
//   save FILE / load FILE savestate
//   peek8 ADDR            print byte (bus address, e.g. 0x03005D90)
#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// from pokeemerald.map (gSaveBlock1Ptr) and offsetof(struct SaveBlock1, vars)
#define VARS_OFFSET 0x139c

// look a symbol up in pokeemerald.map (next to the ROM)
static char mapPath[512];
static uint32_t symbolAddr(const char *name) {
    FILE *m = fopen(mapPath, "r");
    char l[512];
    unsigned a;
    char sym[256];
    uint32_t r = 0;
    // prefer syms.txt (arm-none-eabi-nm -n output, includes static symbols), fall back to the map file
    char symPath[512];
    snprintf(symPath, sizeof symPath, "%.*s/tools/fp_test/syms.txt", (int)(strlen(mapPath) - 14), mapPath);
    FILE *sf = fopen("syms.txt", "r");
    if (sf) {
        unsigned sa; char st[8], sn[256];
        while (fgets(l, sizeof l, sf))
            if (sscanf(l, "%x %7s %255s", &sa, st, sn) == 3 && !strcmp(sn, name)) { fclose(sf); if (m) fclose(m); return sa; }
        fclose(sf);
    }
    while (m && fgets(l, sizeof l, m))
        if (sscanf(l, " 0x%x %255s", &a, sym) == 2 && !strcmp(sym, name)) { r = a; break; }
    if (m) fclose(m);
    return r;
}

// frame wrapper: optional per-frame capture ("capture PREFIX EVERY", "capture off")
static uint32_t *g_buf; static unsigned g_w, g_h;
static char g_capPrefix[256]; static int g_capOn, g_capEvery = 1; static unsigned g_capFrame, g_capIndex;
static void RunF(struct mCore *core) {
    core->runFrame(core);
    if (g_capOn && (g_capFrame++ % g_capEvery) == 0) {
        char name[300];
        snprintf(name, sizeof name, "%s_%05u.ppm", g_capPrefix, g_capIndex++);
        FILE *o = fopen(name, "wb");
        fprintf(o, "P6\n%u %u\n255\n", g_w, g_h);
        for (unsigned i = 0; i < g_w * g_h; i++) { uint32_t p = g_buf[i]; fputc(p & 0xFF, o); fputc((p >> 8) & 0xFF, o); fputc((p >> 16) & 0xFF, o); }
        fclose(o);
        { char ln[300]; snprintf(ln, sizeof ln, "%s.log", g_capPrefix); FILE *lf = fopen(ln, "a"); uint16_t d = core->busRead16(core, 0x04000000); fprintf(lf, "%05u emu_frame=%u dispcnt=%04x page=%u\n", g_capIndex - 1, g_capFrame - 1, d, (d >> 4) & 1); fclose(lf); }
    }
}

static uint32_t parseKeys(char *s) {
    uint32_t k = 0;
    for (char *t = strtok(s, "+"); t; t = strtok(NULL, "+")) {
        if (!strcmp(t, "A")) k |= 1 << GBA_KEY_A;
        else if (!strcmp(t, "B")) k |= 1 << GBA_KEY_B;
        else if (!strcmp(t, "SELECT")) k |= 1 << GBA_KEY_SELECT;
        else if (!strcmp(t, "START")) k |= 1 << GBA_KEY_START;
        else if (!strcmp(t, "RIGHT")) k |= 1 << GBA_KEY_RIGHT;
        else if (!strcmp(t, "LEFT")) k |= 1 << GBA_KEY_LEFT;
        else if (!strcmp(t, "UP")) k |= 1 << GBA_KEY_UP;
        else if (!strcmp(t, "DOWN")) k |= 1 << GBA_KEY_DOWN;
        else if (!strcmp(t, "R")) k |= 1 << GBA_KEY_R;
        else if (!strcmp(t, "L")) k |= 1 << GBA_KEY_L;
    }
    return k;
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    snprintf(mapPath, sizeof mapPath, "%.*smap", (int)(strlen(argv[1]) - 3), argv[1]);
    struct mCore *core = mCoreFind(argv[1]);
    if (!core) { fprintf(stderr, "no core\n"); return 1; }
    core->init(core);
    mCoreInitConfig(core, NULL);
    mCoreLoadFile(core, argv[1]);
    unsigned w, h;
    core->desiredVideoDimensions(core, &w, &h);
    uint32_t *buf = calloc(w * h, 4);
    g_buf = buf; g_w = w; g_h = h;
    core->setVideoBuffer(core, (color_t *) buf, w);
    core->setAudioBufferSize(core, 1024);
    core->reset(core);

    FILE *f = fopen(argv[2], "r");
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char cmd[32], a[256];
        int n = 0;
        if (line[0] == '#' || sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "run")) {
            sscanf(line, "%*s %d", &n);
            for (int i = 0; i < n; i++) RunF(core);
        } else if (!strcmp(cmd, "press")) {
            sscanf(line, "%*s %255s %d", a, &n);
            core->setKeys(core, parseKeys(a));
            for (int i = 0; i < n; i++) RunF(core);
            core->setKeys(core, 0);
            for (int i = 0; i < 8; i++) RunF(core);
        } else if (!strcmp(cmd, "shot")) {
            sscanf(line, "%*s %255s", a);
            FILE *o = fopen(a, "wb");
            fprintf(o, "P6\n%u %u\n255\n", w, h);
            for (unsigned i = 0; i < w * h; i++) {
                uint32_t p = buf[i]; // little-endian 0xAABBGGRR or BGR; mgba default is XBGR8
                fputc(p & 0xFF, o); fputc((p >> 8) & 0xFF, o); fputc((p >> 16) & 0xFF, o);
            }
            fclose(o);
        } else if (!strcmp(cmd, "save") || !strcmp(cmd, "load")) {
            sscanf(line, "%*s %255s", a);
            struct VFile *vf = VFileOpen(a, cmd[0] == 's' ? O_CREAT | O_TRUNC | O_RDWR : O_RDONLY);
            if (!vf) { fprintf(stderr, "cannot open %s\n", a); return 1; }
            if (cmd[0] == 's') mCoreSaveStateNamed(core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
            else mCoreLoadStateNamed(core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
            vf->close(vf);
        } else if (!strcmp(cmd, "setvar")) {
            // setvar VARID VALUE: write a script var through gSaveBlock1Ptr (addresses valid for this build, see README)
            unsigned id, val;
            sscanf(line, "%*s %x %u", &id, &val);
            uint32_t sb1 = core->busRead32(core, symbolAddr("gSaveBlock1Ptr"));
            core->busWrite16(core, sb1 + VARS_OFFSET + (id - 0x4000) * 2, val);
        } else if (!strcmp(cmd, "waitmode")) {
            // waitmode MODE MAXFRAMES: run until (DISPCNT & 7) == MODE and not forced blank; print frames waited
            unsigned mode, max, f = 0;
            sscanf(line, "%*s %u %u", &mode, &max);
            while (f < max) {
                uint16_t d = core->busRead16(core, 0x04000000);
                if ((d & 7) == mode && !(d & 0x80)) break;
                RunF(core);
                f++;
            }
            fprintf(stderr, "waitmode %u: %u frames\n", mode, f);
        } else if (!strcmp(cmd, "waitflip")) {
            // waitflip MAXFRAMES: run until the displayed page (DISPCNT bit 4) changes; print frames waited
            unsigned max, f = 0;
            sscanf(line, "%*s %u", &max);
            uint16_t start = core->busRead16(core, 0x04000000) & 0x10;
            while (f < max && (core->busRead16(core, 0x04000000) & 0x10) == start) {
                RunF(core);
                f++;
            }
            fprintf(stderr, "waitflip: %u frames\n", f);
        } else if (!strcmp(cmd, "pressflip")) {
            // pressflip KEYS MAXFRAMES: hold KEYS until the displayed page flips; print frames until the flip (redraw latency)
            unsigned max, f = 0;
            sscanf(line, "%*s %255s %u", a, &max);
            uint16_t start = core->busRead16(core, 0x04000000) & 0x10;
            core->setKeys(core, parseKeys(a));
            while (f < max && (core->busRead16(core, 0x04000000) & 0x10) == start) {
                RunF(core);
                f++;
            }
            core->setKeys(core, 0);
            for (int i = 0; i < 12; i++) RunF(core);
            fprintf(stderr, "pressflip: %u frames\n", f);
        } else if (!strcmp(cmd, "walkloop")) {
            // walkloop N BLOCK: step Up for BLOCK steps, then Down for BLOCK steps, and so on, up to N steps; stops as soon as DISPCNT leaves Mode 4
            unsigned n, blk = 1, i, f, left = 0;
            sscanf(line, "%*s %u %u", &n, &blk);
            for (i = 0; i < n && !left; i++) {
                core->setKeys(core, (i / blk) & 1 ? 1 << GBA_KEY_DOWN : 1 << GBA_KEY_UP);
                for (f = 0; f < 8; f++) RunF(core);
                core->setKeys(core, 0);
                for (f = 0; f < 30; f++) {
                    RunF(core);
                    if ((core->busRead16(core, 0x04000000) & 7) != 4) { left = 1; break; }
                }
            }
            fprintf(stderr, "walkloop: %u steps, left mode 4: %u\n", i, left);
        } else if (!strcmp(cmd, "dump")) {
            // dump ADDR LEN FILE: write guest memory to a file
            unsigned addr, len;
            sscanf(line, "%*s %x %u %255s", &addr, &len, a);
            FILE *o = fopen(a, "wb");
            for (unsigned i = 0; i < len; i++) fputc(core->busRead8(core, addr + i), o);
            fclose(o);
        } else if (!strcmp(cmd, "watch")) {
            // watch ADDR MAXFRAMES: single-step until the byte at ADDR changes; print pc/lr (find who overwrites memory)
            unsigned addr, max, f = 0;
            if (strchr(line, '*')) { sscanf(strchr(line, '*') + 1, "%x %u", &addr, &max); addr = core->busRead32(core, addr); }
            else sscanf(line, "%*s %x %u", &addr, &max);
            uint8_t start = core->busRead8(core, addr);
            int hit = 0;
            while (f < max && !hit) {
                int32_t fc = core->frameCounter(core);
                while (core->frameCounter(core) == fc) {
                    core->step(core);
                    if (core->busRead8(core, addr) != start) { hit = 1; break; }
                }
                f++;
            }
            uint32_t pc = 0, lr = 0, r0 = 0;
            core->readRegister(core, "pc", &pc);
            core->readRegister(core, "lr", &lr);
            core->readRegister(core, "r0", &r0);
            { uint32_t r[8], sp = 0; char nm[4]; for (int i = 1; i < 8; i++) { snprintf(nm, 4, "r%d", i); core->readRegister(core, nm, &r[i]); } core->readRegister(core, "sp", &sp);
              fprintf(stderr, "regs r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x sp=%08x\n", r[1], r[2], r[3], r[4], r[5], r[6], r[7], sp);
              for (int i = 0; i < 12; i++) fprintf(stderr, "stack+%02x=%08x\n", i * 4, core->busRead32(core, sp + i * 4)); }
            fprintf(stderr, "watch: hit=%d frames=%u pc=%08x lr=%08x r0=%08x new=%02x\n", hit, f, pc, lr, r0, core->busRead8(core, addr));
        } else if (!strcmp(cmd, "regs")) {
            // regs [TAG]: print display registers and gMain callbacks (resolve with: arm-none-eabi-nm -n pokeemerald.elf)
            char tag[64] = ""; if (strlen(line) > 5) { snprintf(tag, sizeof tag, "%s", line + 5); tag[strcspn(tag, "\n")] = 0; }
            uint32_t gm = symbolAddr("gMain");
            fprintf(stderr, "REGS %s DISPCNT=%04x BGCNT=%04x/%04x/%04x/%04x HOFS=%04x/%04x/%04x/%04x VOFS=%04x/%04x/%04x/%04x WININ=%04x WINOUT=%04x BLDCNT=%04x MOSAIC=%04x\n",
                    tag, core->busRead16(core, 0x4000000),
                    core->busRead16(core, 0x4000008), core->busRead16(core, 0x400000A), core->busRead16(core, 0x400000C), core->busRead16(core, 0x400000E),
                    core->busRead16(core, 0x4000010), core->busRead16(core, 0x4000014), core->busRead16(core, 0x4000018), core->busRead16(core, 0x400001C),
                    core->busRead16(core, 0x4000012), core->busRead16(core, 0x4000016), core->busRead16(core, 0x400001A), core->busRead16(core, 0x400001E),
                    core->busRead16(core, 0x4000048), core->busRead16(core, 0x400004A), core->busRead16(core, 0x4000050), core->busRead16(core, 0x400004C));
            { uint32_t gb = symbolAddr("sGpuRegBuffer"), fo = symbolAddr("sFieldCameraOffset"), hp = symbolAddr("sHorizontalCameraPan"), vp = symbolAddr("sVerticalCameraPan");
              uint32_t tx = symbolAddr("gTotalCameraPixelOffsetX"), cx = symbolAddr("gSpriteCoordOffsetX");
              fprintf(stderr, "SHADOW DISPCNT=%04x BGCNT=%04x/%04x/%04x/%04x HOFS=%04x/%04x/%04x/%04x VOFS=%04x/%04x/%04x/%04x locked=%02x\n", gb ? core->busRead16(core, gb) : 0,
                      core->busRead16(core, gb + 8), core->busRead16(core, gb + 10), core->busRead16(core, gb + 12), core->busRead16(core, gb + 14),
                      core->busRead16(core, gb + 16), core->busRead16(core, gb + 20), core->busRead16(core, gb + 24), core->busRead16(core, gb + 28),
                      core->busRead16(core, gb + 18), core->busRead16(core, gb + 22), core->busRead16(core, gb + 26), core->busRead16(core, gb + 30), core->busRead8(core, symbolAddr("sGpuRegBufferLocked")));
              fprintf(stderr, "CAMERA off=%d,%d tile=%d,%d pan=%d,%d total=%d,%d sprOff=%d,%d\n", core->busRead8(core, fo), core->busRead8(core, fo + 1), core->busRead8(core, fo + 2), core->busRead8(core, fo + 3),
                      (int16_t)core->busRead16(core, hp), (int16_t)core->busRead16(core, vp), (int16_t)core->busRead16(core, tx), (int16_t)core->busRead16(core, tx + 2), (int16_t)core->busRead16(core, cx), (int16_t)core->busRead16(core, cx + 2)); }
            fprintf(stderr, "FIELDCB fieldCallback=%08x fieldCallback2=%08x mainState=%u locked=%u\n", core->busRead32(core, symbolAddr("gFieldCallback")), core->busRead32(core, symbolAddr("gFieldCallback2")), core->busRead8(core, gm + 0x438), core->busRead8(core, symbolAddr("sLockFieldControls")));
            { uint32_t oe = symbolAddr("gObjectEvents"), sb1 = core->busRead32(core, symbolAddr("gSaveBlock1Ptr"));
              fprintf(stderr, "PLAYER obj0 tile=%d,%d prev=%d,%d facing=%u savepos=%d,%d map=%u/%u\n", (int16_t)core->busRead16(core, oe + 0x10), (int16_t)core->busRead16(core, oe + 0x12),
                      (int16_t)core->busRead16(core, oe + 0x18), (int16_t)core->busRead16(core, oe + 0x1A), core->busRead8(core, oe + 0x18 + 8) & 0xF,
                      (int16_t)core->busRead16(core, sb1), (int16_t)core->busRead16(core, sb1 + 2), core->busRead8(core, sb1 + 4), core->busRead8(core, sb1 + 5)); }
            fprintf(stderr, "GMAIN cb1=%08x cb2=%08x saved=%08x vblank=%08x\n", core->busRead32(core, gm), core->busRead32(core, gm + 4), core->busRead32(core, gm + 8), core->busRead32(core, gm + 12));
        } else if (!strcmp(cmd, "fuzz")) {
            // fuzz SEED N: N random button actions (hold 1-12 frames, gap 0-25 frames), then idle 600 frames
            unsigned seed, n;
            sscanf(line, "%*s %u %u", &seed, &n);
            srand(seed);
            static const int keyset[] = { GBA_KEY_L, GBA_KEY_A, GBA_KEY_B, GBA_KEY_START, GBA_KEY_SELECT, GBA_KEY_UP, GBA_KEY_DOWN, GBA_KEY_LEFT, GBA_KEY_RIGHT, GBA_KEY_R, GBA_KEY_UP, GBA_KEY_UP, GBA_KEY_L, GBA_KEY_A };
            for (unsigned i = 0; i < n; i++) {
                uint32_t k = 1u << keyset[rand() % (sizeof keyset / sizeof *keyset)];
                if (rand() % 6 == 0) k |= 1u << keyset[rand() % (sizeof keyset / sizeof *keyset)];
                core->setKeys(core, k);
                for (int f = rand() % 12 + 1; f > 0; f--) RunF(core);
                core->setKeys(core, 0);
                for (int f = rand() % 26; f > 0; f--) RunF(core);
                if (getenv("FUZZ_LOG")) {
                    uint32_t gm = symbolAddr("gMain"), gb = symbolAddr("sGpuRegBuffer");
                    fprintf(stderr, "FUZZ #%u keys=%03x DISPCNT=%04x shadow=%04x cb2=%08x\n", i, k, core->busRead16(core, 0x4000000), core->busRead16(core, gb), core->busRead32(core, gm + 4));
                }
            }
            core->setKeys(core, 0);
            if (!getenv("FUZZ_NOIDLE")) for (int f = 0; f < 600; f++) RunF(core);
        } else if (!strcmp(cmd, "framelog")) {
            // framelog N: run N frames printing DISPCNT (hardware/shadow) and callbacks each frame
            unsigned n; sscanf(line, "%*s %u", &n);
            uint32_t gm = symbolAddr("gMain"), gb = symbolAddr("sGpuRegBuffer");
            for (unsigned fi = 0; fi < n; fi++) {
                RunF(core);
                { uint32_t sc = symbolAddr("sGlobalScriptContext"); fprintf(stderr, "SCRIPT %u status=%u mode=%u scriptPtr=%08x native=%08x\n", fi, core->busRead8(core, symbolAddr("sGlobalScriptContextStatus")), core->busRead8(core, sc + 1), core->busRead32(core, sc + 8), core->busRead32(core, sc + 4)); }
                fprintf(stderr, "FRAME %u msgbox=%u DISPCNT=%04x shadow=%04x cb2=%08x vblank=%08x\n", fi, core->busRead8(core, symbolAddr("sFieldMessageBoxMode")), core->busRead16(core, 0x4000000), core->busRead16(core, gb), core->busRead32(core, gm + 4), core->busRead32(core, gm + 12));
            }
        } else if (!strcmp(cmd, "pc")) {
            // pc: print cpu registers (sample a few instructions to see where the code is looping) and a stack walk
            for (int i = 0; i < 6; i++) {
                uint32_t pc, lr, sp, r0, r1, r2, r3;
                core->readRegister(core, "pc", &pc); core->readRegister(core, "lr", &lr); core->readRegister(core, "sp", &sp);
                core->readRegister(core, "r0", &r0); core->readRegister(core, "r1", &r1); core->readRegister(core, "r2", &r2); core->readRegister(core, "r3", &r3);
                fprintf(stderr, "CPU pc=%08x lr=%08x sp=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", pc, lr, sp, r0, r1, r2, r3);
                for (int k = 0; k < 4000; k++) core->step(core);
            }
            { uint32_t sp; core->readRegister(core, "sp", &sp);
              for (int i = 0; i < 300; i++) { uint32_t v = core->busRead32(core, sp + i * 4); if (v >= 0x08000000 && v < 0x0A000000) fprintf(stderr, "STACK+%02x=%08x\n", i * 4, v); } }
        } else if (!strcmp(cmd, "strings")) {
            // strings: print ASCII strings pointed to by words in the current stack (find assert messages)
            uint32_t sp; core->readRegister(core, "sp", &sp);
            for (int i = 0; i < 400; i++) {
                uint32_t v = core->busRead32(core, sp + i * 4);
                if ((v >> 24) == 0x08 || (v >> 24) == 0x02) {
                    char buf[80]; int ok = 1, n = 0;
                    for (; n < 79; n++) { uint8_t c = core->busRead8(core, v + n); if (!c) break; if (c < 32 || c > 126) { ok = 0; break; } buf[n] = c; }
                    buf[n] = 0;
                    if (ok && n >= 6) fprintf(stderr, "STR sp+%x %08x \"%s\"\n", i * 4, v, buf);
                }
            }
        } else if (!strcmp(cmd, "printers")) {
            // printers: walk the text printer list (heap nodes) and show each node's heap block header
            uint32_t p = core->busRead32(core, symbolAddr("sFirstTextPrinter"));
            for (int i = 0; p && i < 8; i++) {
                uint32_t h0 = core->busRead32(core, p - 16), h1 = core->busRead32(core, p - 12), h2 = core->busRead32(core, p - 8), h3 = core->busRead32(core, p - 4);
                fprintf(stderr, "PRINTER node=%08x hdr=%08x %08x size=%u prev=%08x next=%08x isInUse/active byte@+0x1e=%02x nextPrinter=%08x\n", p, h0, h1, h2 & 0x3FFFF, h3, h2, core->busRead8(core, p + 0x1e), core->busRead32(core, p + 0x24));
                p = core->busRead32(core, p + 0x24);
            }
        } else if (!strcmp(cmd, "capture")) {
            // capture PREFIX [EVERY] | capture off: write every EVERY-th emulated frame as PREFIX_NNNNN.ppm
            char pre[256] = "";
            int every = 1;
            sscanf(line, "%*s %255s %d", pre, &every);
            if (!strcmp(pre, "off")) g_capOn = 0;
            else { snprintf(g_capPrefix, sizeof g_capPrefix, "%s", pre); { char ln[300]; snprintf(ln, sizeof ln, "%s.log", pre); remove(ln); } g_capEvery = every > 0 ? every : 1; g_capFrame = 0; g_capIndex = 0; g_capOn = 1; }
        } else if (!strcmp(cmd, "perf")) {
            // perf TAG N: run N frames and report page flips (rendered frames) and the in-ROM render time stats (gFpStats)
            unsigned n; char tag[64] = "";
            sscanf(line, "%*s %63s %u", tag, &n);
            uint32_t st = symbolAddr("gFpStats");
            uint32_t r0 = core->busRead32(core, st + 4), l0 = core->busRead32(core, st + 8);
            unsigned flips = 0; uint16_t last = core->busRead16(core, 0x04000000) & 0x10;
            for (unsigned i = 0; i < n; i++) {
                RunF(core);
                uint16_t d = core->busRead16(core, 0x04000000);
                if ((d & 7) == 4 && (d & 0x10) != last) { flips++; }
                last = d & 0x10;
            }
            uint32_t r1 = core->busRead32(core, st + 4), l1 = core->busRead32(core, st + 8);
            unsigned renders = r1 - r0;
            fprintf(stderr, "PERF %s: %u frames, %u page flips (%.1f fps), %u renders, avg %.0f scanlines (%.0f cycles) per render, max %u, last %u\n",
                    tag, n, flips, flips * 60.0 / n, renders, renders ? (double)(l1 - l0) / renders : 0.0, renders ? (double)(l1 - l0) / renders * 1232 : 0.0,
                    core->busRead32(core, st + 12), core->busRead32(core, st));
        } else if (!strcmp(cmd, "hold")) {
            // hold KEYS N: keep KEYS pressed for N frames (no release tail)
            char k[256] = ""; unsigned n;
            sscanf(line, "%*s %255s %u", k, &n);
            core->setKeys(core, parseKeys(k));
            for (unsigned i = 0; i < n; i++) RunF(core);
        } else if (!strcmp(cmd, "release")) {
            core->setKeys(core, 0);
        } else if (!strcmp(cmd, "profile")) {
            // profile SAMPLES STRIDE: sample the program counter every STRIDE instructions (pipe the PROF lines to profile.py)
            unsigned n, stride;
            sscanf(line, "%*s %u %u", &n, &stride);
            for (unsigned i = 0; i < n; i++) {
                uint32_t pc; core->readRegister(core, "pc", &pc);
                fprintf(stderr, "PROF %08x\n", pc);
                { int32_t fc0 = core->frameCounter(core); int32_t c0 = core->frameCycles(core); int32_t target = c0 + (int32_t)stride;
                  while (core->frameCounter(core) == fc0 && core->frameCycles(core) < target) core->step(core);
                  if (core->frameCounter(core) != fc0) { /* frame boundary: continue */ } }
            }
        } else if (!strcmp(cmd, "peek8")) {
            unsigned addr;
            sscanf(line, "%*s %x", &addr);
            fprintf(stderr, "%08x = %02x\n", addr, core->busRead8(core, addr));
        }
    }
    core->deinit(core);
    return 0;
}
