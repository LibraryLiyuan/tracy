#ifndef __TRACYTRACESESSIONRELATIONS_HPP__
#define __TRACYTRACESESSIONRELATIONS_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionRelationIndexSchemaVersion = 1;

struct TraceSessionRelationStats
{
    uint64_t relations = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionRelationReader
{
public:
    static std::shared_ptr<TraceSessionRelationReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionRelationStats& Stats() const { return m_stats; }
    std::vector<RelationDto> Scan( size_t offset, size_t limit ) const;

private:
    std::filesystem::path m_path;
    uint64_t m_recordsOffset = 0;
    std::string m_fingerprint;
    TraceSessionRelationStats m_stats;
};

std::filesystem::path TraceSessionRelationIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionRelationDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionRelationStats& stats, std::string& error );
bool AuditTraceSessionRelationDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionRelationStats& stats, std::string& error );

}

#endif
