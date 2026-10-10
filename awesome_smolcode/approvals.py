"""Single-use approvals bound to a session and an authenticated actor."""
import secrets
import json
import threading
import time
from .config import redact_data


class Approvals:
    def __init__(self, timeout=120, notify=None):
        self.timeout = timeout
        self.notify = notify or (lambda item: None)
        self.pending = {}
        self.lock = threading.Lock()

    def ask(self, actor, tool, arguments):
        item = {'id': secrets.token_hex(8), 'actor': str(actor), 'tool': tool,
                'arguments': redact_data(arguments),
                'expires': time.time() + self.timeout}
        event = threading.Event()
        with self.lock:
            self.pending[item['id']] = (item, event, False)
        try:
            self.notify(item)
            event.wait(self.timeout)
            with self.lock:
                return self.pending[item['id']][2] if item['id'] in self.pending else False
        finally:
            with self.lock:
                self.pending.pop(item['id'], None)

    def decide(self, actor, ident, allow):
        with self.lock:
            entry = self.pending.get(ident)
            if not entry or entry[0]['actor'] != str(actor) or entry[0]['expires'] < time.time() or entry[1].is_set():
                return False
            self.pending[ident] = (entry[0], entry[1], allow is True)
            entry[1].set()
            return True

    def list(self, actor):
        with self.lock:
            return [dict(item) for item, event, _ in self.pending.values()
                    if item['actor'] == str(actor) and item['expires'] >= time.time() and not event.is_set()]

    def close(self):
        with self.lock:
            for _, event, _ in self.pending.values():
                event.set()
