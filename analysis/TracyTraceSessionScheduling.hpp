#ifndef __TRACYTRACESESSIONSCHEDULING_HPP__
#define __TRACYTRACESESSIONSCHEDULING_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionSchedulingIndexSchemaVersion = 5;

struct TraceSessionSchedulingStats
{
    uint64_t contextSwitchRecords = 0;
    uint64_t wakeupRecords = 0;
    uint64_t threadEvents = 0;
    uint64_t completeThreadEvents = 0;
    uint64_t cpuEvents = 0;
    uint64_t completeCpuEvents = 0;
    // Observed scheduler records that cannot form a single non-overlapping
    // timeline. Facts remain queryable and affected intervals stay incomplete.
    uint64_t sourceGapEvents = 0;
    uint64_t topologyRecords = 0;
    uint64_t topologyCpus = 0;
    uint64_t threadSummaries = 0;
    uint64_t threadNameRecords = 0;
    uint64_t tidToPidRecords = 0;
    uint64_t groupHintRecords = 0;
    uint64_t externalNameMetadataRecords = 0;
    uint64_t externalNameRecords = 0;
    uint64_t externalThreadNameRecords = 0;
    uint64_t fiberNameRecords = 0;
    uint64_t fiberEnterRecords = 0;
    uint64_t fiberLeaveRecords = 0;
    uint64_t cpuUsagePoints = 0;
    uint64_t threadBlocks = 0;
    uint64_t cpuBlocks = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionSchedulingReader
{
public:
    static std::shared_ptr<TraceSessionSchedulingReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionSchedulingStats& Stats() const { return m_stats; }
    std::vector<ContextSwitchDto> ScanThreads( const ScanRange& range ) const;
    std::vector<ContextSwitchDto> ScanThread( std::string_view threadRef,
        const ScanRange& range ) const;
    std::vector<CpuContextSwitchDto> ScanCpus( const ScanRange& range ) const;
    std::vector<CpuContextSwitchDto> ScanCpu( uint32_t cpu,
        const ScanRange& range ) const;
    const std::vector<CpuTopologyDto>& CpuTopology() const;
    const std::vector<ThreadDto>& Threads() const;
    std::vector<CpuUsagePointDto> ScanCpuUsage( size_t offset, size_t limit ) const;

private:
    struct Impl;
    explicit TraceSessionSchedulingReader( std::shared_ptr<Impl> impl );
    std::vector<ContextSwitchDto> ScanThreadsImpl( const ScanRange& range,
        std::optional<uint64_t> thread ) const;
    std::vector<CpuContextSwitchDto> ScanCpusImpl( const ScanRange& range,
        std::optional<uint32_t> cpu ) const;
    std::shared_ptr<Impl> m_impl;
    TraceSessionSchedulingStats m_stats;
};

std::filesystem::path TraceSessionSchedulingIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionSchedulingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionSchedulingStats& stats, std::string& error );
bool AuditTraceSessionSchedulingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionSchedulingStats& stats, std::string& error );

}

#endif
