/*
 * LAN Mod installer for Saints Row: The Third Remastered (PC: Steam, GOG, Epic, ...)
 *
 *   LANMod_Installer.exe                 find the game, install
 *   LANMod_Installer.exe uninstall       find the game, restore the original files
 *   LANMod_Installer.exe status          show what is installed
 *   LANMod_Installer.exe [cmd] "<game folder>"   use that folder
 *   LANMod_Installer.exe build-menu "<game folder>" <out file>   (developers: write the patched
 *                                        menu container only, nothing is installed)
 *
 * What it does
 *  1. Menu: reads main_menu_coop_top.str2_pc from YOUR copy of cache\interface.vpp_pc, unpacks the
 *     menu script, adds the "LAN" entry (text edits that must each match exactly once), repacks it
 *     and writes it back into its own slot of the archive. The archive's directory entry and the
 *     container manifest (vint_doc_containers.asm_pc) get the new sizes. The exact original bytes
 *     are saved to cache\interface.vpp_pc.lanmod_backup first; uninstall restores and verifies them.
 *     No game file is shipped with the mod.
 *  2. LAN joining: copies the mod loader (dinput8.dll) and ploader\patches\lan_join.patch into
 *     the game folder. In the shipped PC version a LAN join always ends with "session is no longer
 *     available"; the loader applies lan_join.patch in memory at every start so LAN joining works.
 *     The game EXE is never modified.
 *
 * Everything is located by name / content, not by fixed file positions, so other store versions
 * work as long as the menu script is the same.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NONE64 0xFFFFFFFFFFFFFFFFull
static const uint8_t VPP_MAGIC[4] = { 0xce, 0x0a, 0x89, 0x51 };
static const uint8_t BLK_MAGIC[8] = { 0xee, 0xdb, 0xee, 0x0f, 0xee, 0xdb, 0xba, 0x00 };
static const char *STR2 = "main_menu_coop_top.str2_pc";
static const char *LUA = "main_menu_coop_top.lua";
static const char *ASM = "vint_doc_containers.asm_pc";
static const char ASM_RECORD[] = "\x1b\x00main_menu_coop_top.vint_doc\x15";
#define ASM_RECORD_LEN (sizeof ASM_RECORD - 1)
static const int ASM_FIELD[3] = { 0x34, 0x4C, 0xAA };   /* compressed size, lua size, lua size */
static int g_quiet_pause = 0;

static void die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("\nERROR: "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (!g_quiet_pause) { printf("\nPress Enter to close."); getchar(); }
    exit(1);
}

