#include "TracyCandidatePolicy.hpp"
#include "TracyCandidatePolicyInternal.hpp"
#include "TracyFrameWindowPolicy.hpp"

#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace candidate_policy
{

using nlohmann::json;

struct SignatureKey
{
    std::string domain;
    std::string signature;
    std::string frameScope;

    bool operator<( const SignatureKey& other ) const
    {
        return std::tie( domain, signature, frameScope ) <
            std::tie( other.domain, other.signature, other.frameScope );
    }
};



bool IsHexDigest( const std::string& value )
{
    return value.size() == 64 && std::all_of( value.begin(), value.end(), []( const unsigned char ch ) {
        return std::isxdigit( ch ) != 0;
    } );
}

std::string Sha256Text( const std::string& value )
{
    Sha256Builder hash;
    hash.Update( value.data(), value.size() );
    return hash.FinalHex();
}

std::string Lower( std::string value )
{
    std::transform( value.begin(), value.end(), value.begin(), []( const unsigned char ch ) {
        return char( std::tolower( ch ) );
    } );
    return value;
}

std::string NumberString( double value )
{
    if( value == 0 ) return "0";
    std::ostringstream stream;
    stream.imbue( std::locale::classic() );
    stream << std::setprecision( 17 ) << value;
    return stream.str();
}

void SetExactValues( CandidateTriggerEvidence& evidence, std::string observed,
    std::optional<std::string> threshold = std::nullopt )
{
    evidence.observedExact = std::move( observed );
    evidence.thresholdExact = std::move( threshold );
}

bool MarkerRuleMatches( const std::string& rule, const PolicySignatureContext& context )
{
    const auto exact = rule.starts_with( "exact:" );
    const auto prefix = rule.starts_with( "prefix:" );
    const auto pattern = exact || prefix ? rule.substr( rule.find( ':' ) + 1 ) : rule;
    const auto match = [&]( const std::string& value ) {
        return prefix ? value.starts_with( pattern ) : value == pattern;
    };
    return match( context.name ) || match( context.path ) || match( context.signatureId );
}

const NeutralSignatureAggregate* FindAggregate( const CandidatePolicyInput& input,
    const std::string& domain, const std::string& signature, const std::string& frameScope )
{
    const auto found = std::find_if( input.aggregate.signatures.begin(), input.aggregate.signatures.end(),
        [&]( const auto& value ) { return value.domain == domain && value.signatureId == signature &&
            value.frameScope == frameScope; } );
    return found == input.aggregate.signatures.end() ? nullptr : &*found;
}

const NeutralMetricAggregate* Metric( const NeutralSignatureAggregate* value, const std::string& metric )
{
    if( value == nullptr ) return nullptr;
    if( metric == "inclusive" || metric == "frame_wall" ) return &value->inclusive;
    if( metric == "wait" || metric == "wait_critical" ) return &value->wait;
    if( metric == "critical_path" ) return &value->criticalPath;
    return &value->exclusive;
}

const ExactDistribution& Scope( const NeutralMetricAggregate& value, const std::string& scope )
{
    return scope == "when_present" ? value.whenPresent : value.perCompleteFrame;
}

std::string ResolveFamily( const std::string& domain, const std::string& signature,
    const std::string& explicitFamily )
{
    return explicitFamily.empty() ? domain + ":" + signature : explicitFamily;
}

void AddRepresentative( FamilyWork& work, uint64_t frame, const std::string& reason,
    const std::vector<std::string>& eventRefs )
{
    auto& value = work.representatives[frame];
    value.frameIndex = frame;
    value.reasons.push_back( reason );
    value.eventRefs.insert( value.eventRefs.end(), eventRefs.begin(), eventRefs.end() );
}

const PolicyFrameEvidence* FindFrame( const PolicySignatureContext& context, uint64_t frame )
{
    const auto found = std::find_if( context.frames.begin(), context.frames.end(),
        [&]( const auto& value ) { return value.frameIndex == frame; } );
    return found == context.frames.end() ? nullptr : &*found;
}

void SelectRepresentatives( FamilyWork& work, const PolicySignatureContext& context,
    const NeutralMetricAggregate* metric )
{
    if( context.frames.empty() ) return;
    auto byTime = context.frames;
    std::sort( byTime.begin(), byTime.end(), []( const auto& left, const auto& right ) {
        return left.frameIndex < right.frameIndex;
    } );
    AddRepresentative( work, byTime.front().frameIndex, "first", byTime.front().eventRefs );
    AddRepresentative( work, byTime.back().frameIndex, "last", byTime.back().eventRefs );

    auto worst = context.frames;
    std::sort( worst.begin(), worst.end(), []( const auto& left, const auto& right ) {
        if( left.valueNs != right.valueNs ) return left.valueNs > right.valueNs;
        return left.frameIndex < right.frameIndex;
    } );
    for( size_t index = 0; index < std::min<size_t>( 3, worst.size() ); ++index )
        AddRepresentative( work, worst[index].frameIndex, "worst", worst[index].eventRefs );

    const auto typicalTarget = metric == nullptr ? 0 : metric->perCompleteFrame.p95;
    auto typical = context.frames;
    std::sort( typical.begin(), typical.end(), [&]( const auto& left, const auto& right ) {
        const auto leftDelta = std::abs( double( left.valueNs ) - typicalTarget );
        const auto rightDelta = std::abs( double( right.valueNs ) - typicalTarget );
        if( leftDelta != rightDelta ) return leftDelta < rightDelta;
        return left.frameIndex < right.frameIndex;
    } );
    for( size_t index = 0; index < std::min<size_t>( 2, typical.size() ); ++index )
        AddRepresentative( work, typical[index].frameIndex, "typical_bad", typical[index].eventRefs );

    std::set<std::string> structures;
    for( const auto& frame : byTime )
    {
        if( frame.structureKey.empty() || !structures.emplace( frame.structureKey ).second ) continue;
        AddRepresentative( work, frame.frameIndex, "structure_change", frame.eventRefs );
    }

    if( metric == nullptr || metric->anomalies.empty() ) return;
    const auto addFrame = [&]( uint64_t frame, const char* reason ) {
        const auto evidence = FindFrame( context, frame );
        AddRepresentative( work, frame, reason,
            evidence == nullptr ? std::vector<std::string>() : evidence->eventRefs );
    };
    if( metric->longestBurstFrames >= 3 && metric->longestBurstStartFrame &&
        metric->longestBurstEndFrame && metric->longestBurstPeakFrame )
    {
        addFrame( *metric->longestBurstStartFrame, "burst_start" );
        addFrame( *metric->longestBurstPeakFrame, "burst_peak" );
        addFrame( *metric->longestBurstEndFrame, "burst_end" );
        return;
    }

    // Backward-compatible path for aggregates written before bounded anomaly
    // representatives carried an explicit longest-burst summary.
    size_t bestBegin = 0;
    size_t bestEnd = 0;
    size_t currentBegin = 0;
    for( size_t index = 1; index <= metric->anomalies.size(); ++index )
    {
        if( index < metric->anomalies.size() && metric->anomalies[index].frameIndex ==
            metric->anomalies[index - 1].frameIndex + 1 ) continue;
        if( index - currentBegin > bestEnd - bestBegin + 1 )
        {
            bestBegin = currentBegin;
            bestEnd = index - 1;
        }
        currentBegin = index;
    }
    if( bestEnd < bestBegin || bestEnd - bestBegin + 1 < 3 ) return;
    const auto peak = std::max_element( metric->anomalies.begin() + bestBegin,
        metric->anomalies.begin() + bestEnd + 1, []( const auto& left, const auto& right ) {
            if( left.valueNs != right.valueNs ) return left.valueNs < right.valueNs;
            return left.frameIndex > right.frameIndex;
        } );
    const auto addAnomaly = [&]( size_t index, const char* reason ) {
        addFrame( metric->anomalies[index].frameIndex, reason );
    };
    addAnomaly( bestBegin, "burst_start" );
    addAnomaly( size_t( peak - metric->anomalies.begin() ), "burst_peak" );
    addAnomaly( bestEnd, "burst_end" );
}

void AddSignal( std::map<std::string, FamilyWork>& families,
    const std::string& familyId, const std::string& domain, const std::string& signature,
    CandidatePriority priority, CandidateTriggerEvidence evidence, bool eligible,
    uint64_t topRank, const PolicySignatureContext* context,
    const NeutralMetricAggregate* metric )
{
    auto& work = families[familyId];
    auto& candidate = work.candidate;
    if( candidate.familyId.empty() )
    {
        candidate.familyId = familyId;
        candidate.priority = priority;
        candidate.domain = domain;
        candidate.signatureId = signature;
        candidate.structuralSignature = signature;
    }
    else if( priority < candidate.priority ||
        ( priority == candidate.priority && std::tie( domain, signature ) <
            std::tie( candidate.domain, candidate.signatureId ) ) )
    {
        candidate.priority = priority;
        candidate.domain = domain;
        candidate.signatureId = signature;
        candidate.structuralSignature = signature;
    }
    candidate.triggers.push_back( evidence.trigger );
    evidence.eligible = eligible;
    candidate.triggerEvidence.push_back( std::move( evidence ) );
    candidate.memberSignatures.push_back( domain + ":" + signature );
    work.policyEligible = work.policyEligible || eligible;
    work.mandatory = work.mandatory || priority == CandidatePriority::P0 ||
        priority == CandidatePriority::P1 || candidate.triggers.back() == CandidateTrigger::UserFocus;
    work.bestTopRank = std::min( work.bestTopRank, topRank );
    if( context != nullptr )
    {
        work.contextWritten = true;
        candidate.frameScope = context->frameScope;
        candidate.threadOrQueue = context->threadOrQueue;
        candidate.observationUnit = context->observationUnit;
        candidate.frameRoot = context->frameRoot;
        candidate.metric = context->metricPreference;
        candidate.structuralSignature = context->path.empty() ? context->signatureId : context->path;
        SelectRepresentatives( work, *context, metric );
    }
}

std::string TriggerSortKey( const CandidateTriggerEvidence& value )
{
    return std::to_string( int( value.trigger ) ) + "\n" + value.metric + "\n" +
        value.scope + "\n" + value.reason + "\n" +
        ( value.observedExact.empty() ? NumberString( value.observedValue ) : value.observedExact ) + "\n" +
        ( value.thresholdExact ? *value.thresholdExact :
            value.thresholdValue ? NumberString( *value.thresholdValue ) : std::string() );
}

bool ValidateInput( const CandidatePolicyInput& input, std::string& error )
{
    // A failed scanner audit is not source degradation and must not publish rankings.
    if( !input.aggregate.qualityComplete || input.aggregate.unreportedGapCount != 0 )
    { error = "neutral_aggregate_incomplete"; return false; }
    if( !IsHexDigest( input.aggregateIdentity ) || !IsHexDigest( input.profileIdentity ) ||
        !IsHexDigest( input.aggregate.contentSha256 ) )
    { error = "candidate_policy_identity_invalid"; return false; }
    if( !input.normalizedProfile.is_object() ||
        Sha256Text( input.normalizedProfile.dump() ) != input.profileIdentity )
    { error = "candidate_policy_profile_identity_mismatch"; return false; }
    try
    {
        const auto& frameBudget = input.normalizedProfile.at( "frame_budget" );
        const auto frameMs = frameBudget.at( "frame_ms" ).get<double>();
        if( !std::isfinite( frameMs ) || frameMs <= 0 )
            throw std::runtime_error( "frame_budget_range" );
        const auto& modules = input.normalizedProfile.at( "module_budgets" );
        if( !modules.is_array() ) throw std::runtime_error( "module_budgets_type" );
        for( const auto& module : modules )
        {
            if( !module.is_object() || module.at( "id" ).get<std::string>().empty() )
                throw std::runtime_error( "module_budget_id" );
            const auto scope = module.at( "scope" ).get<std::string>();
            const auto budgetMs = module.at( "budget_ms" ).get<double>();
            if( ( scope != "per_complete_frame" && scope != "when_present" ) ||
                !std::isfinite( budgetMs ) || budgetMs <= 0 )
                throw std::runtime_error( "module_budget_range" );
            if( module.at( "status" ).get<std::string>().empty() ||
                !module.at( "marker_rules" ).is_array() )
                throw std::runtime_error( "module_budget_contract" );
            for( const auto& rule : module.at( "marker_rules" ) )
                if( !rule.is_string() || rule.get<std::string>().empty() )
                    throw std::runtime_error( "module_budget_marker_rule" );
        }
        if( !input.normalizedProfile.at( "resource_budgets" ).is_object() )
            throw std::runtime_error( "resource_budgets_type" );
        const auto& userFocus = input.normalizedProfile.at( "user_focus" );
        if( !userFocus.is_array() ) throw std::runtime_error( "user_focus_type" );
        for( const auto& focus : userFocus )
            if( !focus.is_string() || focus.get<std::string>().empty() )
                throw std::runtime_error( "user_focus_value" );

        const auto& policy = input.normalizedProfile.at( "candidate_policy" );
        const auto top = policy.at( "top_n" ).get<uint64_t>();
        const auto cumulative = policy.at( "cumulative_contribution" ).get<double>();
        const auto limit = policy.at( "per_domain_limit" ).get<uint64_t>();
        const auto absoluteMs = policy.value( "absolute_frame_cost_ms", 1.0 );
        if( !std::isfinite( absoluteMs ) || absoluteMs <= 0 )
            throw std::runtime_error( "absolute_frame_cost_range" );
        if( top == 0 || top > 1000 || cumulative <= 0 || cumulative > 1 || limit == 0 || limit > 1000 )
            throw std::runtime_error( "candidate_policy_range" );
        const auto& priorities = policy.at( "priorities" );
        if( !priorities.is_array() || priorities.empty() )
            throw std::runtime_error( "candidate_policy_priorities" );
        std::set<std::string> uniquePriorities;
        for( const auto& priority : priorities )
        {
            const auto value = priority.get<std::string>();
            if( value != "P0" && value != "P1" && value != "P2" && value != "P3" && value != "P4" )
                throw std::runtime_error( "candidate_policy_priority_value" );
            if( !uniquePriorities.emplace( value ).second )
                throw std::runtime_error( "candidate_policy_priority_duplicate" );
        }
    }
    catch( const std::exception& exception )
    {
        error = "candidate_policy_profile_invalid:" + std::string( exception.what() );
        return false;
    }
    std::set<SignatureKey> contextKeys;
    for( const auto& context : input.signatures )
        if( !contextKeys.emplace( SignatureKey { context.domain, context.signatureId,
            context.frameScope } ).second )
        { error = "duplicate_policy_signature_context"; return false; }
    std::set<SignatureKey> aggregateKeys;
    for( const auto& aggregate : input.aggregate.signatures )
        if( !aggregateKeys.emplace( SignatureKey { aggregate.domain, aggregate.signatureId,
            aggregate.frameScope } ).second )
        { error = "duplicate_neutral_signature_aggregate"; return false; }
    return true;
}

json TriggerEvidenceJson( const CandidateTriggerEvidence& value )
{
    return {
        { "trigger", CandidateTriggerName( value.trigger ) }, { "metric", value.metric },
        { "scope", value.scope }, { "reason", value.reason },
        { "observed_value", value.observedExact.empty() ? NumberString( value.observedValue ) : value.observedExact },
        { "threshold_value", value.thresholdExact ? json( *value.thresholdExact ) :
            value.thresholdValue ? json( NumberString( *value.thresholdValue ) ) : json( nullptr ) },
        { "unit", value.unit }, { "authority", value.authority }, { "eligible", value.eligible }
    };
}

json RankedJson(const PolicyRankedSignature& ranking)
{
    return {
            { "domain", ranking.domain }, { "ranking", ranking.ranking }, { "rank", ranking.rank },
            { "signature_id", ranking.signatureId }, { "frame_scope", ranking.frameScope },
            { "total_ns", std::to_string( ranking.totalNs ) },
            { "contribution", ranking.contribution },
            { "cumulative_contribution", ranking.cumulativeContribution },
            { "top_policy_eligible", ranking.topPolicyEligible },
            { "not_selected_reason", ranking.notSelectedReason.empty() ? json( nullptr ) : json( ranking.notSelectedReason ) }
    };
}

json CandidateJson(const CandidateFamily& candidate)
{
    json triggers = json::array();
    for( const auto trigger : candidate.triggers ) triggers.emplace_back( CandidateTriggerName( trigger ) );
    json frames = json::array();
    json frameDetails = json::array();
    for( const auto& frame : candidate.representativeFrames )
    {
        if( candidate.observationUnit != "frame" ) continue;
        frames.emplace_back( std::to_string( frame.frameIndex ) );
        frameDetails.push_back( { { "frame", std::to_string( frame.frameIndex ) },
            { "reasons", frame.reasons }, { "event_refs", frame.eventRefs } } );
    }
    json evidence = json::array();
    for( const auto& item : candidate.triggerEvidence ) evidence.push_back( TriggerEvidenceJson( item ) );
    return {
        { "candidate_id", candidate.candidateId }, { "family_id", candidate.familyId },
        { "priority", CandidatePriorityName( candidate.priority ) }, { "domain", candidate.domain },
        { "signature_id", candidate.signatureId }, { "structural_signature", candidate.structuralSignature },
        { "member_signatures", candidate.memberSignatures }, { "triggers", std::move( triggers ) },
        { "trigger_evidence", std::move( evidence ) }, { "representative_frames", std::move( frames ) },
        { "representative_frame_details", std::move( frameDetails ) },
        { "selected", candidate.selected },
        { "not_selected_reason", candidate.notSelectedReason.empty() ? json( nullptr ) : json( candidate.notSelectedReason ) },
        { "quality_status", candidate.qualityStatus }
        , { "frame_scope", candidate.frameScope }, { "thread_or_queue", candidate.threadOrQueue },
        { "metric", candidate.metric }, { "manifestation", candidate.manifestation },
        { "local_evidence", candidate.localEvidence }
        , { "observation_unit", candidate.observationUnit }, { "representative_intervals", candidate.representativeIntervals }
        , { "frame_root", candidate.frameRoot }
    };
}

std::string SerializeInternal( const CandidatePolicyResult& result, bool includeHash )
{
    json document = {
        { "schema_version", CandidateManifestSchemaVersion },
        { "aggregate_identity", result.aggregateIdentity },
        { "aggregate_content_sha256", result.aggregateContentSha256 },
        { "policy_identity", result.policyIdentity }, { "profile_identity", result.profileIdentity },
        { "policy_algorithm", CandidatePolicyAlgorithmId },
        { "candidates", json::array() }, { "ranked_signatures", json::array() },
        { "capture_quality", result.captureQuality },
        { "backlog", { { "total", result.backlog.total }, { "selected", result.backlog.selected },
            { "not_selected", result.backlog.notSelected } } }
    };
    if( includeHash ) document["content_sha256"] = result.contentSha256;
    for( const auto& ranking : result.rankedSignatures )
    {
        document["ranked_signatures"].push_back(RankedJson(ranking));
    }
    for( const auto& candidate : result.candidates )
    {
        document["candidates"].push_back(CandidateJson(candidate));
    }
    return document.dump();
}

bool AtomicReplace( const std::filesystem::path& temporary,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( AnalysisIoPath( temporary ).c_str(), AnalysisIoPath( target ).c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) != FALSE ) return true;
    error = "candidate_manifest_atomic_replace_failed:" + std::to_string( GetLastError() );
#else
    std::error_code ec;
    std::filesystem::rename( temporary, target, ec );
    if( !ec ) return true;
    error = "candidate_manifest_atomic_replace_failed:" + ec.message();
#endif
    return false;
}

