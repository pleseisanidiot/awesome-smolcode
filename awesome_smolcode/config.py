"""Explicit, user-owned configuration. Never execute workspace configuration."""
import dataclasses
import hashlib
import os
from pathlib import Path
import tomllib
from urllib.parse import urlsplit


@dataclasses.dataclass
class Config:
    workspace: Path
    model: str = 'qwen3.5:9b'
    api_url: str = 'http://127.0.0.1:11434/v1/chat/completions'
    permission: str = 'ask'
    sandbox: str = 'required'
    network: bool = False
    timeout: int = 60
    max_output: int = 65536
    max_calls: int = 64
    max_tokens: int = 8192
    mcp: dict = dataclasses.field(default_factory=dict)
    plugins: dict = dataclasses.field(default_factory=dict)
    skill_dirs: list = dataclasses.field(default_factory=list)
    state_dir: Path | None = None

    def __post_init__(self):
        self.workspace = Path(self.workspace).resolve(strict=True)
        if not self.workspace.is_dir():
            raise ValueError('workspace must be a directory')
        if self.permission not in ('read-only', 'ask', 'auto'):
            raise ValueError('permission must be read-only, ask, or auto')
        if self.sandbox not in ('required', 'none'):
            raise ValueError('sandbox must be required or none')
        if any(type(x) is not int or x <= 0 for x in (self.timeout, self.max_output, self.max_calls, self.max_tokens)):
            raise ValueError('limits must be positive integers')
        url = urlsplit(self.api_url)
        if not self.model or url.scheme not in ('http', 'https') or not url.hostname or url.username or url.password:
            raise ValueError('invalid model or API URL')
        if url.scheme == 'http' and url.hostname not in ('localhost', '127.0.0.1', '::1'):
            raise ValueError('remote model endpoints must use HTTPS')
        if self.state_dir is None:
            base = Path(os.getenv('XDG_STATE_HOME', str(Path.home() / '.local/state')))
            digest = hashlib.sha256(str(self.workspace).encode()).hexdigest()[:16]
            self.state_dir = base / 'awesome-smolcode' / digest
        self.state_dir = Path(self.state_dir).resolve()
        if self.state_dir.is_relative_to(self.workspace):
            raise ValueError('state directory must be outside the model workspace')
        self.state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.chmod(self.state_dir, 0o700)

    @classmethod
    def load(cls, workspace, path=None, **overrides):
        source = Path(path).expanduser() if path else Path.home() / '.config/awesome-smolcode/config.toml'
        if path and not source.is_file():
            raise ValueError(f'configuration file not found: {source}')
        data = tomllib.loads(source.read_text()) if source.exists() else {}
        allowed = {f.name for f in dataclasses.fields(cls)} - {'workspace', 'state_dir'}
        unknown = set(data) - allowed
        if unknown:
            raise ValueError(f'unknown configuration keys: {sorted(unknown)}')
        if 'api_url' not in data:
            data['api_url'] = os.getenv('SMOLCODE_API_URL', cls.api_url)
        if 'model' not in data:
            data['model'] = os.getenv('SMOLCODE_MODEL', cls.model)
        data.update({k: v for k, v in overrides.items() if v is not None})
        return cls(Path(workspace), **data)


def redact(value):
    text = str(value)
    for name in ('OPENAI_API_KEY', 'TELEGRAM_BOT_TOKEN'):
        secret = os.getenv(name)
        if secret:
            text = text.replace(secret, '[REDACTED]')
    return text


def redact_data(value):
    """Redact before JSON escaping so credentials with quotes cannot leak."""
    if isinstance(value, str):
        return redact(value)
    if isinstance(value, list):
        return [redact_data(item) for item in value]
    if isinstance(value, dict):
        return {key: redact_data(item) for key, item in value.items()}
    return value
