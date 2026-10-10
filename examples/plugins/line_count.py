"""Executable plugin protocol: one JSON request on stdin, one JSON value on stdout."""
import json
from pathlib import Path
import sys

request = json.loads(sys.stdin.readline())
path = Path(request['arguments']['path'])
root = Path.cwd().resolve()
resolved = path.resolve()
if not resolved.is_relative_to(root) or '.git' in resolved.relative_to(root).parts:
    raise ValueError('path must be inside workspace and outside .git')
with resolved.open() as f:
    lines = sum(1 for _ in f)
print(json.dumps({'path': str(path), 'lines': lines}))
