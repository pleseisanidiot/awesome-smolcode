#include "smolcode.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char prompt[] =
    "You are Smolcode, a minimal AI coding assistant running in a terminal. "
    "You have exactly three tools: read, write, and sh. Inspect code before modifying it. "
    "Make focused changes, run tests when appropriate, and report results accurately. "
    "Be concise. Never claim a command succeeded unless its result confirms success.";

static void message(Agent *a, const char *role, const char *content, size_t len)
{
    if (a->messages.len) buf_puts(&a->messages, ",");
    buf_printf(&a->messages, "{\"role\":\"%s\",\"content\":", role);
    json_quote(&a->messages, content, len); buf_puts(&a->messages, "}");
}

int agent_init(Agent *a)
{
    *a = (Agent){0};
    a->model = getenv("SMOLCODE_MODEL"); a->api_url = getenv("SMOLCODE_API_URL");
    if (!a->model) a->model = MODEL;
    if (!a->api_url) a->api_url = API_URL;
    if (!*a->model) {
        fputs("smolcode: SMOLCODE_MODEL (or MODEL in config.h) must be nonempty\n", stderr); return 0;
    }
    if (!*a->api_url || (strncmp(a->api_url, "http://", 7) && strncmp(a->api_url, "https://", 8))) {
        fputs("smolcode: SMOLCODE_API_URL (or API_URL in config.h) must be an HTTP(S) URL\n", stderr); return 0;
    }
    message(a, "system", prompt, strlen(prompt));
    Buf instructions = {0}; int found = text_load("AGENTS.md", &instructions);
    if (found > 0 && instructions.len) message(a, "system", instructions.data, instructions.len);
    buf_free(&instructions); return found >= 0;
}

static void tool_result(Agent *a, const Buf *id, const Buf *result)
{
    buf_puts(&a->messages, ",{\"role\":\"tool\",\"tool_call_id\":");
    json_quote(&a->messages, id->data, id->len); buf_puts(&a->messages, ",\"content\":");
    json_quote(&a->messages, result->data, result->len); buf_puts(&a->messages, "}");
}

static int nonempty_string(const Json *j, size_t token)
{
    Buf b = {0}; int ok = json_string(j, token, &b) && b.len && !memchr(b.data, 0, b.len);
    buf_free(&b); return ok;
}

/* Validate the whole batch before any side effects or history changes. */
static int valid_calls(const Json *j, size_t calls)
{
    if (calls == J_NONE || (j->tokens[calls].type == J_PRIMITIVE && json_is(j, calls, "null"))) return 1;
    if (j->tokens[calls].type != J_ARRAY) return 0;
    for (size_t c = calls + 1; c < j->tokens[calls].next; c = j->tokens[c].next) {
        size_t f = json_get(j, c, "function"), id = json_get(j, c, "id");
        size_t args = json_get(j, f, "arguments");
        if (!json_is(j, json_get(j, c, "type"), "function") || !nonempty_string(j, id) ||
            !nonempty_string(j, json_get(j, f, "name")) || args == J_NONE ||
            j->tokens[args].type != J_STRING) return 0;
        Buf decoded = {0}; json_string(j, id, &decoded);
        for (size_t prev = calls + 1; prev < c; prev = j->tokens[prev].next) {
            if (json_is(j, json_get(j, prev, "id"), decoded.data)) { buf_free(&decoded); return 0; }
        }
        buf_free(&decoded);
    }
    return 1;
}

