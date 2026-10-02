/*
 * sq_trace.c - see sq_trace.h
 *
 * Build switches:
 *   -DPD_SQ_DUMP=0   stop dumping/logging scripts (keep it once you have them)
 *   -DSQ_DUMP_DIR=\"ux0:data/pacmandash/dump/\"
 */
#include "sq_trace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "utils/logger.h"
#include "utils/settings.h"

#ifndef PD_SQ_DUMP
#define PD_SQ_DUMP 1
#endif
#ifndef SQ_DUMP_DIR
#define SQ_DUMP_DIR "ux0:data/pacmandash/dump/"
#endif

typedef void *HSQUIRRELVM;
typedef int   SQInteger;
typedef int   SQRESULT;
typedef int   (*sq_readfn)(void *up, void *buf, SQInteger size);
typedef int   (*sq_lexfn)(void *up);

/* ------------------------------------------------------------------------ */
/* Patch table: plain-text edits applied to a script before it is compiled.  */
/* Fill it once you have read the dumps. 'enabled' points to a setting;      */
/* the edit is applied only while *enabled != 0.                             */
/* ------------------------------------------------------------------------ */
typedef struct {
    const char *name_contains;   /* part of the source name, e.g. "game" */
    const char *find;            /* exact text to look for (first match)  */
    const char *replace;         /* replacement text                      */
    const int  *enabled;         /* e.g. &setting_infiniteStamina         */
} sq_patch;

static const sq_patch k_patches[] = {
    /* Example (fill with real text from the dump):
     * { "player", "stamina -= 1;", "stamina -= 0;", &setting_infiniteStamina },
     */
    { NULL, NULL, NULL, NULL }
};

/* ------------------------------------------------------------------------ */
static so_hook g_h_compilebuffer, g_h_readclosure, g_h_compile;
static int g_n_cb, g_n_rc, g_n_cp;

#define SEEN_MAX 512
static uint32_t g_seen[SEEN_MAX];
static int      g_seen_n;

static uint32_t fnv1a(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* returns 1 if this (name,content) pair was not dumped before */
static int mark_new(const char *name, const void *data, size_t len) {
    uint32_t h = fnv1a(name, strlen(name)) ^ fnv1a(data, len);
    for (int i = 0; i < g_seen_n; i++)
        if (g_seen[i] == h) return 0;
    if (g_seen_n < SEEN_MAX) g_seen[g_seen_n++] = h;
    return 1;
}

static void safe_name(const char *in, char *out, size_t cap, const char *fallback, int n) {
    const char *base = in ? in : "";
    for (const char *p = base; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
    size_t j = 0;
    for (const char *p = base; *p && j + 1 < cap; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out[j++] = ok ? c : '_';
    }
    out[j] = 0;
    if (j == 0) snprintf(out, cap, "%s_%03d", fallback, n);
}

static void dump_file(const char *fname, const void *data, size_t len) {
    char path[256];
    snprintf(path, sizeof(path), SQ_DUMP_DIR "%s", fname);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) { l_error("[SQ] cannot write %s", path); return; }
    sceIoWrite(fd, data, len);
    sceIoClose(fd);
}

static void preview(const char *s, size_t len, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; i < len && j + 1 < cap && j < 60; i++) {
        unsigned char c = (unsigned char)s[i];
        out[j++] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    out[j] = 0;
}

/* ---- text patching ---- */
static const char *find_text(const char *hay, size_t hl, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || hl < nl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nl) == 0) return hay + i;
    return NULL;
}

/* Returns a malloc'd patched copy, or NULL if nothing applied. */
static char *apply_patches(const char *name, const char *src, int size, int *out_size) {
    char *cur = NULL;
    int   cur_size = size;
    const char *cur_src = src;

    for (const sq_patch *p = k_patches; p->find; p++) {
        if (p->enabled && !*p->enabled) continue;
        if (p->name_contains && !(name && strstr(name, p->name_contains))) continue;
        const char *hit = find_text(cur_src, (size_t)cur_size, p->find);
        if (!hit) continue;

        size_t flen = strlen(p->find), rlen = strlen(p->replace);
        size_t off = (size_t)(hit - cur_src);
        char *n = (char *)malloc((size_t)cur_size - flen + rlen + 1);
        if (!n) break;
        memcpy(n, cur_src, off);
        memcpy(n + off, p->replace, rlen);
        memcpy(n + off + rlen, cur_src + off + flen, (size_t)cur_size - off - flen);
        cur_size = (int)((size_t)cur_size - flen + rlen);
        n[cur_size] = 0;
        free(cur);
        cur = n;
        cur_src = cur;
        l_info("[SQ] patched '%s': \"%s\" -> \"%s\"", name ? name : "?", p->find, p->replace);
    }
    if (cur) *out_size = cur_size;
    return cur;
}

