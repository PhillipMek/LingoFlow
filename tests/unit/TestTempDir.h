#pragma once
//
// Test-only helpers: a unique scratch directory per test case, removed at scope
// exit. Tests must never touch the real %APPDATA% configuration.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

namespace livetest {

class TempDirectory
{
public:
    explicit TempDirectory(std::string_view tag = "cfg")
    {
        static std::mt19937_64 engine{ std::random_device{}() };
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string name = std::string("liveai-") + std::string(tag) + "-"
                               + std::to_string(engine()) + "-" + std::to_string(stamp);

        path_ = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);   // best effort: never fail a test over cleanup
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

    std::filesystem::path file(std::string_view name) const { return path_ / std::string(name); }

private:
    std::filesystem::path path_;
};

inline std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
}

inline void writeFile(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace livetest
