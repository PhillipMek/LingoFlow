# Third-Party Notices

LingoFlow is distributed under the GNU Affero General Public License v3.0
(see `LICENSE`). It is combined with, or interacts with, the following third-party
software. The license texts required by these components ship inside the
repository or are referenced with their official location.

| Component | Version | Licence | How it is used | Where its licence lives |
|---|---|---|---|---|
| JUCE | 9.0.3 | AGPLv3 (optionally commercial) | Application framework (`GUI`, `audio devices`, build integration). Vendored. | `third_party/JUCE/LICENSE.md` |
| Steinberg ASIO SDK | 2.3.4 | Steinberg ASIO License **or** GPLv3 (dual); used under GPLv3, consistent with the project's AGPL combination | ASIO host headers/source used by the platform adapter. Vendored. | `third_party/asiosdk/LICENSE.txt` |
| nlohmann/json | 3.12.0 | MIT | JSON parsing inside the Config and Network modules. Fetched by CMake at configure time (not vendored). | Upstream: https://github.com/nlohmann/json (`LICENSE.MIT` in the fetched source) |
| Catch2 | v3.16.0 | BSL-1.0 | Test framework only; never linked into the shipped application. Fetched by CMake at configure time. | Upstream: https://github.com/catchorg/Catch2 |
| NDI SDK headers | 6.x | NewTek proprietary licence | Compile-time declarations only. The import library is **not** linked: the NDI runtime DLL is loaded dynamically at run time when present (see `docs/licensing.md`). SDK is installed externally. | Provided with the NDI SDK install; not redistributed here |
| Waves SoundGrid ASIO driver | - | Waves EULA | External system prerequisite. Nothing is copied into this repository or the installer. | Installed with the Waves driver |
| OpenAI Realtime Translation API | - | OpenAI terms (network service) | A remote service called over TLS. No OpenAI SDK or library is bundled. | https://openai.com/policies |

The complete corresponding source of this program (including the vendored trees
above and the build scripts) is the repository this file lives in; the licence
notice is visible in the application (Settings, About section).