/* ------------------------------------------------------------------ small buffers */
typedef struct { uint8_t *p; size_t n, cap; } buf_t;
static void bput(buf_t *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 4096; b->p = realloc(b->p, b->cap); if (!b->p) die("out of memory"); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void bbyte(buf_t *b, uint8_t c) { bput(b, &c, 1); }
static void bzero_(buf_t *b, size_t n) { while (n--) bbyte(b, 0); }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint64_t al(uint64_t x) { return (x + 0xFFF) & ~0xFFFull; }

/* ------------------------------------------------------------------ files */
static int read_at(HANDLE h, uint64_t pos, void *dst, size_t n)
{
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)pos;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
    DWORD got; return ReadFile(h, dst, (DWORD)n, &got, NULL) && got == n;
}
static int write_at(HANDLE h, uint64_t pos, const void *src, size_t n)
{
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)pos;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
    DWORD put; return WriteFile(h, src, (DWORD)n, &put, NULL) && put == n;
}
static int file_exists(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static uint8_t *load_file(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(sz ? sz : 1); if (fread(d, 1, sz, f) != (size_t)sz) { fclose(f); free(d); return NULL; }
    fclose(f); *n = (size_t)sz; return d;
}

/* ------------------------------------------------------------------ VPP v6 directory */
typedef struct { char name[128]; uint64_t name_off, off, size, csize, flags; } vent_t;
typedef struct { uint32_t flags; uint64_t n, fsize, dsz, nsz, usz, csz, doff; vent_t *e; } vpp_t;

static int vpp_parse_mem(const uint8_t *d, size_t len, vpp_t *v)
{
    if (len < 0x1000 || memcmp(d, VPP_MAGIC, 4) || rd32(d + 4) != 6) return 0;
    v->flags = rd32(d + 0x14C);
    v->n = rd64(d + 0x158); v->fsize = rd64(d + 0x160); v->dsz = rd64(d + 0x168);
    v->nsz = rd64(d + 0x170); v->usz = rd64(d + 0x178); v->csz = rd64(d + 0x180);
    uint64_t noff = al(0x1000 + v->dsz); v->doff = al(noff + v->nsz);
    if (v->n > 100000 || noff + v->nsz > len) return 0;
    v->e = calloc(v->n ? v->n : 1, sizeof(vent_t));
    for (uint64_t i = 0; i < v->n; i++) {
        const uint8_t *r = d + 0x1000 + i * 0x30;
        vent_t *e = &v->e[i];
        e->name_off = rd64(r); e->off = rd64(r + 16); e->size = rd64(r + 24); e->csize = rd64(r + 32); e->flags = rd64(r + 40);
        if (e->name_off < v->nsz) { strncpy(e->name, (const char *)d + noff + e->name_off, 127); }
    }
    return 1;
}

/* archive on disk: read header + directory + names only (the archive is ~400 MB) */
static int vpp_parse_file(HANDLE h, vpp_t *v)
{
    uint8_t hdr[0x1000];
    if (!read_at(h, 0, hdr, sizeof hdr) || memcmp(hdr, VPP_MAGIC, 4) || rd32(hdr + 4) != 6) return 0;
    uint64_t dsz = rd64(hdr + 0x168), nsz = rd64(hdr + 0x170);
    uint64_t noff = al(0x1000 + dsz);
    size_t need = (size_t)(noff + nsz);
    if (need > 64u << 20) return 0;
    uint8_t *d = malloc(need);
    if (!read_at(h, 0, d, need)) { free(d); return 0; }
    int ok = vpp_parse_mem(d, need, v);
    free(d); return ok;
}

static int vpp_find(const vpp_t *v, const char *name)
{
    for (uint64_t i = 0; i < v->n; i++) if (!_stricmp(v->e[i].name, name)) return (int)i;
    return -1;
}

/* ------------------------------------------------------------------ LZ4 */
static int lz4_decode(const uint8_t *s, size_t n, uint8_t *dst, size_t usize)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t tok = s[i++]; size_t ll = tok >> 4;
        if (ll == 15) { uint8_t b; do { if (i >= n) return 0; b = s[i++]; ll += b; } while (b == 255); }
        if (i + ll > n || o + ll > usize) return 0;
        memcpy(dst + o, s + i, ll); i += ll; o += ll;
        if (i >= n) break;
        if (i + 2 > n) return 0;
        size_t off = s[i] | (s[i + 1] << 8); i += 2;
        if (!off || off > o) return 0;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { if (i >= n) return 0; b = s[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (o + ml > usize) return 0;
        for (size_t k = 0; k < ml; k++) { dst[o] = dst[o - off]; o++; }
    }
    return o == usize;
}

static void lz4_literals(buf_t *out, const uint8_t *d, size_t n)
{
    if (!n) return;
    if (n < 15) bbyte(out, (uint8_t)(n << 4));
    else { bbyte(out, 0xF0); size_t r = n - 15; while (r >= 255) { bbyte(out, 255); r -= 255; } bbyte(out, (uint8_t)r); }
    bput(out, d, n);
}

/* Same algorithm as tools/vpp_write.py lz4_compress (hash chains per exact 4-byte key, up to 256
   candidates, one-step lazy matching), so the output is byte-identical to the Python builder. */
typedef struct { uint32_t key; int32_t head; } kslot_t;
static kslot_t *g_ks; static size_t g_kcap;
static int32_t *g_ins_pos, *g_ins_prev; static size_t g_nins, g_inscap;
static int32_t *ks_head(uint32_t key, int create)
{
    size_t h = (key * 2654435761u) & (g_kcap - 1);
    for (;;) {
        if (g_ks[h].head == -2) { if (!create) return NULL; g_ks[h].key = key; g_ks[h].head = -1; return &g_ks[h].head; }
        if (g_ks[h].key == key) return &g_ks[h].head;
        h = (h + 1) & (g_kcap - 1);
    }
}
static uint32_t key4(const uint8_t *p) { uint32_t k; memcpy(&k, p, 4); return k; }
static void lz_insert(const uint8_t *d, size_t pos)
{
    int32_t *hd = ks_head(key4(d + pos), 1);
    if (g_nins == g_inscap) { g_inscap = g_inscap * 2 + 1024; g_ins_pos = realloc(g_ins_pos, g_inscap * 4); g_ins_prev = realloc(g_ins_prev, g_inscap * 4); }
    g_ins_pos[g_nins] = (int32_t)pos; g_ins_prev[g_nins] = *hd; *hd = (int32_t)g_nins; g_nins++;
}
static int lz_last_is(const uint8_t *d, size_t pos)
{
    int32_t *hd = ks_head(key4(d + pos), 0);
    return hd && *hd >= 0 && g_ins_pos[*hd] == (int32_t)pos;
}
static size_t lz_best(const uint8_t *d, size_t pos, size_t mflimit, size_t *cand_out)
{
    int32_t *hd = ks_head(key4(d + pos), 0);
    size_t bl = 0; size_t bc = (size_t)-1;
    if (!hd) return 0;
    int depth = 0;
    for (int32_t k = *hd; k >= 0 && depth < 256; k = g_ins_prev[k], depth++) {
        size_t cand = (size_t)g_ins_pos[k];
        if (pos - cand > 0xFFFF) break;
        size_t m = 4;
        while (pos + m < mflimit && d[cand + m] == d[pos + m]) m++;
        if (m > bl) { bl = m; bc = cand; }
    }
    *cand_out = bc; return bl;
}
static void lz4_compress(buf_t *out, const uint8_t *d, size_t n)
{
    if (n < 13) { lz4_literals(out, d, n); return; }
    g_kcap = 1; while (g_kcap < n * 2 + 16) g_kcap <<= 1;
    g_ks = malloc(g_kcap * sizeof(kslot_t));
    for (size_t i = 0; i < g_kcap; i++) g_ks[i].head = -2;
    g_nins = 0;
    size_t anchor = 0, i = 0, limit = n - 12, mflimit = n - 5;
    while (i < limit) {
        size_t cand = 0, m = lz_best(d, i, mflimit, &cand);
        if (m >= 4 && i + 1 < limit) {
            lz_insert(d, i);
            size_t c2 = 0, m2 = lz_best(d, i + 1, mflimit, &c2);
            if (m2 > m + 1) { i += 1; m = m2; cand = c2; }
        }
        if (m >= 4) {
            size_t ll = i - anchor, ml = m - 4;
            bbyte(out, (uint8_t)(((ll < 15 ? ll : 15) << 4) | (ml < 15 ? ml : 15)));
            if (ll >= 15) { size_t r = ll - 15; while (r >= 255) { bbyte(out, 255); r -= 255; } bbyte(out, (uint8_t)r); }
            bput(out, d + anchor, ll);
            uint16_t off = (uint16_t)(i - cand); bput(out, &off, 2);
            if (ml >= 15) { size_t r = ml - 15; while (r >= 255) { bbyte(out, 255); r -= 255; } bbyte(out, (uint8_t)r); }
            size_t end = i + m;
            for (size_t p = i; p < (end < limit ? end : limit); p++)
                if (p != i || !lz_last_is(d, p)) lz_insert(d, p);
            i = end; anchor = i;
        } else {
            lz_insert(d, i); i++;
        }
    }
    lz4_literals(out, d + anchor, n - anchor);
    free(g_ks); g_ks = NULL;
}

/* ------------------------------------------------------------------ the menu container (.str2_pc) */
typedef struct { char name[128]; uint8_t *data; size_t size; buf_t raw; } sent_t;   /* raw = original block bytes */

static int str2_unpack(const uint8_t *blob, size_t len, uint32_t *flags, sent_t **out, int *count)
{
    vpp_t v; if (!vpp_parse_mem(blob, len, &v)) return 0;
    if (!(v.flags & 1)) return 0;
    *flags = v.flags; *count = (int)v.n;
    sent_t *se = calloc(v.n ? v.n : 1, sizeof(sent_t));
    uint64_t pos = v.doff;
    for (uint64_t k = 0; k < v.n; k++) {
        vent_t *e = &v.e[k]; sent_t *s = &se[k];
        strcpy(s->name, e->name); s->size = (size_t)e->size; s->data = malloc(s->size ? s->size : 1);
        if (e->size == 0 || e->csize == NONE64) { if (e->size) return 0; continue; }
        while (pos + 8 <= len && memcmp(blob + pos, BLK_MAGIC, 8)) { if (blob[pos]) return 0; pos++; }   /* zero padding */
        uint64_t remain = e->csize, o = 0;
        while (remain > 0) {
            if (pos + 16 > len || memcmp(blob + pos, BLK_MAGIC, 8)) return 0;
            uint32_t c = rd32(blob + pos + 8), u = rd32(blob + pos + 12);
            if (pos + 16 + c > len || o + u > s->size) return 0;
            if (!lz4_decode(blob + pos + 16, c, s->data + o, u)) return 0;
            bput(&s->raw, blob + pos, 16 + c);
            o += u; pos += 16 + c; remain -= 16 + c;
        }
        if (o != s->size) return 0;
    }
    *out = se; free(v.e); return 1;
}

/* tools/vpp_write.py build() for compressed packs */
static void str2_build(buf_t *out, uint32_t flags, sent_t *se, int count)
{
    int condensed = (flags & 2) != 0;
    buf_t names = { 0 }; uint64_t *noffs = calloc(count, 8);
    for (int k = 0; k < count; k++) { noffs[k] = names.n; bput(&names, se[k].name, strlen(se[k].name) + 1); }
    uint64_t dsz = 0x30ull * count, noff = al(0x1000 + dsz), doff = al(noff + names.n);
    buf_t body = { 0 }, dir = { 0 }; uint64_t uoff = 0;
    for (int k = 0; k < count; k++) {
        uint64_t off;
        if (condensed) { off = uoff; uoff += se[k].size; }
        else { off = k ? al(uoff) : 0; uoff = off + se[k].size; if (body.n) bzero_(&body, al(body.n) - body.n); }
        uint64_t csize = NONE64;
        if (se[k].size) { csize = se[k].raw.n; bput(&body, se[k].raw.p, se[k].raw.n); }
        uint64_t rec[6] = { noffs[k], 0, off, se[k].size, csize, 0 };
        bput(&dir, rec, sizeof rec);
    }
    uint8_t h[0x1000] = { 0 };
    memcpy(h, VPP_MAGIC, 4); wr32(h + 4, 6); wr32(h + 0x14C, flags);
    uint64_t f6[6] = { (uint64_t)count, doff + body.n, dsz, names.n, uoff, body.n };
    memcpy(h + 0x158, f6, sizeof f6);
    bput(out, h, sizeof h); bput(out, dir.p, dir.n); bzero_(out, noff - out->n);
    bput(out, names.p, names.n); bzero_(out, doff - out->n); bput(out, body.p, body.n);
    free(names.p); free(body.p); free(dir.p); free(noffs);
}

/* ------------------------------------------------------------------ the menu script edits */
typedef struct { const char *old, *rep; } edit_t;
static const edit_t EDITS[] = {
    { "local ID_COOP_OPTIONS\t\t= 3\r\n",
      "local ID_COOP_OPTIONS\t\t= 3\r\n"
      "local ID_SYSLINK\t\t\t= 4\t-- LAN mod: restored System Link entry\r\n"
      "\r\n"
      "local Syslink_button = {\r\n"
      "\ttype = TYPE_BUTTON,\r\n"
      "\tlabel = \"MAINMENU_SYSLINK_TITLE\",\r\n"
      "\tid = ID_SYSLINK,\r\n"
      "}\r\n" },
    { "\t\tData[2] = Xbox_live_button\r\n\t\tData[3] = Coop_options_button\r\n",
      "\t\tData[2] = Xbox_live_button\r\n\t\tData[3] = Syslink_button\r\n\t\tData[4] = Coop_options_button\r\n" },
    { "\t\tData[1] = Xbox_live_button\r\n\t\tData[2] = Coop_options_button\r\n",
      "\t\tData[1] = Xbox_live_button\r\n\t\tData[2] = Syslink_button\r\n\t\tData[3] = Coop_options_button\r\n" },
    { "\t\telseif current_id == ID_COOP_OPTIONS then\r\n",
      "\t\telseif current_id == ID_SYSLINK then\r\n"
      "\t\t\t-- LAN mod: same DLC gate as online, then switch the co-op menus to System Link mode\r\n"
      "\t\t\tif Whored_mode_active then\r\n"
      "\t\t\t\tInput_tracker:subscribe(false)\r\n"
      "\t\t\t\tgame_check_coop_dlc(\"main_menu_coop_top_dlc_check_lan\")\r\n"
      "\t\t\telse\r\n"
      "\t\t\t\tmain_menu_coop_top_dlc_check_lan(true, false)\r\n"
      "\t\t\tend\r\n"
      "\t\t\treturn\r\n"
      "\t\telseif current_id == ID_COOP_OPTIONS then\r\n" },
};

static size_t count_occ(const uint8_t *d, size_t n, const char *s)
{
    size_t m = strlen(s), c = 0;
    for (size_t i = 0; i + m <= n; i++) if (!memcmp(d + i, s, m)) { c++; i += m - 1; }
    return c;
}
static int lua_has_mod(const uint8_t *d, size_t n) { return count_occ(d, n, "ID_SYSLINK") > 0; }

static void patch_lua(const uint8_t *src, size_t n, buf_t *out)
{
    buf_t cur = { 0 }; bput(&cur, src, n);
    for (size_t k = 0; k < sizeof EDITS / sizeof *EDITS; k++) {
        size_t c = count_occ(cur.p, cur.n, EDITS[k].old);
        if (c != 1) die("the menu script is not the one this mod expects (edit %d matched %d times).\n"
                        "Your game version may differ; nothing was changed.", (int)k + 1, (int)c);
        size_t m = strlen(EDITS[k].old), i = 0;
        while (memcmp(cur.p + i, EDITS[k].old, m)) i++;
        buf_t nx = { 0 };
        bput(&nx, cur.p, i); bput(&nx, EDITS[k].rep, strlen(EDITS[k].rep)); bput(&nx, cur.p + i + m, cur.n - i - m);
        free(cur.p); cur = nx;
    }
    *out = cur;
}

/* Build the patched container from the original one. Also returns the manifest values. */
static void build_menu(const uint8_t *orig, size_t olen, buf_t *out, uint32_t vals_old[3], uint32_t vals_new[3])
{
    uint32_t flags; sent_t *se; int cnt;
    if (!str2_unpack(orig, olen, &flags, &se, &cnt)) die("could not read %s (unexpected format); nothing was changed.", STR2);
    int li = -1;
    for (int k = 0; k < cnt; k++) if (!_stricmp(se[k].name, LUA)) li = k;
    if (li < 0) die("%s has no %s; nothing was changed.", STR2, LUA);
    if (lua_has_mod(se[li].data, se[li].size)) die("the menu already contains the LAN entry");
    vals_old[0] = (uint32_t)rd64(orig + 0x180); vals_old[1] = vals_old[2] = (uint32_t)se[li].size;
    buf_t lua; patch_lua(se[li].data, se[li].size, &lua);
    buf_t pay = { 0 }; lz4_compress(&pay, lua.p, lua.n);
    se[li].raw.n = 0;
    uint8_t bh[16]; memcpy(bh, BLK_MAGIC, 8); wr32(bh + 8, (uint32_t)pay.n); wr32(bh + 12, (uint32_t)lua.n);
    bput(&se[li].raw, bh, 16); bput(&se[li].raw, pay.p, pay.n);
    free(se[li].data); se[li].data = lua.p; se[li].size = lua.n;
    str2_build(out, flags, se, cnt);
    /* self-check: unpack what we built and compare the script */
    uint32_t f2; sent_t *s2; int c2;
    if (!str2_unpack(out->p, out->n, &f2, &s2, &c2) || f2 != flags || c2 != cnt || s2[li].size != lua.n || memcmp(s2[li].data, lua.p, lua.n))
        die("internal check of the rebuilt menu failed; nothing was changed.");
    vals_new[0] = (uint32_t)rd64(out->p + 0x180); vals_new[1] = vals_new[2] = (uint32_t)lua.n;
}

/* ------------------------------------------------------------------ install / uninstall the menu */
typedef struct {
    char magic[8]; uint64_t archive_size, dir_pos, slot_pos, slot_len, asm_pos[3]; uint32_t asm_val[3];
    uint8_t dir[0x30];   /* followed by slot_len bytes of the original slot */
} backup_hdr_t;

typedef struct { HANDLE h; vpp_t v; int k; uint64_t dir_pos, slot_pos, room, size; uint64_t asm_pos[3]; uint32_t asm_val[3]; } menu_loc_t;

static void locate_menu(const char *game, menu_loc_t *L, int writable)
{
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\cache\\interface.vpp_pc", game);
    L->h = CreateFileA(path, GENERIC_READ | (writable ? GENERIC_WRITE : 0), FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (L->h == INVALID_HANDLE_VALUE) die(writable ? "cannot open %s for writing (is the game running?)" : "cannot open %s", path);
    if (!vpp_parse_file(L->h, &L->v)) die("%s is not a packfile this mod understands", path);
    if (L->v.flags & 1) die("interface.vpp_pc is compressed in your version; this mod expects it uncompressed");
    L->k = vpp_find(&L->v, STR2); if (L->k < 0) die("%s not found in interface.vpp_pc", STR2);
    vent_t *e = &L->v.e[L->k];
    uint64_t nxt = (uint64_t)-1;
    for (uint64_t i = 0; i < L->v.n; i++) if (L->v.e[i].off > e->off && L->v.e[i].off < nxt) nxt = L->v.e[i].off;
    L->dir_pos = 0x1000 + 0x30ull * L->k; L->slot_pos = L->v.doff + e->off; L->room = nxt - e->off; L->size = e->size;
    /* manifest record */
    int ak = vpp_find(&L->v, ASM); if (ak < 0) die("%s not found", ASM);
    vent_t *a = &L->v.e[ak];
    uint8_t *blob = malloc((size_t)a->size);
    if (!read_at(L->h, L->v.doff + a->off, blob, (size_t)a->size)) die("cannot read %s", ASM);
    size_t hit = (size_t)-1; int nh = 0;
    for (size_t i = 0; i + ASM_RECORD_LEN <= a->size; i++) if (!memcmp(blob + i, ASM_RECORD, ASM_RECORD_LEN)) { hit = i; nh++; }
    if (nh != 1) die("menu record found %d times in %s (expected once)", nh, ASM);
    if (hit + 0xB0 > a->size || memcmp(blob + hit + 0x90, "main_menu_coop_top.lua", 22)) die("menu record layout differs in %s", ASM);
    for (int f = 0; f < 3; f++) { L->asm_pos[f] = L->v.doff + a->off + hit + ASM_FIELD[f]; L->asm_val[f] = rd32(blob + hit + ASM_FIELD[f]); }
    free(blob);
}

static void backup_path(const char *game, char *out) { snprintf(out, MAX_PATH, "%s\\cache\\interface.vpp_pc.lanmod_backup", game); }

static int menu_installed(menu_loc_t *L, uint8_t **cur_out)
{
    uint8_t *cur = malloc((size_t)L->size);
    if (!read_at(L->h, L->slot_pos, cur, (size_t)L->size)) die("cannot read the menu container");
    uint32_t fl; sent_t *se; int cnt;
    if (!str2_unpack(cur, (size_t)L->size, &fl, &se, &cnt)) die("cannot unpack the menu container");
    int inst = 0;
    for (int k = 0; k < cnt; k++) if (!_stricmp(se[k].name, LUA)) inst = lua_has_mod(se[k].data, se[k].size);
    if (cur_out) *cur_out = cur; else free(cur);
    return inst;
}

static void install_menu(const char *game)
{
    menu_loc_t L; locate_menu(game, &L, 1);
    uint8_t *cur; int inst = menu_installed(&L, &cur);
    char bp[MAX_PATH]; backup_path(game, bp);
    if (inst) { printf("  menu: LAN entry already installed\n"); CloseHandle(L.h); return; }
    uint32_t vold[3], vnew[3]; buf_t nb = { 0 };
    build_menu(cur, (size_t)L.size, &nb, vold, vnew);
    for (int f = 0; f < 3; f++)
        if (L.asm_val[f] != vold[f]) die("the container manifest does not match the menu container (%u vs %u); nothing was changed", L.asm_val[f], vold[f]);
    if (nb.n > L.room) die("the patched menu (%u bytes) does not fit its slot (%u bytes); nothing was changed", (unsigned)nb.n, (unsigned)L.room);
    /* backup */
    uint8_t *slot = malloc((size_t)L.room);
    backup_hdr_t B; memset(&B, 0, sizeof B); memcpy(B.magic, "LANMOD1", 8);
    LARGE_INTEGER fs; GetFileSizeEx(L.h, &fs); B.archive_size = (uint64_t)fs.QuadPart;
    B.dir_pos = L.dir_pos; B.slot_pos = L.slot_pos; B.slot_len = L.room;
    for (int f = 0; f < 3; f++) { B.asm_pos[f] = L.asm_pos[f]; B.asm_val[f] = L.asm_val[f]; }
    if (!read_at(L.h, L.dir_pos, B.dir, 0x30) || !read_at(L.h, L.slot_pos, slot, (size_t)L.room)) die("cannot read the original bytes");
    FILE *bf = fopen(bp, "wb");
    if (!bf || fwrite(&B, sizeof B, 1, bf) != 1 || fwrite(slot, 1, (size_t)L.room, bf) != L.room || fclose(bf)) die("cannot write the backup %s", bp);
    /* write */
    uint8_t *ns = calloc(1, (size_t)L.room); memcpy(ns, nb.p, nb.n);
    uint8_t nd[0x30]; memcpy(nd, B.dir, 0x30); wr64(nd + 0x18, nb.n);
    int ok = write_at(L.h, L.slot_pos, ns, (size_t)L.room) && write_at(L.h, L.dir_pos, nd, 0x30);
    for (int f = 0; f < 3 && ok; f++) { uint8_t b4[4]; wr32(b4, vnew[f]); ok = write_at(L.h, L.asm_pos[f], b4, 4); }
    FlushFileBuffers(L.h); CloseHandle(L.h);
    if (!ok) die("writing interface.vpp_pc failed - run the installer again with 'uninstall' to restore it");
    /* verify by reading it back like the game does */
    menu_loc_t V; locate_menu(game, &V, 0);
    if (!menu_installed(&V, NULL) || V.size != nb.n || V.asm_val[0] != vnew[0] || V.asm_val[1] != vnew[1]) die("verification after writing failed - run 'uninstall'");
    CloseHandle(V.h);
    printf("  menu: LAN entry added (menu container %u -> %u bytes, slot %u). Backup: %s\n", (unsigned)L.size, (unsigned)nb.n, (unsigned)L.room, bp);
}

static void uninstall_menu(const char *game)
{
    char bp[MAX_PATH]; backup_path(game, bp);
    size_t n; uint8_t *b = load_file(bp, &n);
    if (!b) { printf("  menu: no backup found (menu not installed by this mod)\n"); return; }
    backup_hdr_t B; if (n < sizeof B) die("backup file is damaged");
    memcpy(&B, b, sizeof B);
    if (memcmp(B.magic, "LANMOD1", 8) || n != sizeof B + B.slot_len) die("backup file is damaged");
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\cache\\interface.vpp_pc", game);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) die("cannot open %s for writing (is the game running?)", path);
    LARGE_INTEGER fs; GetFileSizeEx(h, &fs);
    if ((uint64_t)fs.QuadPart != B.archive_size) die("interface.vpp_pc changed size since the mod was installed (game update?). Use your store's 'verify game files' instead.");
    int ok = write_at(h, B.slot_pos, b + sizeof B, (size_t)B.slot_len) && write_at(h, B.dir_pos, B.dir, 0x30);
    for (int f = 0; f < 3 && ok; f++) { uint8_t b4[4]; wr32(b4, B.asm_val[f]); ok = write_at(h, B.asm_pos[f], b4, 4); }
    FlushFileBuffers(h); CloseHandle(h);
    if (!ok) die("restoring interface.vpp_pc failed. Use your store's 'verify game files'.");
    menu_loc_t V; locate_menu(game, &V, 0);
    if (menu_installed(&V, NULL)) die("restore verification failed. Use your store's 'verify game files'.");
    CloseHandle(V.h);
    DeleteFileA(bp); free(b);
    printf("  menu: original restored\n");
}

/* ------------------------------------------------------------------ loader + LAN join patch */
#define PATCH_NAME     "lan_join.patch"
#define OLD_PATCH_NAME "lan_join_fix.patch"   /* name used by the first build; removed when found */
static int is_our_loader(const char *p)
{
    size_t n; uint8_t *d = load_file(p, &n); if (!d) return 0;
    int ours = 0; const char *tag = "SRTTR Mod Loader"; size_t m = strlen(tag);
    for (size_t i = 0; i + m <= n; i++) if (!memcmp(d + i, tag, m)) { ours = 1; break; }
    free(d); return ours;
}

static void exe_dir(char *out)
{
    GetModuleFileNameA(NULL, out, MAX_PATH);
    char *s = strrchr(out, '\\'); if (s) *s = 0;
}

/* Earlier builds used a folder named "modloader". Remove only our own files from it (the LAN patch
   under either name and the old logs); the folders are removed only if nothing else is left in them. */
static void remove_old_folder(const char *game)
{
    char dir[MAX_PATH], p[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\modloader", game);
    DWORD a = GetFileAttributesA(dir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;
    snprintf(p, sizeof p, "%s\\patches\\" PATCH_NAME, dir); DeleteFileA(p);
    snprintf(p, sizeof p, "%s\\patches\\" OLD_PATCH_NAME, dir); DeleteFileA(p);
    for (int i = 1; i <= 8; i++) {
        if (i == 1) snprintf(p, sizeof p, "%s\\modloader.log", dir); else snprintf(p, sizeof p, "%s\\modloader-%d.log", dir, i);
        DeleteFileA(p);
    }
    snprintf(p, sizeof p, "%s\\patches", dir); RemoveDirectoryA(p);
    snprintf(p, sizeof p, "%s\\plugins", dir); RemoveDirectoryA(p);
    if (RemoveDirectoryA(dir)) printf("  removed the old 'modloader' folder from an earlier version\n");
    else printf("  note: the old 'modloader' folder still contains other files; it was left in place\n");
}

static void install_loader(const char *game)
{
    char src[MAX_PATH], srcp[MAX_PATH], dll[MAX_PATH], dir[MAX_PATH], pdir[MAX_PATH], patch[MAX_PATH], me[MAX_PATH];
    exe_dir(me);
    snprintf(src, sizeof src, "%s\\files\\dinput8.dll", me);
    snprintf(srcp, sizeof srcp, "%s\\files\\" PATCH_NAME, me);
    if (!file_exists(src) || !file_exists(srcp)) die("the 'files' folder next to the installer is missing (unzip the whole mod)");
    snprintf(dll, sizeof dll, "%s\\dinput8.dll", game);
    if (file_exists(dll) && !is_our_loader(dll)) die("another dinput8.dll is already in the game folder (another mod or tool). Not overwriting it.");
    snprintf(dir, sizeof dir, "%s\\ploader", game); CreateDirectoryA(dir, NULL);
    snprintf(pdir, sizeof pdir, "%s\\ploader\\patches", game); CreateDirectoryA(pdir, NULL);
    snprintf(patch, sizeof patch, "%s\\" PATCH_NAME, pdir);
    if (!CopyFileA(src, dll, FALSE) || !CopyFileA(srcp, patch, FALSE)) die("copying the mod loader failed (is the game running?)");
    char old[MAX_PATH]; snprintf(old, sizeof old, "%s\\" OLD_PATCH_NAME, pdir);
    if (file_exists(old)) DeleteFileA(old);
    printf("  LAN joining: mod loader (dinput8.dll) + ploader\\patches\\" PATCH_NAME " installed\n");
    remove_old_folder(game);
}

static void uninstall_loader(const char *game)
{
    char dll[MAX_PATH], patch[MAX_PATH], pdir[MAX_PATH], dir[MAX_PATH], q[MAX_PATH];
    remove_old_folder(game);
    snprintf(dll, sizeof dll, "%s\\dinput8.dll", game);
    snprintf(pdir, sizeof pdir, "%s\\ploader\\patches", game);
    snprintf(patch, sizeof patch, "%s\\" PATCH_NAME, pdir);
    snprintf(dir, sizeof dir, "%s\\ploader", game);
    if (file_exists(patch)) DeleteFileA(patch);
    snprintf(patch, sizeof patch, "%s\\" OLD_PATCH_NAME, pdir);
    if (file_exists(patch)) DeleteFileA(patch);
    /* keep the loader if other mods use it */
    snprintf(q, sizeof q, "%s\\*", pdir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(q, &fd); int others = 0;
    if (h != INVALID_HANDLE_VALUE) { do { if (fd.cFileName[0] != '.') others++; } while (FindNextFileA(h, &fd)); FindClose(h); }
    char plug[MAX_PATH]; snprintf(plug, sizeof plug, "%s\\ploader\\plugins\\*", game);
    h = FindFirstFileA(plug, &fd);
    if (h != INVALID_HANDLE_VALUE) { do { if (fd.cFileName[0] != '.') others++; } while (FindNextFileA(h, &fd)); FindClose(h); }
    if (others) { printf("  LAN joining patch removed; mod loader kept (other mods use it)\n"); return; }
    if (file_exists(dll) && is_our_loader(dll)) DeleteFileA(dll);
    char logp[MAX_PATH];
    for (int i = 1; i <= 8; i++) {
        if (i == 1) snprintf(logp, sizeof logp, "%s\\ploader.log", dir); else snprintf(logp, sizeof logp, "%s\\ploader-%d.log", dir, i);
        DeleteFileA(logp);
    }
    snprintf(plug, sizeof plug, "%s\\ploader\\plugins", game); RemoveDirectoryA(plug);
    RemoveDirectoryA(pdir); RemoveDirectoryA(dir);
    printf("  LAN joining patch + mod loader removed\n");
}

/* ------------------------------------------------------------------ finding the game */
static int is_game_dir(const char *d)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\cache\\interface.vpp_pc", d); if (!file_exists(p)) return 0;
    snprintf(p, sizeof p, "%s\\SRTTR.exe", d); if (file_exists(p)) return 1;
    WIN32_FIND_DATAA fd; snprintf(p, sizeof p, "%s\\SRTTR*.exe", d);
    HANDLE h = FindFirstFileA(p, &fd); if (h != INVALID_HANDLE_VALUE) { FindClose(h); return 1; }
    return 0;
}

static char g_found[16][MAX_PATH]; static int g_nfound;
static void add_candidate(const char *d)
{
    char full[MAX_PATH]; if (!GetFullPathNameA(d, MAX_PATH, full, NULL)) return;
    size_t l = strlen(full); while (l > 3 && (full[l - 1] == '\\' || full[l - 1] == '/')) full[--l] = 0;
    if (!is_game_dir(full)) return;
    for (int i = 0; i < g_nfound; i++) if (!_stricmp(g_found[i], full)) return;
    if (g_nfound < 16) strcpy(g_found[g_nfound++], full);
}

static int reg_str(HKEY root, const char *key, const char *val, char *out, DWORD n, REGSAM extra)
{
    HKEY h; if (RegOpenKeyExA(root, key, 0, KEY_READ | extra, &h)) return 0;
    DWORD t, sz = n; int ok = !RegQueryValueExA(h, val, NULL, &t, (BYTE *)out, &sz) && (t == REG_SZ || t == REG_EXPAND_SZ);
    RegCloseKey(h); if (ok) out[n - 1] = 0; return ok;
}

static void find_steam(void)
{
    char steam[MAX_PATH] = "";
    if (!reg_str(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", steam, sizeof steam, 0) &&
        !reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath", steam, sizeof steam, KEY_WOW64_32KEY)) return;
    for (char *c = steam; *c; c++) if (*c == '/') *c = '\\';
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\steamapps\\common\\Saints Row The Third Remastered", steam); add_candidate(p);
    /* other Steam libraries: steamapps\libraryfolders.vdf, lines like  "path"  "D:\\SteamLibrary" */
    char vdf[MAX_PATH]; snprintf(vdf, sizeof vdf, "%s\\steamapps\\libraryfolders.vdf", steam);
    size_t n; uint8_t *d = load_file(vdf, &n); if (!d) return;
    char *s = malloc(n + 1); memcpy(s, d, n); s[n] = 0; free(d);
    for (char *q = strstr(s, "\"path\""); q; q = strstr(q + 6, "\"path\"")) {
        char *a = strchr(q + 6, '"'); if (!a) break; char *b = strchr(a + 1, '"'); if (!b) break;
        char lib[MAX_PATH]; size_t o = 0;
        for (char *c = a + 1; c < b && o < MAX_PATH - 1; c++) { if (c[0] == '\\' && c[1] == '\\') c++; lib[o++] = *c; }
        lib[o] = 0;
        snprintf(p, sizeof p, "%s\\steamapps\\common\\Saints Row The Third Remastered", lib); add_candidate(p);
    }
    free(s);
}

static void find_gog(void)
{
    HKEY h; if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\GOG.com\\Games", 0, KEY_READ | KEY_WOW64_32KEY, &h)) return;
    char sub[256];
    for (DWORD i = 0; RegEnumKeyA(h, i, sub, sizeof sub) == ERROR_SUCCESS; i++) {
        char key[512], name[512] = "", path[MAX_PATH] = "";
        snprintf(key, sizeof key, "SOFTWARE\\GOG.com\\Games\\%s", sub);
        reg_str(HKEY_LOCAL_MACHINE, key, "gameName", name, sizeof name, KEY_WOW64_32KEY);
        if (!strstr(name, "Saints Row") && !strstr(name, "SAINTS ROW")) continue;
        if (reg_str(HKEY_LOCAL_MACHINE, key, "path", path, sizeof path, KEY_WOW64_32KEY)) add_candidate(path);
    }
    RegCloseKey(h);
}

static void find_epic(void)
{
    char pat[MAX_PATH] = "C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests\\*.item";
    char pd[MAX_PATH]; DWORD l = GetEnvironmentVariableA("ProgramData", pd, sizeof pd);
    if (l && l < sizeof pd) snprintf(pat, sizeof pat, "%s\\Epic\\EpicGamesLauncher\\Data\\Manifests\\*.item", pd);
    char base[MAX_PATH]; strcpy(base, pat); *strrchr(base, '\\') = 0;
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    do {
        char f[MAX_PATH]; snprintf(f, sizeof f, "%s\\%s", base, fd.cFileName);
        size_t n; uint8_t *d = load_file(f, &n); if (!d) continue;
        char *s = malloc(n + 1); memcpy(s, d, n); s[n] = 0; free(d);
        char *q = strstr(s, "\"InstallLocation\"");
        if (q) {
            char *a = strchr(q + 17, '"'); char *b = a ? strchr(a + 1, '"') : NULL;
            if (a && b) {
                char loc[MAX_PATH]; size_t o = 0;
                for (char *c = a + 1; c < b && o < MAX_PATH - 1; c++) { if (c[0] == '\\' && c[1] == '\\') c++; loc[o++] = *c; }
                loc[o] = 0; add_candidate(loc);
            }
        }
        free(s);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void choose_game(const char *arg, char *game)
{
    if (arg) { add_candidate(arg); if (!g_nfound) die("'%s' is not a Saints Row: The Third Remastered folder (needs cache\\interface.vpp_pc and SRTTR.exe)", arg); strcpy(game, g_found[0]); return; }
    char me[MAX_PATH]; exe_dir(me); add_candidate(me);
    char up[MAX_PATH]; snprintf(up, sizeof up, "%s\\..", me); add_candidate(up);
    find_steam(); find_gog(); find_epic();
    if (g_nfound == 1) { strcpy(game, g_found[0]); return; }
    if (g_nfound > 1) {
        printf("Found more than one copy of the game:\n");
        for (int i = 0; i < g_nfound; i++) printf("  %d) %s\n", i + 1, g_found[i]);
        printf("Which one? (number): ");
        char line[32]; if (!fgets(line, sizeof line, stdin)) die("no choice");
        int k = atoi(line); if (k < 1 || k > g_nfound) die("invalid choice");
        strcpy(game, g_found[k - 1]); return;
    }
    printf("Could not find Saints Row: The Third Remastered automatically.\n"
           "Paste the game folder (the one containing SRTTR.exe) and press Enter:\n> ");
    char line[MAX_PATH]; if (!fgets(line, sizeof line, stdin)) die("no folder given");
    line[strcspn(line, "\r\n")] = 0;
    char *s = line; if (*s == '"') { s++; char *e = strrchr(s, '"'); if (e) *e = 0; }
    add_candidate(s);
    if (!g_nfound) die("'%s' is not the game folder", s);
    strcpy(game, g_found[0]);
}

static int game_running(void)
{
    HWND w = NULL; (void)w;
    /* SRTTR.exe keeps interface.vpp_pc open; opening it for writing fails then. Try that. */
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = "install", *arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "install") || !_stricmp(argv[i], "uninstall") || !_stricmp(argv[i], "status") || !_stricmp(argv[i], "build-menu")) cmd = argv[i];
        else if (!arg) arg = argv[i];
    }
    if (!_stricmp(cmd, "build-menu")) {
        g_quiet_pause = 1;
        if (argc < 4) die("usage: build-menu <game folder> <out file>");
        menu_loc_t L; locate_menu(argv[2], &L, 0);
        uint8_t *cur; if (menu_installed(&L, &cur)) die("the LAN entry is already installed in this copy");
        uint32_t vo[3], vn[3]; buf_t nb = { 0 }; build_menu(cur, (size_t)L.size, &nb, vo, vn);
        FILE *f = fopen(argv[3], "wb"); fwrite(nb.p, 1, nb.n, f); fclose(f);
        printf("menu container %u -> %u bytes (slot %u); manifest %u/%u/%u -> %u/%u/%u (file has %u/%u/%u)\n",
               (unsigned)L.size, (unsigned)nb.n, (unsigned)L.room, vo[0], vo[1], vo[2], vn[0], vn[1], vn[2], L.asm_val[0], L.asm_val[1], L.asm_val[2]);
        return 0;
    }
    printf("LAN Mod for Saints Row: The Third Remastered\n============================================\n");
    char game[MAX_PATH]; choose_game(arg, game);
    printf("Game folder: %s\n\n", game);
    (void)game_running;
    if (!_stricmp(cmd, "status")) {
        menu_loc_t L; locate_menu(game, &L, 0);
        char bp[MAX_PATH]; backup_path(game, bp); char dll[MAX_PATH]; snprintf(dll, sizeof dll, "%s\\dinput8.dll", game);
        char patch[MAX_PATH]; snprintf(patch, sizeof patch, "%s\\ploader\\patches\\" PATCH_NAME, game);
        printf("  menu LAN entry : %s\n", menu_installed(&L, NULL) ? "installed" : "not installed");
        printf("  menu backup    : %s\n", file_exists(bp) ? "present" : "none");
        printf("  mod loader     : %s\n", file_exists(dll) ? (is_our_loader(dll) ? "installed" : "a different dinput8.dll") : "not installed");
        printf("  LAN joining    : %s\n", file_exists(patch) ? "installed" : "not installed");
        CloseHandle(L.h);
    } else if (!_stricmp(cmd, "uninstall")) {
        printf("Uninstalling...\n"); uninstall_menu(game); uninstall_loader(game);
        printf("\nDone. The game is back to its original state.\n");
    } else {
        printf("Installing...\n"); install_menu(game); install_loader(game);
        printf("\nDone. Start the game: Co-op Campaign now has a LAN entry (host: New Game / Load Game,\n"
               "others on the same network: Find Game). Every player needs the mod.\n"
               "To remove it, run:  LANMod_Installer.exe uninstall\n");
    }
    if (!g_quiet_pause && !arg) { printf("\nPress Enter to close."); getchar(); }
    return 0;
}
