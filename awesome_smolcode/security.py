"""Permission checks, race-resistant workspace files, and subprocess isolation."""
import contextlib
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import subprocess
import time

from .config import redact


class Denied(RuntimeError):
    pass


class Policy:
    def __init__(self, config, approve=None, emit=None):
        self.config = config
        self.approve = approve or (lambda name, args: False)
        self.emit = emit or (lambda event: None)

    def check(self, name, args, mutation=True):
        if not mutation:
            return
        mode = self.config.permission
        granted = mode == 'auto' or (mode == 'ask' and self.approve(name, args))
        self.emit({'type': 'permission', 'tool': name, 'allowed': granted})
        if not granted:
            raise Denied(f'permission denied for {name}')

    def parts(self, path):
        if not isinstance(path, str) or not path or '\0' in path:
            raise Denied('expected a nonempty path')
        p = Path(path)
        if p.is_absolute():
            try:
                p = p.relative_to(self.config.workspace)
            except ValueError:
                raise Denied('path is outside the workspace') from None
        if '..' in p.parts or not p.parts or any(x in ('.git', '.smolcode', '.awesome-smolcode') for x in p.parts):
            raise Denied('path traversal or protected metadata')
        return p.parts

    @contextlib.contextmanager
    def parent(self, path):
        parts = self.parts(path)
        fd = os.open(self.config.workspace, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            for part in parts[:-1]:
                nxt = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                os.close(fd)
                fd = nxt
            yield fd, parts[-1]
        finally:
            os.close(fd)

    def read(self, path, limit=None):
        import stat
        with self.parent(path) as (parent, name):
            fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
            with os.fdopen(fd, 'rb') as f:
                if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
                    raise Denied('read requires a regular file')
                data = f.read((limit or self.config.max_output) + 1)
                if len(data) > (limit or self.config.max_output):
                    raise Denied('file exceeds output limit')
                return data.decode('utf-8', errors='replace')

    def write(self, path, content):
        import secrets
        import stat
        if not isinstance(content, str) or len(content.encode()) > self.config.max_output:
            raise Denied('content must be a string within the output limit')
        self.check('write', {'path': path, 'content': content})
        with self.parent(path) as (parent, name):
            try:
                st = os.stat(name, dir_fd=parent, follow_symlinks=False)
                if not stat.S_ISREG(st.st_mode) or st.st_nlink > 1:
                    raise Denied('write requires a regular, non-linked file')
                mode = stat.S_IMODE(st.st_mode) & 0o777
                preserve_mode = True
            except FileNotFoundError:
                mode = 0o644
                preserve_mode = False
            temp = '.awesome-write-' + secrets.token_hex(8)
            fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode, dir_fd=parent)
            try:
                with os.fdopen(fd, 'wb') as f:
                    if preserve_mode:
                        os.fchmod(f.fileno(), mode)
                    f.write(content.encode())
                    f.flush()
                    os.fsync(f.fileno())
                os.rename(temp, name, src_dir_fd=parent, dst_dir_fd=parent)
            finally:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(temp, dir_fd=parent)
        return f'Wrote {len(content.encode())} bytes.'

    def command(self, argv, env_names=()):
        if not isinstance(argv, list) or not argv or any(not isinstance(x, str) or '\0' in x for x in argv):
            raise ValueError('command must be a nonempty list of strings')
        env = {'PATH': os.getenv('PATH', '/usr/bin:/bin'), 'LANG': 'C.UTF-8', 'HOME': '/tmp'}
        for name in env_names:
            if name in ('OPENAI_API_KEY', 'TELEGRAM_BOT_TOKEN'):
                raise Denied('agent credentials cannot be passed to extensions')
            if name in os.environ:
                env[name] = os.environ[name]
        if self.config.sandbox == 'none':
            return argv, env
        bwrap = shutil.which('bwrap')
        if not bwrap:
            raise Denied('bubblewrap unavailable; install it or explicitly select --sandbox none')
        args = [bwrap, '--die-with-parent', '--new-session', '--unshare-all']
        if self.config.network:
            args += ['--share-net']
        for path in ('/usr', '/bin', '/sbin', '/lib', '/lib64'):
            if Path(path).exists():
                args += ['--ro-bind', path, path]
        args += ['--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--dir', '/etc']
        if self.config.network:
            for path in ('/etc/resolv.conf', '/etc/ssl/certs'):
                if Path(path).exists():
                    args += ['--ro-bind', path, path]
        root = str(self.config.workspace)
        args += ['--bind', root, root, '--chdir', root]
        for name in ('.git', '.smolcode', '.awesome-smolcode'):
            path = self.config.workspace / name
            if path.exists() and not path.is_symlink():
                args += ['--ro-bind', str(path), str(path)]
        args += ['--', *argv]
        return args, env

    def run(self, argv, payload=None, env_names=(), merge_stderr=True):
        args, env = self.command(argv, env_names)
        proc = subprocess.Popen(args, cwd=self.config.workspace, env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT if merge_stderr else subprocess.PIPE,
                                start_new_session=True)
        # Input is bounded by callers and is written before reading; cap below pipe size.
        try:
            if payload:
                raw = payload.encode()
                if len(raw) > 4096:
                    raise Denied('extension input exceeds 4096 bytes')
                proc.stdin.write(raw)
            proc.stdin.close()
            output = bytearray()
            diagnostics = bytearray()
            truncated = False
            diagnostics_truncated = False
            deadline = time.monotonic() + self.config.timeout
            with selectors.DefaultSelector() as sel:
                sel.register(proc.stdout, selectors.EVENT_READ, output)
                if not merge_stderr:
                    sel.register(proc.stderr, selectors.EVENT_READ, diagnostics)
                while sel.get_map():
                    if time.monotonic() >= deadline:
                        raise TimeoutError('subprocess timed out')
                    for key, _ in sel.select(min(0.1, max(0, deadline - time.monotonic()))):
                        chunk = os.read(key.fileobj.fileno(), 8192)
                        if not chunk:
                            sel.unregister(key.fileobj)
                            continue
                        destination = key.data
                        room = self.config.max_output - len(destination)
                        destination.extend(chunk[:room])
                        if destination is output:
                            truncated |= len(chunk) > room
                        else:
                            diagnostics_truncated |= len(chunk) > room
            proc.wait(timeout=max(0.01, deadline - time.monotonic()))
            text = redact(output.decode(errors='replace')) + ('\n[Output truncated]' if truncated else '')
            if merge_stderr:
                return proc.returncode, text
            errors = redact(diagnostics.decode(errors='replace'))
            if diagnostics_truncated:
                errors += '\n[Diagnostics truncated]'
            return proc.returncode, text, errors
        finally:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
            proc.stdout.close()
            if proc.stderr:
                proc.stderr.close()
            if not proc.stdin.closed:
                proc.stdin.close()
