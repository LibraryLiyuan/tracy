#ifndef TRACY_QUERY_STRING_STORE_HPP
#define TRACY_QUERY_STRING_STORE_HPP

#include "TracyPagedRecordStore.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace tracy::query
{

enum class QueryStringPointerKind : uint8_t { String = 0, Thread = 1, External = 2 };

class QueryStringStoreWriter
{
public:
    QueryStringStoreWriter( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {}, uint64_t maximumBufferedPairs = 1024 * 1024 );
    ~QueryStringStoreWriter();
    QueryStringStoreWriter( const QueryStringStoreWriter& ) = delete;
    QueryStringStoreWriter& operator=( const QueryStringStoreWriter& ) = delete;
    void StringsBegin( uint64_t count );
    void StringBegin( uint64_t ordinal, uint64_t token, uint64_t bytes );
    void StringBytes( uint64_t ordinal, uint64_t offset, const void* data, uint64_t bytes );
    void StringEnd( uint64_t ordinal );
    void StringsEnd();
    void PointerTableBegin( QueryStringPointerKind kind, uint64_t count );
    void PointerRecord( QueryStringPointerKind kind, uint64_t ordinal, uint64_t id, uint64_t firstToken, uint64_t secondToken );
    void PointerTableEnd( QueryStringPointerKind kind );
    void Commit();
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};

class QueryStringStoreReader
{
public:
    QueryStringStoreReader( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QueryStringStoreReader();
    QueryStringStoreReader( const QueryStringStoreReader& ) = delete;
    QueryStringStoreReader& operator=( const QueryStringStoreReader& ) = delete;
    uint64_t StringCount() const;
    uint64_t PointerCount( QueryStringPointerKind kind ) const;
    uint64_t StringLength( uint64_t ordinal ) const;
    std::string ReadOrdinal( uint64_t ordinal, uint64_t maximumBytes ) const;
    std::optional<std::string> ResolveToken( uint64_t token, uint64_t maximumBytes ) const;
    std::optional<std::string> ResolvePointer( QueryStringPointerKind kind, uint64_t id, uint64_t maximumBytes ) const;
    std::optional<std::pair<std::string, std::string>> ResolveExternal( uint64_t id, uint64_t maximumBytesPerValue ) const;
    void VisitOrdinalBytes( uint64_t ordinal, uint64_t maximumChunkBytes,
        const std::function<void( const void*, uint64_t )>& visitor ) const;
    void ValidateAll() const;
private:
    struct Impl; std::unique_ptr<Impl> m_impl;
};

}
#endif
