#ifndef SMOLCODE_H
#define SMOLCODE_H

#include <signal.h>
#include "config.h"
#include "json.h"

extern volatile sig_atomic_t interrupted;
typedef struct {
    Buf messages, session;
    const char *model, *api_url;
    int token_parameter, save_failed;
} Agent;
int agent_init(Agent *a);
int agent_turn(Agent *a, const char *input, size_t len);
int agent_recover(Agent *a);
int api_request(Agent *a, Buf *response);
Buf tool_execute(const char *name, const char *arguments, size_t len);
int text_load(const char *path, Buf *out);
int session_start(Agent *a, const char *name);
int session_save(Agent *a);

#endif
