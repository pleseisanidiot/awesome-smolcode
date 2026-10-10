import contextlib
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from awesome_smolcode.agent import Agent, Provider
from awesome_smolcode.approvals import Approvals
from awesome_smolcode.config import Config
from awesome_smolcode.cli import terminal_text
from awesome_smolcode.context import Context
from awesome_smolcode.extensions import Extensions
from awesome_smolcode.gui import App, make_server
from awesome_smolcode.security import Denied, Policy
from awesome_smolcode.telegram import Telegram
from awesome_smolcode.tools import Tools


def call(name, args, ident='c1'):
    return {'id': ident, 'type': 'function', 'function': {'name': name, 'arguments': json.dumps(args)}}


def reply(content=None, calls=None):
    msg = {'role': 'assistant', 'content': content}
    if calls is not None:
        msg['tool_calls'] = calls
    return msg


class FakeProvider:
    def __init__(self, messages):
        self.messages = iter(messages)
        self.requests = []

    def complete(self, messages, tools):
        self.requests.append((messages, tools))
        return next(self.messages)


class RuntimeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.root = self.base / 'project'
        self.root.mkdir()
        self.config = Config(self.root, state_dir=self.base / 'state', sandbox='none', permission='auto', timeout=1)
        self.policy = Policy(self.config)

    def tearDown(self):
        self.tmp.cleanup()

    def test_path_escape_and_metadata(self):
        for name in ('../secret', '/etc/passwd', '.git/config', 'foo/../../secret', ''):
            with self.subTest(name=name), self.assertRaises(Denied):
                self.policy.read(name)

    def test_symlink_files_and_parents_are_rejected(self):
        (self.base / 'secret').write_text('secret')
        (self.root / 'link').symlink_to(self.base / 'secret')
        (self.root / 'dir').symlink_to(self.base, target_is_directory=True)
        for path in ('link', 'dir/secret'):
            with self.subTest(path=path), self.assertRaises(OSError):
                self.policy.read(path)
            with self.subTest(path=path), self.assertRaises((OSError, Denied)):
                self.policy.write(path, 'changed')
        self.assertEqual((self.base / 'secret').read_text(), 'secret')

    def test_atomic_write_preserves_executable_mode(self):
        file = self.root / 'script'
        file.write_text('old')
        file.chmod(0o755)
        self.policy.write('script', 'new')
        self.assertEqual(file.read_text(), 'new')
        self.assertEqual(file.stat().st_mode & 0o777, 0o755)
        self.assertEqual(list(self.root.glob('.awesome-write-*')), [])

    def test_hardlink_write_denied(self):
        (self.base / 'secret').write_text('old')
        os.link(self.base / 'secret', self.root / 'linked')
        with self.assertRaises(Denied):
            self.policy.write('linked', 'new')
        self.assertEqual((self.base / 'secret').read_text(), 'old')

    def test_read_only_denies_all_mutating_tools(self):
        self.config.permission = 'read-only'
        (self.root / 'file').write_text('ok')
        tools = Tools(self.config, self.policy)
        self.assertEqual(tools.execute('read', {'path': 'file'}), 'ok')
        for name, args in [('write', {'path': 'file', 'content': 'bad'}), ('sh', {'command': 'touch bad'})]:
            with self.subTest(name=name), self.assertRaises(Denied):
                tools.execute(name, args)
        self.assertFalse((self.root / 'bad').exists())

    def test_ask_approval_is_single_operation(self):
        self.config.permission = 'ask'
        seen = []
        policy = Policy(self.config, lambda name, args: seen.append(name) or False)
        with self.assertRaises(Denied):
            policy.write('x', 'y')
        self.assertEqual(seen, ['write'])
        self.assertFalse((self.root / 'x').exists())

    def test_shell_scrubs_credentials_and_reports_status(self):
        with patch.dict(os.environ, OPENAI_API_KEY='model-secret', TELEGRAM_BOT_TOKEN='bot-secret'):
            code, output = self.policy.run(['/bin/sh', '-c', 'test -z "$OPENAI_API_KEY" && test -z "$TELEGRAM_BOT_TOKEN"; printf done; exit 7'])
        self.assertEqual(code, 7)
        self.assertEqual(output, 'done')

    def test_subprocess_timeout_and_output_bound(self):
        start = time.monotonic()
        with self.assertRaises(TimeoutError):
            self.policy.run(['/bin/sh', '-c', 'sleep 30 & wait'])
        self.assertLess(time.monotonic() - start, 3)
        self.config.max_output = 32
        code, text = self.policy.run([sys.executable, '-c', 'print("x"*10000)'])
        self.assertEqual(code, 0)
        self.assertLess(len(text), 100)
        self.assertIn('truncated', text)

    def test_real_sandbox_blocks_host_and_network_and_metadata(self):
        if not shutil.which('bwrap'):
            self.skipTest('bubblewrap unavailable')
        self.config.sandbox = 'required'
        code, output = self.policy.run(['/bin/true'])
        if code:
            self.skipTest('user namespaces unavailable: ' + output)
        (self.base / 'secret').write_text('host secret')
        (self.root / '.git').mkdir()
        (self.root / '.git/config').write_text('unchanged')
        code, output = self.policy.run(['/bin/sh', '-c', f'test ! -e {self.base}/secret && test ! -e {self.config.state_dir} || exit 99; echo changed > .git/config; echo workspace > output'])
        self.assertEqual(code, 0)
        self.assertEqual((self.root / '.git/config').read_text(), 'unchanged')
        self.assertEqual((self.root / 'output').read_text(), 'workspace\n')
        self.assertIn('Read-only', output)
        code, output = self.policy.run(['/usr/bin/python3', '-c', 'import socket; socket.create_connection(("1.1.1.1",443),1)'])
        self.assertNotEqual(code, 0)

    def test_missing_sandbox_never_falls_back(self):
        self.config.sandbox = 'required'
        with patch('awesome_smolcode.security.shutil.which', return_value=None), self.assertRaises(Denied):
            self.policy.run(['/bin/sh', '-c', 'touch escaped'])
        self.assertFalse((self.root / 'escaped').exists())

    def test_scoped_instructions_block_first_uninformed_write(self):
        (self.root / 'AGENTS.md').write_text('root instructions')
        (self.root / 'src').mkdir()
        (self.root / 'src/AGENTS.md').write_text('nested instructions')
        tools = Tools(self.config, self.policy)
        self.assertIn('root instructions', tools.context.prompt())
        with self.assertRaisesRegex(ValueError, 'scoped instructions'):
            tools.execute('write', {'path': 'src/file', 'content': 'new'})
        self.assertFalse((self.root / 'src/file').exists())
        self.assertIn('nested instructions', tools.context.prompt())
        tools.execute('write', {'path': 'src/file', 'content': 'new'})
        self.assertEqual((self.root / 'src/file').read_text(), 'new')

    def test_skills_discovered_then_loaded_lazily(self):
        skill = self.root / '.agents/skills/review'
        skill.mkdir(parents=True)
        (skill / 'SKILL.md').write_text('---\nname: review\ndescription: Review code\n---\nSecret guidance')
        context = Context(self.config, self.policy)
        self.assertIn('Review code', context.prompt())
        self.assertNotIn('Secret guidance', context.prompt())
        context.load_skill('review')
        self.assertIn('Secret guidance', context.prompt())
        with self.assertRaises(ValueError):
            context.load_skill('../other')

    def test_agent_multistep_checkpoint_and_resume(self):
        provider = FakeProvider([reply(calls=[call('write', {'path': 'a', 'content': 'hello'})]),
                                 reply(calls=[call('read', {'path': 'a'}, 'c2')]), reply('done')])
        agent = Agent(self.config, provider=provider)
        self.assertEqual(agent.turn('create a file'), 'done')
        self.assertEqual(len(provider.requests), 3)
        self.assertEqual((self.root / 'a').read_text(), 'hello')
        resumed = Agent(self.config, provider=FakeProvider([reply('resumed')]))
        self.assertEqual(resumed.turn('continue'), 'resumed')
        self.assertEqual(len(resumed.history), 8)
        self.assertEqual(agent.path.stat().st_mode & 0o777, 0o600)

    def test_invalid_batch_never_executes_first_valid_call(self):
        good = call('write', {'path': 'a', 'content': 'bad'})
        malformed = call('write', {'path': 'b'})
        provider = FakeProvider([reply(calls=[good, malformed])])
        with self.assertRaises(ValueError):
            Agent(self.config, provider=provider).turn('task')
        self.assertFalse((self.root / 'a').exists())

    def test_duplicate_call_ids_denied(self):
        provider = FakeProvider([reply(calls=[call('write', {'path': 'a', 'content': 'bad'}), call('read', {'path': 'a'})])])
        with self.assertRaises(ValueError):
            Agent(self.config, provider=provider).turn('task')
        self.assertFalse((self.root / 'a').exists())

    def test_crash_recovery_never_replays_pending_calls(self):
        agent = Agent(self.config)
        agent.history = [{'role': 'user', 'content': 'task'}, reply(calls=[call('write', {'path': 'a', 'content': 'bad'})])]
        agent.save()
        agent.load()
        self.assertFalse((self.root / 'a').exists())
        self.assertIn('unknown', agent.history[-1]['content'])
        self.assertEqual(agent.history[-1]['role'], 'tool')

    def test_call_limit_keeps_valid_history(self):
        self.config.max_calls = 1
        agent = Agent(self.config, provider=FakeProvider([reply(calls=[call('write', {'path': 'a', 'content': 'ok'})])]))
        self.assertIn('Stopped after 1', agent.turn('task'))
        agent.load()
        self.assertEqual(agent.history[-1]['role'], 'assistant')

    def test_secret_redaction_before_json_escaping(self):
        secret = 'secret"with\\escapes'
        emitted = []
        with patch.dict(os.environ, OPENAI_API_KEY=secret):
            agent = Agent(self.config, emit=emitted.append, provider=FakeProvider([reply(secret)]))
            agent.turn(secret)
            stored = json.loads(agent.path.read_text())
        self.assertEqual(stored['messages'][0]['content'], '[REDACTED]')
        self.assertEqual(emitted[-1]['text'], '[REDACTED]')

    def test_terminal_text_neutralizes_escape_sequences(self):
        text = terminal_text('\x1b]52;c;clipboard\x07\nhello\tworld')
        self.assertNotIn('\x1b', text)
        self.assertNotIn('\x07', text)
        self.assertIn('hello\tworld', text)

    def test_session_cross_process_lock(self):
        import fcntl
        with (self.config.state_dir / 'default.lock').open('w') as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            with self.assertRaisesRegex(RuntimeError, 'another process'):
                Agent(self.config, provider=FakeProvider([reply('no')])).turn('task')

    def test_malformed_session_left_unchanged(self):
        agent = Agent(self.config)
        agent.path.write_text('{bad')
        with self.assertRaises(ValueError):
            agent.turn('task')
        self.assertEqual(agent.path.read_text(), '{bad')

    def test_plugin_protocol_and_permissions(self):
        script = self.root / 'plugin.py'
        script.write_text('import sys,json\nr=json.loads(sys.stdin.readline())\nprint("diagnostic", file=sys.stderr)\nprint(json.dumps({"answer":r["arguments"]["x"]+1}))\n')
        self.config.plugins = {'increment': {'command': [sys.executable, str(script)], 'parameters': {'type': 'object'}}}
        extensions = Extensions(self.config, self.policy)
        extensions.discover()
        self.assertEqual(json.loads(extensions.call('plugin_increment', {'x': 2})), {'answer': 3})
        self.config.permission = 'read-only'
        with self.assertRaises(Denied):
            extensions.call('plugin_increment', {'x': 2})

    def test_mcp_initialize_discovery_call_and_teardown(self):
        script = self.root / 'mcp.py'
        script.write_text('''import sys,json
for line in sys.stdin:
 r=json.loads(line)
 if "id" not in r: continue
 m=r['method']
 if m=='initialize': result={'protocolVersion':'2024-11-05','capabilities':{'tools':{}},'serverInfo':{'name':'mock','version':'1'}}
 elif m=='tools/list': result={'tools':[{'name':'echo','description':'Echo','inputSchema':{'type':'object'}}]}
 elif m=='tools/call': result={'content':[{'type':'text','text':r['params']['arguments']['text']}]}
 print(json.dumps({'jsonrpc':'2.0','id':r['id'],'result':result}),flush=True)
''')
        self.config.mcp = {'mock': {'command': [sys.executable, str(script)]}}
        ext = Extensions(self.config, self.policy)
        try:
            ext.discover()
            self.assertIn('mcp_mock_echo', ext.tools)
            result = json.loads(ext.call('mcp_mock_echo', {'text': 'hello'}))
            self.assertEqual(result['content'][0]['text'], 'hello')
            self.config.permission = 'read-only'
            with self.assertRaises(Denied):
                ext.call('mcp_mock_echo', {'text': 'bad'})
        finally:
            ext.close()
        self.assertIsNone(ext.servers[0].proc)

    def test_mcp_start_denied_before_process_creation(self):
        self.config.permission = 'read-only'
        self.config.mcp = {'bad': {'command': ['/bin/sh', '-c', 'touch bad']}}
        ext = Extensions(self.config, self.policy)
        ext.discover()
        self.assertFalse((self.root / 'bad').exists())
        self.assertEqual(ext.tools, {})

    def test_approval_actor_nonce_replay_and_expiry(self):
        broker = Approvals(timeout=1)
        outcome = []
        worker = threading.Thread(target=lambda: outcome.append(broker.ask('alice', 'write', {'path': 'a'})))
        worker.start()
        for _ in range(100):
            pending = broker.list('alice')
            if pending: break
            time.sleep(0.01)
        ident = pending[0]['id']
        self.assertFalse(broker.decide('bob', ident, True))
        self.assertTrue(broker.decide('alice', ident, True))
        self.assertFalse(broker.decide('alice', ident, True))
        worker.join(2)
        self.assertEqual(outcome, [True])
        expired = Approvals(timeout=0.02)
        self.assertFalse(expired.ask('alice', 'sh', {}))
        self.assertEqual(expired.list('alice'), [])

    def test_gui_auth_origin_and_approval_routes(self):
        app = App(self.config, 'gui')
        app.agent.provider = FakeProvider([reply('hello')])
        server = make_server(app, 0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        def req(method, path, value=None, auth=True, origin=None):
            conn = http.client.HTTPConnection('127.0.0.1', server.server_port, timeout=2)
            headers = {'Content-Type': 'application/json'}
            if auth: headers['Authorization'] = 'Bearer ' + app.token
            if origin: headers['Origin'] = origin
            conn.request(method, path, json.dumps(value) if value else None, headers)
            r = conn.getresponse()
            result = (r.status, r.read())
            conn.close()
            return result
        try:
            self.assertEqual(req('GET', '/api/state', auth=False)[0], 401)
            self.assertEqual(req('GET', '/')[0], 200)
            self.assertEqual(req('POST', '/api/message', {'message': 'hi'}, origin='https://evil.example')[0], 403)
            self.assertEqual(req('POST', '/api/message', {'message': 'hi'})[0], 202)
            for _ in range(100):
                if not app.busy: break
                time.sleep(0.01)
            state = json.loads(req('GET', '/api/state')[1])
            self.assertIn('hello', [e.get('text') for e in state['events']])
            self.assertEqual(req('POST', '/api/approval', {'id': 'invalid', 'allow': True})[0], 409)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
            app.agent.close()

    def test_telegram_allowlist_private_chats_and_commands(self):
        with patch.dict(os.environ, TELEGRAM_BOT_TOKEN='123:test'):
            bot = Telegram(self.config, [123])
        sent = []
        bot.send = lambda user, text: sent.append((user, text))
        def update(user, kind, text):
            return {'message': {'from': {'id': user}, 'chat': {'id': user, 'type': kind}, 'text': text}}
        bot.handle(update(999, 'private', '/start'))
        bot.handle(update(123, 'group', '/start'))
        self.assertEqual(sent, [])
        bot.handle(update(123, 'private', '/status'))
        self.assertIn('Permissions:', sent[-1][1])
        bot.handle(update(123, 'private', '/new'))
        self.assertIn(123, bot.sessions)
        with patch.dict(os.environ, TELEGRAM_BOT_TOKEN='123:test'):
            restarted = Telegram(self.config, [123])
        self.assertEqual(restarted.sessions[123], bot.sessions[123])
        with patch.dict(os.environ, TELEGRAM_BOT_TOKEN='123:test'), self.assertRaises(ValueError):
            Telegram(self.config, [])

    def test_telegram_task_approval_and_result(self):
        self.config.permission = 'ask'
        with patch.dict(os.environ, TELEGRAM_BOT_TOKEN='123:test'):
            bot = Telegram(self.config, [123])
        sent = []
        bot.send = lambda user, text: sent.append((user, text))
        def make_agent(config, session, approve):
            provider = FakeProvider([reply(calls=[call('write', {'path': 'telegram-file', 'content': 'approved'})]), reply('completed')])
            return Agent(config, session, approve=approve, provider=provider)
        def message(text):
            return {'message': {'from': {'id': 123}, 'chat': {'id': 123, 'type': 'private'}, 'text': text}}
        with patch('awesome_smolcode.telegram.Agent', side_effect=make_agent):
            bot.handle(message('make a file'))
            for _ in range(200):
                pending = bot.approvals.list(123)
                if pending: break
                time.sleep(0.01)
            self.assertTrue(pending)
            self.assertFalse((self.root / 'telegram-file').exists())
            bot.handle(message('/approve ' + pending[0]['id']))
            for _ in range(200):
                if 123 not in bot.busy: break
                time.sleep(0.01)
        self.assertNotIn(123, bot.busy)
        self.assertEqual((self.root / 'telegram-file').read_text(), 'approved')
        self.assertIn((123, 'completed'), sent)

    def test_config_rejects_remote_http_invalid_limits_and_workspace_state(self):
        for kwargs in ({'api_url': 'http://evil.example/v1'}, {'timeout': 0}, {'state_dir': self.root / 'state'}, {'permission': 'unsafe'}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                Config(self.root, **kwargs)
        with self.assertRaisesRegex(ValueError, 'configuration file not found'):
            Config.load(self.root, self.base / 'missing.toml')

    def test_provider_real_http_multistep_and_parameter_fallback(self):
        received = []
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                received.append(body)
                if 'max_completion_tokens' in body:
                    value = {'error': {'message': 'unsupported max_completion_tokens'}}
                    status = 400
                else:
                    status = 200
                    message = reply(calls=[call('write', {'path': 'result', 'content': 'mock success'})]) if len(body['messages']) == 2 else reply('verified')
                    value = {'choices': [{'message': message}]}
                raw = json.dumps(value).encode()
                self.send_response(status)
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.config.api_url = f'http://127.0.0.1:{server.server_port}/v1/chat/completions'
        try:
            with patch.dict(os.environ, OPENAI_API_KEY='test'):
                agent = Agent(self.config)
                self.assertEqual(agent.turn('write a file'), 'verified')
            self.assertEqual((self.root / 'result').read_text(), 'mock success')
            self.assertIn('max_tokens', received[1])
            self.assertEqual(received[-1]['messages'][-1]['role'], 'tool')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_provider_does_not_follow_redirects_with_key(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_POST(self):
                self.send_response(307)
                self.send_header('Location', 'http://127.0.0.1:1/steal')
                self.send_header('Content-Length', '0')
                self.end_headers()
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.config.api_url = f'http://127.0.0.1:{server.server_port}/'
        try:
            with patch.dict(os.environ, OPENAI_API_KEY='test'), self.assertRaisesRegex(RuntimeError, 'HTTP 307'):
                Provider(self.config).complete([], [])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()
