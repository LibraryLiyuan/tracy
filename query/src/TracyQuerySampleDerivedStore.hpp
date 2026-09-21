#ifndef __TRACYQUERYSAMPLEDERIVEDSTORE_HPP__
#define __TRACYQUERYSAMPLEDERIVEDSTORE_HPP__

#include "TracyPagedRecordStore.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace tracy::query
{

struct QuerySampleSymbolStatsRow
{
    uint64_t address = 0;
    uint32_t inclusiveSamples = 0;
    uint32_t exclusiveSamples = 0;
    uint64_t childSamples = 0;
};

uint64_t BuildQuerySampleDerivedStore( std::filesystem::path output,
    std::filesystem::path samples, std::filesystem::path callstacks,
    std::filesystem::path callstackFrames, std::string sourceFingerprint,
    analysis::PagedRecordOptions options = {} );

class QuerySampleDerivedStoreReader
{
public:
    QuerySampleDerivedStoreReader( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QuerySampleDerivedStoreReader();
    QuerySampleDerivedStoreReader( const QuerySampleDerivedStoreReader& ) = delete;
    QuerySampleDerivedStoreReader& operator=( const QuerySampleDerivedStoreReader& ) = delete;

    uint64_t Count() const;
    uint64_t ChildSampleSymbolCount() const;
    uint64_t ChildSampleCount() const;
    std::optional<QuerySampleSymbolStatsRow> Find( uint64_t address ) const;
    void ValidateAll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
