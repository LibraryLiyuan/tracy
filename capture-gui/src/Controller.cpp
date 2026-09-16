#include "Controller.hpp"
#include "TracyStreamJournal.hpp"
#include "TracyStreamSnapshotMap.hpp"
#include <algorithm>
#include <fstream>
#include <thread>
namespace capturegui
{
using namespace std::chrono_literals;
static uint64_t FileSize(const fs::path &p)
{
    std::error_code e;
    auto n = fs::file_size(p, e);
    return e ? 0 : n;
}
static uint64_t Number(const Json &j)
{
    if (j.is_string())
        return std::stoull(j.get<std::string>());
    return j.get<uint64_t>();
}
Controller::~Controller()
{
    m_cancel = true;
    if (m_worker.joinable())
        m_worker.join();
}
ViewState Controller::Snapshot()
{
    std::lock_guard lock(m_mutex);
    return m_view;
}
void Controller::Spawn(std::function<void()> action)
{
    if (m_worker.joinable())
        m_worker.join();
    m_cancel = false;
    m_stop = false;
    {
        std::lock_guard lock(m_mutex);
        m_view.busy = true;
    }
    m_worker = std::jthread([this, action] {
        try
        {
            action();
        }
        catch (const std::exception &e)
        {
            if (m_task.is_object() && m_task.contains("processes"))
                for (auto &p : m_task["processes"])
                    TerminateOwned(ProcessIdentity::FromJson(p["identity"]));
            try
            {
                State(m_cancel ? "interrupted" : "failed", e.what());
            }
            catch (...)
            {
                std::lock_guard lock(m_mutex);
                m_view.state = "failed";
                m_view.message = e.what();
            }
        }
        std::lock_guard lock(m_mutex);
        m_view.busy = false;
    });
}
void Controller::State(const std::string &state, const std::string &message)
{
    if (m_task.is_object())
    {
        m_task["state"] = state;
        m_task["message"] = message;
        m_task["updated"] = Timestamp();
    }
    {
        std::lock_guard lock(m_mutex);
        m_view.state = state;
        m_view.stage.clear();
        m_view.percent = -1;
        m_view.message = message;
    }
    Persist();
}
void Controller::Persist()
{
    if (m_directory.empty())
        return;
    AtomicJson(m_directory / L"task.json", m_task);
    std::lock_guard lock(m_mutex);
    m_view.task = m_task;
    m_view.directory = PathText(m_directory);
}
void Controller::CheckCancelled()
{
    if (m_cancel)
        throw std::runtime_error("用户强制关闭，文件保留供恢复检查");
}
void Controller::Owned(const std::string &role, const Process &p, const fs::path &exe,
                       const std::vector<std::string> &args)
{
    m_task["processes"].push_back({{"role", role},
                                   {"identity", p.Identity().ToJson()},
                                   {"exe", PathText(exe)},
                                   {"sha256", Sha256(exe)},
                                   {"args", args}});
    Persist();
}
bool Controller::Launch(const Settings &s)
{
    if (Snapshot().busy)
        return false;
    m_directory.clear();
    m_task = Json::object();
    Spawn([this, s] {
        State("launching", "正在启动游戏；管理员模式可能显示系统确认");
        auto target = LaunchPlayer(s);
        {
            std::lock_guard lock(m_mutex);
            m_view.target = target;
        }
        State("waiting_ready", "游戏已启动，请准备场景并确认就绪");
    });
    return true;
}
bool Controller::Start(const Settings &s, ProcessIdentity t)
{
    if (Snapshot().busy)
        return false;
    Spawn([=, this] { Work(s, t); });
    return true;
}
bool Controller::Recover(const fs::path &p)
{
    if (Snapshot().busy)
        return false;
    Spawn([=, this] {
        auto task = ReadJson(p / L"task.json");
        auto settings = Settings::FromJson(task.at("settings"));
        Work(settings, ProcessIdentity::FromJson(task.at("target")), p);
    });
    return true;
}
void Controller::Stop()
{
    m_stop = true;
}
void Controller::ForceClose()
{
    m_cancel = true;
    auto v = Snapshot();
    if (!v.directory.empty())
    {
        auto task = v.task;
        task["state"] = "interrupted";
        task["message"] = "用户强制关闭，等待恢复检查";
        try
        {
            AtomicJson(fs::path(Wide(v.directory)) / L"interrupted.json", task);
        }
        catch (...)
        {
        }
        if (task.contains("processes"))
            for (auto &p : task["processes"])
                TerminateOwned(ProcessIdentity::FromJson(p["identity"]));
    }
}
void Controller::Reset()
{
    if (Snapshot().busy)
        return;
    if (m_worker.joinable())
        m_worker.join();
    std::lock_guard lock(m_mutex);
    auto t = m_view.target;
    m_view = ViewState{};
    m_view.target = t;
}
void Controller::Work(Settings s, ProcessIdentity target, const fs::path &recover)
{
    const bool restoring = !recover.empty();
    m_directory.clear();
    m_task = Json::object();
    fs::path tools = fs::absolute(Wide(s.toolDirectory));
    for (auto name : {L"tracy-capture.exe", L"tracy-stream-convert.exe", L"tracy-query.exe"})
        if (!fs::is_regular_file(tools / name))
            throw std::runtime_error("缺少必需工具：" + PathText(tools / name));
    if (s.port < 1 || s.port > 65535 || s.drainSeconds < 0 || s.drainSeconds > 3600 || s.seconds < 1 ||
        s.stopGiB >= s.warnGiB || !s.memoryMiB)
        throw std::runtime_error("端口、时长或保护阈值无效");
    if (!restoring && (!s.ready || !Alive(target)))
        throw std::runtime_error("请明确选择存活目标并确认场景已就绪");
    if (!restoring && (!fs::is_regular_file(Wide(s.ini))))
        throw std::runtime_error("请选择实际存在的 INI 文件");
    if (restoring)
    {
        m_directory = fs::absolute(recover);
        m_task = ReadJson(m_directory / L"task.json");
        if (m_task.value("schema", 0) != 1)
            throw std::runtime_error("不支持的任务记录版本");
    }
    else
    {
        m_directory = NewTaskDirectory(fs::absolute(Wide(s.output)), s.name);
        s.name = PathText(m_directory.filename());
        m_task = {{"schema", 1},
                  {"name", s.name},
                  {"created", Timestamp()},
                  {"settings", s.ToJson()},
                  {"target", target.ToJson()},
                  {"processes", Json::array()},
                  {"attempt", 0}};
        fs::copy_file(Wide(s.ini), m_directory / L"requested.ini");
        m_task["requested_ini_sha256"] = Sha256(m_directory / L"requested.ini");
    }
    auto previous = m_task.value("processes", Json::array());
    int attempt = m_task.value("attempt", 0) + 1;
    m_task["attempt"] = attempt;
    const auto logDir = m_directory / (L"attempt-" + std::to_wstring(attempt));
    fs::create_directories(logDir);
    const auto stream = m_directory / Wide(s.name + ".tracy-stream"),
               candidate = m_directory / Wide(s.name + ".pending.tracy"),
               final = m_directory / Wide(s.name + ".tracy"), stop = m_directory / L"capture.stop",
               status = m_directory / L"capture-status.json";
    m_task["stream"] = PathText(stream);
    m_task["candidate"] = PathText(candidate);
    m_task["final"] = PathText(final);
    {
        std::lock_guard lock(m_mutex);
        m_view = ViewState{};
        m_view.busy = true;
        m_view.target = target;
    }
    State("preflight", "检查工具与任务身份");
    // Query is a single long-lived session for this attempt, never an AI process.
    if (restoring)
        for (auto &p : previous)
            if (p.value("role", "") == "query")
                TerminateOwned(ProcessIdentity::FromJson(p["identity"]));
    Process query;
    fs::create_directories(logDir / L"query/cache");
    std::vector<std::string> queryArgs = {"--mcp",
                                          "--allow-root",
                                          PathText(m_directory),
                                          "--analysis-root",
                                          PathText(logDir / L"query"),
                                          "--analysis-cache-root",
                                          PathText(logDir / L"query/cache")};
    query.Start(tools / L"tracy-query.exe", queryArgs, logDir / L"query.stderr.log", true);
    Owned("query", query, tools / L"tracy-query.exe", queryArgs);
    Mcp mcp(query, m_cancel, logDir / L"evidence");
    mcp.Initialize();
    auto describe = mcp.Tool("tracy_describe", Json::object());
    if (describe.value("schema_version", "") != "1.35.0")
        throw std::runtime_error("需要 Query schema 1.35.0");
    bool captureNeeded = !restoring;
    Process capture;
    bool adopted = false;
    if (restoring)
    {
        for (auto &p : previous)
            if (p.value("role", "") == "capture" && Alive(ProcessIdentity::FromJson(p["identity"])))
            {
                capture.Adopt(ProcessIdentity::FromJson(p["identity"]));
                adopted = true;
                captureNeeded = true;
                break;
            }
    }
    if (captureNeeded)
    {
        if (!adopted)
        {
            auto space = fs::space(m_directory);
            if (space.available <= s.stopGiB * (1ull << 30))
                throw std::runtime_error("磁盘剩余空间低于停止阈值");
            if (fs::exists(stop) || fs::exists(stream))
                throw std::runtime_error("录制文件已存在，拒绝覆盖");
            std::vector<std::string> args = {"--headless",
                                             "--status-json",
                                             PathText(status),
                                             "-a",
                                             "127.0.0.1",
                                             "-p",
                                             std::to_string(s.port),
                                             "-j",
                                             PathText(stream),
                                             "-d",
                                             std::to_string(s.drainSeconds),
                                             "-x",
                                             PathText(stop)};
            capture.Start(tools / L"tracy-capture.exe", args, logDir / L"capture.log");
            Owned("capture", capture, tools / L"tracy-capture.exe", args);
        }
        State("connecting", "正在连接目标；首次数据到达后开始计时");
        auto started = std::chrono::steady_clock::now();
        bool recording = false, stopSent = fs::exists(stop);
        while (capture.Running())
        {
            CheckCancelled();
            Json observed = Json::object();
            try
            {
                observed = ReadJson(status);
            }
            catch (...)
            {
            }
            bool data = observed.value("first_data_received", false);
            uint64_t elapsed = observed.value("elapsed_ms", 0ull);
            auto memory = capture.Memory();
            auto free = fs::space(m_directory).available;
            if (data && !recording)
            {
                recording = true;
                State("recording", "已收到数据，正在录制");
                if (s.sound)
                    MessageBeep(MB_OK);
            }
            std::string reason;
            if (data && observed.value("client_pid", 0ull) != target.pid)
                reason = "target_mismatch";
            else if (m_stop)
                reason = "user";
            else if (s.timed && data && elapsed >= uint64_t(s.seconds) * 1000)
                reason = "timer";
            else if (free <= s.stopGiB * (1ull << 30))
                reason = "disk_protection";
            else if (memory >= s.memoryMiB * (1ull << 20))
                reason = "memory_protection";
            else if (!Alive(target))
                reason = "target_exited";
            else if (!data && std::chrono::steady_clock::now() - started > 60s)
                reason = "connection_timeout";
            if (!reason.empty() && !stopSent)
            {
                std::ofstream marker(stop, std::ios::binary);
                marker << reason;
                marker.close();
                if (!marker)
                    throw std::runtime_error("无法请求正常停止");
                stopSent = true;
                m_task["stop_reason"] = reason;
                State(data ? "draining" : "connecting", data ? "正在排空，请保持游戏运行" : "正在取消连接");
            }
            {
                std::lock_guard lock(m_mutex);
                m_view.elapsedMs = elapsed;
                m_view.streamBytes = FileSize(stream);
                m_view.memoryBytes = memory;
                m_view.freeBytes = free;
                if (free <= s.warnGiB * (1ull << 30) && !stopSent)
                    m_view.message = "磁盘空间不足警告；正在录制";
            }
            std::this_thread::sleep_for(200ms);
        }
        m_task["capture_exit_code"] = capture.ExitCode();
        try
        {
            auto observed = ReadJson(status);
            m_task["capture_status"] = observed;
            m_task["elapsed_ms"] = observed.value("elapsed_ms", 0ull);
            {
                std::lock_guard lock(m_mutex);
                m_view.elapsedMs = m_task["elapsed_ms"].get<uint64_t>();
            }
            if (!observed.value("first_data_received", false))
            {
                query.Kill();
                State("cancelled", "未收到有效数据，本轮已结束");
                return;
            }
        }
        catch (const nlohmann::json::exception &)
        {
        }
    }
    if (m_task.value("stop_reason", "") == "target_mismatch")
        throw std::runtime_error("实际连接 Client 的 PID 与选择目标不一致，已停止录制");
    CheckCancelled();
    State("draining", "检查 stream 封口和完整性");
    if (!fs::is_regular_file(stream))
        throw std::runtime_error("没有可恢复的 stream 文件");
    auto size = FileSize(stream);
    std::this_thread::sleep_for(1000ms);
    if (FileSize(stream) != size)
        throw std::runtime_error("stream 仍在变化，不能转换");
    auto scan = tracy::stream::ScanJournal(stream);
    if (!scan.HasRecoverablePrefix() || scan.header.protocolVersion != 90)
        throw std::runtime_error("stream 不可恢复或协议不是 90");
    bool complete = scan.complete && m_task.value("capture_exit_code", DWORD(999)) == 0;
    m_task["completeness"] = complete ? "Complete" : "RecoverablePrefix";
    m_task["watermark_ns"] = std::to_string(scan.lastMonotonicNs);
    auto streamSha = Sha256(stream);
    if (restoring && m_task.contains("stream_sha256") && m_task["stream_sha256"] != streamSha)
        throw std::runtime_error("原始 stream 已变化，旧候选和校验证据失效");
    m_task["stream_sha256"] = streamSha;
    m_task["stream_bytes"] = size;
    {
        std::lock_guard lock(m_mutex);
        m_view.gameMayClose = true;
    }
    if (fs::exists(final))
    {
        // A crash between publication and the final task write is repaired only from saved evidence.
        if (m_task.value("validated_sha256", "") != Sha256(final))
            throw std::runtime_error("正式文件已存在，但无法核实发布证据");
        std::string error;
        if (!tracy::stream::WriteConvertedSnapshotMap(stream, scan, final, error))
            throw std::runtime_error(error);
        query.Kill();
        State(complete ? "complete" : "partial", "发布记录已恢复");
        return;
    }
    if (restoring)
        for (auto &p : previous)
            if (p.value("role", "") == "converter" && Alive(ProcessIdentity::FromJson(p["identity"])))
            {
                State("converting", "已恢复转换监控，现在可以关闭游戏");
                Process active;
                active.Adopt(ProcessIdentity::FromJson(p["identity"]));
                while (active.Running())
                {
                    CheckCancelled();
                    std::this_thread::sleep_for(250ms);
                }
                if (active.ExitCode() != 0 || !fs::exists(candidate))
                    throw std::runtime_error("接管的转换失败，原始文件已保留");
                m_task["candidate_sha256"] = Sha256(candidate);
                Persist();
            }
    bool reuse =
        restoring && fs::exists(candidate) && m_task.value("candidate_sha256", "") == Sha256(candidate);
    if (!reuse)
    {
        if (fs::exists(candidate))
        {
            auto failed = m_directory / L"failed-artifacts";
            fs::create_directories(failed);
            fs::rename(candidate, failed / (L"attempt-" + std::to_wstring(attempt) + L".pending.tracy"));
        }
        State("converting", "正在转换，现在可以关闭游戏");
        Process converter;
        bool adoptedConverter = false;
        if (restoring)
            for (auto &p : previous)
                if (p.value("role", "") == "converter" && Alive(ProcessIdentity::FromJson(p["identity"])))
                {
                    converter.Adopt(ProcessIdentity::FromJson(p["identity"]));
                    adoptedConverter = true;
                    break;
                }
        fs::path progress = m_directory / L"conversion-progress.json";
        if (!adoptedConverter)
        {
            std::vector<std::string> args = {"-i",
                                             PathText(stream),
                                             "-o",
                                             PathText(candidate),
                                             "--progress-json",
                                             PathText(progress),
                                             "--report-json",
                                             PathText(logDir / L"conversion-report.json"),
                                             "--keep-failed-output"};
            converter.Start(tools / L"tracy-stream-convert.exe", args, logDir / L"converter.log");
            Owned("converter", converter, tools / L"tracy-stream-convert.exe", args);
        }
        while (converter.Running())
        {
            CheckCancelled();
            try
            {
                auto p = ReadJson(progress);
                std::lock_guard lock(m_mutex);
                m_view.stage = p.value("stage", "");
                m_view.percent = p.value("percent", -1.0);
                m_view.memoryBytes = p.contains("working_set_bytes") ? Number(p["working_set_bytes"]) : 0;
            }
            catch (...)
            {
            }
            std::this_thread::sleep_for(250ms);
        }
        if (converter.ExitCode() != 0 || !fs::is_regular_file(candidate))
            throw std::runtime_error("转换失败；原始 stream 已保留，可重试。退出码 " +
                                     std::to_string(converter.ExitCode()));
        m_task["candidate_sha256"] = Sha256(candidate);
        Persist();
    }
    CheckCancelled();
    State("validating", "正在校验文件和采集质量");
    auto opened = mcp.Tool("tracy_trace_open", {{"path", PathText(candidate)}});
    auto trace = opened.at("data").at("trace_id").get<std::string>();
    for (;;)
    {
        CheckCancelled();
        auto statusReply = mcp.Tool("tracy_trace_status", {{"trace_id", trace}});
        auto state = statusReply.at("data").at("status").at("state").get<std::string>();
        if (state == "ready")
            break;
        if (state == "failed" || state == "closed")
            throw std::runtime_error("Query 无法加载候选文件：" + statusReply.dump());
        {
            std::lock_guard lock(m_mutex);
            m_view.stage = state;
            m_view.percent = -1;
        }
        std::this_thread::sleep_for(400ms);
    }
    {
        std::lock_guard lock(m_mutex);
        m_view.stage = "checking";
        m_view.percent = -1;
    }
    auto inspect = [&](const std::string &method) {
        return mcp.Tool("tracy_inspect",
                        {{"trace_id", trace}, {"method", method}, {"params", Json::object()}});
    };
    Json evidence;
    for (auto method : {"trace.identity", "trace.app_info", "system.capabilities", "capture.context",
                        "capture.coverage", "gpu.catalog.status", "gpu.catalog.validation"})
    {
        CheckCancelled();
        evidence[method] = inspect(method);
    }
    evidence["overview"] = mcp.Tool("tracy_overview", {{"trace_id", trace}});
    auto validation = mcp.Tool("tracy_validate", {{"trace_id", trace}, {"async", false}});
    if (validation.at("data").contains("job_id"))
    {
        auto id = validation.at("data").at("job_id");
        for (;;)
        {
            CheckCancelled();
            auto state = mcp.Tool("tracy_job", {{"operation", "status"}, {"job_id", id}});
            auto jobState = state.at("data").at("state").get<std::string>();
            if (jobState == "failed" || jobState == "cancelled")
                throw std::runtime_error("Query validation job failed");
            if (jobState == "completed")
                break;
            std::this_thread::sleep_for(400ms);
        }
        validation = mcp.Tool("tracy_job", {{"operation", "result"}, {"job_id", id}});
    }
    evidence["validation"] = validation;
    AtomicJson(logDir / L"validation.json", evidence);
    {
        std::lock_guard lock(m_mutex);
        m_view.stage = "publishing";
        m_view.percent = -1;
    }
    // Structural query failures block publication; domain findings remain visible limitations.
    auto close = mcp.Tool("tracy_trace_close", {{"trace_id", trace}});
    (void)close;
    query.Kill();
    while (query.Running())
    {
        CheckCancelled();
        std::this_thread::sleep_for(25ms);
    }
    if (!CanPublish(true, m_task.at("candidate_sha256").get<std::string>() == Sha256(candidate),
                    scan.HasRecoverablePrefix()))
        throw std::runtime_error("候选身份变化，拒绝发布");
    m_task["validation_evidence"] = PathText(logDir / L"validation.json");
    m_task["validated_sha256"] = m_task["candidate_sha256"];
    m_task["quality"] = validation["data"];
    m_task["quality_passed"] = validation["data"].value("valid", false);
    m_task["actual_config"] = evidence["capture.context"]["data"];
    Persist();
    if (!MoveFileExW(candidate.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("发布失败：" + ErrorText());
    std::string error;
    if (!tracy::stream::WriteConvertedSnapshotMap(stream, scan, final, error))
        throw std::runtime_error("文件已发布，但索引映射更新失败，可恢复：" + error);
    m_task["final_bytes"] = FileSize(final);
    {
        std::lock_guard lock(m_mutex);
        m_view.quality = validation["data"].value("valid", false)
                             ? "未发现结构校验错误；仍需查看缺失域和采集限制。"
                             : "校验已完成，但存在数据质量问题；相关域的分析受限。";
        m_view.actualConfig = "实际配置已写入校验证据。";
        m_view.percent = 100;
    }
    State(complete ? "complete" : "partial",
          complete ? "录制完整，转换与校验已完成" : "仅有效前缀可用于分析；尾部数据不完整");
    if (fs::exists(m_directory / L"interrupted.json"))
        fs::rename(m_directory / L"interrupted.json", logDir / L"previous-interruption.json");
    if (s.sound)
        MessageBeep(MB_OK);
}
std::vector<Json> Controller::History(const fs::path &output)
{
    std::vector<Json> result;
    std::error_code ec;
    if (!fs::is_directory(output, ec))
        return result;
    for (auto &item : fs::directory_iterator(output, ec))
    {
        try
        {
            if (item.is_directory())
            {
                auto j = ReadJson(item.path() / L"task.json");
                j["directory"] = PathText(item.path());
                if (fs::exists(item.path() / L"interrupted.json"))
                    j["state"] = "interrupted";
                result.push_back(std::move(j));
            }
        }
        catch (...)
        {
        }
    }
    std::sort(result.begin(), result.end(),
              [](auto &a, auto &b) { return a.value("created", "") > b.value("created", ""); });
    return result;
}
std::string Controller::AnalysisRequest(const Json &task)
{
    auto s = task.at("settings");
    return "请使用 diagnose-jn-unity-tracy 技能分析本次录制。\n文件：" + task.value("final", "") +
           "\n名称：" + task.value("name", "") + "\n场景：" + s.value("scene", "") + "\n操作备注：" +
           s.value("note", "") + "\n完整性：" + task.value("completeness", "未知") + "\n校验证据：" +
           task.value("validation_evidence", "") +
           "\n请先核实输入、实际采集配置、质量限制和分析配置，再进行单 Trace 分析。\n" +
           (task.value("completeness", "") == "RecoverablePrefix"
                ? "只分析已验证前缀，尾部不完整；不能据此得出没有问题的结论。"
                : "");
}
} // namespace capturegui
