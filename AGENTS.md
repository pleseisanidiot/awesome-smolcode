# Working on awesome-smolcode

The root C11 sources are the compatible upstream runtime. New integrations live
in `awesome_smolcode/`, a Python 3.11+ standard-library package. Keep the two
runtime contracts explicit in documentation.

All agent tool entry points must pass through the shared permission layer.
Sandbox failures must never silently fall back to unrestricted execution.
Never pass model or Telegram credentials into subprocesses, log their values,
or commit local configuration containing secrets.

Validate complete tool batches before side effects. Preserve checkpoint-before-
execution ordering and never replay pending tools during recovery. Keep sessions
outside the model workspace. Prefer focused changes and tests that exercise
observable behavior and failure paths.

Run `python -m unittest discover -s tests_python -v` for runtime changes. Run
`make && make check` for C changes. GUI changes should also exercise the optional
`tests_python/browser_smoke.py` Chromium flow when its dependencies are available.
Report real service checks separately from mock-based integration checks.
