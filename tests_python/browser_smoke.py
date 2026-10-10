"""Optional Chromium GUI smoke check; requires playwright and chromium."""
import json
from pathlib import Path
import tempfile
import threading

from playwright.sync_api import sync_playwright
from awesome_smolcode.config import Config
from awesome_smolcode.gui import App, make_server


class DemoProvider:
    def __init__(self):
        self.step = 0

    def complete(self, messages, tools):
        self.step += 1
        if self.step == 1:
            return {'role': 'assistant', 'content': 'I’ll create a small greeting script, then show you the result.',
                    'tool_calls': [{'id': 'demo-write', 'type': 'function', 'function': {
                        'name': 'write', 'arguments': json.dumps({'path': 'hello.py', 'content': 'print("Hello from awesome-smolcode!")\n'})}}]}
        return {'role': 'assistant', 'content': 'Created hello.py in your workspace.\n\nRun it with: python3 hello.py\n\nThe file was written successfully. I have not run the script.'}


def main():
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        root = base / 'demo-workspace'
        root.mkdir()
        config = Config(root, state_dir=base / 'state', permission='ask')
        app = App(config, 'gui-demo')
        app.agent.provider = DemoProvider()
        server = make_server(app, 0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with sync_playwright() as playwright:
                browser = playwright.chromium.launch(executable_path='/usr/bin/chromium', headless=True, args=['--no-sandbox'])
                page = browser.new_page(viewport={'width': 1400, 'height': 1000}, device_scale_factor=1)
                errors = []
                page.on('pageerror', lambda error: errors.append(str(error)))
                page.goto(f'http://127.0.0.1:{server.server_port}/#token={app.token}')
                page.get_by_text('Ready', exact=True).wait_for()
                page.locator('#message').fill('Create hello.py with a friendly greeting.')
                page.locator('#send').click()
                page.get_by_text('Allow once', exact=True).wait_for()
                assert not (root / 'hello.py').exists(), 'write happened before approval'
                page.get_by_text('Allow once', exact=True).click()
                page.get_by_text('Created hello.py in your workspace.', exact=False).wait_for()
                assert (root / 'hello.py').read_text() == 'print("Hello from awesome-smolcode!")\n'
                page.get_by_text('Ready', exact=True).wait_for()
                page.screenshot(path=str(Path(__file__).resolve().parents[1] / 'docs/gui.png'), full_page=True)
                page.set_viewport_size({'width': 390, 'height': 844})
                assert page.locator('#send').is_visible()
                assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth')
                assert not errors, errors
                browser.close()
                print('PASS Chromium: chat, approval before write, answer, mobile layout, no console errors')
        finally:
            app.approvals.close()
            server.shutdown()
            server.server_close()
            thread.join()
            app.agent.close()


if __name__ == '__main__':
    main()
