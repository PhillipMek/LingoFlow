# Third-Party Notices

LingoFlow is distributed under the GNU Affero General Public License v3.0
(see `LICENSE`). It is combined with, or interacts with, the following third-party
software. The license texts required by these components ship inside the
repository or are referenced with their official location.

| Component | Version | Licence | How it is used | Where its licence lives |
|---|---|---|---|---|
| JUCE | 9.0.3 | AGPLv3 (optionally commercial) | Application framework (`GUI`, `audio devices`, build integration). Vendored. | `third_party/JUCE/LICENSE.md` (also `LICENSES/JUCE-LICENSE.md`); full AGPLv3 text: `LICENSE` |
| Steinberg ASIO SDK | 2.3.4 | Steinberg ASIO License **or** GPLv3 (dual); used under GPLv3, consistent with the project's AGPL combination | ASIO host headers/source used by the platform adapter. Vendored. | `third_party/asiosdk/LICENSE.txt` (also `LICENSES/ASIO-SDK-LICENSE.txt`); full GPLv3 text: `LICENSES/GPL-3.0.txt` |
| nlohmann/json | 3.12.0 | MIT | JSON parsing inside the Config and Network modules. Fetched by CMake at configure time (not vendored). | `LICENSES/nlohmann-json-MIT.txt` (verbatim from the upstream `LICENSE.MIT`) |
| Catch2 | v3.16.0 | BSL-1.0 | Test framework only; never linked into the shipped application. Fetched by CMake at configure time. | `LICENSES/Catch2-BOOST-SOFTWARE-LICENSE-1.0.txt` (verbatim from the upstream `LICENSE.txt`) |
| NDI SDK headers | 6.x | NewTek proprietary licence | Compile-time declarations only. The import library is **not** linked: the NDI runtime DLL is loaded dynamically at run time when present (see `docs/licensing.md`). SDK is installed externally. | Provided with the NDI SDK install; not redistributed here |
| Waves SoundGrid ASIO driver | - | Waves EULA | External system prerequisite. Nothing is copied into this repository or the installer. | Installed with the Waves driver |
| OpenAI Realtime Translation API | - | OpenAI terms (network service) | A remote service called over TLS. No OpenAI SDK or library is bundled. | https://openai.com/policies |

The `LICENSES/` directory collects the verbatim licence texts of every
dependency so that a recipient of any copy of this repository - or of the
release package, which carries `LICENSE`, `THIRD_PARTY_NOTICES.md` and
`LICENSES/` next to the executable - has the full licence chain in hand.

The complete corresponding source of this program (including the vendored trees
above and the build scripts) is the repository this file lives in. The in-app
licence notice required by the project's own licensing policy
(`docs/licensing.md`) is an open release item, tracked there.
