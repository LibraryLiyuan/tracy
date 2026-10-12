#include "Controller.hpp"
#include "Presentation.hpp"
#include "TracyStreamJournal.hpp"
#include "TracyStreamSnapshotMap.hpp"
#include <algorithm>
#include <fstream>
#include <optional>
#include <thread>
namespace capturegui
{
using namespace std::chrono_literals;
// An event store build's progress, from the end of its log.
static StoreProgress ReadStoreProgress(const fs::path &log)
{
    std::ifstream in(log, std::ios::binary);
    if (!in)
        return {};
    in.seekg(0, std::ios::end);
    const auto size = int64_t(in.tellg());
    if (size <= 0)
        return {};
    const auto from = std::max<int64_t>(0, size - 4096);
    in.seekg(from);
    std::string tail(size_t(size - from), '\0');
    in.read(tail.data(), std::streamsize(tail.size()));
    tail.resize(size_t(in.gcount()));
    return ParseStoreProgress(tail);
}
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
// Removes the work directories of event store builds of this journal that
// ended without publishing. Only called while no store build of it runs.
static void RemoveStoreWork(const fs::path &stream)
{
    const auto prefix = stream.filename().wstring() + L".store.building-";
    std::error_code e;
    for (auto &entry : fs::directory_iterator(stream.parent_path(), e))
        if (entry.path().filename().wstring().starts_with(prefix))
            fs::remove_all(entry.path(), e);
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
bool Controller::Restart(const Settings &s, ProcessIdentity running)
{
    if (Snapshot().busy)
        return false;
    m_directory.clear();
    m_task = Json::object();
    Spawn([this, s, running] {
        State("launching", "正在关闭游戏，随后用所选 INI 重新启动；可能显示管理员确认");
        ProcessIdentity target;
        try
        {
            target = RestartPlayer(s, running);
        }
        catch (const std::exception &e)
        {
            // The setup stays open, so the game can be started from it.
            State("ready", std::string("没有完成重启：") + e.what());
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            m_view.target = target;
        }
        State("waiting_ready", "游戏已用所选 INI 重新启动，请准备场景并确认就绪");
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
bool Controller::Recover(const fs::path &p, const Settings *currentTools)
{
    if (Snapshot().busy)
        return false;
    const bool useCurrentTools = currentTools != nullptr;
    const auto toolDirectory = currentTools ? currentTools->toolDirectory : std::string();
    const bool customTools = currentTools && currentTools->customTools;
    Spawn([=, this] {
        auto task = ReadJson(p / L"task.json");
        auto settings = Settings::FromJson(task.at("settings"));
        if (useCurrentTools)
        {
            settings.toolDirectory = toolDirectory;
            settings.customTools = customTools;
        }
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
    PreferBundledTools(s, ExecutableDirectory());
    m_directory.clear();
    m_task = Json::object();
    fs::path tools = fs::absolute(Wide(s.toolDirectory));
    for (auto name : {L"tracy-capture.exe", L"tracy-stream-export.exe", L"tracy-query.exe"})
        if (!fs::is_regular_file(tools / name))
            throw std::runtime_error("缺少必需工具：" + PathText(tools / name));
    if (s.port < 1 || s.port > 65535 || s.drainSeconds < 0 || s.drainSeconds > 3600 || s.seconds < 1 ||
        s.stopGiB >= s.warnGiB || !s.memoryMiB)
        throw std::runtime_error("端口、时长或保护阈值无效");
    if (!restoring && (!s.ready || !Alive(target)))
        throw std::runtime_error("请明确选择存活目标并确认场景已就绪");
    if (!restoring && (!fs::is_regular_file(Wide(s.ini))))
        throw std::runtime_error("请选择实际存在的 INI 文件");
    // The game read its INI when it started; a recording never claims another.
    const auto loaded = restoring ? LoadedIni{} : QueryLoadedIni(target);
    if (!restoring && CompareIni(loaded, s.ini) == IniMatch::Different)
        throw std::runtime_error("所选 INI 未生效：游戏启动时读取的是 " + loaded.path + "；请用所选配置重启游戏");
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
        m_task["loaded_ini"] = loaded.ToJson();
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
        if (m_task.contains("loaded_ini"))
        {
            const auto &ini = m_task["loaded_ini"];
            const auto source = ini.value("source", "");
            m_view.actualConfig = "游戏启动时读取的 INI：" + ini.value("path", "") +
                                  (source == "reported" ? "（游戏报告）" : source == "inferred" ? "（按启动参数推断）" : "（无法确认）");
        }
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
    // The event store is built while the recording runs, following the
    // journal; the stop marker tells it that the journal is final.
    const auto storeStop = m_directory / L"store.follow.stop", storeSummary = m_directory / L"store-summary.json";
    Process storeBuild;
    bool storeStarted = false;
    // Saving is timed from the stop request to publication.
    std::optional<std::chrono::steady_clock::time_point> stopRequestedAt;
    const auto secondsSince = [](std::chrono::steady_clock::time_point since) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    };
    // While the game is recorded, the capture, the event store following it and
    // this window run on the efficiency cores of a hybrid CPU: Unity pins the
    // game's main and render threads to the performance cores, and recording
    // work there made the game miss vsync. JN_TRACY_CAPTURE_CORES=any turns it
    // off. When the recording ends, the store and the window get every core
    // back to finish quickly.
    const auto toolCores = [] {
        char mode[16] = {};
        const auto length = GetEnvironmentVariableA("JN_TRACY_CAPTURE_CORES", mode, DWORD(sizeof(mode)));
        if (length && length < sizeof(mode) && _stricmp(mode, "any") == 0)
            return std::vector<CpuSet>{};
        return EfficiencyCpuSets(SystemCpuSets());
    }();
    {
        Json logical = Json::array();
        for (auto &set : toolCores)
            logical.push_back(set.logical);
        m_task["capture_cores"] = {{"mode", toolCores.empty() ? "any" : "efficiency"}, {"logical", logical}};
    }
    struct WindowCores
    {
        bool active = false;
        void Release()
        {
            if (active)
                SetProcessCpuSets(GetCurrentProcess(), {});
            active = false;
        }
        ~WindowCores()
        {
            Release();
        }
    } windowCores;
    const auto startStore = [&] {
        std::vector<std::string> args = {PathText(stream), "--store-only", "--follow", PathText(storeStop), "--summary", PathText(storeSummary)};
        // The store keeps up with the journal at below normal priority: the
        // game being recorded comes first.
        storeBuild.Start(tools / L"tracy-stream-export.exe", args, logDir / L"store.log", false, true);
        Owned("store", storeBuild, tools / L"tracy-stream-export.exe", args);
        if (!toolCores.empty())
            SetProcessCpuSets(storeBuild.Handle(), toolCores);
        storeStarted = true;
    };
    if (restoring)
        for (auto &p : previous)
            if (p.value("role", "") == "store" && Alive(ProcessIdentity::FromJson(p["identity"])))
            {
                storeBuild.Adopt(ProcessIdentity::FromJson(p["identity"]));
                storeStarted = true;
                break;
            }
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
        if (!toolCores.empty())
        {
            SetProcessCpuSets(capture.Handle(), toolCores);
            if (storeStarted)
                SetProcessCpuSets(storeBuild.Handle(), toolCores);
            windowCores.active = SetProcessCpuSets(GetCurrentProcess(), toolCores);
        }
        State("connecting", "正在连接目标；首次数据到达后开始计时");
        auto lastConnectionProgress = std::chrono::steady_clock::now();
        uint64_t lastConnectionBytes = FileSize(stream);
        Json lastObserved = Json::object();
        bool recording = false, stopSent = fs::exists(stop);
        while (capture.Running())
        {
            CheckCancelled();
            if (!storeStarted && fs::exists(stream) && !fs::exists(storeSummary))
                startStore();
            try
            {
                auto observed = ReadJson(status);
                if (observed.is_object() && observed.contains("first_data_received"))
                {
                    // A transient read or malformed status must not erase a connected session.
                    (void)observed.value("first_data_received", false);
                    (void)observed.value("elapsed_ms", 0ull);
                    (void)observed.value("client_pid", 0ull);
                    lastObserved = std::move(observed);
                }
            }
            catch (...)
            {
            }
            const auto &observed = lastObserved;
            const auto currentConnectionBytes = FileSize(stream);
            if (currentConnectionBytes > lastConnectionBytes)
                lastConnectionProgress = std::chrono::steady_clock::now();
            lastConnectionBytes = currentConnectionBytes;
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
            else if (CaptureConnectionTimedOut(recording, data, uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastConnectionProgress).count())))
                reason = "connection_timeout";
            if (!reason.empty() && !stopSent)
            {
                std::ofstream marker(stop, std::ios::binary);
                marker << reason;
                marker.close();
                if (!marker)
                    throw std::runtime_error("无法请求正常停止");
                stopSent = true;
                stopRequestedAt = std::chrono::steady_clock::now();
                m_task["stop_reason"] = reason;
                State(data ? "draining" : "connecting", data ? "正在排空，请保持游戏运行" : "正在取消连接");
            }
            {
                std::lock_guard lock(m_mutex);
                m_view.elapsedMs = elapsed;
                m_view.streamBytes = currentConnectionBytes;
                m_view.memoryBytes = memory;
                m_view.freeBytes = free;
                m_view.recordingBytesPerSecond = observed.value("bytes_per_second", 0.0);
                m_view.remainingDiskSeconds = observed.value("remaining_disk_seconds", -1.0);
                m_view.committedBytes = observed.contains("committed_bytes") ? Number(observed["committed_bytes"]) : 0;
                m_view.durableBytes = observed.contains("durable_bytes") ? Number(observed["durable_bytes"]) : 0;
                m_view.backpressureWaits = observed.contains("backpressure_wait_count") ? Number(observed["backpressure_wait_count"]) : 0;
                if (free <= s.warnGiB * (1ull << 30) && !stopSent)
                    m_view.message = "磁盘空间不足警告；正在录制";
            }
            std::this_thread::sleep_for(200ms);
        }
        m_task["capture_exit_code"] = capture.ExitCode();
        windowCores.Release();
        if (storeStarted && !toolCores.empty())
            SetProcessCpuSets(storeBuild.Handle(), {});
        if (stopRequestedAt)
            m_task["processing_seconds"]["drain"] = secondsSince(*stopRequestedAt);
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
    const auto journalScanStart = std::chrono::steady_clock::now();
    auto scan = tracy::stream::ScanJournal(stream);
    m_task["processing_seconds"]["journal_scan"] = secondsSince(journalScanStart);
    if (!scan.HasRecoverablePrefix() || scan.header.protocolVersion != 90)
        throw std::runtime_error("stream 不可恢复或协议不是 90");
    bool complete = scan.complete && m_task.value("capture_exit_code", DWORD(999)) == 0;
    if (m_task.contains("capture_status") && m_task["capture_status"].contains("capture_complete"))
        complete = complete && m_task["capture_status"].value("capture_complete", false);
    m_task["completeness"] = complete ? "Complete" : "RecoverablePrefix";
    m_task["watermark_ns"] = std::to_string(scan.lastMonotonicNs);
    const auto streamIdentityStart = std::chrono::steady_clock::now();
    auto streamSha = Sha256(stream);
    m_task["processing_seconds"]["stream_identity"] = secondsSince(streamIdentityStart);
    if (restoring && m_task.contains("stream_sha256") && m_task["stream_sha256"] != streamSha)
        throw std::runtime_error("原始 stream 已变化，旧候选和校验证据失效");
    m_task["stream_sha256"] = streamSha;
    m_task["stream_bytes"] = size;
    {
        std::lock_guard lock(m_mutex);
        m_view.gameMayClose = true;
    }
    // The journal is final: the following store build finishes. If it did not
    // run or failed, the store is built (or verified) from the final journal.
    {
        std::ofstream marker(storeStop, std::ios::binary);
    }
    if (storeStarted)
    {
        State("converting", "录制已保存，正在完成索引，现在可以关闭游戏");
        { std::lock_guard lock(m_mutex); m_view.stage = "EventStore"; m_view.percent = -1; }
        const auto storeFinishStart = std::chrono::steady_clock::now();
        while (storeBuild.Running())
        {
            CheckCancelled();
            const auto progress = ReadStoreProgress(logDir / L"store.log");
            {
                std::lock_guard lock(m_mutex);
                m_view.memoryBytes = storeBuild.Memory();
                m_view.percent = progress.percent;
                m_view.progressText = progress.step;
            }
            std::this_thread::sleep_for(250ms);
        }
        m_task["store_follow_exit_code"] = storeBuild.ExitCode();
        m_task["processing_seconds"]["store_finish"] = secondsSince(storeFinishStart);
    }
    RemoveStoreWork(stream);
    // Every recording is published as its journal and the event store that
    // followed it: the long-capture view and Query read the store directly, so
    // a recording opens seconds after the stop, whatever its length. A task
    // that an earlier version published as a .tracy keeps it.
    const auto storeDirectory = fs::path(stream.wstring() + L".store");
    if (fs::exists(final) && m_task.value("output_kind", "") != "store")
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
    // A .tracy conversion an earlier version started for this task is not resumed.
    if (restoring)
        for (auto &p : previous)
            if (p.value("role", "") == "converter")
                TerminateOwned(ProcessIdentity::FromJson(p["identity"]));
    // A summary without its store, or of other journal content (a recovered
    // recording whose tail was cut), is rebuilt.
    const auto staleSummary = [&] {
        try
        {
            return !fs::exists(storeDirectory / L"manifest.json") || ReadJson(storeSummary).value("source_sha256", "") != streamSha;
        }
        catch (...)
        {
            return true;
        }
    };
    if (fs::exists(storeSummary) && staleSummary())
    {
        fs::remove(storeSummary);
        // A published store of other journal content is kept aside, never overwritten.
        if (fs::exists(storeDirectory / L"manifest.json"))
        {
            const auto failed = m_directory / L"failed-artifacts";
            fs::create_directories(failed);
            fs::rename(storeDirectory, failed / (L"attempt-" + std::to_wstring(attempt) + L".store"));
        }
    }
    if (!fs::exists(storeSummary))
    {
        State("converting", "录制已保存，正在建立索引，现在可以关闭游戏");
        { std::lock_guard lock(m_mutex); m_view.stage = "EventStoreBuild"; m_view.percent = -1; }
        Process build;
        std::vector<std::string> args = {PathText(stream), "--store-only", "--summary", PathText(storeSummary)};
        build.Start(tools / L"tracy-stream-export.exe", args, logDir / L"store-build.log");
        Owned("store", build, tools / L"tracy-stream-export.exe", args);
        const auto storeBuildStart = std::chrono::steady_clock::now();
        while (build.Running())
        {
            CheckCancelled();
            const auto progress = ReadStoreProgress(logDir / L"store-build.log");
            {
                std::lock_guard lock(m_mutex);
                m_view.memoryBytes = build.Memory();
                m_view.percent = progress.percent;
                m_view.progressText = progress.step;
            }
            std::this_thread::sleep_for(250ms);
        }
        m_task["processing_seconds"]["store_build"] = secondsSince(storeBuildStart);
        if (build.ExitCode() != 0 || !fs::exists(storeSummary))
            throw std::runtime_error("索引建立失败；原始录制已保留，可重试。退出码 " + std::to_string(build.ExitCode()));
    }
    query.Kill();
    auto summary = ReadJson(storeSummary);
    if (summary.value("source_sha256", "") != streamSha || !fs::exists(storeDirectory / L"manifest.json"))
        throw std::runtime_error("索引与原始录制不一致，拒绝发布");
    m_task["output_kind"] = "store";
    m_task["store"] = PathText(storeDirectory);
    m_task["final"] = PathText(stream);
    m_task["store_summary"] = PathText(storeSummary);
    m_task["quality"] = summary;
    m_task["quality_passed"] = summary.value("valid", false);
    if (stopRequestedAt)
        m_task["processing_seconds"]["save_total"] = secondsSince(*stopRequestedAt);
    Persist();
    {
        std::lock_guard lock(m_mutex);
        m_view.quality = summary.value("valid", false) ? "录制已保存并核验；仍需查看缺失域和采集限制。"
                                                       : "录制已保存，但存在数据质量问题；相关域的分析受限。";
        m_view.percent = 100;
        m_view.progressText.clear();
    }
    State(complete ? "complete" : "partial",
          complete ? "录制完整，可以打开" : "仅有效前缀可用于分析；尾部数据不完整");
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
    const bool store = task.value("output_kind", "") == "store";
    return "请使用 diagnose-jn-unity-tracy 技能分析本次录制。\n文件：" + task.value("final", "") +
           ( store ? "\n格式：tracy-stream 录制及其事件库（" + task.value("store", "") + "），由 Query 直接读取事件库" : std::string() ) +
           "\n名称：" + task.value("name", "") + "\n场景：" + s.value("scene", "") + "\n操作备注：" +
           s.value("note", "") + "\n完整性：" + task.value("completeness", "未知") + "\n校验证据：" +
           task.value("validation_evidence", "") +
           "\n请先核实输入、实际采集配置、质量限制和分析配置，再进行单 Trace 分析。\n" +
           (task.value("completeness", "") == "RecoverablePrefix"
                ? "只分析已验证前缀，尾部不完整；不能据此得出没有问题的结论。"
                : "");
}
} // namespace capturegui
