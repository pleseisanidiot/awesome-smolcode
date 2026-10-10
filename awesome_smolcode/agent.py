"""Checkpointed OpenAI-compatible agent loop shared by all interfaces."""
import contextlib
import fcntl
import json
import os
import re
import tempfile
import threading
import urllib.error
import urllib.request

from .config import redact, redact_data
from .security import Policy
from .tools import Tools


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Provider:
    def __init__(self, config):
        self.config = config
        self.opener = urllib.request.build_opener(NoRedirect)
        self.token_parameter = 'max_completion_tokens'

    def complete(self, messages, tools):
        key = os.getenv('OPENAI_API_KEY', '')
        if not key or '\n' in key or '\r' in key:
            raise ValueError('set OPENAI_API_KEY (use ollama for local Ollama)')
        body = {'model': self.config.model, 'messages': messages, 'tools': tools}
        body[self.token_parameter] = self.config.max_tokens
        for attempt in range(2):
            req = urllib.request.Request(self.config.api_url, data=json.dumps(body).encode(),
                                         headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + key})
            try:
                with self.opener.open(req, timeout=self.config.timeout) as response:
                    raw = response.read(8 * 1024 * 1024 + 1)
                if len(raw) > 8 * 1024 * 1024:
                    raise ValueError('model response exceeds 8 MiB')
                return json.loads(raw)['choices'][0]['message']
            except urllib.error.HTTPError as exc:
                message = redact(exc.read(65536).decode(errors='replace'))
                if (attempt == 0 and self.token_parameter == 'max_completion_tokens' and exc.code in (400, 422)
                        and 'max_completion_tokens' in message and any(s in message.lower() for s in ('unsupported', 'not supported', 'unknown', 'unrecognized'))):
                    body.pop(self.token_parameter)
                    self.token_parameter = 'max_tokens'
                    body[self.token_parameter] = self.config.max_tokens
                    continue
                raise RuntimeError(f'Model HTTP {exc.code}: {message}') from None


