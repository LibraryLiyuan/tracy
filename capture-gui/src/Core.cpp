#include "Core.hpp"
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <commdlg.h>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <set>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sstream>
#include <tlhelp32.h>
#include <winternl.h>
namespace capturegui
{
std::wstring Wide(const std::string &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8");
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n);
    return r;
}
std::string Utf8(const std::wstring &s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::string PathText(const fs::path &p)
{
    return Utf8(p.wstring());
}
std::string ErrorText(DWORD code)
{
    wchar_t *message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (wchar_t *)&message, 0, nullptr);
    std::string result = message ? Utf8(message) : std::to_string(code);
    if (message)
        LocalFree(message);
    return result;
}
std::wstring Quote(const std::wstring &text)
{
    std::wstring r = L"\"";
    unsigned slashes = 0;
    for (auto c : text)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        if (c == L'\"')
            r.append(slashes * 2 + 1, L'\\');
        else
            r.append(slashes, L'\\');
        slashes = 0;
        r += c;
    }
    r.append(slashes * 2, L'\\');
    return r + L'\"';
}
std::string Timestamp()
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char b[40];
    snprintf(b, sizeof(b), "%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    return b;
}
Json ReadJson(const fs::path &p)
{
    HANDLE file = CreateFileW(p.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot read " + PathText(p));
    struct CloseFile
    {
        HANDLE value;
        ~CloseFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } close{file};
    std::string contents;
    std::array<char, 4096> bytes;
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr))
            throw std::runtime_error("Cannot read " + PathText(p) + ": " + ErrorText());
        if (!read) break;
        contents.append(bytes.data(), read);
    }
    CloseHandle(close.value);
    close.value = INVALID_HANDLE_VALUE;
    return Json::parse(contents);
}
void AtomicJson(const fs::path &p, const Json &data)
{
    fs::create_directories(p.parent_path());
    auto temp = p;
    temp += L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f << data.dump(2);
        f.flush();
        if (!f)
            throw std::runtime_error("Cannot write " + PathText(temp));
    }
    DWORD error = ERROR_SUCCESS;
    for (unsigned attempt = 0; attempt < 20; ++attempt)
    {
        if (MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
        Sleep(5);
    }
    throw std::runtime_error("Cannot publish " + PathText(p) + ": " + ErrorText(error));
}
std::string Sha256(const fs::path &path)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider unavailable");
    struct Guard
    {
        BCRYPT_ALG_HANDLE &a;
        BCRYPT_HASH_HANDLE &h;
        ~Guard()
        {
            if (h)
                BCryptDestroyHash(h);
            if (a)
                BCryptCloseAlgorithmProvider(a, 0);
        }
    } guard{alg, hash};
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("SHA256 initialization failed");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot hash " + PathText(path));
    std::array<char, 65536> bytes;
    while (input)
    {
        input.read(bytes.data(), bytes.size());
        if (input.gcount() && BCryptHashData(hash, (PUCHAR)bytes.data(), ULONG(input.gcount()), 0) < 0)
            throw std::runtime_error("SHA256 read failed");
    }
    if (!input.eof())
        throw std::runtime_error("SHA256 input read failed");
    unsigned char digest[32];
    if (BCryptFinishHash(hash, digest, 32, 0) < 0)
        throw std::runtime_error("SHA256 finalization failed");
    std::ostringstream out;
    for (auto b : digest)
        out << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
    return out.str();
}
bool ValidName(const std::string &n)
{
    if (n.empty() || n.size() > 180 || n.back() == '.' || n.back() == ' ' || n == "." || n == "..")
        return false;
    for (unsigned char c : n)
        if (c < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos)
            return false;
    std::string base = n.substr(0, n.find('.'));
    for (auto &c : base)
        c = char(toupper((unsigned char)c));
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL")
        return false;
    if (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' &&
        base[3] <= '9')
        return false;
    return true;
}
bool CaptureConnectionTimedOut(bool receivedData, bool latestStatusHasData, uint64_t idleMs)
{
    return !receivedData && !latestStatusHasData && idleMs > 60000;
}
std::vector<CpuSet> EfficiencyCpuSets(const std::vector<CpuSet> &all)
{
    if (all.empty())
        return {};
    uint8_t lowest = 255, highest = 0;
    for (auto &set : all)
    {
        lowest = (std::min)(lowest, set.efficiencyClass);
        highest = (std::max)(highest, set.efficiencyClass);
    }
    std::vector<CpuSet> result;
    if (lowest < highest)
        for (auto &set : all)
            if (set.efficiencyClass == lowest)
                result.push_back(set);
    return result;
}
std::vector<CpuSet> SystemCpuSets()
{
    using GetInfo = BOOL(WINAPI *)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    const auto getInfo = reinterpret_cast<GetInfo>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemCpuSetInformation"));
    if (!getInfo)
        return {};
    ULONG size = 0;
    getInfo(nullptr, 0, &size, GetCurrentProcess(), 0);
    if (!size)
        return {};
    std::vector<uint8_t> buffer(size);
    if (!getInfo(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), size, &size, GetCurrentProcess(), 0))
        return {};
    std::vector<CpuSet> result;
    for (ULONG offset = 0; offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= size;)
    {
        const auto *info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION *>(buffer.data() + offset);
        if (!info->Size)
            break;
        if (info->Type == CpuSetInformation)
            result.push_back({info->CpuSet.Id, info->CpuSet.EfficiencyClass, info->CpuSet.LogicalProcessorIndex});
        offset += info->Size;
    }
    return result;
}
bool SetProcessCpuSets(HANDLE process, const std::vector<CpuSet> &sets)
{
    using SetSets = BOOL(WINAPI *)(HANDLE, const ULONG *, ULONG);
    const auto setSets = reinterpret_cast<SetSets>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessDefaultCpuSets"));
    if (!setSets || !process)
        return false;
    std::vector<ULONG> ids;
    for (auto &set : sets)
        ids.push_back(set.id);
    return setSets(process, ids.empty() ? nullptr : ids.data(), ULONG(ids.size())) != FALSE;
}
void PreferBundledTools(Settings &settings, const fs::path &directory)
{
    if (settings.customTools) return;
    std::error_code error;
    for (auto name : {L"tracy-capture.exe", L"tracy-stream-export.exe", L"tracy-query.exe"})
        if (!fs::is_regular_file(directory / name, error)) return;
    settings.toolDirectory = PathText(fs::absolute(directory));
}
fs::path NewTaskDirectory(const fs::path &root, const std::string &name)
{
    if (!ValidName(name))
        throw std::runtime_error("录制名称包含无效字符或保留名称");
    fs::create_directories(root);
    for (int i = 0; i < 10000; ++i)
    {
        auto p = root / Wide(name + (i ? "_" + std::to_string(i) : ""));
        std::error_code e;
        if (fs::create_directory(p, e))
            return fs::absolute(p);
        if (e)
            throw std::runtime_error(e.message());
    }
    throw std::runtime_error("Too many duplicate names");
}
fs::path ExecutableDirectory()
{
    std::wstring b(32768, 0);
    auto n = GetModuleFileNameW(nullptr, b.data(), DWORD(b.size()));
    b.resize(n);
    return fs::path(b).parent_path();
}
fs::path SettingsDirectory()
{
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
        throw std::runtime_error("LocalAppData unavailable");
    fs::path root = fs::path(p) / L"JNTracyCapture";
    CoTaskMemFree(p);
    return root;
}
void OpenPath(const fs::path &p)
{
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        throw std::runtime_error("Cannot open " + PathText(p));
}
std::string ChoosePath(HWND owner, bool folder, const wchar_t *filter)
{
    if (folder)
    {
        IFileOpenDialog *d = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
            return {};
        DWORD opts = 0;
        d->GetOptions(&opts);
        d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        std::string r;
        if (SUCCEEDED(d->Show(owner)))
        {
            IShellItem *item = nullptr;
            if (SUCCEEDED(d->GetResult(&item)))
            {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
                {
                    r = Utf8(p);
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        d->Release();
        return r;
    }
    wchar_t path[32768] = {};
    OPENFILENAMEW o{sizeof(o)};
    o.hwndOwner = owner;
    o.lpstrFilter = filter;
    o.lpstrFile = path;
    o.nMaxFile = 32768;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o) ? Utf8(path) : std::string();
}
Json Settings::ToJson() const
{
    return {{"schema", 1},
            {"player", player},
            {"working_directory", workingDirectory},
            {"arguments", arguments},
            {"ini", ini},
            {"output", output},
            {"tools", toolDirectory},
            {"tools_custom", customTools},
            {"viewer", viewer},
            {"name", name},
            {"scene", scene},
            {"note", note},
            {"admin", admin},
            {"existing", existing},
            {"timed", timed},
            {"seconds", seconds},
            {"port", port},
            {"drain_seconds", drainSeconds},
            {"warn_gib", warnGiB},
            {"stop_gib", stopGiB},
            {"memory_mib", memoryMiB},
            {"hotkey", hotkey},
            {"hotkey_modifiers", hotkeyModifiers},
            {"sound", sound},
            {"target_pid", targetPid}};
}
Settings Settings::FromJson(const Json &j)
{
    Settings s;
    s.player = j.value("player", "");
    s.workingDirectory = j.value("working_directory", "");
    s.arguments = j.value("arguments", "");
    s.ini = j.value("ini", "");
    s.output = j.value("output", PathText(SettingsDirectory() / L"Captures"));
    s.toolDirectory = j.value("tools", PathText(ExecutableDirectory()));
    s.customTools = j.value("tools_custom", false);
    s.viewer = j.value("viewer", "");
    s.name = j.value("name", "Player_" + Timestamp());
    s.scene = j.value("scene", "");
    s.note = j.value("note", "");
    s.admin = j.value("admin", true);
    s.existing = j.value("existing", false);
    s.timed = j.value("timed", false);
    s.seconds = j.value("seconds", 60);
    s.port = j.value("port", 8086);
    s.drainSeconds = j.value("drain_seconds", 30);
    s.warnGiB = j.value("warn_gib", 50ull);
    s.stopGiB = j.value("stop_gib", 20ull);
    s.memoryMiB = j.value("memory_mib", 8192ull);
    s.hotkey = j.value("hotkey", VK_F9);
    s.hotkeyModifiers = j.value("hotkey_modifiers", unsigned(MOD_CONTROL | MOD_SHIFT));
    s.sound = j.value("sound", true);
    return s;
}
Json ProcessIdentity::ToJson() const
{
    return {{"pid", pid}, {"created", created}};
}
ProcessIdentity ProcessIdentity::FromJson(const Json &j)
{
    return {j.value("pid", 0ul), j.value("created", 0ull)};
}
ProcessIdentity Identify(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(h, &c, &e, &k, &u))
        throw std::runtime_error(ErrorText());
    return {GetProcessId(h), (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime};
}
bool Alive(const ProcessIdentity &i)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, i.pid);
    if (!h)
        return false;
    bool alive = false;
    try
    {
        alive = Identify(h).created == i.created && WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    }
    catch (...)
    {
    }
    CloseHandle(h);
    return alive;
}
void TerminateOwned(const ProcessIdentity &i)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, i.pid);
    if (!h)
        return;
    try
    {
        if (Identify(h).created == i.created)
            TerminateProcess(h, 130);
    }
    catch (...)
    {
    }
    CloseHandle(h);
}
std::vector<std::pair<ProcessIdentity, std::string>> LocalProcesses()
{
    std::vector<std::pair<ProcessIdentity, std::string>> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W e{sizeof(e)};
    if (Process32FirstW(snap, &e))
        do
        {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
            if (h)
            {
                wchar_t p[32768];
                DWORD size = 32768;
                if (QueryFullProcessImageNameW(h, 0, p, &size))
                {
                    try
                    {
                        result.emplace_back(Identify(h), Utf8(p));
                    }
                    catch (...)
                    {
                    }
                }
                CloseHandle(h);
            }
        } while (Process32NextW(snap, &e));
    CloseHandle(snap);
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    return result;
}
std::vector<std::string> IniFiles(const Settings &s)
{
    std::set<std::string> files;
    std::vector<fs::path> dirs = {fs::path(Wide(s.player)).parent_path(), ExecutableDirectory() / L"presets"};
    if (!s.ini.empty())
        dirs.push_back(fs::path(Wide(s.ini)).parent_path());
    for (auto &dir : dirs)
    {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec))
            continue;
        for (auto &item : fs::directory_iterator(dir, ec))
            if (item.is_regular_file(ec) && item.path().extension() == L".ini" &&
                item.path().filename().wstring().starts_with(L"JNTracy"))
                files.insert(PathText(item.path()));
    }
    return {files.begin(), files.end()};
}
namespace
{
using QueryProcess = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
QueryProcess NtQuery()
{
    static const auto query =
        reinterpret_cast<QueryProcess>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    return query;
}
// The x64 layout of the process parameters read here, unchanged since
// Windows Vista: the PEB points to them at 0x20.
struct RemoteString
{
    USHORT length, maximum;
    ULONG padding;
    ULONG64 buffer;
};
struct RemoteParameters
{
    BYTE header[0x38];
    RemoteString currentDirectory;
    ULONG64 currentDirectoryHandle;
    RemoteString dllPath, imagePath, commandLine;
    ULONG64 environment;
};
static_assert(offsetof(RemoteParameters, currentDirectory) == 0x38 && offsetof(RemoteParameters, commandLine) == 0x70 &&
              offsetof(RemoteParameters, environment) == 0x80);
constexpr ULONG64 PebParameters = 0x20, EnvironmentSize = 0x3F0, EnvironmentLimit = 4ull << 20;
bool ReadRemote(HANDLE process, ULONG64 address, void *out, size_t size)
{
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out, size, &read) && read == size;
}
std::wstring ReadRemoteString(HANDLE process, const RemoteString &s)
{
    if (!s.buffer || !s.length || s.length % 2)
        return {};
    std::wstring text(s.length / 2, L'\0');
    return ReadRemote(process, s.buffer, text.data(), s.length) ? text : std::wstring();
}
// A process's command line, current directory and environment, from its
// memory; empty when it cannot be read.
struct RemoteProcess
{
    std::wstring commandLine, directory, environment;
};
RemoteProcess ReadRemoteProcess(HANDLE process)
{
    RemoteProcess result;
    BOOL wow64 = FALSE;
    PROCESS_BASIC_INFORMATION basic{};
    ULONG64 parameters = 0;
    RemoteParameters p{};
    const auto query = NtQuery();
    if (!query || !IsWow64Process(process, &wow64) || wow64 ||
        query(process, 0, &basic, sizeof(basic), nullptr) < 0 || !basic.PebBaseAddress ||
        !ReadRemote(process, ULONG64(basic.PebBaseAddress) + PebParameters, &parameters, sizeof(parameters)) ||
        !parameters || !ReadRemote(process, parameters, &p, sizeof(p)))
        return result;
    result.commandLine = ReadRemoteString(process, p.commandLine);
    result.directory = ReadRemoteString(process, p.currentDirectory);
    if (!p.environment)
        return result;
    // The block is read up to its recorded size, or else to the end of its
    // memory region; it ends at the first empty entry.
    ULONG64 size = 0;
    if (!ReadRemote(process, parameters + EnvironmentSize, &size, sizeof(size)) || !size || size > EnvironmentLimit)
    {
        MEMORY_BASIC_INFORMATION region{};
        size = 0;
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(p.environment), &region, sizeof(region)))
            size = (std::min)(ULONG64(region.RegionSize) - (p.environment - ULONG64(region.BaseAddress)), EnvironmentLimit);
    }
    std::wstring block(size_t(size / 2), L'\0');
    if (!block.empty() && ReadRemote(process, p.environment, block.data(), block.size() * 2))
        result.environment = std::move(block);
    return result;
}
// The command line of a process this program may not read the memory of.
std::wstring QueryCommandLine(HANDLE process)
{
    const auto query = NtQuery();
    ULONG size = 0;
    if (!query)
        return {};
    query(process, 60 /* ProcessCommandLineInformation */, nullptr, 0, &size);
    if (size < sizeof(UNICODE_STRING) || size > (1u << 20))
        return {};
    std::vector<BYTE> bytes(size);
    if (query(process, 60, bytes.data(), size, &size) < 0)
        return {};
    const auto *text = reinterpret_cast<const UNICODE_STRING *>(bytes.data());
    return text->Buffer ? std::wstring(text->Buffer, text->Length / 2) : std::wstring();
}
std::wstring ImagePath(HANDLE process)
{
    std::wstring path(32768, L'\0');
    DWORD size = DWORD(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size))
        return {};
    path.resize(size);
    return path;
}
std::string Lower(std::string text)
{
    for (auto &c : text)
        c = char(tolower((unsigned char)c));
    return text;
}
// Opens the process for reading its memory where allowed, else for querying.
HANDLE OpenForQuery(const ProcessIdentity &target, bool &memory)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, target.pid);
    memory = h != nullptr;
    if (!h)
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
    if (h)
    {
        try
        {
            if (Identify(h).created == target.created)
                return h;
        }
        catch (...)
        {
        }
        CloseHandle(h);
    }
    return nullptr;
}
std::wstring QuoteArgument(const std::wstring &argument)
{
    return argument.empty() || argument.find_first_of(L" \t\"") != std::wstring::npos ? Quote(argument) : argument;
}
} // namespace
Json LoadedIni::ToJson() const
{
    return {{"source", source}, {"path", path}, {"sha256", sha256}, {"status", status}, {"preset", preset}};
}
std::wstring EnvironmentValue(const std::wstring &block, const std::wstring &name)
{
    for (size_t start = 0; start < block.size();)
    {
        const auto end = block.find(L'\0', start);
        const std::wstring_view entry(block.data() + start, (end == std::wstring::npos ? block.size() : end) - start);
        if (entry.empty())
            break;
        // Names are case-insensitive; an entry such as "=C:=C:\" has an empty name.
        if (entry.size() > name.size() && entry[name.size()] == L'=' &&
            CompareStringOrdinal(entry.data(), int(name.size()), name.data(), int(name.size()), TRUE) == CSTR_EQUAL)
            return std::wstring(entry.substr(name.size() + 1));
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    return {};
}
std::pair<std::wstring, std::wstring> SplitIniArgument(const std::wstring &commandLine)
{
    std::pair<std::wstring, std::wstring> result;
    int count = 0;
    if (commandLine.empty())
        return result;
    auto **argv = CommandLineToArgvW(commandLine.c_str(), &count);
    if (!argv)
        return result;
    for (int i = 1; i < count; ++i)
    {
        const std::wstring argument = argv[i];
        if (_wcsicmp(argument.c_str(), L"-jn-tracy-config") == 0 || _wcsicmp(argument.c_str(), L"--jn-tracy-config") == 0)
        {
            if (i + 1 < count)
                result.second = argv[++i];
            continue;
        }
        if (!result.first.empty())
            result.first += L' ';
        result.first += QuoteArgument(argument);
    }
    LocalFree(argv);
    return result;
}
LoadedIni QueryLoadedIni(const ProcessIdentity &target)
{
    LoadedIni loaded;
    bool memory = false;
    HANDLE h = target.pid ? OpenForQuery(target, memory) : nullptr;
    if (!h)
        return loaded;
    struct Close
    {
        HANDLE value;
        ~Close() { CloseHandle(value); }
    } close{h};
    const auto remote = memory ? ReadRemoteProcess(h) : RemoteProcess{};
    if (const auto status = EnvironmentValue(remote.environment, L"JN_TRACY_INI_STATUS"); !status.empty())
    {
        loaded.source = "reported";
        loaded.status = Utf8(status);
        loaded.path = Utf8(EnvironmentValue(remote.environment, L"JN_TRACY_INI_PATH"));
        loaded.sha256 = Lower(Utf8(EnvironmentValue(remote.environment, L"JN_TRACY_INI_SHA256")));
        loaded.preset = Utf8(EnvironmentValue(remote.environment, L"JN_TRACY_INI_PRESET"));
        return loaded;
    }
    // The engine's lookup: -jn-tracy-config, else JNTracy.ini beside
    // JNTracyClient.dll, which sits beside the program; a program without it
    // is no JN Tracy game.
    const auto image = ImagePath(h);
    const auto commandLine = remote.commandLine.empty() ? QueryCommandLine(h) : remote.commandLine;
    std::error_code e;
    if (image.empty() || !fs::is_regular_file(fs::path(image).parent_path() / L"JNTracyClient.dll", e))
        return loaded;
    const auto option = SplitIniArgument(commandLine).second;
    fs::path path = option.empty() ? fs::path(image).parent_path() / L"JNTracy.ini" : fs::path(option);
    if (path.is_relative())
        path = (remote.directory.empty() ? fs::path(image).parent_path() : fs::path(remote.directory)) / path;
    loaded.source = "inferred";
    loaded.path = PathText(path.lexically_normal());
    if (fs::is_regular_file(path, e))
    {
        loaded.status = "inferred";
        loaded.sha256 = Sha256(path);
    }
    else
        loaded.status = "not_found";
    return loaded;
}
IniMatch CompareIni(const LoadedIni &loaded, const std::string &ini)
{
    std::error_code e;
    if (loaded.source == "unknown" || ini.empty() || !fs::is_regular_file(Wide(ini), e))
        return IniMatch::Unknown;
    // Without a file the game runs the compiled defaults.
    if (loaded.sha256.empty())
        return IniMatch::Different;
    return Lower(Sha256(Wide(ini))) == Lower(loaded.sha256) ? IniMatch::Same : IniMatch::Different;
}
LaunchInfo ReadLaunchInfo(const ProcessIdentity &target)
{
    LaunchInfo info;
    bool memory = false;
    HANDLE h = OpenForQuery(target, memory);
    if (!h)
        return info;
    const auto remote = memory ? ReadRemoteProcess(h) : RemoteProcess{};
    const auto commandLine = remote.commandLine.empty() ? QueryCommandLine(h) : remote.commandLine;
    info.exe = Utf8(ImagePath(h));
    info.arguments = Utf8(SplitIniArgument(commandLine).first);
    info.directory = Utf8(remote.directory);
    CloseHandle(h);
    return info;
}
bool CloseProcess(const ProcessIdentity &target, DWORD graceMs)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
    if (!h)
        return GetLastError() != ERROR_ACCESS_DENIED || !Alive(target);
    struct Close
    {
        HANDLE value;
        ~Close() { CloseHandle(value); }
    } close{h};
    try
    {
        if (Identify(h).created != target.created)
            return true;
    }
    catch (...)
    {
        return false;
    }
    if (WaitForSingleObject(h, 0) != WAIT_TIMEOUT)
        return true;
    // Its visible top-level windows get the close button's message.
    EnumWindows(
        [](HWND window, LPARAM pid) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == DWORD(pid) && IsWindowVisible(window) && !GetWindow(window, GW_OWNER))
                PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        },
        LPARAM(target.pid));
    if (WaitForSingleObject(h, graceMs) == WAIT_TIMEOUT)
    {
        if (!TerminateProcess(h, 1))
            return false;
        WaitForSingleObject(h, 10000);
    }
    return WaitForSingleObject(h, 0) != WAIT_TIMEOUT;
}
bool RunsElevated(const ProcessIdentity &target)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED;
    bool elevated = true;
    HANDLE token = nullptr;
    if (OpenProcessToken(h, TOKEN_QUERY, &token))
    {
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
            elevated = elevation.TokenIsElevated != 0;
        CloseHandle(token);
    }
    CloseHandle(h);
    return elevated;
}
bool IsEditor(const std::string &exe)
{
    return _wcsicmp(fs::path(Wide(exe)).filename().c_str(), L"Unity.exe") == 0;
}
Preflight RunPreflight(const Settings &s, ProcessIdentity target)
{
    Preflight result;
    auto &errors = result.errors;
    result.target = target;
    result.alive = target.pid && Alive(target);
    if (!result.alive)
        errors.push_back("请选择存活的 Player / Editor");
    std::error_code ec;
    if (!fs::is_regular_file(Wide(s.ini), ec))
        errors.push_back("请选择存在的 INI 文件");
    else if (result.alive)
    {
        result.loaded = QueryLoadedIni(target);
        result.match = CompareIni(result.loaded, s.ini);
        if (result.match == IniMatch::Different)
        {
            errors.push_back("所选 INI 未生效：游戏启动时读取的是 " +
                             PathText(fs::path(Wide(result.loaded.path)).filename()) + "，需要用所选配置重启游戏");
            const fs::path loaded = Wide(result.loaded.path);
            result.loadedFileUnchanged = !result.loaded.sha256.empty() && fs::is_regular_file(loaded, ec) &&
                                         Sha256(loaded) == result.loaded.sha256;
        }
    }
    for (auto name : {L"tracy-capture.exe", L"tracy-stream-export.exe", L"tracy-query.exe"})
        if (!fs::is_regular_file(fs::path(Wide(s.toolDirectory)) / name, ec))
            errors.push_back("缺少工具：" + Utf8(name));
    if (!ValidName(s.name))
        errors.push_back("录制名称无效");
    if (s.output.empty())
        errors.push_back("请选择保存位置");
    if (s.port < 1 || s.port > 65535)
        errors.push_back("端口应为 1–65535");
    if (s.timed && s.seconds < 1)
        errors.push_back("定时时长必须大于零");
    if (s.stopGiB >= s.warnGiB || !s.memoryMiB || s.drainSeconds < 0 || s.drainSeconds > 3600)
        errors.push_back("资源保护或排空参数无效");
    return result;
}
std::vector<std::string> PreflightErrors(const Settings &s, ProcessIdentity target)
{
    return RunPreflight(s, target).errors;
}

} // namespace capturegui
