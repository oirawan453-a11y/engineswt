// SwitchCE - cheat engine sederhana untuk Nintendo Switch (Atmosphere, dmnt:cht)
// Konsep sama seperti Cheat Engine di PC: scan nilai -> ubah di game -> scan lanjutan -> edit/freeze.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <switch.h>
#include "dmntcht.h"

// ---------------------------------------------------------------- tipe data

typedef enum { T_U8, T_U16, T_U32, T_U64, T_F32, T_F64, T_COUNT } ValType;
static const char *TYPE_NAME[T_COUNT] = {"u8 (1 byte)", "u16 (2 byte)", "u32 (4 byte)",
                                         "u64 (8 byte)", "float", "double"};
static const u32 TYPE_SIZE[T_COUNT] = {1, 2, 4, 8, 4, 8};

typedef enum { C_EQ, C_CHANGED, C_UNCHANGED, C_INC, C_DEC, C_COUNT } CmpMode;
static const char *CMP_NAME[C_COUNT] = {"Sama dengan nilai...", "Berubah", "Tidak berubah",
                                        "Naik", "Turun"};

static const char *REGION_NAME[4] = {"main", "heap", "alias", "aslr"};

typedef struct { u64 addr; u64 val; } Hit;

#define CHUNK_SIZE   0x100000
#define MAX_FROZEN   64
#define PAGE_ROWS    28
#define FLOAT_TOL    0.5

// ---------------------------------------------------------------- state global

static Hit *g_hits;
static u64 g_hit_cap, g_hit_count;
static bool g_has_scan, g_overflow;
static ValType g_type = T_U32, g_scan_type = T_U32;

static DmntCheatProcessMetadata g_meta;
static MemoryInfo *g_maps;
static u64 g_map_count;

static u64 g_frozen[MAX_FROZEN];
static int g_frozen_count;

static PadState g_pad;
static char g_status[160];

// ---------------------------------------------------------------- helper nilai

static bool is_float(ValType t) { return t == T_F32 || t == T_F64; }

static u64 load_val(const u8 *p, ValType t) {
    u64 v = 0;
    memcpy(&v, p, TYPE_SIZE[t]);
    return v;
}

static double as_double(u64 v, ValType t) {
    if (t == T_F32) { float f; u32 x = (u32)v; memcpy(&f, &x, 4); return f; }
    double d; memcpy(&d, &v, 8); return d;
}

static bool match(u64 cur, u64 prev, u64 target, CmpMode m, ValType t) {
    switch (m) {
        case C_EQ:
            if (is_float(t)) {
                double a = as_double(cur, t);
                return !isnan(a) && fabs(a - as_double(target, t)) <= FLOAT_TOL;
            }
            return cur == target;
        case C_CHANGED:   return cur != prev;
        case C_UNCHANGED: return cur == prev;
        case C_INC: return is_float(t) ? as_double(cur, t) > as_double(prev, t) : cur > prev;
        case C_DEC: return is_float(t) ? as_double(cur, t) < as_double(prev, t) : cur < prev;
        default: return false;
    }
}

static bool parse_val(const char *s, ValType t, u64 *out) {
    char *end;
    if (is_float(t)) {
        double d = strtod(s, &end);
        if (end == s) return false;
        if (t == T_F32) { float f = (float)d; u32 x; memcpy(&x, &f, 4); *out = x; }
        else memcpy(out, &d, 8);
        return true;
    }
    u64 u;
    if (s[0] == '-') u = (u64)strtoll(s, &end, 0);
    else u = strtoull(s, &end, 0);
    if (end == s) return false;
    if (TYPE_SIZE[t] < 8) u &= (1ULL << (TYPE_SIZE[t] * 8)) - 1;
    *out = u;
    return true;
}

static void fmt_val(u64 v, ValType t, char *buf, size_t n) {
    if (is_float(t)) snprintf(buf, n, "%g", as_double(v, t));
    else snprintf(buf, n, "%llu", (unsigned long long)v);
}

