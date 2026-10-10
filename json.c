#define _POSIX_C_SOURCE 200809L
#include "json.h"
#include "config.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xrealloc(void *p, size_t size)
{
    void *q = realloc(p, size ? size : 1);
    if (!q) { fputs("smolcode: out of memory\n", stderr); exit(EXIT_FAILURE); }
    return q;
}

void buf_add(Buf *b, const void *data, size_t len)
{
    if (len > SIZE_MAX - b->len - 1) {
        fputs("smolcode: buffer too large\n", stderr); exit(EXIT_FAILURE);
    }
    size_t needed = b->len + len + 1;
    if (needed > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < needed) cap = cap > SIZE_MAX / 2 ? needed : cap * 2;
        b->data = xrealloc(b->data, cap);
        b->cap = cap;
    }
    if (len) memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) { fputs("smolcode: formatting failed\n", stderr); exit(EXIT_FAILURE); }
    char *s = xrealloc(NULL, (size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_add(b, s, (size_t)n);
    free(s);
}

void buf_free(Buf *b) { free(b->data); memset(b, 0, sizeof(*b)); }

/* A valid UTF-8 sequence length, or zero. Reject overlongs and surrogates. */
static size_t utf8(const unsigned char *s, size_t n)
{
    if (!n) return 0;
    if (s[0] < 0x80) return 1;
    size_t k = s[0] >= 0xc2 && s[0] <= 0xdf ? 2 :
               s[0] >= 0xe0 && s[0] <= 0xef ? 3 :
               s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : 0;
    if (!k || n < k) return 0;
    for (size_t i = 1; i < k; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    if ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] >= 0xa0) ||
        (s[0] == 0xf0 && s[1] < 0x90) || (s[0] == 0xf4 && s[1] >= 0x90)) return 0;
    return k;
}

void json_quote(Buf *b, const void *data, size_t len)
{
    const unsigned char *s = data;
    buf_puts(b, "\"");
    for (size_t i = 0; i < len;) {
        unsigned char c = s[i];
        size_t k = utf8(s + i, len - i);
        if (c == '"' || c == '\\') {
            char pair[2] = {'\\', (char)c}; buf_add(b, pair, 2);
        } else if (c < 0x20 || !k) {
            buf_printf(b, "\\u%04x", (unsigned)c);
        } else { buf_add(b, s + i, k); }
        i += k ? k : 1;
    }
    buf_puts(b, "\"");
}

/* Never expose the configured credential in diagnostics or tool results. */
void redact(Buf *b)
{
    const char *key = getenv(API_KEY_ENV);
    if (!key || !*key || !b->len) return;
    size_t k = strlen(key);
    for (size_t i = 0; k <= b->len && i <= b->len - k; i++)
        if (!memcmp(b->data + i, key, k)) { memset(b->data + i, '*', k); i += k - 1; }
}

void safe_print(const void *data, size_t len)
{
    Buf b = {0}; buf_add(&b, data, len); redact(&b);
    fwrite(b.data, 1, b.len, stdout);
    buf_free(&b);
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unicode(const char *s, size_t len, size_t *pos, unsigned *u)
{
    if (*pos > len || len - *pos < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex(s[(*pos)++]); if (h < 0) return 0;
        v = v * 16 + (unsigned)h;
    }
    if (v >= 0xd800 && v <= 0xdbff) {
        if (len - *pos < 6 || s[*pos] != '\\' || s[*pos + 1] != 'u') return 0;
        *pos += 2;
        unsigned lo = 0;
        for (int i = 0; i < 4; i++) {
            int h = hex(s[(*pos)++]); if (h < 0) return 0;
            lo = lo * 16 + (unsigned)h;
        }
        if (lo < 0xdc00 || lo > 0xdfff) return 0;
        v = 0x10000 + ((v - 0xd800) << 10) + lo - 0xdc00;
    } else if (v >= 0xdc00 && v <= 0xdfff) return 0;
    *u = v;
    return 1;
}

static void codepoint(Buf *b, unsigned u)
{
    unsigned char s[4]; size_t n;
    if (u < 0x80) { s[0] = (unsigned char)u; n = 1; }
    else if (u < 0x800) { s[0] = 0xc0 | (u >> 6); s[1] = 0x80 | (u & 63); n = 2; }
    else if (u < 0x10000) {
        s[0] = 0xe0 | (u >> 12); s[1] = 0x80 | ((u >> 6) & 63); s[2] = 0x80 | (u & 63); n = 3;
    } else {
        s[0] = 0xf0 | (u >> 18); s[1] = 0x80 | ((u >> 12) & 63);
        s[2] = 0x80 | ((u >> 6) & 63); s[3] = 0x80 | (u & 63); n = 4;
    }
    buf_add(b, s, n);
}

/* Validate and optionally decode a JSON string, including embedded NULs. */
static int string(const char *s, size_t len, size_t *pos, Buf *out)
{
    if (*pos >= len || s[(*pos)++] != '"') return 0;
    while (*pos < len) {
        unsigned char c = (unsigned char)s[(*pos)++];
        if (c == '"') return 1;
        if (c < 0x20) return 0;
        if (c == '\\') {
            if (*pos == len) return 0;
            c = (unsigned char)s[(*pos)++];
            if (c == 'u') {
                unsigned u; if (!unicode(s, len, pos, &u)) return 0;
                if (out) codepoint(out, u);
                continue;
            }
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break;
            case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
            default: return 0;
            }
            if (out) buf_add(out, &c, 1);
        } else {
            size_t k = utf8((const unsigned char *)s + *pos - 1, len - *pos + 1);
            if (!k) return 0;
            if (out) buf_add(out, s + *pos - 1, k);
            *pos += k - 1;
        }
    }
    return 0;
}