void Local(Families& families, const PolicySignatureContext& context, const frame_window::Signal& signal)
{
    const auto identity = context.domain + ":" + context.signatureId + ":" + context.frameScope +
        ":" + context.threadOrQueue + ":" + context.metricPreference + ":" + signal.manifestation;
    CandidateTriggerEvidence evidence { signal.manifestation == "frame_cost" ?
        CandidateTrigger::FrameLocal : CandidateTrigger::WindowLocal, context.metricPreference,
        context.frameScope, signal.reason, double( signal.valueNs ),
        std::nullopt, "ns", CandidatePolicyAlgorithmId, true };
    SetExactValues( evidence, std::to_string( signal.valueNs ) );
    AddSignal( families, identity, context.domain, context.signatureId, context.frameRoot ? CandidatePriority::P1 : CandidatePriority::P2,
        std::move( evidence ), true, std::numeric_limits<uint64_t>::max(), nullptr, nullptr );
    auto& work = families.at( identity );
    work.mandatory = true;
    work.localWritten = true;
    work.candidate.structuralSignature = context.path.empty() ? context.signatureId : context.path;
    work.candidate.frameScope = context.frameScope;
    work.candidate.threadOrQueue = context.threadOrQueue;
    work.candidate.metric = context.metricPreference;
    work.candidate.manifestation = signal.manifestation;
    work.candidate.observationUnit = context.observationUnit;
    work.candidate.localEvidence = signal.evidence;
    for( const auto& frame : signal.representatives )
        AddRepresentative( work, frame.frameIndex, signal.reason, frame.eventRefs );
}

