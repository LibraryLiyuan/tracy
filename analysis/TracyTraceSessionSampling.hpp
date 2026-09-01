#ifndef __TRACYTRACESESSIONSAMPLING_HPP__
#define __TRACYTRACESESSIONSAMPLING_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionSamplingIndexSchemaVersion = 2;

struct TraceSessionSamplingStats
{
    uint64_t events = 0;
    uint64_t samples = 0;
    uint64_t contextSwitchSamples = 0;
    uint64_t dictionaryEntries = 0;
    uint64_t callstackPayloads = 0;
    uint64_t hardwareEvents = 0;
    uint64_t hardwareAddresses = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionSamplingReader
{
public:
    static std::shared_ptr<TraceSessionSamplingReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionSamplingStats& Stats() const { return m_stats; }
    std::vector<SampleDto> Scan( const ScanRange& range ) const;
    std::vector<HardwareSampleDto> HardwareSamples() const;
    std::vector<HardwareSampleEventDto> HardwareSampleEvents(
        uint64_t address, std::string_view kind, size_t offset, size_t limit ) const;

private:
    struct Impl;
    explicit TraceSessionSamplingReader( std::shared_ptr<Impl> impl );
    std::shared_ptr<Impl> m_impl;
    TraceSessionSamplingStats m_stats;
};

std::filesystem::path TraceSessionSamplingIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionSamplingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionSamplingStats& stats, std::string& error );
bool AuditTraceSessionSamplingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionSamplingStats& stats, std::string& error );

}

#endif