class Agent:
    def __init__(self, config, session='default', approve=None, emit=None, provider=None):
        if not re.fullmatch(r'[a-zA-Z0-9_-]{1,64}', session):
            raise ValueError('invalid session name')
        self.config, self.session = config, session
        self.emit = lambda event: (emit or (lambda event: None))(redact_data(event))
        self.policy = Policy(config, approve, self.emit)
        self.tools = Tools(config, self.policy)
        self.provider = provider or Provider(config)
        self.lock = threading.Lock()
        self.path = config.state_dir / (session + '.json')
        self.history = []

    def save(self):
        fd, temp = tempfile.mkstemp(prefix='.checkpoint-', dir=self.config.state_dir)
        try:
            with os.fdopen(fd, 'w') as f:
                f.write(json.dumps(redact_data({'version': 1, 'messages': self.history}), ensure_ascii=False, indent=2))
                f.flush()
                os.fsync(f.fileno())
            os.replace(temp, self.path)
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(temp)

    def load(self):
        self.history = []
        if not self.path.exists():
            return
        with self.path.open() as f:
            value = json.load(f)
        if value.get('version') != 1 or not isinstance(value.get('messages'), list):
            raise ValueError('malformed conversation; file left unchanged')
        pending = []
        messages = value['messages']
        for msg in messages:
            if not isinstance(msg, dict) or msg.get('role') not in ('user', 'assistant', 'tool'):
                raise ValueError('invalid conversation role')
            if msg.get('role') == 'tool':
                if not pending or pending.pop(0) != msg.get('tool_call_id') or not isinstance(msg.get('content'), str):
                    raise ValueError('unmatched tool result in conversation')
            else:
                if pending:
                    raise ValueError('incomplete tool results inside conversation')
                if msg.get('role') == 'assistant':
                    calls = self.validate_message(msg, check_tools=False)
                    pending = [call['id'] for call in calls]
                elif not isinstance(msg.get('content'), str):
                    raise ValueError('invalid user content')
        self.history = messages
        for ident in pending:
            self.history.append({'role': 'tool', 'tool_call_id': ident,
                                 'content': 'Error: execution status unknown after interruption. Inspect state before retrying. This call was not replayed.'})
        if pending:
            self.save()
            self.emit({'type': 'notice', 'text': 'Recovered pending calls without replaying them.'})

    def validate_message(self, msg, check_tools=True):
        if not isinstance(msg, dict) or msg.get('role') != 'assistant' or (msg.get('content') is not None and not isinstance(msg['content'], str)):
            raise ValueError('malformed assistant response')
        calls = msg.get('tool_calls') or []
        if not isinstance(calls, list) or len(calls) > self.config.max_calls:
            raise ValueError('invalid or excessive tool-call batch')
        ids = set()
        for call in calls:
            if not isinstance(call, dict) or call.get('type') != 'function' or not isinstance(call.get('id'), str) or not call['id'] or call['id'] in ids:
                raise ValueError('invalid or duplicate tool-call ID')
            ids.add(call['id'])
            fn = call.get('function')
            if not isinstance(fn, dict) or not isinstance(fn.get('name'), str) or not isinstance(fn.get('arguments'), str):
                raise ValueError('malformed tool function')
            args = json.loads(fn['arguments'])
            if not isinstance(args, dict):
                raise ValueError('tool arguments must be an object')
            if check_tools:
                self.tools.validate(fn['name'], args)
        if not calls and msg.get('content') is None:
            if isinstance(msg.get('refusal'), str):
                msg['content'] = msg['refusal']
            else:
                raise ValueError('empty assistant response')
        return calls

    def turn(self, text):
        if not isinstance(text, str) or not text.strip() or len(text.encode()) > 65536:
            raise ValueError('message must contain 1–65536 bytes')
        with self.lock:
            # Cross-process lock prevents CLI/GUI/Telegram from overwriting each other.
            fd = os.open(self.config.state_dir / (self.session + '.lock'), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'w') as lock:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    raise RuntimeError('session is active in another process') from None
                self.load()
                self.history.append({'role': 'user', 'content': text})
                self.save()
                executed = 0
                answers = []
                while executed < self.config.max_calls:
                    schemas = self.tools.schemas()
                    msg = self.provider.complete([{'role': 'system', 'content': self.tools.context.prompt()}, *self.history], schemas)
                    calls = self.validate_message(msg)
                    # Preserve only standard assistant fields, excluding provider metadata.
                    saved = {'role': 'assistant', 'content': msg.get('content')}
                    if calls:
                        saved['tool_calls'] = calls
                    self.history.append(saved)
                    self.save()
                    content = re.sub(r'^\s*<think>.*?</think>\s*', '', msg.get('content') or '', flags=re.S)
                    if content.lstrip().startswith('<think>'):
                        content = ''
                    if content:
                        answers.append(content)
                        self.emit({'type': 'assistant', 'text': content})
                    if not calls:
                        return '\n\n'.join(answers)
                    for call in calls:
                        fn = call['function']
                        try:
                            if executed >= self.config.max_calls:
                                result = 'Error: call limit reached; not executed.'
                            else:
                                executed += 1
                                result = self.tools.execute(fn['name'], json.loads(fn['arguments']))
                        except Exception as exc:
                            result = f'Error: {exc}'
                        result = redact(result)
                        encoded = result.encode()
                        if len(encoded) > self.config.max_output:
                            result = encoded[:self.config.max_output].decode(errors='replace') + '\n[Output truncated]'
                        self.history.append({'role': 'tool', 'tool_call_id': call['id'], 'content': result})
                        self.save()
                        self.emit({'type': 'result', 'tool': fn['name'], 'text': result})
                stop = f'Stopped after {self.config.max_calls} tool calls; completed results were saved.'
                self.history.append({'role': 'assistant', 'content': stop})
                self.save()
                self.emit({'type': 'notice', 'text': stop})
                return '\n\n'.join([*answers, stop])

    def reset(self):
        # A reset uses a new session in interfaces; history files remain intact.
        raise RuntimeError('choose a new session name to start a fresh conversation')

    def close(self):
        self.tools.extensions.close()
