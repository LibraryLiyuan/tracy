#ifndef __TRACYCANDIDATEPOLICYCACHE_HPP__
#define __TRACYCANDIDATEPOLICYCACHE_HPP__
#include "TracyCandidatePolicy.hpp"
#include "TracyAnalysisCacheTable.hpp"
#include "TracyNeutralStatisticsCache.hpp"
namespace tracy::analysis
{
struct CandidatePolicyCacheOptions
{
    AnalysisCacheTableOptions table;
    uint64_t sortBufferBytes = 8*1024*1024;
    uint64_t familyBytes = 4*1024*1024;
    uint64_t frameWorkspaceBytes = 128*1024*1024;
};
// metadata must not contain signature/ranking/context vectors. The immutable
// neutral cache is the only statistics/context source. No all-result DOM.
std::string BuildCandidatePolicyCache(const std::filesystem::path& root,
    NeutralStatisticsCacheReader& neutral, const CandidatePolicyInput& metadata,
    CandidatePolicyCacheOptions options = {});
class CandidatePolicyCacheReader
{
public:
    CandidatePolicyCacheReader(std::filesystem::path root, std::string policyIdentity,
        CandidatePolicyCacheOptions options = {});
    ~CandidatePolicyCacheReader();
    std::string SummaryJson() const;
    uint64_t SummaryBytes() const;
    std::string ContentSha256() const;
    AnalysisCacheRecord CandidateAt(uint64_t ordinal);
    AnalysisCachePage Candidates(uint64_t ordinal, size_t limit, uint64_t bytes);
    AnalysisCachePage RankedSignatures(uint64_t ordinal, size_t limit, uint64_t bytes);
    std::optional<AnalysisCacheRecord> Candidate(std::string_view candidateId);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
#endif
