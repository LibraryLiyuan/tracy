#ifndef __TRACYCANDIDATEPOLICY_HPP__
#define __TRACYCANDIDATEPOLICY_HPP__

#include "TracyExactStatistics.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace tracy::analysis
{

inline constexpr uint32_t CandidatePolicyAlgorithmVersion = 3;
inline constexpr const char* CandidatePolicyAlgorithmId = "candidate-policy-v3";

enum class CandidateTrigger : uint8_t
{
    Budget,
    Top,
    Anomaly,
    Capacity,
    Quality,
    UserFocus,
    FrameLocal,
    WindowLocal
};

enum class CandidatePriority : uint8_t
{
    P0,
    P1,
    P2,
    P3,
    P4
};

struct PolicyFrameEvidence
{
    uint64_t frameIndex = 0;
    int64_t valueNs = 0;
    std::vector<std::string> eventRefs;
    std::string structureKey;
    bool exact = true;
    std::optional<int64_t> beginNs;
    std::optional<int64_t> endNs;
};

struct PolicySignatureContext
{
    std::string domain;
    std::string signatureId;
    std::string familyId;
    std::string parentSignatureId;
    std::string name;
    std::string path;
    std::string frameScope;
    std::string metricPreference = "exclusive";
    std::string moduleBudgetId;
    std::string budgetScope;
    bool frameRoot = false;
    bool provenCriticalPath = false;
    std::vector<PolicyFrameEvidence> frames;
    // Representative-only legacy contexts must never be used as a full scan.
    bool frameSeriesComplete = false;
    std::string threadOrQueue;
    uint64_t seriesOffset = 0;
    uint64_t seriesCount = 0;
    std::string seriesSha256;
    std::string observationUnit = "frame";
};

struct PolicyFrameTimeline
{
    std::string frameScope;
    std::string name;
    std::vector<PolicyFrameEvidence> frames;
    std::string observationUnit = "frame";
};

struct PolicyCapacityFact
{
    std::string domain;
    std::string signatureId;
    std::string familyId;
    std::string metric;
    uint64_t valueBytes = 0;
    std::optional<uint64_t> frameIndex;
    bool exact = true;
    std::string description;
};

struct CandidateTriggerEvidence
{
    CandidateTrigger trigger = CandidateTrigger::Top;
    std::string metric;
    std::string scope;
    std::string reason;
    double observedValue = 0;
    std::optional<double> thresholdValue;
    std::string unit;
    std::string authority;
    bool eligible = true;
    std::string observedExact;
    std::optional<std::string> thresholdExact;
};

struct CandidateRepresentativeFrame
{
    uint64_t frameIndex = 0;
    std::vector<std::string> reasons;
    std::vector<std::string> eventRefs;
};

struct CandidateFamily
{
    std::string candidateId;
    std::string familyId;
    CandidatePriority priority = CandidatePriority::P4;
    std::string domain;
    std::string signatureId;
    std::string structuralSignature;
    std::vector<std::string> memberSignatures;
    std::vector<CandidateTrigger> triggers;
    std::vector<CandidateTriggerEvidence> triggerEvidence;
    std::vector<CandidateRepresentativeFrame> representativeFrames;
    bool selected = false;
    std::string notSelectedReason;
    std::string qualityStatus = "complete";
    std::string frameScope;
    std::string threadOrQueue;
    std::string metric;
    std::string manifestation;
    nlohmann::json localEvidence = nlohmann::json::object();
    std::string observationUnit = "frame";
    nlohmann::json representativeIntervals = nlohmann::json::array();
    bool frameRoot = false;
};

struct PolicyRankedSignature
{
    std::string domain;
    std::string ranking;
    uint64_t rank = 0;
    std::string signatureId;
    std::string frameScope;
    int64_t totalNs = 0;
    double contribution = 0;
    double cumulativeContribution = 0;
    bool topPolicyEligible = false;
    std::string notSelectedReason;
};

struct CandidateBacklog
{
    uint64_t total = 0;
    uint64_t selected = 0;
    uint64_t notSelected = 0;
};

struct CandidatePolicyInput
{
    std::string aggregateIdentity;
    std::string profileIdentity;
    nlohmann::json normalizedProfile = nlohmann::json::object();
    NeutralStatisticsResult aggregate;
    std::vector<PolicySignatureContext> signatures;
    std::vector<PolicyCapacityFact> capacityFacts;
    std::vector<PolicyFrameTimeline> frameTimelines;
    // Loads ONE signature's complete sparse series. Missing rows are known-zero
    // only within a validated FrameSet; explicit unknown rows remain unknown.
    std::function<std::vector<PolicyFrameEvidence>( const PolicySignatureContext& )> readFrameSeries;
    std::function<bool()> cancelled;
};

struct CandidatePolicyResult
{
    bool valid = false;
    std::string error;
    std::string aggregateIdentity;
    std::string aggregateContentSha256;
    std::string profileIdentity;
    std::string policyIdentity;
    std::string contentSha256;
    std::vector<PolicyRankedSignature> rankedSignatures;
    std::vector<CandidateFamily> candidates;
    // Source limitations are report appendices, never performance candidates.
    nlohmann::json captureQuality = nlohmann::json::array();
    CandidateBacklog backlog;
};

CandidatePolicyResult EvaluateCandidatePolicy( const CandidatePolicyInput& input );
std::string SerializeCandidatePolicyResult( const CandidatePolicyResult& result );
bool WriteCandidatePolicyManifest( const std::filesystem::path& path,
    const CandidatePolicyResult& result, std::string& error );

const char* CandidateTriggerName( CandidateTrigger trigger );
const char* CandidatePriorityName( CandidatePriority priority );

}

#endif