void Budget(Families& families, const CandidatePolicyInput& input, const NeutralSignatureAggregate* aggregate, const PolicySignatureContext& context,
    const std::function<void(uint64_t)>& beforeSignal)
{
    const auto frameBudgetMs = input.normalizedProfile.at( "frame_budget" ).at( "frame_ms" ).get<double>();
    if( aggregate == nullptr ) return;
    if( context.frameRoot && aggregate->inclusive.perCompleteFrame.exact &&
        aggregate->inclusive.perCompleteFrame.p95 / 1000000.0 > frameBudgetMs )
    {
        if( beforeSignal ) beforeSignal( 0 );
        CandidateTriggerEvidence evidence { CandidateTrigger::Budget, "frame_wall", "per_complete_frame",
            "p95_exceeds_confirmed_frame_budget", aggregate->inclusive.perCompleteFrame.p95,
            frameBudgetMs * 1000000.0, "ns", "confirmed", true };
        SetExactValues( evidence, NumberString( aggregate->inclusive.perCompleteFrame.p95 ),
            NumberString( frameBudgetMs * 1000000.0 ) );
        AddSignal( families, ResolveFamily( context.domain, context.signatureId, context.familyId ),
            context.domain, context.signatureId, CandidatePriority::P1, std::move( evidence ), true,
            std::numeric_limits<uint64_t>::max(), &context, &aggregate->inclusive );
    }
    for( const auto& module : input.normalizedProfile.at( "module_budgets" ) )
    {
        if( module.at( "status" ) == "planning_reference" ) continue;
        const auto moduleId = module.at( "id" ).get<std::string>();
        bool matched = !context.moduleBudgetId.empty() && context.moduleBudgetId == moduleId;
        if( !matched ) for( const auto& rule : module.at( "marker_rules" ) )
            if( MarkerRuleMatches( rule.get<std::string>(), context ) ) { matched = true; break; }
        if( !matched ) continue;
        const auto scopeName = module.at( "scope" ).get<std::string>();
        if( !context.budgetScope.empty() && context.budgetScope != scopeName ) continue;
        const auto metricName = context.metricPreference.empty() ? "exclusive" : context.metricPreference;
        const auto metric = Metric( aggregate, metricName );
        if( metric == nullptr ) continue;
        const auto& distribution = Scope( *metric, scopeName );
        const auto thresholdMs = module.at( "budget_ms" ).get<double>();
        if( !distribution.exact || distribution.p95 / 1000000.0 <= thresholdMs ) continue;
        if( beforeSignal ) beforeSignal( moduleId.size() + scopeName.size() + metricName.size() +
            module.at( "status" ).get_ref<const std::string&>().size() );
        const auto authority = module.at( "status" ).get<std::string>();
        const auto priority = authority == "fixed" ? CandidatePriority::P1 : CandidatePriority::P2;
        CandidateTriggerEvidence evidence { CandidateTrigger::Budget, metricName, scopeName,
            "p95_exceeds_module_budget:" + moduleId, distribution.p95,
            thresholdMs * 1000000.0, "ns", authority, true };
        SetExactValues( evidence, NumberString( distribution.p95 ),
            NumberString( thresholdMs * 1000000.0 ) );
        AddSignal( families, ResolveFamily( context.domain, context.signatureId, context.familyId ),
            context.domain, context.signatureId, priority, std::move( evidence ), true,
            std::numeric_limits<uint64_t>::max(), &context, metric );
    }
}

