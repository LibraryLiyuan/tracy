#ifndef __TRACYANALYSISSCANCACHE_HPP__
#define __TRACYANALYSISSCANCACHE_HPP__
#include "TracyAnalysisCacheTable.hpp"
#include <nlohmann/json.hpp>
namespace tracy::query
{
struct AnalysisScanProducts;
inline constexpr const char* AnalysisScanCacheFormatId = "query-analysis-cache-v2";
// The generation directory must be fresh. Its bundle header is published last,
// after checked neutral tables, per-record metadata, and the frame-series file.
std::string PublishAnalysisNeutralBundle(const std::filesystem::path& generation,
    const std::string& identity, AnalysisScanProducts& products,
    analysis::AnalysisCacheTableOptions options = {});
AnalysisScanProducts OpenAnalysisNeutralBundle(const std::filesystem::path& generation,
    const std::string& identity, const std::string& headerSha256,
    analysis::AnalysisCacheTableOptions options = {}, uint64_t metadataBytes = 128*1024*1024);
// Internal immutable phase metadata, published beneath the generation lease.
analysis::AnalysisCacheTableDescriptor SaveAnalysisPhaseMetadata(const std::filesystem::path& path,
    const std::string& identity,const AnalysisScanProducts& products,analysis::AnalysisCacheTableOptions options);
AnalysisScanProducts LoadAnalysisPhaseMetadata(const std::filesystem::path& path,
    const std::string& identity,const std::string& contentSha256,analysis::AnalysisCacheTableOptions options);
}
#endif
