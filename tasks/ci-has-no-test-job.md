# CI never runs the test suite

Status: **open.** Noticed 2026-07-31 while shipping v0.2.2.

`.github/workflows/publish.yml` is the only workflow and it does `uv build` +
PyPI trusted publishing. Nothing runs `tests/`. So the concurrency regressions
added in 0.2.2 are only ever exercised when someone remembers to run them by hand.

This matters more than usual here because `AGENTS.md` now points future agents at
that suite as the guard for the staging-path invariants. A guard nobody runs is
worse than no guard: it reads as coverage.

## Why it is not a one-line fix

The suite is `@skipUnless(sys.platform == "darwin")` — it compiles and codesigns a
real trampoline. On the `ubuntu-latest` runner the whole class skips and the job
goes green while testing nothing, which is the failure mode above with a badge on
it. A useful job needs `runs-on: macos-latest`.

## Suggested shape

```yaml
test:
  runs-on: macos-latest
  steps:
    - uses: actions/checkout@v6
    - uses: astral-sh/setup-uv@v8.0.0
    - run: python3 -m unittest discover -s tests -v
```

Then make the `build` job in `publish.yml` depend on it, so a release cannot ship
past a red suite. No dependency install step is needed — the tests are stdlib only,
deliberately (see the Conventions section in `AGENTS.md`).

Worth checking at the same time whether the assertion counts still bite on a CI
runner: the races are timing-dependent, and a slower or less parallel runner may
need `ROUNDS` raised above 3 to stay sensitive. Verify by reverting the
staging-path fix on a branch and confirming CI actually goes red — per `AGENTS.md`,
revert the *staging* fix, not the retry, or the mutation passes.
