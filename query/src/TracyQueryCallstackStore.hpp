#ifndef TRACY_QUERY_CALLSTACK_STORE_HPP
#define TRACY_QUERY_CALLSTACK_STORE_HPP

#include "TracyPagedRecordStore.hpp"
#include "TracyEvent.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace tracy::query
{

class QueryCallstackStoreWriter
{
public:
    QueryCallstackStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryCallstackStoreWriter();
    QueryCallstackStoreWriter( const QueryCallstackStoreWriter& ) = delete;
    QueryCallstackStoreWriter& operator=( const QueryCallstackStoreWriter& ) = delete;
    void Begin( uint64_t count );
    void CallstackBegin( uint32_t id, uint32_t count );
    void CallstackFrames( uint32_t id, uint32_t offset, const CallstackFrameId* frames, uint32_t count );
    void CallstackEnd( uint32_t id );
    void Commit();
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};

class QueryCallstackStoreReader
{
public:
    QueryCallstackStoreReader( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryCallstackStoreReader();
    QueryCallstackStoreReader( const QueryCallstackStoreReader& ) = delete;
    QueryCallstackStoreReader& operator=( const QueryCallstackStoreReader& ) = delete;
    uint64_t Count() const;
    std::span<const CallstackFrameId> Frames( uint32_t id ) const;
    analysis::PagedRecordReader::ReadStats RecordReadStatistics() const;
    void VisitFrames( uint32_t id, uint32_t maximumFramesPerChunk,
        const std::function<void( const CallstackFrameId*, uint32_t )>& visitor ) const;
    void ValidateAll() const;
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};

struct QueryCallstackFrameDefinition
{
    CallstackFrameId id {};
    StringIdx imageName;
    std::span<const CallstackFrame> frames;
};

class QueryCallstackFrameStoreWriter
{
public:
    QueryCallstackFrameStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {}, uint64_t maximumBufferedPairs = 1024 * 1024 );
    ~QueryCallstackFrameStoreWriter();
    QueryCallstackFrameStoreWriter( const QueryCallstackFrameStoreWriter& ) = delete;
    QueryCallstackFrameStoreWriter& operator=( const QueryCallstackFrameStoreWriter& ) = delete;
    void Begin( uint64_t count );
    void FrameBegin( uint64_t ordinal, CallstackFrameId id, uint8_t count, StringIdx imageName );
    void FrameRecords( uint64_t ordinal, uint32_t offset, const CallstackFrame* frames, uint32_t count );
    void FrameEnd( uint64_t ordinal );
    void Commit();
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};

class QueryCallstackFrameStoreReader
{
public:
    QueryCallstackFrameStoreReader( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryCallstackFrameStoreReader();
    QueryCallstackFrameStoreReader( const QueryCallstackFrameStoreReader& ) = delete;
    QueryCallstackFrameStoreReader& operator=( const QueryCallstackFrameStoreReader& ) = delete;
    uint64_t Count() const;
    std::optional<QueryCallstackFrameDefinition> Find( CallstackFrameId id ) const;
    analysis::PagedRecordReader::ReadStats RecordReadStatistics() const;
    void ValidateAll() const;
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};

}
#endif
