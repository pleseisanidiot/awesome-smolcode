#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
src=$PWD
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp main.c agent.c api.c tools.c json.c session.c json.h smolcode.h "$tmp/"
# Test settings are confined to a disposable copy; production config is untouched.
sed -e 's|^#define MODEL .*|#define MODEL "gpt-6"|' \
    -e 's|^#define API_URL .*|#define API_URL "http://127.0.0.1:19083/v1/chat/completions"|' \
    -e 's|^#define API_KEY_ENV .*|#define API_KEY_ENV "OPENAI_API_KEY"|' \
    -e 's/TOOL_TIMEOUT_SECONDS 60/TOOL_TIMEOUT_SECONDS 1/' config.h > "$tmp/config.h"
cd "$tmp"
${CC:-cc} -O2 -std=c11 -Wall -Wextra -Wpedantic -fPIC -shared \
    "$src/tests/io.c" -o test-io.so
${CC:-cc} ${TEST_CFLAGS:--O2} -std=c11 -Wall -Wextra -Wpedantic \
    main.c agent.c api.c tools.c json.c session.c "$src/tests/transport.c" \
    -Wl,--wrap=curl_easy_perform -lcurl -o smolcode
${CC:-cc} ${TEST_CFLAGS:--O2} -std=c11 -Wall -Wextra -Wpedantic -I. \
    "$src/tests/verify.c" json.c tools.c agent.c api.c session.c -lcurl -o verify
LD_PRELOAD="$tmp/test-io.so" ./verify