// ---------------------------------------------------------------- keyboard

static bool input_text(const char *header, const char *initial, bool numeric, char *out, size_t len) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    if (initial) swkbdConfigSetInitialText(&kbd, initial);
    if (numeric) {
        swkbdConfigSetType(&kbd, SwkbdType_NumPad);
        swkbdConfigSetLeftOptionalSymbolKey(&kbd, "-");
        swkbdConfigSetRightOptionalSymbolKey(&kbd, ".");
    }
    swkbdConfigSetStringLenMax(&kbd, (u32)len - 1);
    Result rc = swkbdShow(&kbd, out, len);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out[0] != '\0';
}

// ---------------------------------------------------------------- memori game

static bool attach(void) {
    Result rc = dmntchtInitialize();
    if (R_FAILED(rc)) {
        printf("Gagal konek ke dmnt:cht (0x%x).\nPastikan konsol pakai Atmosphere.\n", rc);
        return false;
    }
    bool has = false;
    dmntchtHasCheatProcess(&has);
    if (!has && R_FAILED(dmntchtForceOpenCheatProcess())) {
        printf("Tidak ada game yang sedang berjalan.\n"
               "Buka game dulu, tekan HOME, lalu buka app ini dari Homebrew Menu.\n");
        return false;
    }
    rc = dmntchtGetCheatProcessMetadata(&g_meta);
    if (R_FAILED(rc)) { printf("Gagal membaca info game (0x%x).\n", rc); return false; }
    return true;
}

static bool map_ok(const MemoryInfo *m) {
    if ((m->perm & Perm_Rw) != Perm_Rw) return false;
    return m->type == MemType_Heap || m->type == MemType_CodeMutable ||
           m->type == MemType_ModuleCodeMutable;
}

static void refresh_maps(void) {
    free(g_maps);
    g_maps = NULL;
    g_map_count = 0;
    u64 total = 0;
    if (R_FAILED(dmntchtGetCheatProcessMappingCount(&total)) || total == 0) return;
    MemoryInfo *all = malloc(total * sizeof(MemoryInfo));
    if (!all) return;
    u64 got = 0;
    dmntchtGetCheatProcessMappings(all, total, 0, &got);
    g_maps = malloc((got ? got : 1) * sizeof(MemoryInfo));
    for (u64 i = 0; i < got; i++)
        if (map_ok(&all[i])) g_maps[g_map_count++] = all[i];
    free(all);
}

// panjang maksimum yang aman dibaca mulai dari addr (tidak melewati ujung mapping)
static u64 readable_len(u64 addr) {
    u64 lo = 0, hi = g_map_count;
    while (lo < hi) {
        u64 mid = (lo + hi) / 2;
        const MemoryInfo *m = &g_maps[mid];
        if (addr < m->addr) hi = mid;
        else if (addr >= m->addr + m->size) lo = mid + 1;
        else {
            u64 left = m->addr + m->size - addr;
            return left < CHUNK_SIZE ? left : CHUNK_SIZE;
        }
    }
    return 0;
}

static int region_of(u64 addr, u64 *off) {
    const DmntMemoryRegionExtents r[3] = {g_meta.main_nso_extents, g_meta.heap_extents, g_meta.alias_extents};
    for (int i = 0; i < 3; i++) {
        if (r[i].size && addr >= r[i].base && addr < r[i].base + r[i].size) {
            *off = addr - r[i].base;
            return i;
        }
    }
    *off = addr - g_meta.address_space_extents.base;
    return 3;
}

static void progress(const char *what, u64 done, u64 total) {
    int pct = total ? (int)(done * 100 / total) : 100;
    printf("\x1b[44;1H%s... %3d%%   hasil: %llu        ", what, pct, (unsigned long long)g_hit_count);
    consoleUpdate(NULL);
}

// ---------------------------------------------------------------- scan

