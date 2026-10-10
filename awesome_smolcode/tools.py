import json
from .context import Context
from .extensions import Extensions


def schema(name, description, props, required):
    return {'type': 'function', 'function': {'name': name, 'description': description,
            'parameters': {'type': 'object', 'properties': props, 'required': required, 'additionalProperties': False}}}


class Tools:
    def __init__(self, config, policy):
        self.config, self.policy = config, policy
        self.context = Context(config, policy)
        self.extensions = Extensions(config, policy)
        self.builtins = [
            schema('read', 'Read a regular UTF-8 workspace file.', {'path': {'type': 'string'}}, ['path']),
            schema('write', 'Atomically create or overwrite a workspace file. Parent must exist.',
                   {'path': {'type': 'string'}, 'content': {'type': 'string'}}, ['path', 'content']),
            schema('sh', 'Run a shell command under the configured permissions and sandbox.', {'command': {'type': 'string'}}, ['command']),
            schema('list_files', 'List directory entries in the workspace.', {'path': {'type': 'string'}}, []),
            schema('load_skill', 'Load named SKILL.md guidance into conversation context.', {'name': {'type': 'string'}}, ['name'])]

    def schemas(self):
        self.extensions.discover()
        return self.builtins + [s for s, _ in self.extensions.tools.values()]

    def validate(self, name, args):
        if not isinstance(args, dict):
            raise ValueError('tool arguments must be an object')
        builtin = next((x['function']['parameters'] for x in self.builtins if x['function']['name'] == name), None)
        if builtin:
            if set(args) - set(builtin['properties']) or set(builtin['required']) - set(args):
                raise ValueError(f'invalid arguments for {name}')
            if any(not isinstance(v, str) for v in args.values()):
                raise ValueError('builtin arguments must be strings')
        elif name not in self.extensions.tools:
            raise ValueError(f'unknown tool: {name}')

    def execute(self, name, args):
        self.validate(name, args)
        self.policy.emit({'type': 'tool', 'tool': name, 'arguments': args})
        if name in ('read', 'write'):
            self.context.note_path(args['path'])
        if name == 'read':
            return self.policy.read(args['path'])
        if name == 'write':
            self.context.guard_write(args['path'])
            return self.policy.write(args['path'], args['content'])
        if name == 'sh':
            self.policy.check(name, args)
            code, output = self.policy.run(['/bin/sh', '-c', args['command']])
            return f'Exit status: {code}\n{output}'
        if name == 'load_skill':
            return self.context.load_skill(args['name'])
        if name == 'list_files':
            import os
            path = args.get('path', '.')
            if path == '.':
                fd = os.open(self.config.workspace, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            else:
                self.context.note_path(path + '/placeholder')
                with self.policy.parent(path) as (parent, entry):
                    fd = os.open(entry, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
            try:
                names = sorted(n for n in os.listdir(fd) if n not in ('.git', '.smolcode', '.awesome-smolcode'))
                return json.dumps(names)[:self.config.max_output]
            finally:
                os.close(fd)
        return self.extensions.call(name, args)