PolicyRankedSignature Top(Families& families, const CandidatePolicyInput& input, const std::string& domain, const char* rankingName,
    const NeutralRankingEntry& entry, uint64_t index, double previousCumulative,
    const PolicySignatureContext* context, const NeutralSignatureAggregate* aggregate)
{
    const auto& policy = input.normalizedProfile.at("candidate_policy");
    const auto topN = policy.at("top_n").get<uint64_t>();
    const auto perDomainLimit = policy.at("per_domain_limit").get<uint64_t>();
    const auto cumulativeTarget = policy.at("cumulative_contribution").get<double>();
    bool qualified = index < perDomainLimit && ( index < topN ||
        ( index != 0 && previousCumulative < cumulativeTarget ) );
    PolicyRankedSignature ranked { domain, rankingName, uint64_t( index + 1 ),
        entry.signatureId, entry.frameScope, entry.totalNs, entry.contribution,
        entry.cumulativeContribution, qualified, {} };
    if( entry.totalNs <= 0 ) { qualified = false; ranked.topPolicyEligible = false; ranked.notSelectedReason = "zero_total"; }
    else if( !qualified ) ranked.notSelectedReason = "outside_top_policy";
    if( entry.totalNs <= 0 ) return ranked;
    const auto metric = Metric( aggregate, rankingName );
    auto priority = index < topN ? CandidatePriority::P2 : CandidatePriority::P4;
    if( context != nullptr && context->provenCriticalPath ) priority = CandidatePriority::P1;
    CandidateTriggerEvidence evidence { CandidateTrigger::Top, rankingName, "all_complete_frames",
        qualified ? ( index < topN ? "top_n" : "cumulative_contribution_extension" ) :
            "outside_top_policy", double( entry.totalNs ), std::nullopt, "ns",
        "deterministic_ranking", qualified };
    SetExactValues( evidence, std::to_string( entry.totalNs ) );
    AddSignal( families, ResolveFamily( domain, entry.signatureId,
        context == nullptr ? std::string() : context->familyId ), domain,
        entry.signatureId, priority, std::move( evidence ), qualified, uint64_t( index + 1 ),
        context, metric );
    return ranked;
}

