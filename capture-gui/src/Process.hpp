#pragma once
#include "Core.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
namespace capturegui
{
class Process
{
  public:
    Process() = default;
    ~Process();
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;
    void Start(const fs::path &exe, const std::vector<std::string> &args, const fs::path &log,
               bool pipe = false);
    void Adopt(const ProcessIdentity &identity);
    bool Running() const;
    DWORD ExitCode() const;
    uint64_t Memory() const;
    void Kill();
    ProcessIdentity Identity() const
    {
        return m_identity;
    }
    void Send(const std::string &text);
    std::string Line(std::atomic<bool> &cancel, std::chrono::seconds timeout);

  private:
    HANDLE m_process = nullptr, m_in = nullptr, m_out = nullptr;
    ProcessIdentity m_identity;
    std::string m_buffer;
};
ProcessIdentity LaunchPlayer(const Settings &settings);
class Mcp
{
  public:
    Mcp(Process &process, std::atomic<bool> &cancel, const fs::path &evidence)
        : m_process(process), m_cancel(cancel), m_evidence(evidence)
    {
    }
    Json Request(const std::string &method, const Json &params);
    Json Tool(const std::string &name, const Json &args);
    void Initialize();

  private:
    Process &m_process;
    std::atomic<bool> &m_cancel;
    fs::path m_evidence;
    int m_next = 0;
};
} // namespace capturegui