int agent_recover(Agent *a)
{
    Buf wrapped = {0}; Json j = {0};
    buf_puts(&wrapped, "["); buf_add(&wrapped, a->messages.data, a->messages.len); buf_puts(&wrapped, "]");
    if (!json_parse(&j, wrapped.data, wrapped.len)) { buf_free(&wrapped); return 0; }
    size_t pending = 0, next = J_NONE; int ok = 1;
    for (size_t m = 1; ok && m < j.tokens[0].next; m = j.tokens[m].next) {
        size_t role = json_get(&j, m, "role"), content = json_get(&j, m, "content");
        if (content == J_NONE || j.tokens[content].type != J_STRING) { ok = 0; break; }
        if (json_is(&j, role, "tool")) {
            Buf id = {0};
            if (pending) json_string(&j, json_get(&j, next, "id"), &id);
            ok = pending && json_is(&j, json_get(&j, m, "tool_call_id"), id.data);
            buf_free(&id);
            if (ok) { pending--; next = j.tokens[next].next; }
        } else {
            if (pending) { ok = 0; break; }
            size_t calls = json_get(&j, m, "tool_calls");
            if (json_is(&j, role, "assistant")) {
                ok = valid_calls(&j, calls);
                if (ok && calls != J_NONE && j.tokens[calls].type == J_ARRAY) {
                    pending = j.tokens[calls].count; next = calls + 1;
                }
            } else ok = json_is(&j, role, "system") || json_is(&j, role, "user");
        }
    }
    if (ok && pending) {
        Buf result = {0};
        buf_puts(&result, "Error: execution status unknown: conversation ended before a result was saved. Inspect the current state before retrying.");
        for (; pending; pending--) {
            Buf id = {0}; json_string(&j, json_get(&j, next, "id"), &id);
            tool_result(a, &id, &result); buf_free(&id); next = j.tokens[next].next;
        }
        const char *notice = "Recovered an interrupted conversation. Unfinished tool calls were not replayed; their execution status is unknown.";
        message(a, "assistant", notice, strlen(notice)); puts(notice); buf_free(&result);
    }
    json_free(&j); buf_free(&wrapped); return ok;
}

static void assistant(Agent *a, const Json *j, size_t content, size_t calls)
{
    buf_puts(&a->messages, ",{\"role\":\"assistant\",\"content\":");
    if (content == J_NONE) buf_puts(&a->messages, "null");
    else json_raw(j, content, &a->messages);
    if (calls != J_NONE && j->tokens[calls].type == J_ARRAY && j->tokens[calls].count) {
        buf_puts(&a->messages, ",\"tool_calls\":"); json_raw(j, calls, &a->messages);
    }
    buf_puts(&a->messages, "}");
}

/* Hide leading reasoning only; keep literal tags in code and the original history. */
static void hide_reasoning(Buf *text)
{
    size_t pos = 0, cut = 0;
    for (;;) {
        while (pos < text->len && isspace((unsigned char)text->data[pos])) pos++;
        if (text->len - pos < 7 || memcmp(text->data + pos, "<think>", 7)) break;
        pos += 7;
        while (text->len - pos >= 8 && memcmp(text->data + pos, "</think>", 8)) pos++;
        if (text->len - pos < 8) { text->len = 0; text->data[0] = 0; return; }
        cut = pos += 8;
    }
    if (!cut) return;
    while (cut < text->len && (text->data[cut] == '\r' || text->data[cut] == '\n')) cut++;
    memmove(text->data, text->data + cut, text->len - cut);
    text->len -= cut; text->data[text->len] = 0;
}

static void print_response(const Buf *text)
{
    Buf out = {0}; const char *term = getenv("TERM");
    int ansi = isatty(STDOUT_FILENO) && (!term || strcmp(term, "dumb"));
    size_t code = 0;
    for (size_t i = 0; i < text->len;) {
        if (text->data[i] == '`') {
            size_t end = i + 1;
            while (end < text->len && text->data[end] == '`') end++;
            if (!code) code = end - i; else if (code == end - i) code = 0;
            buf_add(&out, text->data + i, end - i); i = end; continue;
        }
        if (!code && text->len - i >= 2 && !memcmp(text->data + i, "**", 2)) {
            size_t end = i + 2;
            while (text->len - end >= 2 && text->data[end] != '\n' &&
                   text->data[end] != '`' && memcmp(text->data + end, "**", 2)) end++;
            if (text->len - end >= 2 && end > i + 2 && !memcmp(text->data + end, "**", 2)) {
                if (ansi) buf_puts(&out, "\033[1m");
                buf_add(&out, text->data + i + 2, end - i - 2);
                if (ansi) buf_puts(&out, "\033[22m");
                i = end + 2; continue;
            }
        }
        buf_add(&out, text->data + i++, 1);
    }
    safe_print(out.data, out.len);
    if (out.len && out.data[out.len - 1] != '\n') putchar('\n');
    buf_free(&out);
}

