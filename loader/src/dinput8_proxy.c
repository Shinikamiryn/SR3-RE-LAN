/*
 * SRTTR Mod Loader - dinput8.dll proxy for Saints Row: The Third Remastered (x64)
 *
 * The game imports DINPUT8.dll; Windows loads this copy from the game folder first. We forward the
 * DirectInput exports to the real System32\dinput8.dll and, on a background thread:
 *   1. apply byte patches described in  <game>\modloader\patches\*.patch
 *   2. load plugin DLLs from            <game>\modloader\plugins\*.dll
 * Everything is logged to               <game>\modloader\modloader.log
 *
 * Patch file format (one patch per file, '#' starts a comment):
 *   name    = lan_join_host_addr
 *   sig     = f6 c3 40 48 8b 5c 24 30 74 16 83 7e 10 00 74 52 b8 02 00 00 00   (?? = wildcard)
 *   offset  = 14            # where the new bytes go, relative to the signature start
 *   bytes   = eb            # replacement bytes
 * A patch is applied only if the signature matches exactly once inside the EXE's code. If instead
 * the already-patched form matches once, it is reported as already applied. Nothing on disk changes.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>

#define LOADER_VERSION "1.0.0"
#define MAX_SIG 256

static HMODULE g_real;
static char g_root[MAX_PATH];      /* <game>\modloader */
const char *ml_root(void) { return g_root; }
static CRITICAL_SECTION g_logcs;
static FILE *g_log;


/* ------------------------------------------------------------------ logging */
static void vlog(const char *fmt, va_list ap)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logcs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_logcs);
}
static void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }
void ml_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap); }

/* modloader\config.ini: string value of key (trimmed), or "" */
/* modloader\config.ini: "key = value" lines, '#' comments. Returns the integer value or def. */
/* ------------------------------------------------------------------ real dinput8 forwarding */
static void load_real(void)
{
    if (g_real) return;
    char p[MAX_PATH];
    UINT n = GetSystemDirectoryA(p, MAX_PATH);
    if (!n || n > MAX_PATH - 16) return;
    strcat(p, "\\dinput8.dll");
    g_real = LoadLibraryA(p);
}

static FARPROC real_proc(const char *name)
{
    load_real();
    return g_real ? GetProcAddress(g_real, name) : NULL;
}

typedef HRESULT (WINAPI *pDirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
typedef HRESULT (WINAPI *pDllGetClassObject)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *pNoArgs)(void);

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, void *outer)
{
    pDirectInput8Create f = (pDirectInput8Create)real_proc("DirectInput8Create");
    HRESULT r = f ? f(inst, ver, riid, out, outer) : E_FAIL;
    return r;
}
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, LPVOID *o)
{
    pDllGetClassObject f = (pDllGetClassObject)real_proc("DllGetClassObject");
    return f ? f(c, r, o) : E_FAIL;
}
HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
HRESULT WINAPI DllRegisterServer(void)
{
    pNoArgs f = (pNoArgs)real_proc("DllRegisterServer"); return f ? f() : E_FAIL;
}
HRESULT WINAPI DllUnregisterServer(void)
{
    pNoArgs f = (pNoArgs)real_proc("DllUnregisterServer"); return f ? f() : E_FAIL;
}

/* ------------------------------------------------------------------ patch engine */
typedef struct {
    char name[64];
    uint8_t sig[MAX_SIG]; uint8_t mask[MAX_SIG]; int siglen;
    int offset;
    uint8_t bytes[MAX_SIG]; int nbytes;
} patch_t;

static void trim(char *s)
{
    char *c = strchr(s, '#'); if (c) *c = 0;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    char *b = s; while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static int parse_hex(const char *s, uint8_t *out, uint8_t *mask, int max)
{
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ',') s++;
        if (!*s) break;
        if (n >= max) return -1;
        if (s[0] == '?' ) { out[n] = 0; if (mask) mask[n] = 0; n++; s += (s[1] == '?') ? 2 : 1; continue; }
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) return -1;
        out[n] = (uint8_t)v; if (mask) mask[n] = 1; n++;
        s += 2;
    }
    return n;
}