void Anomalies(Families& families, const NeutralSignatureAggregate& signature, const PolicySignatureContext* context)
{
    const std::pair<const char*, const NeutralMetricAggregate*> metrics[] = {
        { "inclusive", &signature.inclusive }, { "exclusive", &signature.exclusive },
        { "wait", &signature.wait }, { "critical_path", &signature.criticalPath }
    };
    for( const auto& [metricName, metric] : metrics )
    {
        if( metric->pattern == AnomalyPattern::None ) continue;
        const auto anomalyCount = metric->anomalyCount != 0 || metric->anomalies.empty() ?
            metric->anomalyCount : uint64_t( metric->anomalies.size() );
        auto priority = metric->pattern == AnomalyPattern::IsolatedSpike ?
            CandidatePriority::P3 : CandidatePriority::P2;
        if( context != nullptr && context->provenCriticalPath ) priority = CandidatePriority::P1;
        CandidateTriggerEvidence evidence { CandidateTrigger::Anomaly, metricName, "per_complete_frame",
            AnomalyPatternName( metric->pattern ), double( anomalyCount ), std::nullopt,
            "frames", "robust_median_mad", true };
        SetExactValues( evidence, std::to_string( anomalyCount ) );
        AddSignal( families, ResolveFamily( signature.domain, signature.signatureId,
            context == nullptr ? std::string() : context->familyId ), signature.domain,
            signature.signatureId, priority, std::move( evidence ), true,
            std::numeric_limits<uint64_t>::max(), context, metric );
    }
}

void Capacity(Families& families, const CandidatePolicyInput& input, const PolicyCapacityFact& fact)
{
    const auto& resourceBudgets = input.normalizedProfile.at("resource_budgets");
    const auto bytesKey = fact.metric + "_bytes";
    const auto statusKey = fact.metric + "_status";
    if( !resourceBudgets.contains( bytesKey ) || !resourceBudgets.contains( statusKey ) ) return;
    const auto threshold = resourceBudgets.at( bytesKey ).get<uint64_t>();
    if( !fact.exact || fact.valueBytes <= threshold ) return;
    const auto authority = resourceBudgets.at( statusKey ).get<std::string>();
    const auto priority = authority == "fixed" ? CandidatePriority::P1 : CandidatePriority::P2;
    CandidateTriggerEvidence evidence { CandidateTrigger::Capacity, fact.metric, "capture_peak",
        fact.description, double( fact.valueBytes ), double( threshold ), "bytes", authority, true };
    SetExactValues( evidence, std::to_string( fact.valueBytes ), std::to_string( threshold ) );
    AddSignal( families, ResolveFamily( fact.domain, fact.signatureId, fact.familyId ), fact.domain,
        fact.signatureId, priority, std::move( evidence ), true,
        std::numeric_limits<uint64_t>::max(), nullptr, nullptr );
    if( fact.frameIndex ) AddRepresentative( families[ResolveFamily( fact.domain,
        fact.signatureId, fact.familyId )], *fact.frameIndex, "capacity_peak", {} );
}

