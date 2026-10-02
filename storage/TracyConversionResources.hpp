#ifndef TRACY_CONVERSION_RESOURCES_HPP
#define TRACY_CONVERSION_RESOURCES_HPP
#include "../analysis/TracyAnalysisWorkspaceBudget.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
namespace tracy::storage
{
inline constexpr uint64_t ConversionDiskRatioLimit=5;
uint64_t ConversionDiskLimit(uint64_t streamBytes,uint64_t absoluteDiskLimit=0);
struct ResourceSample
{
    uint64_t privateBytes=0,workingSetBytes=0,physicalTotal=0,physicalAvailable=0;
    uint64_t peakPrivateBytes=0,peakWorkingSetBytes=0;
    uint64_t systemCommit=0,systemCommitLimit=0;
    uint32_t physicalLoadPercent=0;
};
struct ResourceLimits
{
    uint64_t streamBytes=0,absoluteDiskLimit=0;
    uint64_t targetPrivateBytes=16ull<<30,maximumPrivateBytes=32ull<<30;
    uint64_t workspaceBytes=256ull<<20,commitReserveBytes=2ull<<30,diskReserveBytes=2ull<<30;
    uint64_t clusterBytes=4096;
    uint64_t controlReserveBytes=0;
};
class ConversionResources
{
public:
    using SampleProvider=std::function<ResourceSample()>;
    ConversionResources(ResourceLimits limits,SampleProvider sample,std::function<uint64_t()> freeDisk,std::function<bool()> cancelled={});
    ~ConversionResources();
    void Check(uint64_t nextAllocation=0);
    void CheckFast() const;
    void Cancel() noexcept { m_cancelled=true; }
    void Fail(const char* reason) noexcept;
    const char* Failure() const noexcept;
    bool Cancelled() const noexcept { return m_cancelled.load(std::memory_order_acquire); }
    void StartMonitoring();
    void StopMonitoring() noexcept;
    bool SoftTargetExceeded() const { return m_soft.load(); }
    std::shared_ptr<analysis::AnalysisWorkspaceBudget> Workspace() const { return m_workspace; }
    void BeforeFileGrowth(const std::filesystem::path& path,uint64_t logicalSize);
    void BeforeControlGrowth(const std::filesystem::path& path,uint64_t logicalSize);
    void KeepOwned(const std::filesystem::path& path);
    bool CleanupOwned() noexcept;
    void ReplaceOwned(const std::filesystem::path& from,const std::filesystem::path& to,bool externalDestination=false);
    bool RemoveOwned(const std::filesystem::path& path) noexcept;
    void RenameOwned(const std::filesystem::path& from,const std::filesystem::path& to);
    uint64_t DiskLimit() const { return m_diskLimit; }
    uint64_t DiskCurrent() const;
    uint64_t DiskPeak() const;
    uint64_t PeakPrivate() const { return m_peakPrivate.load(); }
    uint64_t PeakWorkingSet() const { return m_peakWorking.load(); }
    uint64_t PeakSystemCommit() const { return m_peakSystemCommit.load(); }
    uint64_t MinimumCommitHeadroom() const { return m_minCommitHeadroom.load(); }
    struct FileSnapshot { std::filesystem::path path;uint64_t bytes;bool kept; };
    std::vector<FileSnapshot> Files() const;
    static ResourceSample SystemSample();
    static uint64_t VolumeFree(const std::filesystem::path& path);
    static uint64_t VolumeCluster(const std::filesystem::path& path);
    static std::filesystem::path VolumeRoot(const std::filesystem::path& path);
private:
    void CheckSample(const ResourceSample& sample,uint64_t allocation);
    void Grow(const std::filesystem::path&,uint64_t,bool control);
    ResourceLimits m_limits;
    uint64_t m_diskLimit,m_diskCurrent=0,m_diskPeak=0,m_controlUsed=0;
    SampleProvider m_sample;
    std::function<uint64_t()> m_freeDisk;
    std::function<bool()> m_cancelProbe;
    std::shared_ptr<analysis::AnalysisWorkspaceBudget> m_workspace;
    analysis::AnalysisWorkspaceReservation m_registryMemory;
    analysis::AnalysisWorkspaceReservation m_controlMemory;
    uint64_t m_controlRegistryBytes=0;
    bool m_initializing=true;
    mutable std::mutex m_filesLock,m_failureLock;
    struct OwnedFile { uint64_t bytes=0;bool control=false,keep=false; };
    std::map<std::filesystem::path,OwnedFile> m_files;
    char m_failure[256]{};
    std::atomic<bool> m_failed=false,m_cancelled=false,m_stop=false,m_soft=false;
    std::atomic<uint64_t> m_peakPrivate=0,m_peakWorking=0;
    std::atomic<uint64_t> m_peakSystemCommit=0,m_minCommitHeadroom=UINT64_MAX;
    std::thread m_monitor;
};
}
#endif
