#ifndef __TRACYANALYSISCACHEQUERY_HPP__
#define __TRACYANALYSISCACHEQUERY_HPP__
#include "TracyAnalysisCacheTable.hpp"
#include <nlohmann/json.hpp>
namespace tracy::analysis
{
// Thread-confined query view. The caller binds identity to the immutable source
// content and serializes access to the source reader. Filters have durable
// ordinal indexes; only one filter reader is retained by a view.
class AnalysisCacheQuery
{
public:
    AnalysisCacheQuery(std::filesystem::path indexRoot, std::string sourceIdentity,
        uint64_t records, std::function<AnalysisCacheRecord(uint64_t)> read,
        AnalysisCacheTableOptions options = {}, uint64_t responseBytes = 3*1024*1024);
    ~AnalysisCacheQuery();
    nlohmann::json Page(size_t limit, const std::string& cursor,
        const std::vector<std::string>& fields, const nlohmann::json& filter);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
#endif
