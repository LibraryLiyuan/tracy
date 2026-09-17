#ifndef __TRACYQUERYSAMPLESTORE_HPP__
#define __TRACYQUERYSAMPLESTORE_HPP__

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tracy::query
{

inline constexpr uint32_t QuerySampleStoreRecordBytes = 16;

enum class QuerySampleKind : uint8_t
{
    Main = 0,
    ContextSwitch = 1
};

struct QuerySampleRow
{
    uint64_t ordinal = 0;
    uint64_t thread = 0;
    int64_t timeNs = 0;
    uint32_t callstack = 0;
    QuerySampleKind kind = QuerySampleKind::Main;
};

class QuerySampleStoreWriter
{
public:
    QuerySampleStoreWriter( std::filesystem::path root, std::string sourceFingerprint );
    ~QuerySampleStoreWriter();
    QuerySampleStoreWriter( const QuerySampleStoreWriter& ) = delete;
    QuerySampleStoreWriter& operator=( const QuerySampleStoreWriter& ) = delete;

    void Begin( uint64_t thread, QuerySampleKind kind, uint64_t count );
    void Append( uint64_t index, int64_t timeNs, uint32_t callstack );
    void End();
    void Commit();

    uint64_t Count() const;
    uint64_t MainCount() const;
    uint64_t ContextSwitchCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class QuerySampleStoreReader
{
public:
    QuerySampleStoreReader( std::filesystem::path root, std::string sourceFingerprint );
    ~QuerySampleStoreReader();
    QuerySampleStoreReader( const QuerySampleStoreReader& ) = delete;
    QuerySampleStoreReader& operator=( const QuerySampleStoreReader& ) = delete;

    uint64_t Count() const;
    uint64_t MainCount() const;
    uint64_t ContextSwitchCount() const;
    std::vector<QuerySampleRow> Scan( int64_t startNs, int64_t endNs, uint64_t offset, size_t limit,
        std::optional<QuerySampleKind> kind = {} ) const;
    void ValidateAll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
