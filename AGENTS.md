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

## Agent Output Policy
- Do not print secret values in terminal output summaries.
- When referring to sensitive paths, mention path only, never file contents.
