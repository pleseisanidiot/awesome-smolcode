"""Telegram long-polling adapter with a mandatory private-chat user allowlist."""
import contextlib
import hashlib
import json
import os
import secrets
import tempfile
import threading
import time
import urllib.request

from .agent import Agent, NoRedirect
from .approvals import Approvals
from .config import redact


class Telegram:
    def __init__(self, config, users):
        token = os.getenv('TELEGRAM_BOT_TOKEN', '')
        if not token or '\n' in token or '\r' in token or '/' in token:
            raise ValueError('set TELEGRAM_BOT_TOKEN to a valid bot token')
        self.users = {int(user) for user in users}
        if not self.users or any(user <= 0 for user in self.users):
            raise ValueError('Telegram requires an explicit positive user-ID allowlist')
        self.config = config
        self.url = 'https://api.telegram.org/bot' + token + '/'
        self.opener = urllib.request.build_opener(NoRedirect)
        self.sessions = {}
        self.agents = {}
        self.busy = set()
        self.lock = threading.Lock()
        self.approvals = Approvals(notify=self.notify)
        digest = hashlib.sha256(token.encode()).hexdigest()[:16]
        self.offset_path = config.state_dir / f'telegram-{digest}.offset'
        self.offset = int(self.offset_path.read_text()) if self.offset_path.exists() else 0
        self.sessions_path = config.state_dir / f'telegram-{digest}.sessions.json'
        if self.sessions_path.exists():
            saved = json.loads(self.sessions_path.read_text())
            self.sessions = {int(user): name for user, name in saved.items() if int(user) in self.users}

    def api(self, method, **payload):
        request = urllib.request.Request(self.url + method, data=json.dumps(payload).encode(), headers={'Content-Type': 'application/json'})
        try:
            with self.opener.open(request, timeout=40) as response:
                raw = response.read(4 * 1024 * 1024 + 1)
            if len(raw) > 4 * 1024 * 1024:
                raise ValueError('Telegram response exceeds limit')
            result = json.loads(raw)
            if not result.get('ok'):
                raise RuntimeError(result.get('description', 'Telegram API error'))
            return result.get('result')
        except Exception as exc:
            raise RuntimeError(redact(exc)) from None

    def send(self, chat, text):
        text = redact(text) or '(No visible answer)'
        for start in range(0, len(text), 3000):
            self.api('sendMessage', chat_id=int(chat), text=text[start:start + 3000])

    def notify(self, item):
        args = json.dumps(item['arguments'], ensure_ascii=False)
        self.send(item['actor'], f'Permission requested: {item["tool"]}\n{args[:2000]}\n\n/approve {item["id"]}\n/deny {item["id"]}\nExpires in 2 minutes.')

    def handle(self, update):
        message = update.get('message', {})
        user = message.get('from', {}).get('id')
        chat = message.get('chat', {})
        text = message.get('text', '')
        # No replies or model requests for unauthorized users or group chats.
        if user not in self.users or chat.get('type') != 'private' or chat.get('id') != user or not isinstance(text, str):
            return
        if text.startswith('/approve ') or text.startswith('/deny '):
            cmd, ident = text.split(maxsplit=1)
            ok = self.approvals.decide(user, ident.strip(), cmd == '/approve')
            self.send(user, 'Decision recorded.' if ok else 'Approval expired, already decided, or belongs to another user.')
            return
        if text in ('/start', '/help'):
            self.send(user, 'Awesome Smolcode\nSend a task to work in the configured workspace.\n/new starts a fresh conversation.\n/status shows runtime controls.\n/approve ID or /deny ID decides one tool request.')
            return
        if text == '/status':
            self.send(user, f'Workspace: {self.config.workspace}\nModel: {self.config.model}\nPermissions: {self.config.permission}\nSandbox: {self.config.sandbox}\nNetwork: {self.config.network}')
            return
        with self.lock:
            if user in self.busy:
                busy = True
            else:
                busy = False
                if text == '/new':
                    self.sessions[user] = 'telegram-' + str(user) + '-' + secrets.token_hex(4)
                elif text.strip():
                    self.busy.add(user)
        if busy:
            self.send(user, 'Your agent is working. You can still approve or deny pending requests.')
            return
        if text == '/new':
            self.checkpoint_sessions()
            self.send(user, 'Started a fresh conversation; previous history is preserved.')
            return
        if not text.strip():
            return
        session = self.sessions.setdefault(user, f'telegram-{user}')
        def work():
            agent = None
            try:
                agent = Agent(self.config, session, approve=lambda tool, args: self.approvals.ask(user, tool, args))
                with self.lock:
                    self.agents[user] = agent
                answer = agent.turn(text)
                self.send(user, answer)
            except Exception as exc:
                with contextlib.suppress(Exception):
                    self.send(user, f'Error: {redact(exc)}')
            finally:
                if agent:
                    agent.close()
                with self.lock:
                    self.busy.discard(user)
                    self.agents.pop(user, None)
        threading.Thread(target=work, daemon=True).start()

    def checkpoint(self):
        self.atomic_save(self.offset_path, str(self.offset))

    def checkpoint_sessions(self):
        with self.lock:
            text = json.dumps(self.sessions)
        self.atomic_save(self.sessions_path, text)

    def atomic_save(self, path, text):
        fd, temp = tempfile.mkstemp(dir=self.config.state_dir, prefix='.telegram-offset-')
        try:
            with os.fdopen(fd, 'w') as f:
                f.write(text)
                f.flush()
                os.fsync(f.fileno())
            os.replace(temp, path)
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(temp)

    def run(self):
        print(f'Telegram polling started for {len(self.users)} allowed users; Ctrl+C stops.')
        try:
            while True:
                try:
                    updates = self.api('getUpdates', offset=self.offset, timeout=25, allowed_updates=['message'])
                    for update in updates:
                        ident = update.get('update_id')
                        if not isinstance(ident, int) or ident < self.offset:
                            continue
                        # Checkpoint receipt before side effects: never replay a command on restart.
                        self.offset = ident + 1
                        self.checkpoint()
                        self.handle(update)
                except Exception as exc:
                    print(f'Telegram: {redact(exc)}')
                    time.sleep(2)
        except KeyboardInterrupt:
            pass
        finally:
            self.approvals.close()
            with self.lock:
                agents = list(self.agents.values())
            for agent in agents:
                with agent.lock:
                    agent.close()
