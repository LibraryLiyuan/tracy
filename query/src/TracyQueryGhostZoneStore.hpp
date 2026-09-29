#ifndef __TRACYQUERYGHOSTZONESTORE_HPP__
#define __TRACYQUERYGHOSTZONESTORE_HPP__

#include "TracyQueryStringStore.hpp"
#include "TracyPagedRecordStore.hpp"
#include "TracyTraceSource.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tracy::query
{

inline constexpr uint32_t QueryGhostZoneStoreRecordBytes = 72;

struct QueryGhostZoneRow
{
    uint64_t ordinal = 0;
    uint64_t thread = 0;
    std::optional<uint64_t> parent;
    int64_t startNs = 0;
    int64_t endNs = 0;
    std::string name;
    std::string file;
    uint32_t line = 0;
    std::string address;
    uint32_t depth = 0;
    uint32_t childCount = 0;
    bool inlineFrame = false;
};

struct QueryGhostZoneBuildStatistics
{
    uint64_t pathRequests = 0;
    uint64_t pathResolutions = 0;
    uint64_t pathCacheHits = 0;
    uint64_t pathCacheEvictions = 0;
    uint64_t pathCachePeakBytes = 0;
    uint64_t scratchRecordReads = 0;
    uint64_t scratchRecordWrites = 0;
    uint64_t scratchWriteCalls = 0;
    uint64_t finalJoinBatches = 0;
    uint64_t stringBlockLoads = 0, frameBlockLoads = 0, callstackBlockLoads = 0;
    uint64_t stringDecodedBytes = 0, frameDecodedBytes = 0, callstackDecodedBytes = 0;
};

struct QueryGhostZoneFact
{
    int64_t startNs = 0, endNs = 0;
    uint64_t thread = 0, parent = UINT64_MAX;
    uint64_t name = UINT64_MAX, file = UINT64_MAX, address = 0;
    uint32_t line = 0, depth = 0, childCount = 0;
    bool inlineFrame = false;
};

class QueryGhostZoneStoreWriter
{
public:
    QueryGhostZoneStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryGhostZoneStoreWriter();
    QueryGhostZoneStoreWriter( const QueryGhostZoneStoreWriter& ) = delete;
    QueryGhostZoneStoreWriter& operator=( const QueryGhostZoneStoreWriter& ) = delete;

    void Begin( uint64_t count );
    void Append( uint64_t ordinal, const QueryGhostZoneFact& value );
    void AppendBatch( uint64_t ordinal, const QueryGhostZoneFact* values, size_t count );
    void Commit();
    uint64_t Count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class QueryGhostZoneStoreReader
{
public:
    QueryGhostZoneStoreReader( std::filesystem::path root, std::string sourceFingerprint,
        std::filesystem::path globalStrings, analysis::PagedRecordOptions options = {} );
    ~QueryGhostZoneStoreReader();
    QueryGhostZoneStoreReader( const QueryGhostZoneStoreReader& ) = delete;
    QueryGhostZoneStoreReader& operator=( const QueryGhostZoneStoreReader& ) = delete;

    uint64_t Count() const;
    std::vector<QueryGhostZoneRow> Scan( int64_t startNs, int64_t endNs, uint64_t offset, size_t limit ) const;
    std::vector<QueryGhostZoneRow> ScanOrdinals( uint64_t offset, size_t limit,
        int64_t startNs = std::numeric_limits<int64_t>::min(),
        int64_t endNs = std::numeric_limits<int64_t>::max() ) const;
    analysis::PagedRecordReader::ReadStats ReadStatistics() const;
    void ValidateAll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Rebuilds Ghost Zone facts from the persisted Sample/Callstack/Frame/String
// sidecars without reopening a Worker with the full Sample table resident.
uint64_t BuildQueryGhostZoneStore( std::filesystem::path output,
    std::filesystem::path samples, std::filesystem::path callstacks,
    std::filesystem::path callstackFrames, std::filesystem::path strings,
    std::string sourceFingerprint, int64_t samplingPeriodNs,
    analysis::PagedRecordOptions options = {}, QueryGhostZoneBuildStatistics* statistics = nullptr );

}

#endif
