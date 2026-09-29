#ifndef __TRACYANALYSISDISKBUDGET_HPP__
#define __TRACYANALYSISDISKBUDGET_HPP__
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace tracy::analysis
{
struct AnalysisDiskUsageSnapshot
{
    uint64_t currentBytes=0,peakBytes=0,reservedBytes=0,activeOperations=0,rejectedGrowths=0;
};

// Logical live file bytes in the analysis namespaces, including unpublished
// work. This is cooperative accounting, not an OS quota or allocated clusters.
// No per-record/per-file registry is retained. Recount only at an idle operation
// boundary; concurrent operations in one Manager share the incremental ledger.
class AnalysisDiskUsage
{
public:
    explicit AnalysisDiskUsage(std::vector<std::filesystem::path> roots)
    {
        for(auto& root:roots)
        {
            root=std::filesystem::absolute(root).lexically_normal();
            if(std::find(m_roots.begin(),m_roots.end(),root)==m_roots.end()) m_roots.emplace_back(std::move(root));
        }
    }
    static uint64_t Bytes(const std::filesystem::path& path)
    {
        if(!std::filesystem::exists(path)) return 0;
        if(std::filesystem::is_regular_file(path)) return std::filesystem::file_size(path);
        uint64_t bytes=0;
        for(const auto& file:std::filesystem::recursive_directory_iterator(path))
        {
            if(!file.is_regular_file()) continue;
            const auto size=file.file_size();
            if(size>UINT64_MAX-bytes) throw std::runtime_error("analysis_scan_cache_disk_budget");
            bytes+=size;
        }
        return bytes;
    }
    void Begin(uint64_t controlReserve)
    {
        std::lock_guard lock(m_mutex);
        if(!m_active)
        {
            uint64_t bytes=0;
            for(const auto& root:m_roots)
            {
                const auto count=Bytes(root);
                if(count>UINT64_MAX-bytes) throw std::runtime_error("analysis_scan_cache_disk_budget");
                bytes+=count;
            }
            m_bytes=bytes;
            m_peak=std::max(m_peak,m_bytes);
        }
        if(controlReserve>UINT64_MAX-m_reserved) throw std::runtime_error("analysis_scan_cache_disk_budget");
        ++m_active; m_reserved+=controlReserve;
    }
    void End(uint64_t controlReserve)
    { std::lock_guard lock(m_mutex); --m_active; m_reserved-=controlReserve; }
    void AddRoots(const std::vector<std::filesystem::path>& roots,uint64_t limit=UINT64_MAX)
    {
        std::lock_guard lock(m_mutex);
        std::vector<std::filesystem::path> added;
        uint64_t bytes=0;
        for(auto root:roots)
        {
            root=std::filesystem::absolute(root).lexically_normal();
            if(std::find(m_roots.begin(),m_roots.end(),root)!=m_roots.end() ||
                std::find(added.begin(),added.end(),root)!=added.end()) continue;
            const auto count=Bytes(root);
            if(count>UINT64_MAX-bytes) { ++m_rejected; throw std::runtime_error("analysis_scan_cache_disk_budget"); }
            bytes+=count; added.emplace_back(std::move(root));
        }
        if(m_bytes>limit || m_reserved>limit-m_bytes || bytes>limit-m_bytes-m_reserved)
        { ++m_rejected; throw std::runtime_error("analysis_scan_cache_disk_budget"); }
        m_roots.insert(m_roots.end(),std::make_move_iterator(added.begin()),std::make_move_iterator(added.end()));
        m_bytes+=bytes; m_peak=std::max(m_peak,m_bytes);
    }
    void Grow(uint64_t bytes,uint64_t limit=UINT64_MAX,bool respectReserve=true)
    {
        std::lock_guard lock(m_mutex);
        const auto reserved=respectReserve?m_reserved:0;
        if(m_bytes>limit || reserved>limit-m_bytes || bytes>limit-m_bytes-reserved)
        {
            ++m_rejected;
            throw std::runtime_error("analysis_scan_cache_disk_budget");
        }
        m_bytes+=bytes;
        m_peak=std::max(m_peak,m_bytes);
    }
    void Release(uint64_t bytes)
    { std::lock_guard lock(m_mutex); m_bytes=bytes>m_bytes?0:m_bytes-bytes; }
    uint64_t Current() const { std::lock_guard lock(m_mutex); return m_bytes; }
    AnalysisDiskUsageSnapshot Snapshot() const
    {
        std::lock_guard lock(m_mutex);
        return {m_bytes,m_peak,m_reserved,m_active,m_rejected};
    }
private:
    std::vector<std::filesystem::path> m_roots;
    mutable std::mutex m_mutex;
    uint64_t m_bytes=0,m_peak=0,m_reserved=0,m_active=0,m_rejected=0;
};
class AnalysisDiskActivity
{
public:
    AnalysisDiskActivity(std::shared_ptr<AnalysisDiskUsage> usage,uint64_t controlReserve=0)
        :m_usage(std::move(usage)),m_reserve(controlReserve) { if(m_usage) m_usage->Begin(m_reserve); }
    ~AnalysisDiskActivity() { if(m_usage) m_usage->End(m_reserve); }
    AnalysisDiskActivity(const AnalysisDiskActivity&)=delete;
    AnalysisDiskActivity& operator=(const AnalysisDiskActivity&)=delete;
private:
    std::shared_ptr<AnalysisDiskUsage> m_usage;
    uint64_t m_reserve;
};
struct AnalysisDiskBudget
{
    struct TraceLimits
    {
        uint64_t finalBytes;
        uint64_t peakBytes;
        void CheckFinal(uint64_t bytes) const
        {
            if(bytes>finalBytes) throw std::runtime_error("analysis_final_disk_ratio");
        }
    };
    static TraceLimits ForTrace(uint64_t bytes)
    {
        if(!bytes || bytes>UINT64_MAX/16) throw std::runtime_error("analysis_disk_trace_size");
        return {bytes*8,bytes*16};
    }
    std::shared_ptr<AnalysisDiskUsage> usage;
    uint64_t maximumBytes=UINT64_MAX;
    std::function<void()> checkSpace;
    std::function<void(uint64_t)> onReject;
    void Grow(uint64_t bytes) const
    {
        try
        {
            if(checkSpace) checkSpace();
            if(usage) usage->Grow(bytes,maximumBytes);
        }
        catch(...)
        {
            if(onReject) try { onReject(bytes); } catch(...) {}
            throw;
        }
    }
};
inline void AnalysisDiskGrow(const std::shared_ptr<AnalysisDiskBudget>& budget,uint64_t bytes)
{ if(budget) budget->Grow(bytes); }

// Call only for files/directories owned by the caller. A failed deletion keeps
// its charge; partial directory cleanup releases only the bytes actually gone.
inline void AnalysisDiskRemove(const std::filesystem::path& path,
    const std::shared_ptr<AnalysisDiskBudget>& budget,std::error_code& error,bool recursive=false)
{
    uint64_t before=0;
    try { if(budget && budget->usage) before=AnalysisDiskUsage::Bytes(path); }
    catch(...) { error=std::make_error_code(std::errc::io_error); return; }
    if(recursive) std::filesystem::remove_all(path,error);
    else std::filesystem::remove(path,error);
    if(!budget || !budget->usage) return;
    try
    {
        const auto remaining=error?AnalysisDiskUsage::Bytes(path):0;
        if(remaining<=before) budget->usage->Release(before-remaining);
    }
    catch(...) {} // Conservative until the next idle recount.
}
}
#endif