static int load_patch(const char *path, patch_t *p)
{
    memset(p, 0, sizeof *p); p->offset = -1;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0; char *k = line, *v = eq + 1; trim(k); trim(v);
        if (!_stricmp(k, "name")) strncpy(p->name, v, sizeof p->name - 1);
        else if (!_stricmp(k, "sig")) p->siglen = parse_hex(v, p->sig, p->mask, MAX_SIG);
        else if (!_stricmp(k, "offset")) p->offset = atoi(v);
        else if (!_stricmp(k, "bytes")) p->nbytes = parse_hex(v, p->bytes, NULL, MAX_SIG);
    }
    fclose(f);
    if (!p->name[0]) { const char *b = strrchr(path, '\\'); strncpy(p->name, b ? b + 1 : path, sizeof p->name - 1); }
    if (p->siglen <= 0 || p->nbytes <= 0 || p->offset < 0 || p->offset + p->nbytes > p->siglen) {
        logf_("patch %s: invalid file (sig=%d bytes=%d offset=%d)", p->name, p->siglen, p->nbytes, p->offset);
        return 0;
    }
    return 1;
}

static int code_range(uint8_t **start, size_t *len)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!memcmp(s->Name, ".text", 6)) { *start = base + s->VirtualAddress; *len = s->Misc.VirtualSize; return 1; }
    }
    return 0;
}

/* count matches of sig (with mask) in [start,len); store first match */
static int find_sig(uint8_t *start, size_t len, const uint8_t *sig, const uint8_t *mask, int n, uint8_t **first)
{
    int hits = 0;
    *first = NULL;
    if ((size_t)n > len) return 0;
    for (size_t i = 0; i + n <= len; i++) {
        if (start[i] != sig[0] && mask[0]) continue;
        int j = 0;
        for (; j < n; j++) if (mask[j] && start[i + j] != sig[j]) break;
        if (j == n) { if (!hits) *first = start + i; hits++; if (hits > 1) break; }
    }
    return hits;
}

static void apply_patch(patch_t *p, uint8_t *text, size_t textlen)
{
    uint8_t *hit;
    int n = find_sig(text, textlen, p->sig, p->mask, p->siglen, &hit);
    if (n == 1) {
        uint8_t *at = hit + p->offset;
        DWORD old;
        if (!VirtualProtect(at, p->nbytes, PAGE_EXECUTE_READWRITE, &old)) {
            logf_("patch %s: VirtualProtect failed (%lu)", p->name, GetLastError()); return;
        }
        memcpy(at, p->bytes, p->nbytes);
        VirtualProtect(at, p->nbytes, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, p->nbytes);
        logf_("patch %s: APPLIED at rva 0x%llx (%d byte(s))", p->name,
              (unsigned long long)(at - (uint8_t *)GetModuleHandleA(NULL)), p->nbytes);
        return;
    }
    /* already patched? build patched signature */
    uint8_t ps[MAX_SIG]; memcpy(ps, p->sig, p->siglen); memcpy(ps + p->offset, p->bytes, p->nbytes);
    uint8_t pm[MAX_SIG]; memcpy(pm, p->mask, p->siglen); memset(pm + p->offset, 1, p->nbytes);
    int m = find_sig(text, textlen, ps, pm, p->siglen, &hit);
    if (n == 0 && m == 1) logf_("patch %s: already applied", p->name);
    else logf_("patch %s: NOT applied - signature matched %d time(s) (patched form %d) - different game version?", p->name, n, m);
}

/* ------------------------------------------------------------------ static table relocation
 * A *.reloc file moves a fixed-size table in the game's static data to a bigger buffer, for tables
 * sized for 2 players. Every instruction that addresses the table RIP-relatively is listed:
 *     name     = ...
 *     new_size = 0x17700          # bytes of the new buffer (zero-filled)
 *     old_size = 0x5dc0           # bytes copied from the old table
 *     ref      = <sig with ??> ; <offset of the disp32 in sig> ; <target - table start> [; <matches, default 1>
 *                [; <new target - new table start, default = old; for end-of-table bounds>
 *                [; base]]]   "base": the displacement is image-relative ([reg + imagebase_reg + rva]),
 *                              not RIP-relative; it is rewritten to the new buffer's offset from the image
 * All refs must match exactly as often as given and agree on the table address, otherwise nothing is changed.
 * The buffer is allocated within +-2 GB of the game image so a disp32 can reach it. */
