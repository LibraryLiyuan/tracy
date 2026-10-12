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
bool CaptureConnectionTimedOut(bool receivedData, bool latestStatusHasData, uint64_t idleMs);
// A CPU set as Windows reports it.
struct CpuSet
{
    uint32_t id = 0;
    uint8_t efficiencyClass = 0;
    uint8_t logical = 0;
};
// The efficiency cores of a hybrid CPU: the sets of the lowest efficiency class
// when there is more than one class, otherwise none.
std::vector<CpuSet> EfficiencyCpuSets(const std::vector<CpuSet> &all);
std::vector<CpuSet> SystemCpuSets();
// Keeps a process's threads on the given CPU sets; an empty list lifts the
// restriction. False when Windows has no CPU sets or refuses.
bool SetProcessCpuSets(HANDLE process, const std::vector<CpuSet> &sets);
void PreferBundledTools(struct Settings &settings, const fs::path &directory);
fs::path NewTaskDirectory(const fs::path &root, const std::string &name);
fs::path ExecutableDirectory();
fs::path SettingsDirectory();
void OpenPath(const fs::path &path);
std::string ChoosePath(HWND owner, bool folder, const wchar_t *filter = L"All files\0*.*\0");
struct Settings
{
    std::string player, workingDirectory, arguments, ini, output, toolDirectory, viewer, name, scene, note;
    bool admin = true, existing = false, ready = false, timed = false, sound = true, customTools = false;
    int seconds = 60, port = 8086, drainSeconds = 30, hotkey = VK_F9;
    // Recordings at least this many minutes long are published as their
    // journal and event store instead of a .tracy file; 0 always does.
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
// The INI a running game or editor loaded. The engine reads it once, at
// startup, and publishes what it read in the process environment
// (JN_TRACY_INI_*), so a running process never switches to another INI.
struct LoadedIni
{
    // "reported" when read from the process; "inferred" from its command line
    // by the engine's lookup when its memory cannot be read (it runs elevated)
    // or it has not reported yet; otherwise "unknown".
    std::string source = "unknown", path, sha256, status, preset;
    Json ToJson() const;
};
LoadedIni QueryLoadedIni(const ProcessIdentity &target);
enum class IniMatch
{
    Same,
    Different,
    Unknown
};
// Same when the file holds what the process loaded, whatever its name.
IniMatch CompareIni(const LoadedIni &loaded, const std::string &ini);
// The value of `name` in an environment block (NAME=value\0 ... \0\0).
std::wstring EnvironmentValue(const std::wstring &block, const std::wstring &name);
// A command line's arguments after the program without -jn-tracy-config, and
// that option's value.
std::pair<std::wstring, std::wstring> SplitIniArgument(const std::wstring &commandLine);
// What a running process was started with, to start it again.
struct LaunchInfo
{
    std::string exe, arguments, directory;
};
LaunchInfo ReadLaunchInfo(const ProcessIdentity &target);
// Asks the process to close, as its window's close button does, and ends it
// when it still runs after `graceMs`. False while it runs: Windows refuses
// when it runs elevated and this program does not.
bool CloseProcess(const ProcessIdentity &target, DWORD graceMs);
// Whether the process runs elevated; one this program may not query runs
// above it.
bool RunsElevated(const ProcessIdentity &target);
bool IsEditor(const std::string &exe);
struct Preflight
{
    std::vector<std::string> errors;
    ProcessIdentity target;
    bool alive = false;
    LoadedIni loaded;
    IniMatch match = IniMatch::Unknown;
    // The file the game loaded still holds what it read, so it can be selected.
    bool loadedFileUnchanged = false;
};
Preflight RunPreflight(const Settings &settings, ProcessIdentity target);
std::vector<std::string> PreflightErrors(const Settings &settings, ProcessIdentity target);
} // namespace capturegui