int agent_turn(Agent *a, const char *input, size_t len)
{
    message(a, "user", input, len);
    if (!session_save(a)) return 0;
    size_t executed = 0;
    for (;;) {
        Buf response = {0}, text = {0}; Json j = {0};
        if (!api_request(a, &response)) { buf_free(&response); return 0; }
        int parsed = json_parse(&j, response.data, response.len);
        size_t choices = parsed ? json_get(&j, 0, "choices") : J_NONE;
        size_t choice = choices != J_NONE && j.tokens[choices].type == J_ARRAY && j.tokens[choices].count ? choices + 1 : J_NONE;
        size_t m = json_get(&j, choice, "message");
        size_t content = json_get(&j, m, "content"), calls = json_get(&j, m, "tool_calls");
        int valid_content = content == J_NONE || j.tokens[content].type == J_STRING ||
            (j.tokens[content].type == J_PRIMITIVE && json_is(&j, content, "null"));
        int has_calls = calls != J_NONE && j.tokens[calls].type == J_ARRAY && j.tokens[calls].count;
        if (!parsed || !json_is(&j, json_get(&j, m, "role"), "assistant") || !valid_content || !valid_calls(&j, calls)) {
            puts("smolcode: malformed API response; no tools executed.");
            json_free(&j); buf_free(&response); return 0;
        }
        json_string(&j, content, &text);
        if (!text.len && !has_calls) {
            size_t refusal = json_get(&j, m, "refusal");
            if (json_string(&j, refusal, &text) && text.len) content = refusal;
        }
        if (!has_calls && !text.len && (content == J_NONE || json_is(&j, content, "null"))) {
            puts("smolcode: API response has no assistant content.");
            json_free(&j); buf_free(&response); buf_free(&text); return 0;
        }
        assistant(a, &j, content, calls);
        if (!session_save(a)) { json_free(&j); buf_free(&response); buf_free(&text); return 0; }
        size_t original_len = text.len; hide_reasoning(&text);
        if (original_len && !text.len && !has_calls) puts("smolcode: model returned reasoning without a visible answer.");
        if (text.len) print_response(&text);
        buf_free(&text);
        if (!has_calls) {
            size_t finish = json_get(&j, choice, "finish_reason");
            if (json_is(&j, finish, "length")) puts("smolcode: response stopped at the provider's output limit.");
            if (json_is(&j, finish, "content_filter")) puts("smolcode: response stopped by the provider's content filter.");
            json_free(&j); buf_free(&response); return 1;
        }
        int limited = 0;
        for (size_t c = calls + 1; c < j.tokens[calls].next; c = j.tokens[c].next) {
            Buf id = {0}, name = {0}, args = {0}, result = {0};
            size_t f = json_get(&j, c, "function");
            json_string(&j, json_get(&j, c, "id"), &id);
            json_string(&j, json_get(&j, f, "name"), &name);
            json_string(&j, json_get(&j, f, "arguments"), &args);
            if (executed >= 64) {
                limited = 1; buf_puts(&result, "Error: not executed: 64 tool calls per user turn limit reached.");
            } else { result = tool_execute(name.data, args.data, args.len); executed++; }
            tool_result(a, &id, &result);
            buf_free(&id); buf_free(&name); buf_free(&args); buf_free(&result);
            if (!session_save(a)) { json_free(&j); buf_free(&response); return 0; }
        }
        json_free(&j); buf_free(&response);
        if (limited || interrupted || executed >= 64) {
            const char *stop = interrupted ? "Operation interrupted; completed tool results are retained." :
                "Stopped after 64 tool calls in one user turn; completed tool results are retained.";
            message(a, "assistant", stop, strlen(stop)); session_save(a); puts(stop); return 0;
        }
    }
}
