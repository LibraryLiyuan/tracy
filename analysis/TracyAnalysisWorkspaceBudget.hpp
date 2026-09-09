#ifndef __TRACYANALYSISWORKSPACEBUDGET_HPP__
#define __TRACYANALYSISWORKSPACEBUDGET_HPP__
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
namespace tracy::analysis
{
struct AnalysisWorkspaceSnapshot
{
    uint64_t targetBytes=0,maximumBytes=0,currentBytes=0,peakBytes=0,rejectedReservations=0;
};
// Shared conservative accounting for analysis-owned allocations. Reservations
// are made BEFORE growth and follow allocation ownership. This is not an OS
// allocator cap, and does not include the source Worker's loaded trace memory.
class AnalysisWorkspaceBudget
{
public:
    explicit AnalysisWorkspaceBudget(uint64_t maximumBytes=4ull*1024*1024*1024,
        uint64_t targetBytes=2ull*1024*1024*1024)
        :m_maximum(maximumBytes),m_target(targetBytes)
    {
        if(!maximumBytes || maximumBytes>4ull*1024*1024*1024 || targetBytes>maximumBytes)
            throw std::runtime_error("analysis_workspace_configuration");
    }
    AnalysisWorkspaceSnapshot Snapshot() const
    { return {m_target,m_maximum,m_current.load(),m_peak.load(),m_rejected.load()}; }
private:
    friend class AnalysisWorkspaceReservation;
    [[noreturn]] void Reject()
    { ++m_rejected; throw std::runtime_error("analysis_workspace_budget"); }
    void Acquire(uint64_t bytes)
    {
        auto current=m_current.load(std::memory_order_relaxed);
        do { if(bytes>m_maximum-current) Reject(); }
        while(!m_current.compare_exchange_weak(current,current+bytes,std::memory_order_relaxed));
        const auto next=current+bytes;
        auto peak=m_peak.load(std::memory_order_relaxed);
        while(peak<next && !m_peak.compare_exchange_weak(peak,next,std::memory_order_relaxed)) {}
    }
    void Release(uint64_t bytes) noexcept { m_current.fetch_sub(bytes,std::memory_order_relaxed); }
    const uint64_t m_maximum,m_target;
    std::atomic<uint64_t> m_current=0,m_peak=0,m_rejected=0;
};
class AnalysisWorkspaceReservation
{
public:
    explicit AnalysisWorkspaceReservation(std::shared_ptr<AnalysisWorkspaceBudget> budget={},uint64_t bytes=0)
        :m_budget(std::move(budget)) { Resize(bytes); }
    ~AnalysisWorkspaceReservation() { Release(); }
    AnalysisWorkspaceReservation(const AnalysisWorkspaceReservation&)=delete;
    AnalysisWorkspaceReservation& operator=(const AnalysisWorkspaceReservation&)=delete;
    AnalysisWorkspaceReservation(AnalysisWorkspaceReservation&& other) noexcept
        :m_budget(std::move(other.m_budget)),m_bytes(std::exchange(other.m_bytes,0)) {}
    AnalysisWorkspaceReservation& operator=(AnalysisWorkspaceReservation&& other) noexcept
    {
        if(this!=&other) { Release(); m_budget=std::move(other.m_budget); m_bytes=std::exchange(other.m_bytes,0); }
        return *this;
    }
    void Resize(uint64_t bytes)
    {
        if(m_budget) {
            if(bytes>m_bytes) m_budget->Acquire(bytes-m_bytes);
            else m_budget->Release(m_bytes-bytes);
        }
        m_bytes=bytes;
    }
    void Add(uint64_t bytes)
    {
        if(bytes>UINT64_MAX-m_bytes) {
            if(m_budget) m_budget->Reject();
            throw std::runtime_error("analysis_workspace_budget");
        }
        Resize(m_bytes+bytes);
    }
    uint64_t Bytes() const { return m_bytes; }
private:
    void Release() noexcept { if(m_budget) m_budget->Release(m_bytes); m_bytes=0; }
    std::shared_ptr<AnalysisWorkspaceBudget> m_budget;
    uint64_t m_bytes=0;
};
}
#endif
