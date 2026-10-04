// getenv is the SDK's own documented runtime-discovery idiom (Examples/C++/
// NDIlib_DynamicLoad reads NDILIB_REDIST_FOLDER exactly like this); the
// deprecation opt-out matches Main.cpp and the other probes.
#define _CRT_SECURE_NO_WARNINGS 1

#include "NDI/Real/NdiRuntime.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#endif

namespace liveai {
namespace ndi {
namespace real {

#ifdef _WIN32
namespace {

const char* runtimePathVariable()
{
    return std::getenv(NDILIB_REDIST_FOLDER);   // NDI_RUNTIME_DIR_V6
}

} // namespace

NdiRuntime::NdiRuntime()
{
    // Step 1: find the DLL the way the SDK example does - the runtime installer
    // publishes its folder in NDI_RUNTIME_DIR_V6; if that is missing or wrong,
    // fall back to the system search path (the installer adds it there too on
    // most setups).
    std::string path;
    if (const char* dir = runtimePathVariable(); dir != nullptr && dir[0] != '\0')
    {
        path = dir;
        path += "\\" NDILIB_LIBRARY_NAME;
    }

    HMODULE module = nullptr;
    if (!path.empty())
        module = LoadLibraryA(path.c_str());
    if (module == nullptr)
        module = LoadLibraryA(NDILIB_LIBRARY_NAME);

    if (module == nullptr)
    {
        error_ = std::string("NDI runtime (") + NDILIB_LIBRARY_NAME +
                 ") was not found: install the NDI 6 runtime, or set " NDILIB_REDIST_FOLDER
                 " to its folder";
        return;
    }

    // Step 2: the single entry point of the dynamic-load contract.
    using LoadFn = const NDIlib_v6* (*)();
    const auto load = reinterpret_cast<LoadFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "NDIlib_v6_load")));

    if (load == nullptr)
    {
        FreeLibrary(module);
        error_ = std::string(NDILIB_LIBRARY_NAME) +
                 " loaded but has no NDIlib_v6_load entry - the installed runtime "
                 "predates the v6 dynamic-load API; NDI 6 runtime is required";
        return;
    }

    lib_ = load();
    module_ = module;

    if (lib_ == nullptr)
    {
        error_ = "NDIlib_v6_load returned no function table";
        return;
    }

    // Step 3: the initialize call the SDK's dynamic-load example makes. It is
    // marked deprecated in 6.3 headers (newer runtimes self-initialize) but the
    // example still calls it and a refusal still means "cannot run NDI here"
    // (CPU support), so we call it and honour a false - documented in
    // docs/ndi-protocol.md, not guessed.
    if (!lib_->initialize())
    {
        lib_ = nullptr;
        error_ = "NDI initialize refused by the runtime (unsupported CPU or runtime problem)";
    }
}

#else // !_WIN32

NdiRuntime::NdiRuntime()
{
    error_ = "this build's NDI support targets Windows (AGENTS.md 4: the product is a Windows app)";
}

#endif // _WIN32

const NdiRuntime& NdiRuntime::instance()
{
    static const NdiRuntime runtime;   // thread-safe magic static; loads at most once
    return runtime;
}

std::string NdiRuntime::versionText() const
{
    if (lib_ == nullptr)
        return {};

    const char* text = lib_->version();
    return text != nullptr ? std::string(text) : std::string();
}

} // namespace real
} // namespace ndi
} // namespace liveai
