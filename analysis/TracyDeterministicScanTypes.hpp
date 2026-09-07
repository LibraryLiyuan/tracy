#ifndef __TRACYDETERMINISTICSCANTYPES_HPP__
#define __TRACYDETERMINISTICSCANTYPES_HPP__

#include <cstdint>
#include <string>
#include <string_view>

namespace tracy::analysis
{

inline constexpr uint32_t NativeScanApiSchemaVersion = 1;
inline constexpr uint32_t NeutralAggregateSchemaVersion = 1;
inline constexpr uint32_t PolicyEvaluationSchemaVersion = 1;
inline constexpr uint32_t CandidateManifestSchemaVersion = 1;
inline constexpr uint32_t AnalysisProfileSchemaVersion = 1;

enum class ScanState : uint8_t
{
    ContractOnly,
    Queued,
    Validating,
    Scanning,
    Aggregating,
    EvaluatingPolicy,
    Auditing,
    Complete,
    CancelledResumable,
    Failed
};

enum class ScanErrorCode : uint8_t
{
    None,
    NotImplementedSchema1,
    InvalidProfile,
    IdentityMismatch,
    ResourceLimitResumable,
    InvalidAggregate,
    Cancelled
};

struct ScanIdentity
{
    std::string traceStrongId;
    std::string aggregateIdentity;
    std::string policyIdentity;
};

struct ScanSchemaIdentity
{
    uint32_t nativeScan = NativeScanApiSchemaVersion;
    uint32_t neutralAggregate = NeutralAggregateSchemaVersion;
    uint32_t policyEvaluation = PolicyEvaluationSchemaVersion;
    uint32_t candidateManifest = CandidateManifestSchemaVersion;
    uint32_t analysisProfile = AnalysisProfileSchemaVersion;
};

struct ScanProgress
{
    ScanState state = ScanState::ContractOnly;
    uint64_t completed = 0;
    uint64_t total = 0;
    std::string stage;
};

struct ScanDescriptor
{
    ScanIdentity identity;
    ScanSchemaIdentity schemas;
    ScanProgress progress;
};

constexpr std::string_view ScanStateName( ScanState state )
{
    switch( state )
    {
    case ScanState::ContractOnly: return "contract_only";
    case ScanState::Queued: return "queued";
    case ScanState::Validating: return "validating";
    case ScanState::Scanning: return "scanning";
    case ScanState::Aggregating: return "aggregating";
    case ScanState::EvaluatingPolicy: return "evaluating_policy";
    case ScanState::Auditing: return "auditing";
    case ScanState::Complete: return "complete";
    case ScanState::CancelledResumable: return "cancelled_resumable";
    case ScanState::Failed: return "failed";
    }
    return "failed";
}

constexpr std::string_view ScanErrorCodeName( ScanErrorCode code )
{
    switch( code )
    {
    case ScanErrorCode::None: return "";
    case ScanErrorCode::NotImplementedSchema1: return "NOT_IMPLEMENTED_SCAN_SCHEMA_1";
    case ScanErrorCode::InvalidProfile: return "INVALID_ANALYSIS_PROFILE";
    case ScanErrorCode::IdentityMismatch: return "SCAN_IDENTITY_MISMATCH";
    case ScanErrorCode::ResourceLimitResumable: return "RESOURCE_LIMIT_RESUMABLE";
    case ScanErrorCode::InvalidAggregate: return "INVALID_NEUTRAL_AGGREGATE";
    case ScanErrorCode::Cancelled: return "CANCELLED";
    }
    return "INTERNAL_ERROR";
}

}

#endif
