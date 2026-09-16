#include "Process.hpp"
#include <psapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <thread>
namespace capturegui
{
Process::~Process()
{
    Kill();
    if (m_in)
        CloseHandle(m_in);
    if (m_out)
        CloseHandle(m_out);
    if (m_process)
        CloseHandle(m_process);
}
void Process::Start(const fs::path &exe, const std::vector<std::string> &args, const fs::path &log, bool pipe)
{
    if (m_process)
        throw std::runtime_error("Process already assigned");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    fs::create_directories(log.parent_path());
    HANDLE file = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create process log: " + ErrorText());
    HANDLE input = nullptr, output = file;
    auto cleanup = [&] {
        CloseHandle(file);
        if (input)
            CloseHandle(input);
        if (pipe && output)
            CloseHandle(output);
    };
    if (pipe)
    {
        if (!CreatePipe(&input, &m_in, &security, 0) || !CreatePipe(&m_out, &output, &security, 0))
        {
            cleanup();
            throw std::runtime_error(ErrorText());
        }
        SetHandleInformation(m_in, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(m_out, HANDLE_FLAG_INHERIT, 0);
    }
    else
        input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                            OPEN_EXISTING, 0, nullptr);
    std::wstring command = Quote(exe.wstring());
    for (auto &arg : args)
        command += L" " + Quote(Wide(arg));
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = file;
    PROCESS_INFORMATION info{};
    BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             exe.parent_path().c_str(), &startup, &info);
    auto error = GetLastError();
    cleanup();
    if (!ok)
        throw std::runtime_error(ErrorText(error));
    CloseHandle(info.hThread);
    m_process = info.hProcess;
    m_identity = Identify(m_process);
}
void Process::Adopt(const ProcessIdentity &identity)
{
    if (m_process)
        throw std::runtime_error("Process already assigned");
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_TERMINATE | SYNCHRONIZE,
                           FALSE, identity.pid);
    if (!h)
        throw std::runtime_error(ErrorText());
    if (Identify(h).created != identity.created)
    {
        CloseHandle(h);
        throw std::runtime_error("进程身份已变化，不能接管");
    }
    m_process = h;
    m_identity = identity;
}
bool Process::Running() const
{
    return m_process && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}
DWORD Process::ExitCode() const
{
    DWORD code = 0;
    if (!m_process || !GetExitCodeProcess(m_process, &code))
        throw std::runtime_error(ErrorText());
    return code;
}
uint64_t Process::Memory() const
{
    PROCESS_MEMORY_COUNTERS p{};
    p.cb = sizeof(p);
    return m_process && GetProcessMemoryInfo(m_process, &p, sizeof(p)) ? p.WorkingSetSize : 0;
}
void Process::Kill()
{
    if (m_process && Running())
        TerminateProcess(m_process, 130);
}
void Process::Send(const std::string &text)
{
    size_t offset = 0;
    while (offset < text.size())
    {
        DWORD written = 0;
        if (!WriteFile(m_in, text.data() + offset, DWORD(text.size() - offset), &written, nullptr) ||
            !written)
            throw std::runtime_error("MCP stdin closed");
        offset += written;
    }
}
std::string Process::Line(std::atomic<bool> &cancel, std::chrono::seconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        if (cancel)
            throw std::runtime_error("任务中断");
        auto end = m_buffer.find('\n');
        if (end != std::string::npos)
        {
            auto line = m_buffer.substr(0, end);
            m_buffer.erase(0, end + 1);
            return line;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(m_out, nullptr, 0, nullptr, &available, nullptr))
            throw std::runtime_error("MCP stdout closed");
        if (available)
        {
            char bytes[65536];
            DWORD read = 0;
            if (!ReadFile(m_out, bytes, (std::min)(available, DWORD(sizeof(bytes))), &read, nullptr))
                throw std::runtime_error("MCP read failed");
            m_buffer.append(bytes, read);
            if (m_buffer.size() > 64 * 1024 * 1024)
                throw std::runtime_error("MCP response exceeds 64 MiB");
        }
        else
        {
            if (!Running())
                throw std::runtime_error("Query exited before response");
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("MCP request timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
}
ProcessIdentity LaunchPlayer(const Settings &s)
{
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComGuard { HRESULT result; ~ComGuard() { if(SUCCEEDED(result)) CoUninitialize(); } } comGuard{com};
    fs::path exe = fs::absolute(Wide(s.player));
    if (!fs::is_regular_file(exe))
        throw std::runtime_error("Player 程序不存在");
    if (!fs::is_regular_file(Wide(s.ini)))
        throw std::runtime_error("INI 文件不存在");
    if (s.arguments.find("jn-tracy-config") != std::string::npos)
        throw std::runtime_error("请通过采集配置选择 INI，不要在启动参数中重复指定");
    std::wstring args =
        Wide(s.arguments) + L" -jn-tracy-config " + Quote(fs::absolute(Wide(s.ini)).wstring());
    std::wstring dir = s.workingDirectory.empty() ? exe.parent_path().wstring() : Wide(s.workingDirectory);
    if (s.admin)
    {
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas";
        info.lpFile = exe.c_str();
        info.lpParameters = args.c_str();
        info.lpDirectory = dir.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
            throw std::runtime_error(GetLastError() == ERROR_CANCELLED ? "已取消管理员启动" : ErrorText());
        auto identity = Identify(info.hProcess);
        CloseHandle(info.hProcess);
        return identity;
    }
    std::wstring command = Quote(exe.wstring()) + L" " + args;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(),
                        &startup, &info))
        throw std::runtime_error(ErrorText());
    auto identity = Identify(info.hProcess);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return identity;
}
Json Mcp::Request(const std::string &method, const Json &params)
{
    int id = ++m_next;
    Json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    AtomicJson(m_evidence / (std::to_string(id) + "-request.json"), request);
    m_process.Send(request.dump() + "\n");
    for (;;)
    {
        auto line = m_process.Line(m_cancel, std::chrono::seconds(300));
        if (line.empty())
            continue;
        auto response = Json::parse(line);
        if (!response.contains("id"))
            continue;
        if (response["id"] != id)
            throw std::runtime_error("MCP response identity mismatch");
        AtomicJson(m_evidence / (std::to_string(id) + "-response.json"), response);
        if (response.contains("error"))
            throw std::runtime_error(response["error"].dump());
        return response.at("result");
    }
}
Json Mcp::Tool(const std::string &name, const Json &args)
{
    auto result = Request("tools/call", {{"name", name}, {"arguments", args}});
    if (result.value("isError", false))
        throw std::runtime_error("Query tool failed: " + result.dump().substr(0, 3000));
    Json envelope;
    if (result.contains("structuredContent"))
        envelope = result["structuredContent"];
    else
    {
        for (auto &c : result.at("content"))
            if (c.value("type", "") == "text")
            {
                envelope = Json::parse(c.at("text").get<std::string>());
                break;
            }
    }
    if (envelope.is_null())
        throw std::runtime_error("Missing Query response");
    if (envelope.contains("ok") && !envelope.at("ok").get<bool>())
        throw std::runtime_error(envelope.dump());
    if (envelope.value("partial", false) || envelope.value("truncated", false))
        throw std::runtime_error("Query validation response incomplete");
    return envelope;
}
void Mcp::Initialize()
{
    Request("initialize", {{"protocolVersion", "2025-11-25"},
                           {"capabilities", Json::object()},
                           {"clientInfo", {{"name", "tracy-capture-gui"}, {"version", "1.0.0"}}}});
    m_process.Send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    Request("tools/list", Json::object());
}
} // namespace capturegui
