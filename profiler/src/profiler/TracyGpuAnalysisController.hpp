#ifndef __TRACYGPUANALYSISCONTROLLER_HPP__
#define __TRACYGPUANALYSISCONTROLLER_HPP__

#include "TracyGpuAnalysisStore.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    size_t ResourcePageCount() const;
    size_t AllocationPageCount() const;
    size_t PassPageCount() const;
    uint64_t ResourceCount() const;
    uint64_t AllocationCount() const;
    uint64_t PassCount() const;
    uint64_t ResidencyCount() const;
    size_t ResourcePage() const;
    size_t AllocationPage() const;
    size_t PassPage() const;
    bool LoadResourcePage( size_t page, std::string& error );
    bool LoadAllocationPage( size_t page, std::string& error );
    bool LoadPassPage( size_t page, std::string& error );
    bool LoadPassFrame( uint64_t frame, std::string& error );

private:
    void PublishStatus( GpuAnalysisControllerState state, float progress, std::string stage, std::string error = {} );
    void Run( std::stop_token stopToken );
    bool PublishReader( std::shared_ptr<analysis::GpuAnalysisStoreReader> reader, bool cacheHit, std::string& error );
    void RebuildPageSnapshotLocked();
    void StopPageLoad();

    Worker& m_worker;
    std::filesystem::path m_tracePath;
    std::filesystem::path m_cachePath;
    mutable std::mutex m_mutex;
    GpuAnalysisControllerStatus m_status;
    std::shared_ptr<analysis::GpuAnalysisStoreReader> m_reader;
    std::shared_ptr<const analysis::GpuAnalysisSnapshot> m_snapshot;
    std::vector<analysis::GpuResourceAnalysisRecord> m_resourcePageData;
    std::vector<analysis::GpuAllocationAnalysisRecord> m_allocationPageData;
    std::vector<analysis::GpuPassWorkingSet> m_passPageData;
    size_t m_resourcePage = 0;
    size_t m_allocationPage = 0;
    size_t m_passPage = 0;
    uint64_t m_passFrame = 0;
    bool m_passFrameView = false;
    bool m_pageLoading = false;
    std::atomic<bool> m_cancelBuilderOnStop { false };
    std::jthread m_thread;
    std::jthread m_pageThread;
};

const char* GpuAnalysisControllerStateName( GpuAnalysisControllerState value );

}

#endif