#define MAX_REFS 32
typedef struct { uint8_t sig[MAX_SIG], mask[MAX_SIG]; int siglen, dispoff, count; long long delta, newdelta; int based; } reloc_ref_t;

static void *alloc_near_image(size_t size)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    uintptr_t start = ((uintptr_t)base + nt->OptionalHeader.SizeOfImage + 0xFFFF) & ~(uintptr_t)0xFFFF;
    for (uintptr_t a = start; a < (uintptr_t)base + 0x70000000u; a += 0x10000) {
        void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

static void apply_reloc(const char *path, uint8_t *text, size_t textlen)
{
    static reloc_ref_t refs[MAX_REFS]; int nrefs = 0;
    char name[64] = ""; unsigned long long new_size = 0, old_size = 0;
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *k = line, *v = eq + 1; trim(k); trim(v);
        if (!_stricmp(k, "name")) strncpy(name, v, sizeof name - 1);
        else if (!_stricmp(k, "new_size")) new_size = strtoull(v, NULL, 0);
        else if (!_stricmp(k, "old_size")) old_size = strtoull(v, NULL, 0);
        else if (!_stricmp(k, "ref") && nrefs < MAX_REFS) {
            char *s1 = strchr(v, ';'); char *s2 = s1 ? strchr(s1 + 1, ';') : NULL;
            if (!s2) continue;
            char *s3 = strchr(s2 + 1, ';'); char *s4 = s3 ? strchr(s3 + 1, ';') : NULL;
            char *s5 = s4 ? strchr(s4 + 1, ';') : NULL;
            *s1 = 0; *s2 = 0; if (s3) *s3 = 0; if (s4) *s4 = 0; if (s5) *s5 = 0;
            reloc_ref_t *r = &refs[nrefs];
            r->count = s3 ? atoi(s3 + 1) : 1;
            r->siglen = parse_hex(v, r->sig, r->mask, MAX_SIG);
            r->dispoff = (int)strtol(s1 + 1, NULL, 0); r->delta = strtoll(s2 + 1, NULL, 0);
            r->newdelta = s4 ? strtoll(s4 + 1, NULL, 0) : r->delta;
            r->based = 0;
            if (s5) { char *q = s5 + 1; while (*q == ' ' || *q == '	') q++; r->based = !_strnicmp(q, "base", 4); }
            if (r->siglen > 0 && r->dispoff >= 0 && r->dispoff + 4 <= r->siglen && r->count >= 1 && r->count <= 8) nrefs++;
        }
    }
    fclose(f);
    if (!name[0]) { const char *b = strrchr(path, '\\'); strncpy(name, b ? b + 1 : path, sizeof name - 1); }
    if (!nrefs || !new_size || old_size > new_size) { logf_("reloc %s: invalid file", name); return; }

    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    uint8_t *img_end = base + nt->OptionalHeader.SizeOfImage;
    uint8_t *disp_at[MAX_REFS * 8]; long long disp_delta[MAX_REFS * 8], disp_new[MAX_REFS * 8]; int disp_based[MAX_REFS * 8]; int nd_ = 0; uint8_t *old_table = NULL;
    for (int i = 0; i < nrefs; i++) {
        int found = 0;
        for (size_t pos = 0; pos + refs[i].siglen <= textlen; ) {
            uint8_t *hit; int n = find_sig(text + pos, textlen - pos, refs[i].sig, refs[i].mask, refs[i].siglen, &hit);
            if (!n) break;
            if (++found <= refs[i].count) { disp_delta[nd_] = refs[i].delta; disp_new[nd_] = refs[i].newdelta; disp_based[nd_] = refs[i].based; disp_at[nd_++] = hit + refs[i].dispoff; }
            pos = (size_t)(hit - text) + 1;
        }
        if (found != refs[i].count) { logf_("reloc %s: NOT applied - ref %d matched %d time(s), expected %d - different game version?", name, i + 1, found, refs[i].count); return; }
    }
    for (int i = 0; i < nd_; i++) {
        uint8_t *target = disp_based[i] ? base + *(int32_t *)disp_at[i] : disp_at[i] + 4 + *(int32_t *)disp_at[i];
        if (target < base || target >= img_end) { logf_("reloc %s: already applied (a reference points outside the image)", name); return; }
        uint8_t *t = target - disp_delta[i];
        if (!old_table) old_table = t;
        else if (t != old_table) { logf_("reloc %s: NOT applied - reference %d points at rva 0x%llx, expected 0x%llx", name, i + 1,
                                         (unsigned long long)(t - base), (unsigned long long)(old_table - base)); return; }
    }
    uint8_t *buf = alloc_near_image((size_t)new_size);
    if (!buf) { logf_("reloc %s: NOT applied - no memory within reach of the game image", name); return; }
    memcpy(buf, old_table, (size_t)old_size);
    for (int i = 0; i < nd_; i++) {
        long long nd = (long long)(buf + disp_new[i]) - (long long)(disp_based[i] ? base : disp_at[i] + 4);
        if (nd != (int32_t)nd) { logf_("reloc %s: buffer out of disp32 range (reference %d) - nothing changed", name, i + 1); VirtualFree(buf, 0, MEM_RELEASE); return; }
    }
    for (int i = 0; i < nd_; i++) {
        int32_t nd = (int32_t)((buf + disp_new[i]) - (disp_based[i] ? base : disp_at[i] + 4));
        DWORD old;
        VirtualProtect(disp_at[i], 4, PAGE_EXECUTE_READWRITE, &old);
        memcpy(disp_at[i], &nd, 4);
        VirtualProtect(disp_at[i], 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), disp_at[i], 4);
    }
    logf_("reloc %s: APPLIED - table at rva 0x%llx (0x%llx bytes) moved to %p (0x%llx bytes), %d reference(s)", name,
          (unsigned long long)(old_table - base), old_size, (void *)buf, new_size, nd_);
}

