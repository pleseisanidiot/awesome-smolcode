"""Loopback-only browser interface with authenticated API and approval controls."""
from collections import deque
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import threading
import webbrowser

from .agent import Agent
from .approvals import Approvals
from .config import redact, redact_data


class App:
    def __init__(self, config, session):
        self.config = config
        self.token = secrets.token_urlsafe(32)
        self.events = deque(maxlen=500)
        self.seq = 0
        self.lock = threading.Lock()
        self.busy = False
        self.approvals = Approvals()
        self.agent = Agent(config, session, approve=lambda name, args: self.approvals.ask('gui', name, args), emit=self.emit)

    def emit(self, event):
        with self.lock:
            self.seq += 1
            self.events.append(redact_data({'seq': self.seq, **event}))

    def state(self):
        with self.lock:
            state = {'events': list(self.events), 'busy': self.busy, 'session': self.agent.session,
                     'workspace': str(self.config.workspace), 'model': self.config.model,
                     'permission': self.config.permission, 'sandbox': self.config.sandbox,
                     'network': self.config.network}
        state['approvals'] = self.approvals.list('gui')
        return state

    def submit(self, message):
        if not isinstance(message, str) or not message.strip() or len(message.encode()) > 65536:
            raise ValueError('message must contain 1–65536 bytes')
        with self.lock:
            if self.busy:
                raise RuntimeError('agent is busy')
            self.busy = True
        self.emit({'type': 'user', 'text': message})
        def work():
            try:
                self.agent.turn(message)
            except Exception as exc:
                self.emit({'type': 'error', 'text': redact(exc)})
            finally:
                with self.lock:
                    self.busy = False
        threading.Thread(target=work, daemon=True).start()


def make_server(app, port=8080):
    page = Path(__file__).with_name('web.html').read_bytes()
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def respond(self, code, value, mime='application/json'):
            data = json.dumps(value).encode() if mime == 'application/json' else value
            self.send_response(code)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Referrer-Policy', 'no-referrer')
            self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'")
            self.end_headers()
            self.wfile.write(data)

        def valid_host(self):
            return self.headers.get('Host') == f'127.0.0.1:{self.server.server_port}'

        def authorized(self):
            actual = self.headers.get('Authorization', '')
            return self.valid_host() and secrets.compare_digest(actual, 'Bearer ' + app.token)

        def do_GET(self):
            if not self.valid_host():
                self.respond(403, {'error': 'invalid host'})
            elif self.path == '/':
                self.respond(200, page, 'text/html; charset=utf-8')
            elif self.path == '/api/state' and self.authorized():
                self.respond(200, app.state())
            else:
                self.respond(401, {'error': 'authentication required'})

        def do_POST(self):
            if not self.authorized():
                self.respond(401, {'error': 'authentication required'})
                return
            expected_origin = f'http://127.0.0.1:{self.server.server_port}'
            if self.headers.get('Origin') not in (None, expected_origin):
                self.respond(403, {'error': 'invalid origin'})
                return
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size <= 131072 or self.headers.get('Content-Type') != 'application/json':
                    raise ValueError('expected bounded JSON body')
                value = json.loads(self.rfile.read(size))
                if not isinstance(value, dict):
                    raise ValueError('expected object')
                if self.path == '/api/message':
                    app.submit(value.get('message'))
                    self.respond(202, {'accepted': True})
                elif self.path == '/api/approval':
                    if not app.approvals.decide('gui', value.get('id'), value.get('allow')):
                        self.respond(409, {'error': 'approval expired or already decided'})
                    else:
                        self.respond(200, {'accepted': True})
                else:
                    self.respond(404, {'error': 'unknown endpoint'})
            except (ValueError, RuntimeError, TypeError) as exc:
                self.respond(400, {'error': redact(exc)})

    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    server.daemon_threads = True
    return server


def serve(config, session='default', port=8080, open_browser=False):
    app = App(config, session)
    server = make_server(app, port)
    url = f'http://127.0.0.1:{server.server_port}/#token={app.token}'
    print(f'GUI: {url}\nKeep this URL private; its token authorizes this agent.')
    if open_browser:
        webbrowser.open(url)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        app.approvals.close()
        server.server_close()
        # Let in-flight turn finish denied approvals before stopping extensions.
        with app.agent.lock:
            app.agent.close()
