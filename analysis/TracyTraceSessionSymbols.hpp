#ifndef __TRACYTRACESESSIONSYMBOLS_HPP__
#define __TRACYTRACESESSIONSYMBOLS_HPP__

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

inline constexpr uint32_t TraceSessionSymbolIndexSchemaVersion = 1;

struct TraceSessionSymbolStats
{
    uint64_t callstacks = 0;
    uint64_t callstackEntries = 0;
    uint64_t frameAddresses = 0;
    uint64_t inlineFrames = 0;
    uint64_t symbols = 0;
    uint64_t symbolCodeBytes = 0;
    uint64_t sourceCodeFiles = 0;
    uint64_t sourceCodeBytes = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionSymbolReader
{
public:
    static std::shared_ptr<TraceSessionSymbolReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionSymbolStats& Stats() const { return m_stats; }
    std::vector<CallstackFrameDto> ResolveCallstacks(
        const std::vector<uint32_t>& callstacks, size_t maxDepth ) const;
    std::vector<SymbolDto> Symbols() const;
    std::vector<SymbolAddressMappingDto> AddressMappings( size_t offset, size_t limit ) const;
    std::optional<SymbolAddressMappingDto> ResolveAddress( uint64_t address ) const;
    std::vector<SymbolResourceDto> SymbolResources() const;
    SymbolCodeDto ReadSymbolCode( uint64_t symbolId, size_t maxBytes ) const;
    BinaryResourceChunkDto ReadSymbolCodeBytes( uint64_t symbolId,
        size_t offset, size_t maxBytes ) const;

private:
    struct Impl;
    explicit TraceSessionSymbolReader( std::shared_ptr<Impl> impl );
    std::shared_ptr<Impl> m_impl;
    TraceSessionSymbolStats m_stats;
};

std::filesystem::path TraceSessionSymbolIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionSymbolDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionSymbolStats& stats,
    std::string& error );
bool AuditTraceSessionSymbolDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionSymbolStats& stats,
    std::string& error );

}

#endif
