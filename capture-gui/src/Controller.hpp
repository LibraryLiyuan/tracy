#pragma once
#include "Process.hpp"
#include <mutex>
#include <thread>
namespace capturegui
{
struct ViewState
{
    std::string state = "ready", message = "准备好场景后开始录制", directory, stage, quality, actualConfig;
    // The current step of the stage, shown with its progress.
    std::string progressText;
    bool busy = false, gameMayClose = false;
    uint64_t elapsedMs = 0, streamBytes = 0, memoryBytes = 0, freeBytes = 0;
    double percent = -1;
    double recordingBytesPerSecond = 0, remainingDiskSeconds = -1;
    uint64_t committedBytes = 0, durableBytes = 0, backpressureWaits = 0;
    Json task = Json::object();
    ProcessIdentity target;
};
class Controller
{
  public:
    ~Controller();
    ViewState Snapshot();
    bool Launch(const Settings &settings);
    // Closes the running game and starts it with the selected INI.
    bool Restart(const Settings &settings, ProcessIdentity running);
    bool Start(const Settings &settings, ProcessIdentity target);
    bool Recover(const fs::path &directory, const Settings *currentTools = nullptr);
    void Stop();
    void ForceClose();
    void Reset();
    static std::string AnalysisRequest(const Json &task);
    static std::vector<Json> History(const fs::path &output);

  private:
    void Spawn(std::function<void()> action);
    void Work(Settings settings, ProcessIdentity target, const fs::path &recover = {});
    void State(const std::string &state, const std::string &message);
    void Persist();
    void Owned(const std::string &role, const Process &process, const fs::path &exe,
               const std::vector<std::string> &args);
    void CheckCancelled();
    std::mutex m_mutex;
    ViewState m_view;
    Json m_task;
    fs::path m_directory;
    std::jthread m_worker;
    std::atomic<bool> m_cancel = false, m_stop = false;
};
} // namespace capturegui
