#ifndef __TRACYANALYSISCACHESORT_HPP__
#define __TRACYANALYSISCACHESORT_HPP__
#include "TracyAnalysisCacheTable.hpp"
namespace tracy::analysis
{
// A bounded adapter for Query result/index rows that arrive out of key order.
// Equal keys are an error, not deduplicated observations.
class AnalysisCacheSortedWriter
{
public:
    AnalysisCacheSortedWriter(std::filesystem::path output, std::string identity,
        std::string kind, AnalysisCacheTableOptions options = {}, uint64_t sortBufferBytes = 8*1024*1024);
    ~AnalysisCacheSortedWriter();
    AnalysisCacheSortedWriter(const AnalysisCacheSortedWriter&) = delete;
    AnalysisCacheSortedWriter& operator=(const AnalysisCacheSortedWriter&) = delete;
    void Append(std::string_view key, std::string_view payload);
    AnalysisCacheTableDescriptor Commit();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
#endif