static void first_scan(u64 target) {
    g_scan_type = g_type;
    g_hit_count = 0;
    g_overflow = false;
    refresh_maps();
    u32 sz = TYPE_SIZE[g_scan_type];
    u8 *buf = malloc(CHUNK_SIZE);
    if (!buf) { snprintf(g_status, sizeof g_status, "Memori tidak cukup."); return; }

    u64 total = 0, done = 0;
    for (u64 r = 0; r < g_map_count; r++) total += g_maps[r].size;

    for (u64 r = 0; r < g_map_count && !g_overflow; r++) {
        const MemoryInfo *m = &g_maps[r];
        for (u64 off = 0; off < m->size && !g_overflow; off += CHUNK_SIZE) {
            u64 len = m->size - off < CHUNK_SIZE ? m->size - off : CHUNK_SIZE;
            done += len;
            if (R_FAILED(dmntchtReadCheatProcessMemory(m->addr + off, buf, len))) continue;
            for (u64 i = 0; i + sz <= len; i += sz) {
                u64 v = load_val(buf + i, g_scan_type);
                if (!match(v, 0, target, C_EQ, g_scan_type)) continue;
                if (g_hit_count >= g_hit_cap) { g_overflow = true; break; }
                g_hits[g_hit_count].addr = m->addr + off + i;
                g_hits[g_hit_count].val = v;
                g_hit_count++;
            }
            if ((off & 0x3FFFFFF) == 0) progress("Scan pertama", done, total);
        }
    }
    free(buf);
    g_has_scan = true;
    snprintf(g_status, sizeof g_status, g_overflow
             ? "Hasil terlalu banyak (dipotong di %llu). Ubah nilai di game lalu scan lanjutan."
             : "Scan selesai: %llu hasil.", (unsigned long long)g_hit_count);
}

static void next_scan(CmpMode mode, u64 target) {
    refresh_maps();
    u32 sz = TYPE_SIZE[g_scan_type];
    u8 *buf = malloc(CHUNK_SIZE);
    if (!buf) { snprintf(g_status, sizeof g_status, "Memori tidak cukup."); return; }
    u64 wbase = 0, wlen = 0, keep = 0, before = g_hit_count;

    for (u64 i = 0; i < g_hit_count; i++) {
        u64 a = g_hits[i].addr;
        if (!(wlen && a >= wbase && a + sz <= wbase + wlen)) {
            wbase = a;
            wlen = readable_len(a);
            if (wlen < sz || R_FAILED(dmntchtReadCheatProcessMemory(wbase, buf, wlen))) {
                wlen = 0;
                continue; // alamat sudah tidak valid, buang
            }
        }
        u64 cur = load_val(buf + (a - wbase), g_scan_type);
        if (match(cur, g_hits[i].val, target, mode, g_scan_type)) {
            g_hits[keep].addr = a;
            g_hits[keep].val = cur;
            keep++;
        }
        if ((i & 0x3FFFF) == 0) progress("Scan lanjutan", i, before);
    }
    g_hit_count = keep;
    free(buf);
    snprintf(g_status, sizeof g_status, "Scan lanjutan: %llu -> %llu hasil.",
             (unsigned long long)before, (unsigned long long)keep);
}

// ---------------------------------------------------------------- edit / freeze / simpan

static int frozen_index(u64 addr) {
    for (int i = 0; i < g_frozen_count; i++) if (g_frozen[i] == addr) return i;
    return -1;
}

static void toggle_freeze(u64 addr) {
    int idx = frozen_index(addr);
    if (idx >= 0) {
        dmntchtDisableFrozenAddress(addr);
        g_frozen[idx] = g_frozen[--g_frozen_count];
        snprintf(g_status, sizeof g_status, "Freeze dimatikan.");
        return;
    }
    if (g_frozen_count >= MAX_FROZEN) { snprintf(g_status, sizeof g_status, "Maksimal %d freeze.", MAX_FROZEN); return; }
    u64 v = 0;
    if (R_SUCCEEDED(dmntchtEnableFrozenAddress(addr, TYPE_SIZE[g_scan_type], &v))) {
        g_frozen[g_frozen_count++] = addr;
        snprintf(g_status, sizeof g_status, "Nilai dikunci (freeze).");
    } else snprintf(g_status, sizeof g_status, "Gagal freeze.");
}

