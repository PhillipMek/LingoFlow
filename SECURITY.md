# Security Policy

## Credentials handling

* The OpenAI API key is never stored in the repository, in `config.json`, in
  logs, in diagnostics exports, or in crash data. Production storage is the
  Windows Credential Manager (per-user, LSA-protected); the environment variable
  `OPENAI_API_KEY` is a documented development fallback only.
* The settings dialog masks key entry and clears the field after storing; the UI
  shows the storage location, never the value.
* The diagnostics export applies three layers: gatherers pass presence only,
  credential-shaped keys are redacted by name at render time, and stored secret
  values are erased from the text by value before the file is written. The
  redaction count is announced, never swallowed.
* Transport: TLS 1.2 minimum (TLS 1.3 when available) via WinHTTP with strict
  default certificate validation. The `Authorization` header value is used to
  build the request once and is never logged or stored.

## Reporting a vulnerability

Please open a private security advisory on the repository's Security tab
(GitHub -> Report a vulnerability) rather than a public issue.

## Scope of automated verification vs hardware

CI verifies builds, the full test suite (including security/config tests with
canary credentials) and hardware-free smoke modes. Real-device audio, live
provider sessions, the NDI receiver and clean-machine installs are validated on
the venue protocol only (`docs/rig-checklist.md`, `docs/release.md`) - a green
CI run is not a production readiness claim for those areas.
