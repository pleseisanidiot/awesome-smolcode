#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef struct { char *data; size_t len, cap; } Buf;
void *xrealloc(void *p, size_t size);
void buf_add(Buf *b, const void *data, size_t len);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_free(Buf *b);
void json_quote(Buf *b, const void *data, size_t len);
void safe_print(const void *data, size_t len);
void redact(Buf *b);

typedef enum { J_OBJECT, J_ARRAY, J_STRING, J_PRIMITIVE } JsonType;
typedef struct {
    JsonType type;
    size_t start, end, next, count;
} JsonToken;
typedef struct {
    const char *text;
    size_t len, pos, count, cap;
    JsonToken *tokens;
} Json;
#define J_NONE ((size_t)-1)

int json_parse(Json *j, const char *text, size_t len);
void json_free(Json *j);
size_t json_get(const Json *j, size_t object, const char *key);
int json_is(const Json *j, size_t token, const char *literal);
int json_string(const Json *j, size_t token, Buf *out);
void json_raw(const Json *j, size_t token, Buf *out);

#endif