static void edit_value(u64 addr, u64 cur) {
    char init[48], in[48];
    fmt_val(cur, g_scan_type, init, sizeof init);
    if (!input_text("Nilai baru", init, true, in, sizeof in)) return;
    u64 v;
    if (!parse_val(in, g_scan_type, &v)) { snprintf(g_status, sizeof g_status, "Angka tidak valid."); return; }
    bool frozen = frozen_index(addr) >= 0;
    if (frozen) dmntchtDisableFrozenAddress(addr);
    Result rc = dmntchtWriteCheatProcessMemory(addr, &v, TYPE_SIZE[g_scan_type]);
    if (frozen) { u64 dummy; dmntchtEnableFrozenAddress(addr, TYPE_SIZE[g_scan_type], &dummy); }
    snprintf(g_status, sizeof g_status, R_SUCCEEDED(rc) ? "Nilai diubah." : "Gagal menulis memori.");
}

static void build_id_hex(char *out) {
    for (int i = 0; i < 8; i++) sprintf(out + i * 2, "%02X", g_meta.main_nso_build_id[i]);
}

// Simpan sebagai kode Atmosphere tipe 0 (tulis nilai statis) dan aktifkan langsung.
static void save_cheat(u64 addr, u64 cur) {
    char name[0x40], init[48], in[48];
    if (!input_text("Nama cheat", "Cheat baru", false, name, sizeof name)) return;
    fmt_val(cur, g_scan_type, init, sizeof init);
    if (!input_text("Nilai yang ditulis cheat", init, true, in, sizeof in)) return;
    u64 v;
    if (!parse_val(in, g_scan_type, &v)) { snprintf(g_status, sizeof g_status, "Angka tidak valid."); return; }

    u64 off;
    int region = region_of(addr, &off);
    u32 width = TYPE_SIZE[g_scan_type];

    DmntCheatDefinition def;
    memset(&def, 0, sizeof def);
    snprintf(def.readable_name, sizeof def.readable_name, "%s", name);
    def.opcodes[0] = (width << 24) | ((u32)region << 20) | (u32)((off >> 32) & 0xFF);
    def.opcodes[1] = (u32)off;
    if (width == 8) { def.opcodes[2] = (u32)(v >> 32); def.opcodes[3] = (u32)v; def.num_opcodes = 4; }
    else { def.opcodes[2] = (u32)v; def.num_opcodes = 3; }

    char dir[128], path[160], bid[17];
    build_id_hex(bid);
    mkdir("sdmc:/atmosphere", 0777);
    mkdir("sdmc:/atmosphere/contents", 0777);
    snprintf(dir, sizeof dir, "sdmc:/atmosphere/contents/%016llX", (unsigned long long)g_meta.title_id);
    mkdir(dir, 0777);
    strncat(dir, "/cheats", sizeof dir - strlen(dir) - 1);
    mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/%s.txt", dir, bid);

    FILE *f = fopen(path, "a");
    if (!f) { snprintf(g_status, sizeof g_status, "Gagal menulis %s", path); return; }
    fprintf(f, "[%s]\n", name);
    for (u32 i = 0; i < def.num_opcodes; i++) fprintf(f, "%08X%c", def.opcodes[i], i + 1 < def.num_opcodes ? ' ' : '\n');
    fprintf(f, "\n");
    fclose(f);

    u32 id;
    bool live = R_SUCCEEDED(dmntchtAddCheat(&def, true, &id));
    snprintf(g_status, sizeof g_status, "Tersimpan ke %s.txt%s", bid, live ? " dan langsung aktif." : ".");
}

// ---------------------------------------------------------------- tampilan

static void header(void) {
    char bid[17];
    build_id_hex(bid);
    printf("\x1b[1;1H\x1b[2J== SwitchCE ==  Title ID: %016llX  Build ID: %s\n",
           (unsigned long long)g_meta.title_id, bid);
    printf("--------------------------------------------------------------------------------\n");
}

