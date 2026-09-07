#ifndef __TRACYEXACTSTATISTICS_HPP__
#define __TRACYEXACTSTATISTICS_HPP__

#include "TracyNeutralAggregateStore.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tracy::analysis
{

struct ExactDistribution
{
    bool exact = true;
    uint64_t count = 0;
    uint64_t zeroCount = 0;
    int64_t total = 0;
    int64_t min = 0;
    int64_t max = 0;
    double mean = 0;
    double median = 0;
    double mad = 0;
    double p50 = 0;
    double p90 = 0;
    double p95 = 0;
    double p99 = 0;
    std::string unavailableReason;
};

enum class AnomalyPattern : uint8_t
{
    None,
    IsolatedSpike,
    RecurrentSpike,
    BurstWindow,
    PersistentPressure
};

struct AnomalyInstance
{
    uint64_t frameIndex = 0;
    int64_t valueNs = 0;
    double deltaFromMedianNs = 0;
};

inline constexpr size_t NeutralMaximumRepresentativeAnomalies = 32;

struct NeutralMetricAggregate
{
    ExactDistribution perCompleteFrame;
    ExactDistribution whenPresent;
    AnomalyPattern pattern = AnomalyPattern::None;
    std::vector<AnomalyInstance> anomalies;
    uint64_t anomalyCount = 0;
    bool anomaliesComplete = true;
    uint64_t longestBurstFrames = 0;
    std::optional<uint64_t> longestBurstStartFrame;
    std::optional<uint64_t> longestBurstEndFrame;
    std::optional<uint64_t> longestBurstPeakFrame;
    std::optional<uint64_t> periodFrames;
};

struct NeutralSignatureFrameInput
{
    std::string domain;
    std::string signatureId;
    std::string frameScope;
    uint64_t frameIndex = 0;
    int64_t inclusiveNs = 0;
    int64_t exclusiveNs = 0;
    int64_t waitNs = 0;
    int64_t criticalPathNs = 0;
    uint64_t occurrenceCount = 0;
    bool exact = true;
    bool logical = false;
};

struct NeutralSignatureDenominatorInput
{
    std::string domain;
    std::string signatureId;
    std::string frameScope;
    uint64_t completeFrameCount = 0;
};

struct NeutralDomainAuditInput
{
    std::string domain;
    bool present = false;
    std::string status = "absent";
    uint64_t inputCount = 0;
    uint64_t consumedInputCount = 0;
    uint64_t outputCount = 0;
    std::string inputChecksum;
    std::string consumedChecksum;
    bool qualityComplete = true;
    std::string unavailableReason;
};

struct NeutralStatisticsInput
{
    std::filesystem::path temporaryRoot;
    uint64_t maximumBufferedValues = 65536;
    std::vector<NeutralSignatureFrameInput> runs;
    std::vector<NeutralSignatureDenominatorInput> denominators;
    std::vector<NeutralDomainAuditInput> domainAudit;
};

struct NeutralSignatureAggregate
{
    std::string domain;
    std::string signatureId;
    std::string frameScope;
    bool logical = false;
    bool exact = true;
    uint64_t completeFrameCount = 0;
    uint64_t presentFrameCount = 0;
    uint64_t occurrenceCount = 0;
    uint64_t unknownFrameCount = 0;
    NeutralMetricAggregate inclusive;
    NeutralMetricAggregate exclusive;
    NeutralMetricAggregate wait;
    NeutralMetricAggregate criticalPath;
};

struct NeutralRankingEntry
{
    std::string signatureId;
    std::string frameScope;
    bool logical = false;
    int64_t totalNs = 0;
    int64_t waitNs = 0;
    int64_t criticalPathNs = 0;
    double contribution = 0;
    double cumulativeContribution = 0;
};

struct NeutralDomainRanking
{
    std::string domain;
    std::vector<NeutralRankingEntry> inclusive;
    std::vector<NeutralRankingEntry> exclusive;
    std::vector<NeutralRankingEntry> waitCritical;
};

struct NeutralDomainAuditResult
{
    std::string domain;
    bool present = false;
    std::string status;
    uint64_t inputCount = 0;
    uint64_t consumedInputCount = 0;
    uint64_t outputCount = 0;
    uint64_t actualOutputCount = 0;
    std::string inputChecksum;
    std::string consumedChecksum;
    bool qualityComplete = false;
    std::string unavailableReason;
};

struct NeutralStatisticsResult
{
    bool qualityComplete = false;
    uint64_t unreportedGapCount = 0;
    uint64_t maximumBufferedValuesObserved = 0;
    std::string contentSha256;
    std::vector<std::string> qualityFindings;
    std::vector<NeutralSignatureAggregate> signatures;
    std::vector<NeutralDomainRanking> rankings;
    std::vector<NeutralDomainAuditResult> domains;
};

struct NeutralMergedFrameValues
{
    uint64_t frameIndex = 0;
    int64_t inclusiveNs = 0;
    int64_t exclusiveNs = 0;
    int64_t waitNs = 0;
    int64_t criticalPathNs = 0;
    uint64_t occurrenceCount = 0;
    bool exact = true;
    bool logical = false;
};

using NeutralSignatureFramesSink = std::function<void(
    const NeutralSignatureAggregate&, const std::vector<NeutralMergedFrameValues>& )>;

// Exact, disk-backed SignatureFrameRun aggregation. Only a bounded raw-run
// buffer and one signature's merged frame values are resident at a time.
class NeutralStatisticsStreamBuilder
{
public:
    using SignatureToken = uint32_t;
    static constexpr SignatureToken InvalidSignatureToken = UINT32_MAX;

    NeutralStatisticsStreamBuilder( std::filesystem::path temporaryRoot,
        uint64_t maximumBufferedValues = 65536, size_t maximumBufferedRuns = 65536 );
    ~NeutralStatisticsStreamBuilder();
    NeutralStatisticsStreamBuilder( NeutralStatisticsStreamBuilder&& ) noexcept;
    NeutralStatisticsStreamBuilder& operator=( NeutralStatisticsStreamBuilder&& ) noexcept;
    NeutralStatisticsStreamBuilder( const NeutralStatisticsStreamBuilder& ) = delete;
    NeutralStatisticsStreamBuilder& operator=( const NeutralStatisticsStreamBuilder& ) = delete;

    bool AddRun( const NeutralSignatureFrameInput& value );
    SignatureToken RegisterSignature( std::string_view domain,
        std::string_view signatureId, std::string_view frameScope );
    bool AddRegisteredRun( SignatureToken signature, uint64_t frameIndex,
        int64_t inclusiveNs, int64_t exclusiveNs, int64_t waitNs,
        int64_t criticalPathNs, uint64_t occurrenceCount,
        bool exact, bool logical );
    void AddDenominator( const NeutralSignatureDenominatorInput& value );
    void AddDomainAudit( const NeutralDomainAuditInput& value );
    bool Finish( NeutralStatisticsResult& result, std::string& error,
        const NeutralSignatureFramesSink& signatureSink = {},
        const std::function<bool()>& cancelled = {} );

    size_t MaximumBufferedRunsObserved() const;
    uint64_t InputRunCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

ExactDistribution ComputeExactDistributionExternal(
    const std::vector<int64_t>& explicitValues,
    uint64_t implicitZeros,
    const std::filesystem::path& temporaryRoot,
    const std::string& prefix,
    uint64_t maximumBufferedValues );

NeutralStatisticsResult BuildNeutralStatistics( const NeutralStatisticsInput& input );

std::string SerializeNeutralStatisticsResult( const NeutralStatisticsResult& result );
bool DeserializeNeutralStatisticsResult( const std::string& payload,
    NeutralStatisticsResult& result, std::string& error );

bool PublishNeutralStatistics(
    const std::filesystem::path& storeRoot,
    const NeutralAggregateIdentity& identity,
    const std::string& generation,
    const NeutralStatisticsResult& result,
    NeutralAggregateManifest& manifest,
    std::string& error );

const char* AnomalyPatternName( AnomalyPattern pattern );

}

#endif
