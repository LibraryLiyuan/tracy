#ifndef __TRACYTRACESESSIONSCHEDULING_HPP__
#define __TRACYTRACESESSIONSCHEDULING_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionSchedulingIndexSchemaVersion = 1;

struct TraceSessionSchedulingStats
{
    uint64_t contextSwitchRecords = 0;
    uint64_t wakeupRecords = 0;
    uint64_t threadEvents = 0;
    uint64_t completeThreadEvents = 0;
    uint64_t cpuEvents = 0;
    uint64_t completeCpuEvents = 0;
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
    std::vector<CpuContextSwitchDto> ScanCpus( const ScanRange& range ) const;

private:
    struct Impl;
    explicit TraceSessionSchedulingReader( std::shared_ptr<Impl> impl );
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
