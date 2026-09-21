#ifndef __TRACYQUERYSAMPLEVIEW_HPP__
#define __TRACYQUERYSAMPLEVIEW_HPP__

#include "TracyQuerySampleStore.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tracy::query
{

using QueryContextSwitchStarts = std::function<std::vector<int64_t>( uint64_t thread )>;

void BuildQuerySampleView( const std::filesystem::path& root, std::string sourceFingerprint,
    const QueryContextSwitchStarts& contextSwitchStarts, analysis::PagedRecordOptions options = {} );

class QuerySampleViewReader
{
public:
    QuerySampleViewReader( std::filesystem::path root, std::string sourceFingerprint,
        analysis::PagedRecordOptions options = {} );
    ~QuerySampleViewReader();
    QuerySampleViewReader( const QuerySampleViewReader& ) = delete;
    QuerySampleViewReader& operator=( const QuerySampleViewReader& ) = delete;

    uint64_t Count() const;
    uint64_t MainCount() const;
    uint64_t ContextSwitchCount() const;
    std::vector<QuerySampleRow> Scan( int64_t startNs, int64_t endNs, uint64_t offset, size_t limit,
        std::optional<QuerySampleKind> kind = {} ) const;
    void Visit( const std::function<bool( const QuerySampleRow& )>& visitor ) const;
    void ValidateAll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
