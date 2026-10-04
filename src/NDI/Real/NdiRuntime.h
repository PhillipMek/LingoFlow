#pragma once
//
// NdiRuntime - the ONLY place in this product that touches the NDI SDK headers.
//
// License rule (docs/licensing.md, owner decision of 2026-10-01): the SDK's
// import library `Processing.NDI.Lib.x64.lib` is never linked into the product.
// The runtime DLL is loaded dynamically at run time via the SDK's official
// dynamic-load mechanism (LoadLibrary + GetProcAddress on "NDIlib_v6_load" ->
// the NDIlib_v6 function table) - the exact pattern shipped in the SDK's own
// Examples/C++/NDIlib_DynamicLoad, reproduced from the installed 6.3.2.0 headers
// (all facts dated in docs/ndi-protocol.md).
//
// Consequences accepted here:
//   * machines without the NDI runtime get a clear, testable "unavailable"
//     answer - the product still starts, only subtitles are off (AGENTS.md 12);
//   * the function table is read-only after loading; one process-wide instance
//     (magic statics make it thread-safe);
//   * the module is never freed: the v6 dynamic-load table exposes no cleanup
//     entry (checked against Processing.NDI.DynamicLoad.h) and unloading a live
//     multicast library at exit is a way to crash a shutdown, so keeping it
//     loaded for the process lifetime is the documented, deliberate choice.
//
// No header outside src/NDI/Real/ includes this file - SDK types must not leak
// past the module boundary (INdiOutput.h keeps its promise).

#include <string>

#include "Processing.NDI.Lib.h"

namespace liveai {
namespace ndi {
namespace real {

class NdiRuntime
{
public:
    /// Loads on first use and never again; always safe to call from any thread.
    static const NdiRuntime& instance();

    bool available() const noexcept { return lib_ != nullptr; }

    /// Valid only when available(): the caller checks first - no exceptions,
    /// no silent fallback, no fake table.
    const NDIlib_v6& api() const noexcept { return *lib_; }

    /// What failed in words an operator can act on ("install the NDI runtime"),
    /// empty when the load succeeded.
    const std::string& loadError() const noexcept { return error_; }

    /// The runtime's own version string (NDIlib_version), or empty.
    std::string versionText() const;

private:
    NdiRuntime();

    const NDIlib_v6* lib_ = nullptr;
    std::string error_;
    void* module_ = nullptr;   // HMODULE on Windows; kept loaded (see header)
};

} // namespace real
} // namespace ndi
} // namespace liveai
