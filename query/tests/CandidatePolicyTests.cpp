#include "TracyCandidatePolicy.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

using nlohmann::json;
using namespace tracy::analysis;

namespace
{

std::string JsonSha( const json& value )
{
    const auto text = value.dump();
    Sha256Builder hash;
    hash.Update( text.data(), text.size() );
    return hash.FinalHex();
}

json Profile( uint64_t topN = 2, double cumulative = 0.70, uint64_t limit = 50 )
{
    return {
        { "schema_version", 1 }, { "profile_name", "candidate-policy-test" },
        { "frame_budget", { { "target_fps", 60.0 }, { "frame_ms", 16.666667 } } },
        { "module_budgets", json::array( {
            json { { "id", "logic" }, { "scope", "per_complete_frame" },
                { "budget_ms", 6.0 }, { "status", "planning_reference" },
                { "marker_rules", { "prefix:Logic." } } }
        } ) },
        { "resource_budgets", {
            { "cpu_memory_bytes", 16000000000ULL }, { "cpu_memory_status", "provisional" },
            { "gpu_memory_bytes", 6400000000ULL }, { "gpu_memory_status", "fixed" }
        } },
        { "candidate_policy", { { "top_n", topN },
            { "cumulative_contribution", cumulative }, { "per_domain_limit", limit },
            { "priorities", { "P0", "P1", "P2", "P3", "P4" } } } },
        { "limits", { { "query_memory_target_bytes", 8589934592ULL },
            { "query_memory_hard_bytes", 17179869184ULL },
            { "cache_max_bytes", 137438953472ULL },
            { "minimum_free_disk_bytes", 68719476736ULL } } },
        { "user_focus", { "focus target" } }
    };
}

NeutralSignatureAggregate Signature( const char* id )
{
    NeutralSignatureAggregate value;
    value.domain = "cpu"; value.signatureId = id; value.frameScope = "Player.Frame";
    value.exact = true; value.completeFrameCount = 100; value.presentFrameCount = 100;
    value.inclusive.perCompleteFrame.exact = true;
    value.exclusive.perCompleteFrame.exact = true;
    value.wait.perCompleteFrame.exact = true;
    value.criticalPath.perCompleteFrame.exact = true;
    return value;
}

PolicySignatureContext Context( const char* id, const char* name )
{
    PolicySignatureContext value;
    value.domain = "cpu"; value.signatureId = id; value.name = name;
    value.frameScope = "Player.Frame"; value.budgetScope = "per_complete_frame";
    value.metricPreference = "exclusive";
    return value;
}

bool HasTrigger( const CandidateFamily& candidate, CandidateTrigger trigger )
{
    return std::find( candidate.triggers.begin(), candidate.triggers.end(), trigger ) != candidate.triggers.end();
}

const CandidateFamily& Find( const CandidatePolicyResult& result, const std::string& family )
{
    const auto found = std::find_if( result.candidates.begin(), result.candidates.end(),
        [&]( const auto& value ) { return value.familyId == family; } );
    assert( found != result.candidates.end() );
    return *found;
}

const json& FindJsonCandidate( const json& document, const std::string& family )
{
    const auto found = std::find_if( document.at( "candidates" ).begin(),
        document.at( "candidates" ).end(), [&]( const auto& value ) {
            return value.at( "family_id" ) == family;
        } );
    assert( found != document.at( "candidates" ).end() );
    return *found;
}

void AssertSchemaDeclaresEmittedFields( const json& document )
{
    const auto schema = json::parse( std::ifstream( TRACY_CANDIDATE_MANIFEST_SCHEMA_PATH,
        std::ios::binary ) );
    const auto& rootProperties = schema.at( "properties" );
    for( const auto& key : schema.at( "required" ) )
        assert( document.contains( key.get<std::string>() ) );
    for( const auto& [key, value] : document.items() )
    {
        (void)value;
        assert( rootProperties.contains( key ) );
    }
    const auto& candidateProperties = schema.at( "$defs" ).at( "candidate" ).at( "properties" );
    for( const auto& candidate : document.at( "candidates" ) )
    {
        for( const auto& key : schema.at( "$defs" ).at( "candidate" ).at( "required" ) )
            assert( candidate.contains( key.get<std::string>() ) );
        for( const auto& [key, value] : candidate.items() )
        {
            (void)value;
            assert( candidateProperties.contains( key ) );
        }
    }
}

}