static void run_relocs(uint8_t *text, size_t textlen)
{
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\patches\\*.reloc", g_root);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\patches\\%s", g_root, fd.cFileName);
        apply_reloc(path, text, textlen);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* ------------------------------------------------------------------ code caves
 * A *.cave file replaces >= 5 bytes at a signature with a jump to new code in a buffer next to the
 * image (for fixes that need more room than the original instructions):
 *     name   = ...
 *     sig    = <bytes with ??>
 *     offset = <where the replaced bytes start, in sig>
 *     length = <number of replaced bytes, >= 5; the rest after the jmp become nop>
 *     code   = <hex bytes; "rel:N" = 4-byte rel32 to (signature start + N), for jumps back into the game;
 *               "data" = 8-byte absolute address of the cave's zero-filled data area>
 *     data   = <optional size of that data area in bytes>
 * The cave code must end with its own jump back (e9 rel:<offset+length>). */
static void apply_cave(const char *path, uint8_t *text, size_t textlen)
{
    char name[64] = ""; static uint8_t sig[MAX_SIG], mask[MAX_SIG]; int siglen = 0, off = -1, len = 0; size_t datasz = 0;
    static char codetxt[4096] = "";
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *k = line, *v = eq + 1; trim(k); trim(v);
        if (!_stricmp(k, "name")) strncpy(name, v, sizeof name - 1);
        else if (!_stricmp(k, "sig")) siglen = parse_hex(v, sig, mask, MAX_SIG);
        else if (!_stricmp(k, "offset")) off = atoi(v);
        else if (!_stricmp(k, "length")) len = atoi(v);
        else if (!_stricmp(k, "code")) strncpy(codetxt, v, sizeof codetxt - 1);
        else if (!_stricmp(k, "data")) datasz = (size_t)strtoull(v, NULL, 0);
    }
    fclose(f);
    if (!name[0]) { const char *b = strrchr(path, '\\'); strncpy(name, b ? b + 1 : path, sizeof name - 1); }
    if (siglen <= 0 || off < 0 || len < 5 || off + len > siglen || !codetxt[0]) { logf_("cave %s: invalid file", name); return; }
    uint8_t *hit; int n = find_sig(text, textlen, sig, mask, siglen, &hit);
    if (n != 1) {
        logf_("cave %s: %s (signature matched %d time(s))", name, (n == 0 && text) ? "NOT applied or already applied" : "NOT applied", n);
        return;
    }
    uint8_t *cave = alloc_near_image(4096);
    if (!cave) { logf_("cave %s: NOT applied - no memory within reach", name); return; }
    uint8_t *data = NULL;
    if (datasz) {
        data = VirtualAlloc(NULL, datasz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!data) { logf_("cave %s: NOT applied - could not allocate 0x%llx data bytes", name, (unsigned long long)datasz); return; }
    }
    int cn = 0; char *tok = strtok(codetxt, " 	,");
    while (tok && cn < 4000) {
        if (!_strnicmp(tok, "rel:", 4)) {
            uint8_t *target = hit + strtol(tok + 4, NULL, 0);
            int32_t rel = (int32_t)(target - (cave + cn + 4));
            memcpy(cave + cn, &rel, 4); cn += 4;
        } else if (!_stricmp(tok, "data")) {
            if (!data) { logf_("cave %s: 'data' used without a data size", name); return; }
            uint64_t a = (uint64_t)(uintptr_t)data; memcpy(cave + cn, &a, 8); cn += 8;
        } else { unsigned b; if (sscanf(tok, "%2x", &b) != 1) { logf_("cave %s: bad code token %s", name, tok); return; } cave[cn++] = (uint8_t)b; }
        tok = strtok(NULL, " 	,");
    }
    DWORD old; VirtualProtect(cave, 4096, PAGE_EXECUTE_READ, &old);
    uint8_t *at = hit + off;
    VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old);
    uint8_t jmp[MAX_SIG]; memset(jmp, 0x90, len); jmp[0] = 0xE9;
    int32_t rel = (int32_t)(cave - (at + 5)); memcpy(jmp + 1, &rel, 4);
    memcpy(at, jmp, len);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    logf_("cave %s: APPLIED at rva 0x%llx (%d bytes replaced, %d bytes of new code at %p)", name,
          (unsigned long long)(at - (uint8_t *)GetModuleHandleA(NULL)), len, cn, (void *)cave);
}

