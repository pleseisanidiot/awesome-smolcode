"""MCP stdio JSON-RPC and executable plugin tools; all launches are gated."""
import contextlib
import json
import os
import queue
import re
import signal
import subprocess
import threading
import time


class MCP:
    def __init__(self, name, spec, policy):
        self.name, self.spec, self.policy = name, spec, policy
        self.proc = None
        self.serial = 0
        self.replies = queue.Queue(maxsize=256)
        self.lock = threading.Lock()

    def start(self):
        self.policy.check('mcp_start', {'server': self.name, 'command': self.spec['command']})
        argv, env = self.policy.command(self.spec['command'], self.spec.get('env', []))
        self.proc = subprocess.Popen(argv, cwd=self.policy.config.workspace, env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, start_new_session=True)
        def read():
            try:
                while True:
                    raw = self.proc.stdout.readline(self.policy.config.max_output + 1)
                    if not raw:
                        raise RuntimeError('MCP server closed stdout')
                    if len(raw) > self.policy.config.max_output:
                        raise ValueError('MCP response exceeds limit')
                    self.replies.put_nowait(json.loads(raw))
            except (Exception,) as exc:
                with contextlib.suppress(queue.Full):
                    self.replies.put_nowait(exc)
        threading.Thread(target=read, daemon=True).start()
        initialized = self.rpc('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                                'clientInfo': {'name': 'awesome-smolcode', 'version': '0.1.0'}})
        if not isinstance(initialized, dict) or initialized.get('protocolVersion') != '2024-11-05':
            raise ValueError('MCP server did not negotiate supported protocol version 2024-11-05')
        self.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})

    def send(self, message):
        raw = (json.dumps(message) + '\n').encode()
        if len(raw) > 4096:
            raise ValueError('MCP request exceeds 4096-byte limit')
        # A non-reading server must not block the whole agent indefinitely.
        import select
        fd = self.proc.stdin.fileno()
        os.set_blocking(fd, False)
        deadline = time.monotonic() + self.policy.config.timeout
        view = memoryview(raw)
        while view:
            if time.monotonic() >= deadline:
                raise TimeoutError('MCP write timed out')
            if select.select([], [fd], [], 0.1)[1]:
                try:
                    n = os.write(fd, view)
                    view = view[n:]
                except BlockingIOError:
                    pass

    def rpc(self, method, params):
        with self.lock:
            self.serial += 1
            ident = self.serial
            self.send({'jsonrpc': '2.0', 'id': ident, 'method': method, 'params': params})
            deadline = time.monotonic() + self.policy.config.timeout
            while True:
                msg = self.replies.get(timeout=max(0.01, deadline - time.monotonic()))
                if isinstance(msg, Exception):
                    raise msg
                if not isinstance(msg, dict):
                    raise ValueError('invalid MCP response')
                if 'method' in msg and 'id' in msg:
                    self.send({'jsonrpc': '2.0', 'id': msg['id'], 'error': {'code': -32601, 'message': 'Client requests unsupported'}})
                if msg.get('id') == ident and 'method' not in msg:
                    if 'error' in msg:
                        raise RuntimeError(f'MCP error: {msg["error"]}')
                    if 'result' not in msg:
                        raise ValueError('missing MCP result')
                    return msg['result']
                if time.monotonic() >= deadline:
                    raise TimeoutError('MCP response timed out')

    def tools(self):
        found = []
        cursor = None
        for _ in range(16):
            result = self.rpc('tools/list', {'cursor': cursor} if cursor else {})
            found.extend(result.get('tools', []))
            cursor = result.get('nextCursor')
            if not cursor:
                return found
        raise ValueError('MCP tool pagination exceeds limit')

    def close(self):
        if self.proc:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
            self.proc.stdin.close()
            self.proc.stdout.close()
            self.proc = None


class Extensions:
    def __init__(self, config, policy):
        self.config, self.policy = config, policy
        self.tools = {}
        self.servers = []
        self.ready = False

    def discover(self):
        if self.ready:
            return
        self.ready = True
        for name, spec in self.config.plugins.items():
            self.add(f'plugin_{name}', spec, ('plugin', spec))
        for name, spec in self.config.mcp.items():
            server = MCP(name, spec, self.policy)
            try:
                server.start()
                self.servers.append(server)
                for tool in server.tools():
                    self.add(f'mcp_{name}_{tool["name"]}', tool, ('mcp', server, tool['name']))
            except Exception as exc:
                server.close()
                self.policy.emit({'type': 'error', 'text': f'MCP {name}: {exc}'})

    def add(self, name, spec, target):
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', name) or name in self.tools:
            raise ValueError(f'invalid or duplicate extension tool name: {name}')
        schema = spec.get('inputSchema', spec.get('parameters', {'type': 'object', 'properties': {}}))
        if not isinstance(schema, dict) or schema.get('type') != 'object':
            raise ValueError('extension schema must describe an object')
        self.tools[name] = ({'type': 'function', 'function': {'name': name,
                           'description': str(spec.get('description', name))[:2048], 'parameters': schema}}, target)

    def call(self, name, args):
        schema, target = self.tools[name]
        self.policy.check(name, args)
        if target[0] == 'mcp':
            try:
                return json.dumps(target[1].rpc('tools/call', {'name': target[2], 'arguments': args}), ensure_ascii=False)
            except Exception:
                target[1].close()
                raise
        spec = target[1]
        code, output, diagnostics = self.policy.run(spec['command'], json.dumps({'tool': name, 'arguments': args}) + '\n',
                                                   spec.get('env', []), merge_stderr=False)
        if code:
            raise RuntimeError(f'plugin exited {code}: {diagnostics or output}')
        if diagnostics:
            self.policy.emit({'type': 'notice', 'text': f'{name}: {diagnostics}'})
        value = json.loads(output)
        return json.dumps(value, ensure_ascii=False)

    def close(self):
        for server in self.servers:
            server.close()