int main()
{
    {
        CandidatePolicyInput local;
        local.aggregateIdentity = std::string( 64, 'a' );
        local.normalizedProfile = Profile( 1, 0.5, 1 );
        local.normalizedProfile["user_focus"] = json::array();
        local.profileIdentity = JsonSha( local.normalizedProfile );
        local.aggregate.contentSha256 = std::string( 64, 'b' );
        local.aggregate.qualityComplete = true;
        auto wall = Context( "frame-local-wall", "Player.Frame" ); wall.frameRoot = true;
        auto work = Context( "local-heavy", "ActualWork" );
        wall.frameSeriesComplete = true; work.frameSeriesComplete = true;
        for( uint64_t frameIndex = 0; frameIndex < 180; ++frameIndex )
        {
            wall.frames.push_back( { frameIndex, frameIndex == 40 ? 50'000'000 :
                frameIndex >= 100 ? 24'000'000 : 12'000'000, {}, "" } );
            work.frames.push_back( { frameIndex, frameIndex == 40 ? 8'000'000 :
                frameIndex >= 100 ? 4'000'000 : 100'000, {}, "" } );
        }
        local.signatures = { wall, work };
        local.aggregate.signatures = { Signature( "frame-local-wall" ), Signature( "local-heavy" ) };
        const auto chosen = EvaluateCandidatePolicy( local );
        if( std::none_of( chosen.candidates.begin(), chosen.candidates.end(), []( const auto& c ) {
            return c.signatureId == "frame-local-wall" && c.selected && HasTrigger( c, CandidateTrigger::FrameLocal );
        } ) ) { std::cerr << "Every actual over-budget frame must be checked, not only global P95\n"; return 1; }
        const auto found = std::find_if( chosen.candidates.begin(), chosen.candidates.end(), []( const auto& c ) {
            return c.signatureId == "local-heavy" && c.selected;
        } );
        if( !chosen.valid || found == chosen.candidates.end() )
        { std::cerr << "Frame-local heavy work must survive without global Top or P95 anomaly\n"; return 1; }
        if( std::none_of( found->representativeFrames.begin(), found->representativeFrames.end(),
            []( const auto& f ) { return f.frameIndex == 40; } ) )
        { std::cerr << "Local spike frame must remain an investigation obligation\n"; return 1; }
        const auto stable = std::find_if( chosen.candidates.begin(), chosen.candidates.end(), []( const auto& c ) {
            return c.signatureId == "local-heavy" && c.selected &&
                std::any_of( c.triggerEvidence.begin(), c.triggerEvidence.end(), []( const auto& e ) {
                    return e.reason == "stable_slow_window";
                } );
        } );
        if( stable == chosen.candidates.end() || stable->candidateId == found->candidateId )
        { std::cerr << "Stable 24ms pressure needs a distinct selected window obligation\n"; return 1; }
        auto small = Context( "tail-submillisecond", "TailWork" );
        small.frameSeriesComplete = true; small.threadOrQueue = "Render";
        small.frames.push_back( {179,300000,{},{},true} );
        local.signatures.push_back( small );
        const auto tail = EvaluateCandidatePolicy( local );
        if( !tail.valid || std::none_of( tail.candidates.begin(), tail.candidates.end(), []( const auto& c ) {
            return c.signatureId == "tail-submillisecond" && c.selected && HasTrigger( c, CandidateTrigger::FrameLocal );
        } ) ) { std::cerr << "Tail local Top must bypass absolute and global caps\n"; return 1; }
        for( auto& frame : local.signatures[0].frames ) if( frame.frameIndex >= 100 && frame.frameIndex % 2 ) frame.exact = false;
        const auto unknown = EvaluateCandidatePolicy( local );
        if( !unknown.valid || std::any_of( unknown.candidates.begin(), unknown.candidates.end(), []( const auto& c ) {
            return HasTrigger( c, CandidateTrigger::WindowLocal );
        } ) ) { std::cerr << "Half unknown frames must not produce exact stable windows\n"; return 1; }
    }
    {
        CandidatePolicyInput plateaus;
        plateaus.normalizedProfile = Profile();
        plateaus.aggregateIdentity = std::string(64, 'a');
        plateaus.profileIdentity = JsonSha(plateaus.normalizedProfile);
        plateaus.aggregate.contentSha256 = std::string(64, 'b');
        plateaus.aggregate.qualityComplete = true;
        auto root = Context("different-plateaus", "Player.Frame");
        root.frameRoot = true; root.frameSeriesComplete = true;
        for(uint64_t i=0; i<120; ++i) root.frames.push_back({i,i<60?20'000'000:40'000'000,{},{},true});
        plateaus.signatures = {root};
        const auto chosen = EvaluateCandidatePolicy(plateaus);
        if(!chosen.valid) return 1;
        for(const auto& c : chosen.candidates) if(c.manifestation.starts_with("stable_slow:"))
        {
            const auto first=std::stoull(c.localEvidence.at("frame_begin").get<std::string>());
            const auto last=std::stoull(c.localEvidence.at("frame_end").get<std::string>());
            if(first==0 && last==119) { std::cerr << "Different stable plateaus cannot be merged into a globally stable window\n"; return 1; }
        }
    }
    CandidatePolicyInput input;
    input.aggregateIdentity = std::string( 64, 'a' );
    input.normalizedProfile = Profile();
    input.profileIdentity = JsonSha( input.normalizedProfile );
    input.aggregate.qualityComplete = true;
    input.aggregate.unreportedGapCount = 0;
    input.aggregate.contentSha256 = std::string( 64, 'b' );

    auto frame = Signature( "frame" );
    frame.inclusive.perCompleteFrame.p95 = 20000000;
    auto budget = Signature( "budget" );
    budget.exclusive.perCompleteFrame.p95 = 7000000;
    auto scopeMismatch = Signature( "scope-mismatch" );
    scopeMismatch.exclusive.perCompleteFrame.p95 = 9000000;
    auto undefinedBudget = Signature( "undefined-budget" );
    undefinedBudget.exclusive.perCompleteFrame.p95 = 9000000;
    auto parent = Signature( "parent" );
    auto child = Signature( "child" );
    child.exclusive.pattern = AnomalyPattern::BurstWindow;
    child.exclusive.longestBurstFrames = 3;
    child.exclusive.anomalyCount = 200;
    child.exclusive.anomaliesComplete = false;
    child.exclusive.longestBurstStartFrame = 3;
    child.exclusive.longestBurstPeakFrame = 5;
    child.exclusive.longestBurstEndFrame = 5;
    child.exclusive.anomalies = { { 3, 20000000, 19000000 },
        { 4, 20000000, 19000000 }, { 5, 30000000, 29000000 } };
    auto isolated = Signature( "isolated" );
    isolated.exclusive.pattern = AnomalyPattern::IsolatedSpike;
    isolated.exclusive.anomalies = { { 42, 4000000, 3000000 } };
    auto focus = Signature( "focus" );
    auto low = Signature( "low" );
    input.aggregate.signatures = { frame, budget, scopeMismatch, undefinedBudget,
        parent, child, isolated, focus, low };
    input.aggregate.rankings = { { "cpu", {
        { "parent", "Player.Frame", false, 100, 0, 0, 0.40, 0.40 },
        { "child", "Player.Frame", false, 80, 0, 0, 0.32, 0.72 },
        { "focus", "Player.Frame", false, 15, 0, 0, 0.06, 0.78 },
        { "low", "Player.Frame", false, 5, 0, 0, 0.02, 0.80 }
    }, {}, {} } };
    input.aggregate.domains = {
        { "cpu", true, "complete", 9, 9, 9, 9, std::string( 64, 'c' ),
            std::string( 64, 'c' ), true, {} },
        { "gpu.catalog", true, "invalid", 10, 10, 0, 0, std::string( 64, 'd' ),
            std::string( 64, 'd' ), true, "catalog core gap" }
    };

    auto frameContext = Context( "frame", "Player.Frame" );
    frameContext.frameRoot = true;
    auto budgetContext = Context( "budget", "Logic.Update" );
    budgetContext.moduleBudgetId = "logic";
    auto mismatchContext = Context( "scope-mismatch", "Logic.Fixed" );
    mismatchContext.moduleBudgetId = "logic"; mismatchContext.budgetScope = "when_present";
    auto undefinedContext = Context( "undefined-budget", "NoBudget.Update" );
    undefinedContext.moduleBudgetId = "not-defined";
    auto parentContext = Context( "parent", "Root.Parent" );
    parentContext.familyId = "hot-family";
    auto childContext = Context( "child", "Root.Child" );
    childContext.familyId = "hot-family";
    childContext.frames = {
        { 1, 1000000, { "event-1" }, "tree-a" }, { 2, 2000000, { "event-2" }, "tree-a" },
        { 3, 20000000, { "event-3" }, "tree-a" }, { 4, 20000000, { "event-4" }, "tree-a" },
        { 5, 30000000, { "event-5" }, "tree-a" }, { 6, 1000000, { "event-6" }, "tree-b" }
    };
    auto isolatedContext = Context( "isolated", "Isolated.Work" );
    isolatedContext.frames = { { 42, 4000000, { "event-42" }, "tree-c" } };
    auto focusContext = Context( "focus", "Focus Target Subsystem" );
    auto lowContext = Context( "low", "Low.Work" );
    input.signatures = { frameContext, budgetContext, mismatchContext, undefinedContext,
        parentContext, childContext, isolatedContext, focusContext, lowContext };
    input.capacityFacts = {
        { "gpu.memory", "gpu-local-peak", "gpu-capacity", "gpu_memory",
            7000000000ULL, 77, true, "DXGI Local peak" }
    };

    const auto result = EvaluateCandidatePolicy( input );
    assert( result.valid && result.policyIdentity.size() == 64 && result.contentSha256.size() == 64 );
    assert( result.aggregateIdentity == input.aggregateIdentity );

    if( std::any_of( result.candidates.begin(), result.candidates.end(), []( const auto& value ) {
        return value.domain == "quality" || value.priority == CandidatePriority::P0 ||
            HasTrigger( value, CandidateTrigger::Quality );
    } ) )
    { std::cerr << "capture quality must not enter performance candidates\n"; return 1; }
    const auto qualityDocument = json::parse( SerializeCandidatePolicyResult( result ) );
    assert( qualityDocument.at( "capture_quality" ).size() == 1 );
    assert( qualityDocument.at( "capture_quality" )[0].at( "domain" ) == "gpu.catalog" );
    assert( !qualityDocument.at( "capture_quality" )[0].contains( "priority" ) );
    auto invalidAudit = input;
    invalidAudit.aggregate.qualityComplete = false;
    assert( !EvaluateCandidatePolicy( invalidAudit ).valid );
    invalidAudit.aggregate.qualityComplete = true;
    invalidAudit.aggregate.unreportedGapCount = 1;
    assert( !EvaluateCandidatePolicy( invalidAudit ).valid );
    const auto& capacity = Find( result, "gpu-capacity" );
    assert( capacity.priority == CandidatePriority::P1 && HasTrigger( capacity, CandidateTrigger::Capacity ) );
    const auto& frameBudget = Find( result, "cpu:frame" );
    assert( frameBudget.priority == CandidatePriority::P1 && HasTrigger( frameBudget, CandidateTrigger::Budget ) );
    if( std::any_of( result.candidates.begin(), result.candidates.end(), []( const auto& c ) {
        return c.signatureId == "budget" && HasTrigger( c, CandidateTrigger::Budget );
    } ) ) { std::cerr << "Planning-only module budgets must not create performance candidates\n"; return 1; }
    const auto& hot = Find( result, "hot-family" );
    assert( hot.priority == CandidatePriority::P2 && HasTrigger( hot, CandidateTrigger::Top ) &&
        HasTrigger( hot, CandidateTrigger::Anomaly ) && hot.memberSignatures.size() == 2 );
    const auto hotAnomaly = std::find_if( hot.triggerEvidence.begin(), hot.triggerEvidence.end(),
        []( const auto& value ) { return value.trigger == CandidateTrigger::Anomaly; } );
    assert( hotAnomaly != hot.triggerEvidence.end() && hotAnomaly->observedExact == "200" );
    const auto& anomaly = Find( result, "cpu:isolated" );
    assert( anomaly.priority == CandidatePriority::P3 && HasTrigger( anomaly, CandidateTrigger::Anomaly ) );
    const auto& focused = Find( result, "cpu:focus" );
    assert( focused.priority == CandidatePriority::P2 && HasTrigger( focused, CandidateTrigger::UserFocus ) && focused.selected );
    const auto& lowCandidate = Find( result, "cpu:low" );
    assert( lowCandidate.priority == CandidatePriority::P4 && !lowCandidate.selected &&
        lowCandidate.notSelectedReason == "outside_top_policy" );
    assert( std::none_of( result.candidates.begin(), result.candidates.end(), []( const auto& value ) {
        return value.signatureId == "scope-mismatch" || value.signatureId == "undefined-budget";
    } ) );

    for( size_t index = 1; index < result.candidates.size(); ++index )
        assert( result.candidates[index - 1].priority <= result.candidates[index].priority );

    const auto serialized = json::parse( SerializeCandidatePolicyResult( result ) );
    AssertSchemaDeclaresEmittedFields( serialized );
    const auto& capacityJson = FindJsonCandidate( serialized, "gpu-capacity" );
    const auto capacityEvidence = std::find_if( capacityJson.at( "trigger_evidence" ).begin(),
        capacityJson.at( "trigger_evidence" ).end(), []( const auto& value ) {
            return value.at( "trigger" ) == "capacity";
        } );
    assert( capacityEvidence != capacityJson.at( "trigger_evidence" ).end() );
    assert( capacityEvidence->at( "observed_value" ) == "7000000000" );
    assert( capacityEvidence->at( "threshold_value" ) == "6400000000" );

    std::set<std::string> representativeReasons;
    for( const auto& representative : hot.representativeFrames )
        representativeReasons.insert( representative.reasons.begin(), representative.reasons.end() );
    for( const char* reason : { "worst", "typical_bad", "first", "last",
        "burst_start", "burst_peak", "burst_end", "structure_change" } )
        assert( representativeReasons.contains( reason ) );

    auto reordered = input;
    std::reverse( reordered.signatures.begin(), reordered.signatures.end() );
    std::reverse( reordered.capacityFacts.begin(), reordered.capacityFacts.end() );
    assert( EvaluateCandidatePolicy( reordered ).contentSha256 == result.contentSha256 );

    // The same stable source site may be observed under multiple FrameSets.
    // Policy lookup must use frame_scope as part of the aggregate identity;
    // otherwise the first scope silently supplies the other scope's metrics.
    CandidatePolicyInput scoped;
    scoped.aggregateIdentity = std::string( 64, '7' );
    scoped.normalizedProfile = Profile();
    scoped.profileIdentity = JsonSha( scoped.normalizedProfile );
    scoped.aggregate.qualityComplete = true;
    scoped.aggregate.contentSha256 = std::string( 64, '8' );
    auto playerScoped = Signature( "shared-site" );
    playerScoped.frameScope = "Player.Frame";
    playerScoped.inclusive.perCompleteFrame.p95 = 10'000'000;
    auto renderScoped = Signature( "shared-site" );
    renderScoped.frameScope = "Render.Frame";
    renderScoped.inclusive.perCompleteFrame.p95 = 20'000'000;
    scoped.aggregate.signatures = { playerScoped, renderScoped };
    auto playerContext = Context( "shared-site", "PlayerRoot" );
    playerContext.frameScope = "Player.Frame";
    playerContext.frameRoot = true;
    auto renderContext = Context( "shared-site", "RenderRoot" );
    renderContext.frameScope = "Render.Frame";
    renderContext.frameRoot = true;
    scoped.signatures = { playerContext, renderContext };
    const auto scopedResult = EvaluateCandidatePolicy( scoped );
    assert( scopedResult.valid );
    const auto& scopedCandidate = Find( scopedResult, "cpu:shared-site" );
    assert( HasTrigger( scopedCandidate, CandidateTrigger::Budget ) );
    assert( std::any_of( scopedCandidate.triggerEvidence.begin(), scopedCandidate.triggerEvidence.end(),
        []( const auto& value ) { return value.trigger == CandidateTrigger::Budget &&
            value.scope == "per_complete_frame" && value.observedExact == "20000000"; } ) );

    auto changedProfile = input;
    changedProfile.normalizedProfile["candidate_policy"]["top_n"] = 3;
    changedProfile.profileIdentity = JsonSha( changedProfile.normalizedProfile );
    const auto changed = EvaluateCandidatePolicy( changedProfile );
    assert( changed.valid && changed.aggregateIdentity == result.aggregateIdentity );
    assert( changed.policyIdentity != result.policyIdentity );

    auto restrictedPriorities = input;
    restrictedPriorities.normalizedProfile["candidate_policy"]["priorities"] = { "P0", "P1" };
    restrictedPriorities.profileIdentity = JsonSha( restrictedPriorities.normalizedProfile );
    const auto restricted = EvaluateCandidatePolicy( restrictedPriorities );
    assert( restricted.valid );
    assert( Find( restricted, "cpu:focus" ).selected );

    auto duplicateContext = input;
    duplicateContext.signatures.push_back( duplicateContext.signatures.front() );
    const auto duplicate = EvaluateCandidatePolicy( duplicateContext );
    assert( !duplicate.valid && duplicate.error == "duplicate_policy_signature_context" );

    auto duplicateAggregate = input;
    duplicateAggregate.aggregate.signatures.push_back( duplicateAggregate.aggregate.signatures.front() );
    const auto duplicateNeutral = EvaluateCandidatePolicy( duplicateAggregate );
    assert( !duplicateNeutral.valid && duplicateNeutral.error == "duplicate_neutral_signature_aggregate" );

    auto incompleteProfile = input;
    incompleteProfile.normalizedProfile.erase( "module_budgets" );
    incompleteProfile.profileIdentity = JsonSha( incompleteProfile.normalizedProfile );
    bool incompleteThrew = false;
    CandidatePolicyResult incomplete;
    try { incomplete = EvaluateCandidatePolicy( incompleteProfile ); }
    catch( ... ) { incompleteThrew = true; }
    assert( !incompleteThrew && !incomplete.valid );

    auto invalidPriority = input;
    invalidPriority.normalizedProfile["candidate_policy"]["priorities"] = { "P0", "PX" };
    invalidPriority.profileIdentity = JsonSha( invalidPriority.normalizedProfile );
    assert( !EvaluateCandidatePolicy( invalidPriority ).valid );

    CandidatePolicyInput capped;
    capped.aggregateIdentity = std::string( 64, 'e' );
    capped.normalizedProfile = Profile( 10, 0.80, 50 );
    capped.profileIdentity = JsonSha( capped.normalizedProfile );
    capped.aggregate.qualityComplete = true;
    capped.aggregate.contentSha256 = std::string( 64, '1' );
    NeutralDomainRanking cappedRanking; cappedRanking.domain = "cpu";
    for( uint64_t index = 0; index < 60; ++index )
    {
        const auto id = "rank-" + std::to_string( index );
        auto signature = Signature( id.c_str() );
        capped.aggregate.signatures.push_back( signature );
        cappedRanking.exclusive.push_back( { id, "Player.Frame", false,
            int64_t( 1000 - index ), 0, 0, 0.01, double( index + 1 ) / 100.0 } );
    }
    capped.aggregate.rankings.push_back( std::move( cappedRanking ) );
    capped.aggregate.domains = { { "cpu", true, "complete", 60, 60, 60, 60,
        std::string( 64, 'f' ), std::string( 64, 'f' ), true, {} } };
    const auto cappedResult = EvaluateCandidatePolicy( capped );
    assert( cappedResult.valid && cappedResult.candidates.size() == 60 );
    assert( cappedResult.rankedSignatures.size() == 60 );
    assert( cappedResult.backlog.total == 60 && cappedResult.backlog.selected == 50 &&
        cappedResult.backlog.notSelected == 10 );

    const auto temporary = std::filesystem::temp_directory_path() /
        ( "jn-tracy-candidate-policy-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count() ) );
    std::filesystem::create_directories( temporary );
    std::string error;
    const auto manifestPath = temporary / "candidate-manifest.json";
    assert( WriteCandidatePolicyManifest( manifestPath, result, error ) );
    const auto parsed = json::parse( std::ifstream( manifestPath, std::ios::binary ) );
    assert( parsed["policy_identity"] == result.policyIdentity );
    assert( parsed["backlog"]["total"] == result.backlog.total );
    assert( !parsed.contains( "weighted_score" ) );
    std::error_code ignored;
    std::filesystem::remove_all( temporary, ignored );
    return 0;
}
