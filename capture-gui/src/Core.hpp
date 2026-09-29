#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <windows.h>
namespace capturegui
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
std::wstring Wide(const std::string &text);
std::string Utf8(const std::wstring &text);
std::string PathText(const fs::path &path);
std::wstring Quote(const std::wstring &text);
std::string ErrorText(DWORD code = GetLastError());
std::string Timestamp();
std::string Sha256(const fs::path &path);
void AtomicJson(const fs::path &path, const Json &data);
Json ReadJson(const fs::path &path);
bool ValidName(const std::string &name);
bool CanPublish(bool validated, bool identityMatches, bool recoverable);
fs::path NewTaskDirectory(const fs::path &root, const std::string &name);
fs::path ExecutableDirectory();
fs::path SettingsDirectory();
void OpenPath(const fs::path &path);
std::string ChoosePath(HWND owner, bool folder, const wchar_t *filter = L"All files\0*.*\0");
struct Settings
{
    std::string player, workingDirectory, arguments, ini, output, toolDirectory, viewer, name, scene, note;
    bool admin = true, existing = false, ready = false, timed = false, sound = true;
    int seconds = 60, port = 8086, drainSeconds = 30, hotkey = VK_F9;
    unsigned hotkeyModifiers = MOD_CONTROL | MOD_SHIFT;
    uint64_t warnGiB = 50, stopGiB = 20, memoryMiB = 8192;
    DWORD targetPid = 0;
    Json ToJson() const;
    static Settings FromJson(const Json &data);
};
struct ProcessIdentity
{
    DWORD pid = 0;
    uint64_t created = 0;
    Json ToJson() const;
    static ProcessIdentity FromJson(const Json &);
};
ProcessIdentity Identify(HANDLE process);
bool Alive(const ProcessIdentity &identity);
void TerminateOwned(const ProcessIdentity &identity);
std::vector<std::pair<ProcessIdentity, std::string>> LocalProcesses();
std::vector<std::string> IniFiles(const Settings &settings);
std::vector<std::string> PreflightErrors(const Settings &settings, ProcessIdentity target);
} // namespace capturegui
