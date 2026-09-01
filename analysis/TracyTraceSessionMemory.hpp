#ifndef __TRACYTRACESESSIONMEMORY_HPP__
#define __TRACYTRACESESSIONMEMORY_HPP__

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

inline constexpr uint32_t TraceSessionMemoryIndexSchemaVersion = 2;

struct TraceSessionMemoryStats
{
    uint64_t pools = 0;
    uint64_t events = 0;
    uint64_t eventBlocks = 0;
    uint64_t activeEvents = 0;
    uint64_t allocationEvents = 0;
    uint64_t freeEvents = 0;
    uint64_t discardEvents = 0;
    uint64_t unknownFrees = 0;
    uint64_t fileBytes = 0;
};

class TraceSessionMemoryReader
{
public:
    static std::shared_ptr<TraceSessionMemoryReader> Open(
        const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest,
        std::string& error );

    const TraceSessionMemoryStats& Stats() const { return m_stats; }
    const std::vector<MemoryPoolDto>& Pools() const { return m_pools; }
    std::vector<MemoryEventDto> ScanByStorageOrder( size_t offset, size_t limit ) const;
    std::vector<MemoryEventDto> Scan( const ScanRange& range ) const;
    std::vector<MemoryEventDto> ScanPool( std::string_view poolRef,
        const ScanRange& range ) const;
    std::optional<MemoryEventDto> Get( const MemoryEventKey& key ) const;
    std::optional<std::string> PoolRef( uint64_t nativePool ) const;
    MemoryFrameSnapshot Snapshot( int64_t beginNs, int64_t endNs,
        const std::vector<std::string>& poolRefs, bool allGpuD3D12Pools ) const;

private:
    struct Impl;
    explicit TraceSessionMemoryReader( std::shared_ptr<Impl> impl );
    std::vector<MemoryEventDto> ScanImpl( const ScanRange& range,
        std::optional<size_t> poolIndex ) const;
    std::shared_ptr<Impl> m_impl;
    TraceSessionMemoryStats m_stats;
    std::vector<MemoryPoolDto> m_pools;
};

std::filesystem::path TraceSessionMemoryIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest );
bool BuildTraceSessionMemoryDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionMemoryStats& stats, std::string& error );
bool AuditTraceSessionMemoryDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionMemoryStats& stats, std::string& error );

}

#endif
