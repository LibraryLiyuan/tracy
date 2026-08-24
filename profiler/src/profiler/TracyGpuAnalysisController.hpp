#ifndef __TRACYGPUANALYSISCONTROLLER_HPP__
#define __TRACYGPUANALYSISCONTROLLER_HPP__

#include "TracyGpuAnalysis.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace tracy
{

class Worker;

enum class GpuAnalysisControllerState : uint8_t
{
    Idle,
    Building,
    NotPresent,
    Ready,
    Partial,
    Cancelled,
    Failed
};

struct GpuAnalysisControllerStatus
{
    GpuAnalysisControllerState state = GpuAnalysisControllerState::Idle;
    float progress = 0;
    std::string stage;
    std::string error;
    bool cacheHit = false;
    uint64_t estimatedBytes = 0;
};

class GpuAnalysisController
{
public:
    GpuAnalysisController( Worker& worker, std::filesystem::path tracePath );
    ~GpuAnalysisController();

    void Start();
    void Cancel();
    void Retry();
    bool ClearCache( std::string& error );

    GpuAnalysisControllerStatus Status() const;
    std::shared_ptr<const analysis::GpuAnalysisSnapshot> Snapshot() const;
    const std::filesystem::path& CachePath() const { return m_cachePath; }

private:
    void PublishStatus( GpuAnalysisControllerState state, float progress, std::string stage, std::string error = {} );
    void Run( std::stop_token stopToken );

    Worker& m_worker;
    std::filesystem::path m_tracePath;
    std::filesystem::path m_cachePath;
    mutable std::mutex m_mutex;
    GpuAnalysisControllerStatus m_status;
    std::shared_ptr<const analysis::GpuAnalysisSnapshot> m_snapshot;
    std::jthread m_thread;
};

const char* GpuAnalysisControllerStateName( GpuAnalysisControllerState value );

}

#endif
