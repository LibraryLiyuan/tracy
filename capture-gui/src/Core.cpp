#include "Core.hpp"
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <commdlg.h>
#include <fstream>
#include <iomanip>
#include <set>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sstream>
#include <tlhelp32.h>
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
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read " + PathText(p));
    return Json::parse(f);
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
    if (!MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(ErrorText());
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
bool CanPublish(bool a, bool b, bool c)
{
    return a && b && c;
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
std::vector<std::string> PreflightErrors(const Settings &s, ProcessIdentity target)
{
    std::vector<std::string> errors;
    if (!target.pid || !Alive(target))
        errors.push_back("请选择存活的 Player / Editor");
    std::error_code ec;
    if (!fs::is_regular_file(Wide(s.ini), ec))
        errors.push_back("请选择存在的 INI 文件");
    for (auto name : {L"tracy-capture.exe", L"tracy-stream-convert.exe", L"tracy-query.exe"})
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
    return errors;
}

} // namespace capturegui