/* ------------------------------------------------------------------------ */
/* sq_compilebuffer(v, s, size, sourcename, raiseerror)                      */
/* ------------------------------------------------------------------------ */
static SQRESULT h_compilebuffer(HSQUIRRELVM v, const char *s, SQInteger size,
                                const char *sourcename, SQInteger raiseerror) {
    int n = ++g_n_cb;
    char fname[96], pv[72];

#if PD_SQ_DUMP
    if (s && size > 0) {
        safe_name(sourcename, fname, sizeof(fname), "buf", n);
        preview(s, (size_t)size, pv, sizeof(pv));
        l_info("[SQ] compilebuffer #%d name='%s' size=%d : %s", n,
               sourcename ? sourcename : "(null)", (int)size, pv);
        if (mark_new(fname, s, (size_t)size)) {
            char full[112];
            snprintf(full, sizeof(full), "%s.dump.nut", fname);
            dump_file(full, s, (size_t)size);
        }
    }
#endif

    int new_size = 0;
    char *patched = (s && size > 0) ? apply_patches(sourcename, s, size, &new_size) : NULL;
    SQRESULT r = patched
        ? SO_CONTINUE(SQRESULT, g_h_compilebuffer, v, patched, new_size, sourcename, raiseerror)
        : SO_CONTINUE(SQRESULT, g_h_compilebuffer, v, s, size, sourcename, raiseerror);
    free(patched);
    return r;
}

/* ------------------------------------------------------------------------ */
/* sq_readclosure(v, read, up): wrap the read callback and record the bytes  */
/* ------------------------------------------------------------------------ */
static sq_readfn g_real_read;
static uint8_t  *g_rec;
static size_t    g_rec_len, g_rec_cap;

static void rec_append(const void *p, size_t n) {
    if (g_rec_len + n > g_rec_cap) {
        size_t nc = g_rec_cap ? g_rec_cap * 2 : 65536;
        while (nc < g_rec_len + n) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(g_rec, nc);
        if (!nb) return;
        g_rec = nb;
        g_rec_cap = nc;
    }
    memcpy(g_rec + g_rec_len, p, n);
    g_rec_len += n;
}

static SQInteger rec_read(void *up, void *buf, SQInteger size) {
    SQInteger r = g_real_read(up, buf, size);
    if (r > 0) rec_append(buf, (size_t)r);
    return r;
}

static SQRESULT h_readclosure(HSQUIRRELVM v, sq_readfn read, void *up) {
    int n = ++g_n_rc;
#if PD_SQ_DUMP
    g_real_read = read;
    g_rec_len = 0;
    SQRESULT r = SO_CONTINUE(SQRESULT, g_h_readclosure, v, rec_read, up);
    char fname[64], pv[72];
    preview((const char *)g_rec, g_rec_len < 24 ? g_rec_len : 24, pv, sizeof(pv));
    l_info("[SQ] readclosure #%d bytes=%u ret=%d head=%s", n, (unsigned)g_rec_len, (int)r, pv);
    if (g_rec_len && mark_new("closure", g_rec, g_rec_len)) {
        snprintf(fname, sizeof(fname), "closure_%03d.bin", n);
        dump_file(fname, g_rec, g_rec_len);
    }
    return r;
#else
    (void)n;
    return SO_CONTINUE(SQRESULT, g_h_readclosure, v, read, up);
#endif
}

/* ------------------------------------------------------------------------ */
/* sq_compile(v, lexread, up, sourcename, raiseerror): record lexer chars     */
/* ------------------------------------------------------------------------ */
static sq_lexfn g_real_lex;

static SQInteger rec_lex(void *up) {
    SQInteger c = g_real_lex(up);
    if (c > 0) { uint8_t b = (uint8_t)c; rec_append(&b, 1); }
    return c;
}

static SQRESULT h_compile(HSQUIRRELVM v, sq_lexfn lex, void *up,
                          const char *sourcename, SQInteger raiseerror) {
    int n = ++g_n_cp;
#if PD_SQ_DUMP
    g_real_lex = lex;
    g_rec_len = 0;
    SQRESULT r = SO_CONTINUE(SQRESULT, g_h_compile, v, rec_lex, up, sourcename, raiseerror);
    char fname[96], full[112], pv[72];
    safe_name(sourcename, fname, sizeof(fname), "lex", n);
    preview((const char *)g_rec, g_rec_len, pv, sizeof(pv));
    l_info("[SQ] compile #%d name='%s' chars=%u ret=%d : %s", n,
           sourcename ? sourcename : "(null)", (unsigned)g_rec_len, (int)r, pv);
    if (g_rec_len && mark_new(fname, g_rec, g_rec_len)) {
        snprintf(full, sizeof(full), "%s.lex.nut", fname);
        dump_file(full, g_rec, g_rec_len);
    }
    return r;
#else
    (void)n;
    return SO_CONTINUE(SQRESULT, g_h_compile, v, lex, up, sourcename, raiseerror);
#endif
}

/* ------------------------------------------------------------------------ */
void sq_trace_init(so_module *mod) {
#if PD_SQ_DUMP
    sceIoMkdir(SQ_DUMP_DIR, 0777);
#endif
    uintptr_t a;
    if ((a = so_symbol(mod, "sq_compilebuffer"))) g_h_compilebuffer = hook_addr(a, (uintptr_t)&h_compilebuffer);
    else l_error("[SQ] sq_compilebuffer not found");
    if ((a = so_symbol(mod, "sq_readclosure")))   g_h_readclosure   = hook_addr(a, (uintptr_t)&h_readclosure);
    else l_error("[SQ] sq_readclosure not found");
    if ((a = so_symbol(mod, "sq_compile")))       g_h_compile       = hook_addr(a, (uintptr_t)&h_compile);
    else l_error("[SQ] sq_compile not found");
    l_info("[SQ] tracer installed (dump=%d, dir=%s)", PD_SQ_DUMP, SQ_DUMP_DIR);
}
