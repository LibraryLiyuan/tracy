#ifndef __TRACYANALYSISSCANCACHE_HPP__
#define __TRACYANALYSISSCANCACHE_HPP__
#include "TracyAnalysisCacheTable.hpp"
#include <nlohmann/json.hpp>
namespace tracy::query
{
struct AnalysisScanProducts;
inline constexpr const char* AnalysisScanCacheFormatId = "query-analysis-cache-v1";
// The generation directory must be fresh. Its bundle header is published last,
// after checked neutral tables, per-record metadata, and the frame-series file.
std::string PublishAnalysisNeutralBundle(const std::filesystem::path& generation,
    const std::string& identity, AnalysisScanProducts& products,
    analysis::AnalysisCacheTableOptions options = {});
AnalysisScanProducts OpenAnalysisNeutralBundle(const std::filesystem::path& generation,
    const std::string& identity, const std::string& headerSha256,
    analysis::AnalysisCacheTableOptions options = {}, uint64_t metadataBytes = 128*1024*1024);
}
#endif