static void footer(const char *keys) {
    printf("\x1b[42;1H--------------------------------------------------------------------------------\n");
    printf("%s\n", g_status);
    printf("\x1b[45;1H%s", keys);
}

static void draw_main(int cur) {
    header();
    const char *items[6];
    char t[64], r[64];
    snprintf(t, sizeof t, "Tipe data: < %s >", TYPE_NAME[g_type]);
    snprintf(r, sizeof r, "Lihat hasil (%llu)", (unsigned long long)g_hit_count);
    items[0] = t; items[1] = "Scan baru (nilai pasti)"; items[2] = "Scan lanjutan";
    items[3] = r; items[4] = "Reset scan"; items[5] = "Keluar";
    for (int i = 0; i < 6; i++) printf("%s %s\n", i == cur ? ">" : " ", items[i]);
    printf("\nCara pakai: cari nilai (misal uang 500) -> ubah di game -> scan lanjutan\n"
           "dengan nilai baru -> ulangi sampai hasil tinggal sedikit -> edit/freeze.\n");
    footer("[A] pilih  [Kiri/Kanan] ganti tipe  [+] keluar");
}

static void draw_next(int cur) {
    header();
    printf("Scan lanjutan (%llu hasil, tipe %s)\n\n", (unsigned long long)g_hit_count, TYPE_NAME[g_scan_type]);
    for (int i = 0; i < C_COUNT; i++) printf("%s %s\n", i == cur ? ">" : " ", CMP_NAME[i]);
    footer("[A] pilih  [B] kembali");
}

static void draw_results(u64 cur, u64 top) {
    header();
    printf("Hasil: %llu   (* = freeze)\n\n", (unsigned long long)g_hit_count);
    printf("  %-4s %-22s %-20s %s\n", "No", "Alamat", "Nilai sekarang", "Sebelumnya");
    for (u64 i = top; i < g_hit_count && i < top + PAGE_ROWS; i++) {
        u64 off, now = 0;
        int reg = region_of(g_hits[i].addr, &off);
        dmntchtReadCheatProcessMemory(g_hits[i].addr, &now, TYPE_SIZE[g_scan_type]);
        char a[32], b[32], loc[32];
        fmt_val(now, g_scan_type, a, sizeof a);
        fmt_val(g_hits[i].val, g_scan_type, b, sizeof b);
        snprintf(loc, sizeof loc, "%s+%010llX", REGION_NAME[reg], (unsigned long long)off);
        printf("%s%c%-4llu %-22s %-20s %s\n", i == cur ? ">" : " ",
               frozen_index(g_hits[i].addr) >= 0 ? '*' : ' ',
               (unsigned long long)i + 1, loc, a, b);
    }
    footer("[A] edit  [Y] freeze  [X] simpan cheat  [L/R] halaman  [B] kembali");
}

// ---------------------------------------------------------------- main