void Focus(Families& families, const json& focusJson, const PolicySignatureContext& context, const NeutralSignatureAggregate* aggregate)
{
    const auto focus = Lower(focusJson.get<std::string>());
    const auto haystack = Lower( context.domain + "\n" + context.signatureId + "\n" +
        context.familyId + "\n" + context.name + "\n" + context.path );
    if( haystack.find( focus ) == std::string::npos ) return;
    const auto metric = Metric( aggregate, context.metricPreference );
    CandidateTriggerEvidence evidence { CandidateTrigger::UserFocus, context.metricPreference,
        context.frameScope, "user_focus:" + focusJson.get<std::string>(), 1.0,
        std::nullopt, "match", "user", true };
    SetExactValues( evidence, "1" );
    AddSignal( families, ResolveFamily( context.domain, context.signatureId, context.familyId ),
        context.domain, context.signatureId, CandidatePriority::P2, std::move( evidence ), true,
        std::numeric_limits<uint64_t>::max(), &context, metric );
}

void Finalize(FamilyWork& work, const std::string& familyId, const std::string& policyIdentity)
{
    auto& candidate = work.candidate;
    std::sort( candidate.memberSignatures.begin(), candidate.memberSignatures.end() );
    candidate.memberSignatures.erase( std::unique( candidate.memberSignatures.begin(),
        candidate.memberSignatures.end() ), candidate.memberSignatures.end() );
    std::sort( candidate.triggers.begin(), candidate.triggers.end() );
    candidate.triggers.erase( std::unique( candidate.triggers.begin(), candidate.triggers.end() ),
        candidate.triggers.end() );
    std::sort( candidate.triggerEvidence.begin(), candidate.triggerEvidence.end(),
        []( const auto& left, const auto& right ) { return TriggerSortKey( left ) < TriggerSortKey( right ); } );
    for( auto& [frame, representative] : work.representatives )
    {
        std::sort( representative.reasons.begin(), representative.reasons.end() );
        representative.reasons.erase( std::unique( representative.reasons.begin(), representative.reasons.end() ),
            representative.reasons.end() );
        std::sort( representative.eventRefs.begin(), representative.eventRefs.end() );
        representative.eventRefs.erase( std::unique( representative.eventRefs.begin(), representative.eventRefs.end() ),
            representative.eventRefs.end() );
        candidate.representativeFrames.push_back( std::move( representative ) );
    }
    candidate.candidateId = "candidate:" + Sha256Text( policyIdentity + "\n" + familyId );
}

void Select(FamilyWork& work, const CandidatePolicyInput& input, std::map<std::string, uint64_t>& selectedDiscretionaryByDomain)
{
    const auto& policy = input.normalizedProfile.at("candidate_policy");
    const auto perDomainLimit = policy.at("per_domain_limit").get<uint64_t>();
    std::set<std::string> allowedPriorities;
    for(const auto& p:policy.at("priorities")) allowedPriorities.emplace(p.get<std::string>());
    auto& candidate = work.candidate;
    if( candidate.observationUnit == "l0_segment" )
    {
        for( const auto& timeline : input.frameTimelines ) if( timeline.frameScope == candidate.frameScope )
            for( const auto& frame : candidate.representativeFrames )
            {
                const auto segment = std::find_if( timeline.frames.begin(), timeline.frames.end(),
                    [&]( const auto& s ) { return s.frameIndex == frame.frameIndex; } );
                if( segment == timeline.frames.end() ) continue;
                candidate.representativeIntervals.push_back( {
                    { "l0_segment_ordinal", std::to_string( frame.frameIndex ) },
                    { "begin_ns", segment->beginNs?json(std::to_string(*segment->beginNs)):json(nullptr) },
                    { "end_ns", segment->endNs?json(std::to_string(*segment->endNs)):json(nullptr) },
                    { "event_refs", segment->eventRefs }, { "reasons", frame.reasons },
                    { "player_frame_relation", "unavailable_not_inferred" } } );
                if(!segment->exact) candidate.representativeIntervals.back()["l0_complete"]=false;
                if(!segment->beginNs || !segment->endNs)
                    candidate.representativeIntervals.back()["timing_unavailable_reason"]="source_l0_timestamps_missing";
            }
    }
    const auto priorityAllowed = allowedPriorities.contains( CandidatePriorityName( candidate.priority ) );
    if( work.mandatory ) candidate.selected = true;
    else if( !priorityAllowed ) candidate.notSelectedReason = "priority_not_enabled";
    else if( !work.policyEligible && !work.mandatory ) candidate.notSelectedReason = "outside_top_policy";
    else if( selectedDiscretionaryByDomain[candidate.domain] < perDomainLimit )
    {
        candidate.selected = true;
        ++selectedDiscretionaryByDomain[candidate.domain];
    }
    else candidate.notSelectedReason = "per_domain_limit";
}


}

using namespace candidate_policy;

const char* CandidateTriggerName( CandidateTrigger trigger )
{
    switch( trigger )
    {
    case CandidateTrigger::Budget: return "budget";
    case CandidateTrigger::Top: return "top";
    case CandidateTrigger::Anomaly: return "anomaly";
    case CandidateTrigger::Capacity: return "capacity";
    case CandidateTrigger::Quality: return "quality";
    case CandidateTrigger::UserFocus: return "user_focus";
    case CandidateTrigger::FrameLocal: return "frame_local";
    case CandidateTrigger::WindowLocal: return "window_local";
    }
    return "top";
}

