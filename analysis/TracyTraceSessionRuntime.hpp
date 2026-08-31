#ifndef __TRACYTRACESESSIONRUNTIME_HPP__
#define __TRACYTRACESESSIONRUNTIME_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionRuntimeIndexSchemaVersion = 1;

struct TraceSessionRuntimeStats
{
    uint64_t domainStates = 0;
    uint64_t scriptFrames = 0;
    uint64_t scriptStackEvents = 0;
    uint64_t stringBytes = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionRuntimeReader
{
public:
    static std::shared_ptr<TraceSessionRuntimeReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionRuntimeStats& Stats() const { return m_stats; }
    std::vector<RuntimeDomainStateDto> DomainStates() const;
    std::vector<ScriptFrameDto> ScriptFrames() const;
    std::vector<ScriptStackEventDto> ScriptStackEvents() const;

private:
    std::filesystem::path m_path;
    std::string m_fingerprint;
    uint64_t m_domainOffset = 0;
    uint64_t m_frameOffset = 0;
    uint64_t m_stackOffset = 0;
    uint64_t m_stringOffset = 0;
    TraceSessionRuntimeStats m_stats;
};

std::filesystem::path TraceSessionRuntimeIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionRuntimeDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionRuntimeStats& stats, std::string& error );
bool AuditTraceSessionRuntimeDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionRuntimeStats& stats, std::string& error );

}

#endif
