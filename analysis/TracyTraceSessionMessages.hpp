#ifndef __TRACYTRACESESSIONMESSAGES_HPP__
#define __TRACYTRACESESSIONMESSAGES_HPP__

#include "TracyTraceSessionStore.hpp"
#include "TracyTraceSource.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t TraceSessionMessageIndexSchemaVersion = 1;
inline constexpr size_t TraceSessionMessageEventKindCount = 9;

struct TraceSessionMessageStats
{
    uint64_t messages = 0;
    uint64_t appInfoEvents = 0;
    uint64_t literalStrings = 0;
    uint64_t fileBytes = 0;
    std::array<uint64_t, TraceSessionMessageEventKindCount> eventCounts {};
};

class TraceSessionMessageReader
{
public:
    static std::shared_ptr<TraceSessionMessageReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionMessageStats& Stats() const { return m_stats; }
    std::vector<MessageDto> Scan( const ScanRange& range ) const;

private:
    struct Impl;
    explicit TraceSessionMessageReader( std::shared_ptr<Impl> impl );
    std::shared_ptr<Impl> m_impl;
    TraceSessionMessageStats m_stats;
};

std::filesystem::path TraceSessionMessageIndexRoot(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest );
bool BuildTraceSessionMessageDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionMessageStats& stats,
    std::string& error );
bool AuditTraceSessionMessageDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionMessageStats& stats,
    std::string& error );

}

#endif