const char* CandidatePriorityName( CandidatePriority priority )
{
    switch( priority )
    {
    case CandidatePriority::P0: return "P0";
    case CandidatePriority::P1: return "P1";
    case CandidatePriority::P2: return "P2";
    case CandidatePriority::P3: return "P3";
    case CandidatePriority::P4: return "P4";
    }
    return "P4";
}

CandidatePolicyResult EvaluateCandidatePolicy( const CandidatePolicyInput& input )
{
    CandidatePolicyResult result;
    result.aggregateIdentity = input.aggregateIdentity;
    result.aggregateContentSha256 = input.aggregate.contentSha256;
    result.profileIdentity = input.profileIdentity;
    if( !ValidateInput( input, result.error ) ) return result;
    result.policyIdentity = Sha256Text( std::string( CandidatePolicyAlgorithmId ) + "\n" +
        input.aggregateIdentity + "\n" + input.profileIdentity );

    std::map<SignatureKey, const PolicySignatureContext*> contexts;
    for( const auto& context : input.signatures )
        contexts.emplace( SignatureKey { context.domain, context.signatureId, context.frameScope }, &context );
    const auto findContext = [&]( const std::string& domain, const std::string& signature,
        const std::string& frameScope ) {
        const auto found = contexts.find( { domain, signature, frameScope } );
        return found == contexts.end() ? static_cast<const PolicySignatureContext*>( nullptr ) : found->second;
    };

    std::map<std::string, FamilyWork> families;

    // Local/window pools are independent of global rankings and domain caps.
    std::vector<frame_window::Signal> localSignals;
    try { localSignals = frame_window::Scan( input ); }
    catch( const std::exception& exception )
    { result.error = std::string( "frame_window_scan_failed:" ) + exception.what(); return result; }
    for( const auto& signal : localSignals )
    {
        const auto& context = input.signatures.at( signal.context );
        Local(families, context, signal);
    }

    // Budget pool: frame wall budget and explicitly attributable module budgets only.
    for(const auto& context:input.signatures)
        Budget(families, input, FindAggregate(input, context.domain, context.signatureId, context.frameScope), context);

    // Top pool: preserve every ranked signature; qualification is Top N, then the
    // cumulative extension, capped per domain. Unqualified rows remain auditable backlog.
    for( const auto& ranking : input.aggregate.rankings )
    {
        const std::pair<const char*, const std::vector<NeutralRankingEntry>*> lists[] = {
            { "inclusive", &ranking.inclusive }, { "exclusive", &ranking.exclusive },
            { "wait_critical", &ranking.waitCritical }
        };
        for( const auto& [rankingName, entries] : lists )
        {
            for( size_t index = 0; index < entries->size(); ++index )
            {
                const auto& entry = ( *entries )[index];
                result.rankedSignatures.push_back(Top(families, input, ranking.domain, rankingName, entry, index,
                    index == 0 ? 0 : (*entries)[index-1].cumulativeContribution,
                    findContext(ranking.domain, entry.signatureId, entry.frameScope),
                    FindAggregate(input, ranking.domain, entry.signatureId, entry.frameScope)));
            }
        }
    }

    // Anomaly pool.
    for( const auto& signature : input.aggregate.signatures )
    {
        Anomalies(families, signature, findContext(signature.domain, signature.signatureId, signature.frameScope));
    }

    // Capacity pool.
    for(const auto& fact:input.capacityFacts) Capacity(families, input, fact);

    // Keep capture limitations independently of candidate quotas and priorities.
    for( const auto& domain : input.aggregate.domains )
    {
        if( domain.status != "invalid" && domain.qualityComplete ) continue;
        result.captureQuality.push_back( {
            { "domain", domain.domain }, { "status", domain.status },
            { "reason", domain.unavailableReason.empty() ? "domain_invalid" : domain.unavailableReason },
            { "input_count", std::to_string( domain.inputCount ) },
            { "consumed_input_count", std::to_string( domain.consumedInputCount ) },
            { "audit_complete", domain.qualityComplete }
        } );
    }
    std::sort( result.captureQuality.begin(), result.captureQuality.end(),
        []( const json& a, const json& b ) { return a.dump() < b.dump(); } );

    // User focus pool is an explicit investigation request and is never dropped by
    // Top/cumulative limits.
    for( const auto& focusJson : input.normalizedProfile.at( "user_focus" ) )
    {
        for(const auto& context:input.signatures)
            Focus(families, focusJson, context, FindAggregate(input, context.domain, context.signatureId, context.frameScope));
    }

    std::vector<std::pair<std::string, FamilyWork*>> ordered;
    for( auto& [familyId, work] : families )
    {
        Finalize(work, familyId, result.policyIdentity);
        ordered.emplace_back( familyId, &work );
    }
    std::sort( ordered.begin(), ordered.end(), []( const auto& left, const auto& right ) {
        const auto& a = *left.second;
        const auto& b = *right.second;
        if( a.candidate.priority != b.candidate.priority ) return a.candidate.priority < b.candidate.priority;
        if( a.policyEligible != b.policyEligible ) return a.policyEligible > b.policyEligible;
        if( a.bestTopRank != b.bestTopRank ) return a.bestTopRank < b.bestTopRank;
        return left.first < right.first;
    } );

    std::map<std::string, uint64_t> selectedDiscretionaryByDomain;
    for( const auto& [familyId, workPointer] : ordered )
    {
        auto& work = *workPointer;
        Select(work, input, selectedDiscretionaryByDomain);
        result.candidates.push_back( std::move( work.candidate ) );
    }
    std::sort( result.rankedSignatures.begin(), result.rankedSignatures.end(), []( const auto& left, const auto& right ) {
        return std::tie( left.domain, left.frameScope, left.ranking, left.rank, left.signatureId ) <
            std::tie( right.domain, right.frameScope, right.ranking, right.rank, right.signatureId );
    } );
    result.backlog.total = result.candidates.size();
    result.backlog.selected = std::count_if( result.candidates.begin(), result.candidates.end(),
        []( const auto& value ) { return value.selected; } );
    result.backlog.notSelected = result.backlog.total - result.backlog.selected;
    result.valid = true;
    result.contentSha256 = Sha256Text( SerializeInternal( result, false ) );
    return result;
}