static void space(Json *j)
{
    while (j->pos < j->len && strchr(" \r\n\t", j->text[j->pos]) && j->text[j->pos]) j->pos++;
}

static int number(const char *s, size_t n)
{
    size_t p = 0;
    if (p < n && s[p] == '-') p++;
    if (p == n) return 0;
    if (s[p] == '0') p++;
    else { if (s[p] < '1' || s[p] > '9') return 0;
        while (p < n && s[p] >= '0' && s[p] <= '9') p++; }
    if (p < n && s[p] == '.') {
        size_t start = ++p;
        while (p < n && s[p] >= '0' && s[p] <= '9') p++;
        if (p == start) return 0;
    }
    if (p < n && (s[p] == 'e' || s[p] == 'E')) {
        p++; if (p < n && (s[p] == '+' || s[p] == '-')) p++;
        size_t start = p;
        while (p < n && s[p] >= '0' && s[p] <= '9') p++;
        if (p == start) return 0;
    }
    return p == n;
}

static int value(Json *j, unsigned depth)
{
    space(j);
    if (depth > 128 || j->pos == j->len) return 0;
    if (j->count == j->cap) {
        if (j->cap > SIZE_MAX / 2 / sizeof(*j->tokens)) return 0;
        j->cap = j->cap ? j->cap * 2 : 64;
        j->tokens = xrealloc(j->tokens, j->cap * sizeof(*j->tokens));
    }
    size_t t = j->count++;
    j->tokens[t] = (JsonToken){.start = j->pos};
    char c = j->text[j->pos];
    if (c == '"') {
        j->tokens[t].type = J_STRING;
        if (!string(j->text, j->len, &j->pos, NULL)) return 0;
    } else if (c == '{' || c == '[') {
        j->tokens[t].type = c == '{' ? J_OBJECT : J_ARRAY;
        char close = c == '{' ? '}' : ']';
        j->pos++; space(j);
        if (j->pos < j->len && j->text[j->pos] == close) j->pos++;
        else for (;;) {
            if (c == '{') {
                space(j);
                if (j->pos == j->len || j->text[j->pos] != '"' || !value(j, depth + 1)) return 0;
                space(j);
                if (j->pos == j->len || j->text[j->pos++] != ':') return 0;
            }
            if (!value(j, depth + 1)) return 0;
            j->tokens[t].count++;
            space(j);
            if (j->pos == j->len) return 0;
            if (j->text[j->pos] == close) { j->pos++; break; }
            if (j->text[j->pos++] != ',') return 0;
        }
    } else {
        j->tokens[t].type = J_PRIMITIVE;
        size_t start = j->pos;
        while (j->pos < j->len && !strchr(" \t\r\n,]}", j->text[j->pos])) j->pos++;
        size_t n = j->pos - start;
        const char *s = j->text + start;
        if (!((n == 4 && !memcmp(s, "true", 4)) || (n == 5 && !memcmp(s, "false", 5)) ||
              (n == 4 && !memcmp(s, "null", 4)) || number(s, n))) return 0;
    }
    j->tokens[t].end = j->pos;
    j->tokens[t].next = j->count;
    return 1;
}

int json_parse(Json *j, const char *text, size_t len)
{
    *j = (Json){.text = text, .len = len};
    int ok = value(j, 0); space(j);
    if (!ok || j->pos != len) { json_free(j); return 0; }
    return 1;
}

void json_free(Json *j) { free(j->tokens); memset(j, 0, sizeof(*j)); }

int json_string(const Json *j, size_t token, Buf *out)
{
    if (token >= j->count || j->tokens[token].type != J_STRING) return 0;
    size_t p = j->tokens[token].start;
    buf_add(out, "", 0);
    return string(j->text, j->tokens[token].end, &p, out);
}

int json_is(const Json *j, size_t token, const char *literal)
{
    if (token >= j->count) return 0;
    if (j->tokens[token].type == J_STRING) {
        Buf s = {0}; json_string(j, token, &s);
        int same = s.len == strlen(literal) && !memcmp(s.data, literal, s.len);
        buf_free(&s); return same;
    }
    JsonToken t = j->tokens[token];
    return t.end - t.start == strlen(literal) && !memcmp(j->text + t.start, literal, t.end - t.start);
}

size_t json_get(const Json *j, size_t object, const char *key)
{
    if (object >= j->count || j->tokens[object].type != J_OBJECT) return J_NONE;
    size_t found = J_NONE;
    for (size_t i = object + 1; i < j->tokens[object].next;) {
        size_t v = i + 1;
        if (json_is(j, i, key)) {
            if (found != J_NONE) return J_NONE; /* Ambiguous duplicate keys. */
            found = v;
        }
        i = j->tokens[v].next;
    }
    return found;
}

void json_raw(const Json *j, size_t token, Buf *out)
{
    if (token < j->count) {
        JsonToken t = j->tokens[token]; buf_add(out, j->text + t.start, t.end - t.start);
    }
}