static void run_caves(uint8_t *text, size_t textlen)
{
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\patches\\*.cave", g_root);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\patches\\%s", g_root, fd.cFileName);
        apply_cave(path, text, textlen);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void run_patches(void)
{
    uint8_t *text; size_t textlen;
    if (!code_range(&text, &textlen)) { logf_("no .text section found"); return; }
    run_relocs(text, textlen);
    run_caves(text, textlen);
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\patches\\*.patch", g_root);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { logf_("no patch files in %s\\patches", g_root); return; }
    do {
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\patches\\%s", g_root, fd.cFileName);
        patch_t p;
        if (load_patch(path, &p)) apply_patch(&p, text, textlen);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void load_plugins(void)
{
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\plugins\\*.dll", g_root);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { logf_("no plugins in %s\\plugins", g_root); return; }
    do {
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\plugins\\%s", g_root, fd.cFileName);
        HMODULE m = LoadLibraryA(path);
        logf_("plugin %s: %s", fd.cFileName, m ? "loaded" : "FAILED to load");
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    load_plugins();   /* LoadLibrary must not run inside DllMain */
    logf_("startup complete");
    return 0;
}

static void init(void)
{
    InitializeCriticalSection(&g_logcs);
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\'); if (slash) *slash = 0;
    snprintf(g_root, sizeof g_root, "%s\\modloader", exe);
    CreateDirectoryA(g_root, NULL);
    /* one log per running instance: modloader.log, modloader-2.log, ... (a second copy of the game
       must not truncate the first one's log) */
    for (int i = 1; i <= 8 && !g_log; i++) {
        char lp[MAX_PATH];
        if (i == 1) snprintf(lp, sizeof lp, "%s\\modloader.log", g_root);
        else snprintf(lp, sizeof lp, "%s\\modloader-%d.log", g_root, i);
        HANDLE fh = CreateFileA(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) continue;          /* in use by another instance */
        int fd = _open_osfhandle((intptr_t)fh, 0);
        g_log = fd >= 0 ? _fdopen(fd, "w") : NULL;
        if (!g_log) CloseHandle(fh);
    }
    char full[MAX_PATH]; GetModuleFileNameA(NULL, full, MAX_PATH);
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    logf_("SRTTR Mod Loader %s - %s", LOADER_VERSION, full);
    logf_("image base %p, PE timestamp 0x%08lx, image size 0x%lx, pid %lu", base,
          (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage,
          (unsigned long)GetCurrentProcessId());
    /* Patches are applied synchronously, before the game's own startup code runs (some patches,
       like the single-instance check, target code that executes very early). This only scans
       memory and calls VirtualProtect, which is safe under the loader lock. */
    run_patches();
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        init();
    }
    return TRUE;
}