std::string SerializeCandidatePolicyResult( const CandidatePolicyResult& result )
{
    if( !result.valid ) return {};
    return SerializeInternal( result, true );
}

std::string SerializePolicySignatureContextRecord( const PolicySignatureContext& value )
{
    json frames = json::array();
    for( const auto& frame : value.frames ) frames.push_back( {
        { "frame_index", std::to_string( frame.frameIndex ) }, { "value_ns", std::to_string( frame.valueNs ) },
        { "event_refs", frame.eventRefs }, { "structure_key", frame.structureKey }, { "exact", frame.exact },
        { "begin_ns", frame.beginNs ? json( std::to_string( *frame.beginNs ) ) : json( nullptr ) },
        { "end_ns", frame.endNs ? json( std::to_string( *frame.endNs ) ) : json( nullptr ) } } );
    return json {
        { "context_schema", 1 }, { "domain", value.domain }, { "signature_id", value.signatureId },
        { "family_id", value.familyId }, { "parent_signature_id", value.parentSignatureId },
        { "name", value.name }, { "path", value.path }, { "frame_scope", value.frameScope },
        { "metric_preference", value.metricPreference }, { "module_budget_id", value.moduleBudgetId },
        { "budget_scope", value.budgetScope }, { "frame_root", value.frameRoot },
        { "proven_critical_path", value.provenCriticalPath }, { "frames", std::move( frames ) },
        { "frame_series_complete", value.frameSeriesComplete }, { "thread_or_queue", value.threadOrQueue },
        { "series_offset", std::to_string( value.seriesOffset ) }, { "series_count", std::to_string( value.seriesCount ) },
        { "series_sha256", value.seriesSha256 }, { "observation_unit", value.observationUnit },
        { "source_ordinal", value.sourceOrdinal ? json( std::to_string( *value.sourceOrdinal ) ) : json( nullptr ) }
    }.dump();
}
bool DeserializePolicySignatureContextRecord( const std::string& payload,
    PolicySignatureContext& context, std::string& error )
{
    context = {}; error.clear();
    try
    {
        const auto item = json::parse( payload );
        if( item.at( "context_schema" ).get<uint32_t>() != 1 ) throw std::runtime_error( "schema_mismatch" );
        const auto u64 = []( const json& value ) { return std::stoull( value.get<std::string>() ); };
        PolicySignatureContext value;
        value.domain = item.at( "domain" ).get<std::string>();
        value.signatureId = item.at( "signature_id" ).get<std::string>();
        value.familyId = item.at( "family_id" ).get<std::string>();
        value.parentSignatureId = item.at( "parent_signature_id" ).get<std::string>();
        value.name = item.at( "name" ).get<std::string>();
        value.path = item.at( "path" ).get<std::string>();
        value.frameScope = item.at( "frame_scope" ).get<std::string>();
        value.metricPreference = item.at( "metric_preference" ).get<std::string>();
        value.moduleBudgetId = item.at( "module_budget_id" ).get<std::string>();
        value.budgetScope = item.at( "budget_scope" ).get<std::string>();
        value.frameRoot = item.at( "frame_root" ).get<bool>();
        value.provenCriticalPath = item.at( "proven_critical_path" ).get<bool>();
        value.frameSeriesComplete = item.at( "frame_series_complete" ).get<bool>();
        value.threadOrQueue = item.at( "thread_or_queue" ).get<std::string>();
        value.seriesOffset = u64( item.at( "series_offset" ) );
        value.seriesCount = u64( item.at( "series_count" ) );
        value.seriesSha256 = item.at( "series_sha256" ).get<std::string>();
        value.observationUnit = item.at( "observation_unit" ).get<std::string>();
        if( item.contains( "source_ordinal" ) && !item.at( "source_ordinal" ).is_null() )
            value.sourceOrdinal = u64( item.at( "source_ordinal" ) );
        for( const auto& row : item.at( "frames" ) )
        {
            PolicyFrameEvidence frame;
            frame.frameIndex = u64( row.at( "frame_index" ) );
            frame.valueNs = std::stoll( row.at( "value_ns" ).get<std::string>() );
            frame.eventRefs = row.at( "event_refs" ).get<std::vector<std::string>>();
            frame.structureKey = row.at( "structure_key" ).get<std::string>();
            frame.exact = row.at( "exact" ).get<bool>();
            if( !row.at( "begin_ns" ).is_null() ) frame.beginNs = std::stoll( row.at( "begin_ns" ).get<std::string>() );
            if( !row.at( "end_ns" ).is_null() ) frame.endNs = std::stoll( row.at( "end_ns" ).get<std::string>() );
            value.frames.push_back( std::move( frame ) );
        }
        context = std::move( value ); return true;
    }
    catch( const std::exception& exception )
    { error = "policy_context_record_parse_failed:" + std::string( exception.what() ); return false; }
}

bool WriteCandidatePolicyManifest( const std::filesystem::path& path,
    const CandidatePolicyResult& result, std::string& error )
{
    error.clear();
    if( path.empty() || !result.valid || result.contentSha256.empty() ||
        Sha256Text( SerializeInternal( result, false ) ) != result.contentSha256 )
    { error = "candidate_manifest_invalid"; return false; }
    std::error_code ec;
    std::filesystem::create_directories( AnalysisIoPath( path.parent_path() ), ec );
    if( ec ) { error = "candidate_manifest_directory_failed:" + ec.message(); return false; }
    auto temporary = path;
    temporary += ".tmp";
    const auto payload = SerializeInternal( result, true );
    std::ofstream output( AnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !output ) { error = "candidate_manifest_open_failed"; return false; }
    output << payload << '\n';
    output.flush();
    if( !output ) { error = "candidate_manifest_write_failed"; return false; }
    output.close();
    return AtomicReplace( temporary, path, error );
}

}
