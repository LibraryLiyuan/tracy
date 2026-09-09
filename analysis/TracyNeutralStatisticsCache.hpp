#ifndef __TRACYNEUTRALSTATISTICSCACHE_HPP__
#define __TRACYNEUTRALSTATISTICSCACHE_HPP__
#include "TracyAnalysisCacheTable.hpp"
#include "TracyExactStatistics.hpp"
namespace tracy::analysis
{
struct PolicySignatureContext;
struct NeutralStatisticsCacheOptions
{
    AnalysisCacheTableOptions table;
    uint64_t sortBufferBytes = 8*1024*1024;
    uint64_t scopeMetadataBytes = 16*1024*1024;
};
enum class NeutralRankingMetric { Inclusive, Exclusive, WaitCritical };

// Distinct from the legacy aggregate JSON. Child tables may be complete while
// this cache is not: the one-record header is published only after validation.
class NeutralStatisticsCacheWriter
{
public:
    NeutralStatisticsCacheWriter(std::filesystem::path root, std::string identity,
        NeutralStatisticsCacheOptions options = {});
    ~NeutralStatisticsCacheWriter();
    void AppendStatistics(const NeutralSignatureAggregate& signature);
    // One existing Context schema record, including its full path/series refs.
    void AppendContext(std::string_view payload);
    void Commit(const NeutralStatisticsStreamSummary& summary);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
class NeutralStatisticsCacheReader
{
public:
    NeutralStatisticsCacheReader(std::filesystem::path root, std::string identity,
        NeutralStatisticsCacheOptions options = {});
    ~NeutralStatisticsCacheReader();
    uint64_t SignatureCount() const;
    std::string Identity() const;
    std::string ContentSha256() const;
    std::string SummaryJson() const;
    uint64_t SummaryBytes() const;
    AnalysisCacheRecord SignatureAt(uint64_t ordinal);
    std::optional<AnalysisCacheRecord> Signature(std::string_view domain,
        std::string_view signature, std::string_view frameScope);
    std::optional<AnalysisCacheRecord> Context(std::string_view domain,
        std::string_view signature, std::string_view frameScope);
    AnalysisCachePage Signatures(uint64_t ordinal, size_t limit, uint64_t maxResponseBytes);
    AnalysisCachePage Contexts(uint64_t ordinal, size_t limit, uint64_t maxResponseBytes);
    AnalysisCachePage RankingGroups(uint64_t ordinal, size_t limit, uint64_t maxResponseBytes);
    // Sequential merge-join, one decoded pair per callback. A root-only Context
    // has a null statistics pointer. References are valid only in the callback.
    // Readers are thread-confined; callers must not retain references/re-enter.
    uint64_t VisitSignatureContexts(const std::function<void(
        const NeutralSignatureAggregate*, const PolicySignatureContext&)>& sink);
    // nextOrdinal is relative to this (domain, scope, metric), never another group.
    AnalysisCachePage Ranking(std::string_view domain, std::string_view frameScope,
        NeutralRankingMetric metric, uint64_t ordinal, size_t limit, uint64_t maxResponseBytes);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
#endif
