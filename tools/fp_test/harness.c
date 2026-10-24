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
    while (m && fgets(l, sizeof l, m))
        if (sscanf(l, " 0x%x %255s", &a, sym) == 2 && !strcmp(sym, name)) { r = a; break; }
    if (m) fclose(m);
    return r;
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
            for (int i = 0; i < n; i++) core->runFrame(core);
        } else if (!strcmp(cmd, "press")) {
            sscanf(line, "%*s %255s %d", a, &n);
            core->setKeys(core, parseKeys(a));
            for (int i = 0; i < n; i++) core->runFrame(core);
            core->setKeys(core, 0);
            for (int i = 0; i < 8; i++) core->runFrame(core);
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
                core->runFrame(core);
                f++;
            }
            fprintf(stderr, "waitmode %u: %u frames\n", mode, f);
        } else if (!strcmp(cmd, "waitflip")) {
            // waitflip MAXFRAMES: run until the displayed page (DISPCNT bit 4) changes; print frames waited
            unsigned max, f = 0;
            sscanf(line, "%*s %u", &max);
            uint16_t start = core->busRead16(core, 0x04000000) & 0x10;
            while (f < max && (core->busRead16(core, 0x04000000) & 0x10) == start) {
                core->runFrame(core);
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
                core->runFrame(core);
                f++;
            }
            core->setKeys(core, 0);
            for (int i = 0; i < 12; i++) core->runFrame(core);
            fprintf(stderr, "pressflip: %u frames\n", f);
        } else if (!strcmp(cmd, "peek8")) {
            unsigned addr;
            sscanf(line, "%*s %x", &addr);
            fprintf(stderr, "%08x = %02x\n", addr, core->busRead8(core, addr));
        }
    }
    core->deinit(core);
    return 0;
}
