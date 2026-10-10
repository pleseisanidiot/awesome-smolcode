import argparse
import json
import os
from pathlib import Path
import shutil
import sys

from .agent import Agent
from .config import Config, redact


def parser():
    p = argparse.ArgumentParser(description='Awesome Smolcode: coding agent, GUI, and Telegram bot')
    p.add_argument('--workspace', default='.', help='directory the agent may access')
    p.add_argument('--config', help='explicit trusted TOML configuration (default: user config directory)')
    p.add_argument('--session', default='default', help='conversation name; choose another for a fresh start')
    p.add_argument('--permission', choices=['read-only', 'ask', 'auto'])
    p.add_argument('--sandbox', choices=['required', 'none'])
    p.add_argument('--model')
    p.add_argument('--api-url')
    sub = p.add_subparsers(dest='mode')
    cli = sub.add_parser('chat', help='interactive terminal chat')
    cli.add_argument('prompt', nargs='?', help='run one task and exit')
    gui = sub.add_parser('gui', help='local browser GUI')
    gui.add_argument('--port', type=int, default=8080)
    gui.add_argument('--open', action='store_true', help='open the browser')
    bot = sub.add_parser('telegram', help='Telegram long-polling bot')
    bot.add_argument('--allow-user', type=int, action='append', required=True, help='allowed numeric user ID; repeat for multiple users')
    sub.add_parser('doctor', help='inspect configuration and sandbox readiness without contacting the model')
    return p


def terminal_approve(name, args):
    if not sys.stdin.isatty():
        return False
    print(terminal_text(f'Permission requested: {name}\n{redact(json.dumps(args, ensure_ascii=False, indent=2))}'))
    return input('Allow this operation once? [y/N] ').strip().lower() == 'y'


def display(event):
    if event['type'] in ('assistant', 'notice', 'error'):
        print(terminal_text(event.get('text', '')))
    elif event['type'] == 'tool':
        print(f'● {event["tool"]}')
    elif event['type'] == 'permission' and not event['allowed']:
        print(f'Permission denied: {event["tool"]}')


def terminal_text(value):
    return ''.join(c if c in '\n\t' or (ord(c) >= 32 and ord(c) != 127 and not 128 <= ord(c) <= 159)
                   else f'\\x{ord(c):02x}' for c in redact(value))


def main():
    args = parser().parse_args()
    try:
        config = Config.load(args.workspace, args.config, permission=args.permission, sandbox=args.sandbox,
                             model=args.model, api_url=args.api_url)
        if args.mode == 'doctor':
            print(json.dumps({'workspace': str(config.workspace), 'model': config.model,
                              'api_url': config.api_url, 'permission': config.permission,
                              'sandbox': config.sandbox, 'network': config.network,
                              'bubblewrap': shutil.which('bwrap'), 'api_key_set': bool(os.getenv('OPENAI_API_KEY')),
                              'mcp_servers': list(config.mcp), 'plugins': list(config.plugins),
                              'state_directory': str(config.state_dir)}, indent=2))
            if config.sandbox == 'required':
                from .security import Policy
                code, output = Policy(config).run(['/bin/true'])
                if code:
                    raise RuntimeError(f'bubblewrap probe failed: {output}')
                print('Sandbox probe passed.')
            return
        if args.mode == 'gui':
            from .gui import serve
            serve(config, args.session, args.port, args.open)
            return
        if args.mode == 'telegram':
            from .telegram import Telegram
            Telegram(config, args.allow_user).run()
            return
        agent = Agent(config, args.session, terminal_approve, display)
        try:
            if args.mode == 'chat' and args.prompt is not None:
                agent.turn(args.prompt)
                return
            print(f'awesome-smolcode\nModel: {config.model} · Permissions: {config.permission} · Sandbox: {config.sandbox}\nSession: {args.session}\n/exit quits; /help shows controls.')
            while True:
                try:
                    text = input('> ')
                    if text == '/exit':
                        break
                    if text == '/help':
                        print('/exit quits. Use --session NAME to resume or start another conversation. Ctrl+C cancels a turn.')
                    elif text.strip():
                        agent.turn(text)
                except KeyboardInterrupt:
                    print('\nInterrupted; completed results are saved. Pending calls will be recovered without replay.')
                except EOFError:
                    break
                except Exception as exc:
                    print(terminal_text(f'Error: {exc}'), file=sys.stderr)
        finally:
            agent.close()
    except (Exception,) as exc:
        print(terminal_text(f'awesome-smolcode: {exc}'), file=sys.stderr)
        raise SystemExit(1)
