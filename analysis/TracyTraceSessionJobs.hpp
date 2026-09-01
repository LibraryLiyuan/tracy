#ifndef __TRACYTRACESESSIONJOBS_HPP__
#define __TRACYTRACESESSIONJOBS_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionJobIndexSchemaVersion = 2;

struct TraceSessionJobBuildOptions
{
    // Bounds the in-memory chunk used by the external distinct Job-ID pass.
    // Job facts themselves are streamed directly to disk.
    uint64_t maximumBufferedRecords = 1024 * 1024;
};

struct TraceSessionJobStats
{
    uint64_t jobTypes = 0;
    uint64_t jobs = 0;
    uint64_t schedules = 0;
    uint64_t configs = 0;
    uint64_t dependencies = 0;
    uint64_t stages = 0;
    uint64_t fileBytes = 0;
    uint64_t peakBufferedRecords = 0;
};

class TraceSessionJobReader
{
public:
    struct PageState;
    static std::shared_ptr<TraceSessionJobReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const std::vector<JobDto>& Jobs() const;
    uint64_t Count() const { return m_stats.jobs; }
    std::vector<JobDto> Scan( size_t offset, size_t limit ) const;
    std::optional<JobDto> Get( uint64_t jobId ) const;
    std::vector<JobDto> Dependents( uint64_t jobId, size_t offset, size_t limit ) const;
    std::vector<JobDto> FrameJobs( uint64_t frameId, size_t offset, size_t limit ) const;
    std::vector<JobDto> HandleJobs( uint64_t packedHandle, size_t offset, size_t limit ) const;
    std::vector<JobDto> SlotJobsNear( uint32_t slotIndex, uint64_t jobId, size_t limit ) const;
    JobLatencyStatisticsDto LatencyStatistics() const;
    const TraceSessionJobStats& Stats() const { return m_stats; }

private:
    std::filesystem::path m_root;
    TraceSessionManifest m_session;
    std::shared_ptr<PageState> m_pageState;
    mutable std::vector<JobDto> m_jobs;
    mutable bool m_jobsLoaded = false;
    TraceSessionJobStats m_stats;
};

std::filesystem::path TraceSessionJobIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionJobDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionJobStats& stats, std::string& error,
    const TraceSessionJobBuildOptions& options = {} );
bool AuditTraceSessionJobDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionJobStats& stats,
    std::string& error );
bool BuildTraceSessionJobPagingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, std::string& error,
    const TraceSessionJobBuildOptions& options = {} );
bool AuditTraceSessionJobPagingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, std::string& error );
bool CleanupTraceSessionJobTemporaryRuns( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, std::string& error );

}

#endif
