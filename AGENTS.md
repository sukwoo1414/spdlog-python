# AGENTS.md

## Project Goal
- This repository is privately managed for personal use.
- Prioritize reliability and security over feature velocity.

## Working Rules
- Default branch is `master`; direct commits to `master` are allowed.
- Make small, reviewable commits with clear messages.
- Do not rewrite published history unless explicitly requested.

## Security Requirements (Highest Priority)
- Never commit or push secrets, credentials, personal data, or machine-local sensitive files.
- Explicitly forbidden to read from, copy, or embed values from:
  - `/home/swyang/token`
  - `~/.ssh/*`
  - `~/.aws/*`
  - any `*.pem`, `*.key`, `*.p12`, `*.pfx`, `.env*`, `id_rsa*`, `id_ed25519*`
- Never include access tokens in git remote URLs, commit messages, logs, docs, or code.
- If authentication is required, use local credential helpers or temporary environment variables that are not written to files.

## Pre-Commit / Pre-Push Checklist (Mandatory)
Before every commit and push:
1. Verify changed files: `git status --short`
2. Review staged diff: `git diff --staged`
3. Scan for likely secrets:
   - `rg -n "(ghp_|github_pat_|token|apikey|api_key|secret|password|passwd|private[_-]?key|BEGIN [A-Z ]*PRIVATE KEY)" .`
4. Ensure no local/sensitive files are tracked:
   - `git ls-files | rg -n "(\.env|\.pem$|\.key$|id_rsa|id_ed25519|token)"`
5. Push only after checks are clean.

## If a Secret Is Suspected
- Stop immediately.
- Do not push.
- Remove the secret from tracked files.
- If already committed, rotate/revoke the credential first, then rewrite history as needed.

## Code Change Policy
- Keep behavior-compatible changes unless requested otherwise.
- Add tests for bug fixes when feasible.
- Document non-obvious operational behavior changes in commit message.

## Project-Specific Conventions
- Python package name is `spdlog_swyang`.
- Python import path is also `spdlog_swyang` (not `spdlog`).
- C++ extension module name must stay aligned with packaging:
  - `setup.py` `Extension('spdlog_swyang', ...)`
  - `src/pyspdlog.cpp` `PYBIND11_MODULE(spdlog_swyang, m)`

## Submodule Maintenance (`spdlog`)
- `spdlog` is tracked as a git submodule and should be updated explicitly.
- For upgrade work:
1. Move submodule to target tag/commit.
2. Reinstall and run tests in venv:
   - `pip install -e . --no-build-isolation`
   - `python -m unittest tests.test_spdlog`
3. Run benchmark once for smoke check:
   - `python tests/spdlog_vs_logging.py`
4. Commit only the submodule pointer change unless wrapper code changes are required.

## Logging Behavior Guarantees
- Signal flush is implemented as best-effort for termination signals.
- `SIGKILL` cannot be intercepted and cannot flush by design.
- Default timestamp behavior should remain local time (`PatternTimeType.local`).
- UTC output must be opt-in via `PatternTimeType.utc`.

## Performance Evaluation Notes
- Single-run benchmark deltas are noisy; treat small changes as inconclusive.
- For performance decisions, prefer repeated runs (3-5) and compare averages.

## Agent Output Policy
- Do not print secret values in terminal output summaries.
- When referring to sensitive paths, mention path only, never file contents.
