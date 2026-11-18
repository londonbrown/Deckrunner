#include "Data.hpp"

#include <windows.h>

namespace data
{
namespace
{
std::filesystem::path Setting(const wchar_t* key, const std::filesystem::path& fallback)
{
    wchar_t buf[1024]{};
    GetPrivateProfileStringW(L"data", key, L"", buf, 1024, Ini().c_str());
    return buf[0] ? std::filesystem::path(buf) : fallback;
}

std::filesystem::path FromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), wchar_t{});
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return std::filesystem::path(w);
}
} // namespace

std::filesystem::path PluginDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PluginDir), &self);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path Ini() { return PluginDir() / L"deckrunner.ini"; }
std::filesystem::path Root() { return Setting(L"dir", PluginDir() / L"data"); }
std::filesystem::path SkateAssets() { return Setting(L"skate_assets", Root() / L"skate" / L"assets"); }
std::filesystem::path SkateAudio() { return Setting(L"skate_audio", Root() / L"skate" / L"audio"); }

std::filesystem::path Rig(bool female)
{
    return Setting(L"rigs", Root() / L"rigs") / (female ? L"player_woman_skeleton.rig.json" : L"player_man_skeleton.rig.json");
}

std::string Utf8(const std::filesystem::path& p)
{
    const std::wstring w = p.wstring();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), char{});
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Missing(const std::string& rigUtf8)
{
    std::string out;
    std::error_code ec;
    for (const auto& f : {SkateAssets() / L"private" / L"game.json", SkateAssets() / L"private" / L"skater.glb", FromUtf8(rigUtf8)})
        if (!std::filesystem::is_regular_file(f, ec)) out += (out.empty() ? "" : ", ") + Utf8(f);
    return out;
}
} // namespace data