static void wait_exit(void) {
    printf("\nTekan + untuk keluar.\n");
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    if (!attach()) { wait_exit(); dmntchtExit(); consoleExit(NULL); return 0; }

    for (g_hit_cap = 2 * 1024 * 1024; g_hit_cap >= 65536; g_hit_cap /= 2)
        if ((g_hits = malloc(g_hit_cap * sizeof(Hit)))) break;
    if (!g_hits) { printf("Memori tidak cukup.\n"); wait_exit(); dmntchtExit(); consoleExit(NULL); return 0; }

    enum { S_MAIN, S_NEXT, S_RESULTS } screen = S_MAIN;
    int cur = 0;
    u64 rcur = 0, rtop = 0;
    bool redraw = true, quit = false;
    snprintf(g_status, sizeof g_status, "Siap. Maks %llu hasil per scan.", (unsigned long long)g_hit_cap);

    while (appletMainLoop() && !quit) {
        padUpdate(&g_pad);
        u64 k = padGetButtonsDown(&g_pad);
        if (k) redraw = true;

        if (screen == S_MAIN) {
            if (k & HidNpadButton_Plus) break;
            if (k & HidNpadButton_AnyUp) cur = (cur + 5) % 6;
            if (k & HidNpadButton_AnyDown) cur = (cur + 1) % 6;
            if (cur == 0 && (k & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight | HidNpadButton_A))) {
                if (g_has_scan) snprintf(g_status, sizeof g_status, "Reset scan dulu untuk ganti tipe data.");
                else g_type = (k & HidNpadButton_AnyLeft) ? (g_type + T_COUNT - 1) % T_COUNT : (g_type + 1) % T_COUNT;
            } else if (k & HidNpadButton_A) {
                char in[48];
                u64 v;
                switch (cur) {
                    case 1:
                        if (!input_text("Nilai yang dicari", NULL, true, in, sizeof in)) break;
                        if (!parse_val(in, g_type, &v)) { snprintf(g_status, sizeof g_status, "Angka tidak valid."); break; }
                        first_scan(v);
                        break;
                    case 2:
                        if (g_has_scan) { screen = S_NEXT; cur = 0; }
                        else snprintf(g_status, sizeof g_status, "Lakukan scan baru dulu.");
                        break;
                    case 3:
                        if (g_hit_count) { screen = S_RESULTS; rcur = rtop = 0; }
                        else snprintf(g_status, sizeof g_status, "Belum ada hasil.");
                        break;
                    case 4:
                        g_hit_count = 0; g_has_scan = false;
                        snprintf(g_status, sizeof g_status, "Scan direset.");
                        break;
                    case 5: quit = true; break;
                }
            }
        } else if (screen == S_NEXT) {
            if (k & HidNpadButton_B) { screen = S_MAIN; cur = 2; }
            if (k & HidNpadButton_AnyUp) cur = (cur + C_COUNT - 1) % C_COUNT;
            if (k & HidNpadButton_AnyDown) cur = (cur + 1) % C_COUNT;
            if (k & HidNpadButton_A) {
                u64 v = 0;
                bool ok = true;
                if (cur == C_EQ) {
                    char in[48];
                    ok = input_text("Nilai sekarang", NULL, true, in, sizeof in) && parse_val(in, g_scan_type, &v);
                    if (!ok) snprintf(g_status, sizeof g_status, "Dibatalkan / angka tidak valid.");
                }
                if (ok) { next_scan((CmpMode)cur, v); screen = S_MAIN; cur = 3; }
            }
        } else { // S_RESULTS
            if (k & HidNpadButton_B) { screen = S_MAIN; cur = 3; }
            if ((k & HidNpadButton_AnyUp) && rcur > 0) rcur--;
            if ((k & HidNpadButton_AnyDown) && rcur + 1 < g_hit_count) rcur++;
            if (k & HidNpadButton_R) rcur = rcur + PAGE_ROWS < g_hit_count ? rcur + PAGE_ROWS : g_hit_count - 1;
            if (k & HidNpadButton_L) rcur = rcur > PAGE_ROWS ? rcur - PAGE_ROWS : 0;
            if (rcur < rtop) rtop = rcur;
            if (rcur >= rtop + PAGE_ROWS) rtop = rcur - PAGE_ROWS + 1;
            if (g_hit_count) {
                u64 a = g_hits[rcur].addr, now = 0;
                dmntchtReadCheatProcessMemory(a, &now, TYPE_SIZE[g_scan_type]);
                if (k & HidNpadButton_A) edit_value(a, now);
                if (k & HidNpadButton_Y) toggle_freeze(a);
                if (k & HidNpadButton_X) save_cheat(a, now);
            }
        }

        if (redraw) {
            if (screen == S_MAIN) draw_main(cur);
            else if (screen == S_NEXT) draw_next(cur);
            else draw_results(rcur, rtop);
            redraw = false;
        }
        consoleUpdate(NULL);
    }

    free(g_hits);
    free(g_maps);
    dmntchtExit();
    consoleExit(NULL);
    return 0;
}
