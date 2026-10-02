# Licensing decisions

Decided by the product owner on 2026-10-01 and recorded here as binding for all
later tasks. This file is the reference for the release/audit tasks (026, 027) and
for any dependency added by tasks 015/016/020.

| Dependency | Chosen licence | Consequence for this project |
|---|---|---|
| JUCE 9.0.3 | **AGPLv3** | The application is a GPL-family work. Source for the whole combined work must be offered to every recipient; a `LICENSE` (AGPLv3) plus third-party notices must ship with it; the About/Settings area must show the licence and where to get the source (task 014/015), and the installer must not hide it (task 026). |
| Steinberg ASIO SDK 2.3.4 | **GPLv3** | Compatible with the AGPLv3 combination. Headers/sources used from `third_party/asiosdk` stay GPLv3; the ASIO logo may not be used without Steinberg's separate trademark rules, so no "ASIO compatible" branding in the UI unless that agreement is signed. |
| nlohmann/json 3.12.0 | MIT | Fine. Keep the MIT licence text in the third-party notices. |
| Catch2 v3.16.0 | BSL-1.0 | Test-time only, not linked into the product. Fine. |
| Waves SoundGrid driver | Waves EULA, system install | Not redistributed by us. The product documents it as a required prerequisite; nothing from `C:\Program Files (x86)\Waves` or `System32\SoundGridAsio.dll` is copied into the repository or the installer. |
| NDI SDK 6 / NDI runtime | NewTek proprietary SDK + EULA | **Not GPL-compatible.** See the constraint below. |

## The NDI constraint created by choosing AGPLv3

Linking a proprietary library into a GPL/AGPL program adds a restriction the GPL
does not allow. Therefore, with JUCE under AGPLv3:

1. the NDI SDK import library `Processing.NDI.Lib.x64.lib` must **not** be linked
   into `LingoFlow`;
2. NDI must be reached through runtime loading of `Processing.NDI.Lib.x64.dll`
   (`LoadLibrary` + `GetProcAddress` on the `NDIlib_*` C functions, or the SDK's
   `Processing.NDI.DynamicLoad.h`), behind the existing `INdiOutput` boundary;
3. NDI stays optional at runtime: no DLL, no server, no consumers - the translator
   keeps working (SPEC: NDI failure must not stop audio);
4. the NDI runtime DLL is not embedded in our installer unless the NDI EULA
   redistribution terms at that moment allow it; the installer should instead
   require NDI runtime/tools to be present.

This is a task 016 implementation requirement, not a suggestion, and the
`INdiOutput` design already supports it (no NDI type in any header).

## Source-offer obligation (AGPLv3)

Because the product is distributed as AGPL, before the release build (task 026) we
must have:

* `LICENSE` with the AGPLv3 text and a `NOTICE`/`THIRD_PARTY_NOTICES` listing JUCE,
  ASIO SDK, nlohmann/json, Catch2 and any later dependency;
* the complete corresponding source, including the vendored `third_party/` trees and
  our build scripts, in a form a recipient can rebuild;
* a visible licence notice in the UI (About/Settings) pointing at where the source is;
* no obfuscation of the application sources.

If the product ever needs to be closed-source, the only compliant route is buying the
JUCE 9 commercial licence and removing the AGPL combination - that is a business
decision, not a code change, and it would also relax constraint set above only if
the ASIO SDK moves to its proprietary option as well.

## Secrets and licences interaction

An API key is never part of the distributed source or of `config.json`. With the
repository public-by-licence consequence of AGPLv3, this rule gets extra weight:
the source tree must contain no credential, ever (`.env` is git-ignored; only
`.env.example` with a placeholder is tracked).
