#define _POSIX_C_SOURCE 200809L
#include "smolcode.h"
#include <curl/curl.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char tools[] =
    "[{\"type\":\"function\",\"function\":{\"name\":\"read\","
    "\"description\":\"Read a regular file. Prefer line chunks: start_line/end_line are inclusive, 1-based; "
    "omitted bounds mean 1/EOF. Oversized selections return an error without contents; request fewer lines.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"start_line\":{\"type\":\"integer\",\"minimum\":1},\"end_line\":{\"type\":\"integer\",\"minimum\":1}},"
    "\"required\":[\"path\"],\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write\","
    "\"description\":\"Create or overwrite one regular file with exact content.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"],\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"sh\","
    "\"description\":\"Execute /bin/sh -c as the current user; return combined output and exit status.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}}}]";

static size_t receive(char *ptr, size_t size, size_t count, void *data)
{
    if (interrupted || (count && size > SIZE_MAX / count)) return 0;
    size_t n = size * count; buf_add(data, ptr, n); return n;
}

static int progress(void *data, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    (void)data; (void)a; (void)b; (void)c; (void)d;
    return interrupted != 0;
}

static void api_error(const Buf *body, long status)
{
    printf("smolcode: HTTP %ld", status);
    if (status == 429) fputs(" (rate limited)", stdout);
    Json j = {0}; Buf message = {0};
    if (json_parse(&j, body->data, body->len))
        json_string(&j, json_get(&j, json_get(&j, 0, "error"), "message"), &message);
    if (message.len) { fputs(": ", stdout); safe_print(message.data, message.len); }
    else fputs(": request failed (no valid API error message)", stdout);
    putchar('\n'); json_free(&j); buf_free(&message);
}

static int unsupported(const Buf *body, long status, const char *parameter)
{
    if (status != 400 && status != 422) return 0;
    Json j = {0}; Buf message = {0}; int match = 0;
    if (json_parse(&j, body->data, body->len)) {
        size_t e = json_get(&j, 0, "error");
        json_string(&j, json_get(&j, e, "message"), &message);
        for (size_t i = 0; i < message.len; i++) message.data[i] = (char)tolower((unsigned char)message.data[i]);
        int named = json_is(&j, json_get(&j, e, "param"), parameter) ||
                    (message.data && strstr(message.data, parameter));
        match = named && (json_is(&j, json_get(&j, e, "code"), "unsupported_parameter") ||
                (message.data && (strstr(message.data, "unsupported") || strstr(message.data, "not supported") ||
                 strstr(message.data, "unknown") || strstr(message.data, "unrecognized") ||
                 strstr(message.data, "not permitted") || strstr(message.data, "unexpected"))));
    }
    json_free(&j); buf_free(&message); return match;
}

int api_request(Agent *a, Buf *response)
{
    const char *key = getenv(API_KEY_ENV);
    for (;;) {
        Buf request = {0}, auth = {0};
        buf_puts(&request, "{\"model\":"); json_quote(&request, a->model, strlen(a->model));
        buf_puts(&request, ",\"messages\":["); buf_add(&request, a->messages.data, a->messages.len);
        buf_puts(&request, "],\"tools\":"); buf_puts(&request, tools);
        const char *parameter = a->token_parameter == 0 ? "max_completion_tokens" : "max_tokens";
        if (a->token_parameter < 2) buf_printf(&request, ",\"%s\":%d", parameter, MAX_OUTPUT_TOKENS);
        buf_puts(&request, "}");
        buf_puts(&auth, "Authorization: Bearer "); buf_puts(&auth, key);
        CURL *curl = curl_easy_init(); struct curl_slist *headers = NULL;
        CURLcode rc = CURLE_FAILED_INIT; long status = 0;
        if (!curl) goto done;
        headers = curl_slist_append(NULL, "Content-Type: application/json");
        if (!headers) goto done;
        struct curl_slist *more = curl_slist_append(headers, auth.data);
        if (!more) goto done;
        headers = more;
        if (request.len > INT64_MAX) goto done;
#define SET(option, value) do { rc = curl_easy_setopt(curl, option, value); if (rc != CURLE_OK) goto done; } while (0)
        SET(CURLOPT_URL, a->api_url);
        SET(CURLOPT_HTTPHEADER, headers);
        SET(CURLOPT_POST, 1L);
        SET(CURLOPT_POSTFIELDS, request.data);
        SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request.len);
        SET(CURLOPT_WRITEFUNCTION, receive);
        SET(CURLOPT_WRITEDATA, response);
        SET(CURLOPT_XFERINFOFUNCTION, progress);
        SET(CURLOPT_NOPROGRESS, 0L);
        SET(CURLOPT_NOSIGNAL, 1L);
        SET(CURLOPT_CONNECTTIMEOUT, 15L);
        SET(CURLOPT_TIMEOUT, 300L);
        /* Never forward the credential through an HTTP redirect. */
        SET(CURLOPT_FOLLOWLOCATION, 0L);
#undef SET
        rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) rc = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
done:
        curl_slist_free_all(headers); if (curl) curl_easy_cleanup(curl);
        buf_free(&auth); buf_free(&request);
        if (rc != CURLE_OK) {
            printf("smolcode: %s\n", interrupted ? "request interrupted" : curl_easy_strerror(rc));
            return 0;
        }
        if (status >= 200 && status < 300) return 1;
        if (a->token_parameter < 2 && unsupported(response, status, parameter)) {
            a->token_parameter++;
            printf("smolcode: endpoint rejects %s; retrying %s.\n", parameter,
                   a->token_parameter == 1 ? "with max_tokens" : "without a token limit (provider default)");
            buf_free(response);
            if (interrupted) return 0;
            continue;
        }
        api_error(response, status); return 0;
    }
}
