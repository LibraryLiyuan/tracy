#include "TracyAnalysisScanManager.hpp"

#include "TracyExactStatistics.hpp"
#include "TracyFrameCpuScanner.hpp"
#include "TracyPolicyFrameSeries.hpp"
#include "TracyGpuJobManagedScanner.hpp"
#include "TracyHash.hpp"
#include "TracyMemoryIoSamplingTelemetryScanner.hpp"
#include "TracyNeutralAggregateStore.hpp"
#include "../../public/common/TracyQueue.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace tracy::query
{
namespace
{

using nlohmann::json;
using namespace tracy::analysis;

std::string HashText( std::string_view value )
{
    Sha256Builder hash;
    hash.Update( value.data(), value.size() );
    return hash.FinalHex();
}

bool ReplaceFile( const std::filesystem::path& from, const std::filesystem::path& to,
    std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING |
        MOVEFILE_WRITE_THROUGH ) != FALSE ) return true;
    error = "analysis_scan_atomic_replace_failed:" + std::to_string( GetLastError() );
#else
    std::error_code ec;
    std::filesystem::rename( from, to, ec );
    if( !ec ) return true;
    error = "analysis_scan_atomic_replace_failed:" + ec.message();
#endif
    return false;
}

bool WriteJson( const std::filesystem::path& path, const json& value, std::string& error )
{
    std::error_code ec;
    std::filesystem::create_directories( path.parent_path(), ec );
    if( ec ) { error = "analysis_scan_directory_failed:" + ec.message(); return false; }
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
    if( !output ) { error = "analysis_scan_open_failed"; return false; }
    output << value.dump() << '\n';
    output.flush();
    if( !output ) { error = "analysis_scan_write_failed"; return false; }
    output.close();
    return ReplaceFile( temporary, path, error );
}

json ReadJson( const std::filesystem::path& path )
{
    std::ifstream input( path, std::ios::binary );
    if( !input ) throw std::runtime_error( "analysis_scan_file_missing:" + path.string() );
    return json::parse( input );
}

ScanState ParseState( const std::string& value )
{
    for( const auto state : { ScanState::ContractOnly, ScanState::Queued,
        ScanState::Validating, ScanState::Scanning, ScanState::Aggregating,
        ScanState::EvaluatingPolicy, ScanState::Auditing, ScanState::Complete,
        ScanState::CancelledResumable, ScanState::Failed } )
        if( value == ScanStateName( state ) ) return state;
    return ScanState::Failed;
}

uint64_t JsonU64( const json& value )
{
    if( value.is_string() ) return std::stoull( value.get<std::string>() );
    return value.get<uint64_t>();
}

json ContextDocument( const AnalysisScanProducts& products )
{
    json result = { { "schema_version", 2 }, { "signatures", json::array() },
        { "capacity_facts", json::array() }, { "frame_timelines", json::array() } };
    for( const auto& value : products.signatureContexts )
    {
        json frames = json::array();
        for( const auto& frame : value.frames ) frames.push_back( {
            { "frame_index", std::to_string( frame.frameIndex ) },
            { "value_ns", std::to_string( frame.valueNs ) },
            { "event_refs", frame.eventRefs }, { "structure_key", frame.structureKey } } );
        result["signatures"].push_back( {
            { "domain", value.domain }, { "signature_id", value.signatureId },
            { "family_id", value.familyId }, { "parent_signature_id", value.parentSignatureId },
            { "name", value.name }, { "path", value.path }, { "frame_scope", value.frameScope },
            { "metric_preference", value.metricPreference }, { "module_budget_id", value.moduleBudgetId },
            { "budget_scope", value.budgetScope }, { "frame_root", value.frameRoot },
            { "proven_critical_path", value.provenCriticalPath }, { "frames", std::move( frames ) },
            { "frame_series_complete", value.frameSeriesComplete }, { "thread_or_queue", value.threadOrQueue },
            { "series_offset", std::to_string( value.seriesOffset ) }, { "series_count", std::to_string( value.seriesCount ) },
            { "series_sha256", value.seriesSha256 } } );
        result["signatures"].back()["observation_unit"] = value.observationUnit;
    }
    for( const auto& timeline : products.frameTimelines )
    {
        json frames = json::array();
        for( const auto& frame : timeline.frames ) frames.push_back( { std::to_string( frame.frameIndex ),
            std::to_string( frame.valueNs ), frame.exact, frame.eventRefs,
            frame.beginNs ? json( std::to_string( *frame.beginNs ) ) : json( nullptr ),
            frame.endNs ? json( std::to_string( *frame.endNs ) ) : json( nullptr ) } );
        result["frame_timelines"].push_back( { { "frame_scope", timeline.frameScope }, { "name", timeline.name },
            { "observation_unit", timeline.observationUnit }, { "frames", std::move( frames ) } } );
    }
    for( const auto& value : products.capacityFacts ) result["capacity_facts"].push_back( {
        { "domain", value.domain }, { "signature_id", value.signatureId },
        { "family_id", value.familyId }, { "metric", value.metric },
        { "value_bytes", std::to_string( value.valueBytes ) },
        { "frame_index", value.frameIndex ? json( std::to_string( *value.frameIndex ) ) : json( nullptr ) },
        { "exact", value.exact }, { "description", value.description } } );
    result["content_sha256"] = HashText( result.dump() );
    return result;
}

bool ParseContextDocument( const json& document, AnalysisScanProducts& products, std::string& error )
{
    try
    {
        if( document.value( "schema_version", 0u ) != 2 )
        { error = "analysis_scan_context_schema_mismatch"; return false; }
        auto payload = document;
        const auto sha = payload.at( "content_sha256" ).get<std::string>();
        payload.erase( "content_sha256" );
        if( HashText( payload.dump() ) != sha ) { error = "analysis_scan_context_checksum_mismatch"; return false; }
        for( const auto& item : document.at( "signatures" ) )
        {
            PolicySignatureContext value;
            value.domain = item.at( "domain" ).get<std::string>();
            value.signatureId = item.at( "signature_id" ).get<std::string>();
            value.familyId = item.value( "family_id", std::string() );
            value.parentSignatureId = item.value( "parent_signature_id", std::string() );
            value.name = item.value( "name", std::string() );
            value.path = item.value( "path", std::string() );
            value.frameScope = item.value( "frame_scope", std::string() );
            value.metricPreference = item.value( "metric_preference", std::string( "exclusive" ) );
            value.moduleBudgetId = item.value( "module_budget_id", std::string() );
            value.budgetScope = item.value( "budget_scope", std::string() );
            value.frameRoot = item.value( "frame_root", false );
            value.provenCriticalPath = item.value( "proven_critical_path", false );
            value.frameSeriesComplete = item.at( "frame_series_complete" ).get<bool>();
            value.threadOrQueue = item.at( "thread_or_queue" ).get<std::string>();
            value.seriesOffset = JsonU64( item.at( "series_offset" ) );
            value.seriesCount = JsonU64( item.at( "series_count" ) );
            value.seriesSha256 = item.at( "series_sha256" ).get<std::string>();
            value.observationUnit = item.at( "observation_unit" ).get<std::string>();
            for( const auto& frame : item.at( "frames" ) )
                value.frames.push_back( { JsonU64( frame.at( "frame_index" ) ),
                    int64_t( JsonU64( frame.at( "value_ns" ) ) ),
                    frame.at( "event_refs" ).get<std::vector<std::string>>(),
                    frame.value( "structure_key", std::string() ) } );
            products.signatureContexts.push_back( std::move( value ) );
        }
        for( const auto& item : document.at( "frame_timelines" ) )
        {
            PolicyFrameTimeline timeline;
            timeline.frameScope = item.at( "frame_scope" ).get<std::string>();
            timeline.name = item.at( "name" ).get<std::string>();
            timeline.observationUnit = item.at( "observation_unit" ).get<std::string>();
            for( const auto& frame : item.at( "frames" ) )
            {
                PolicyFrameEvidence value { JsonU64( frame[0] ), int64_t( JsonU64( frame[1] ) ),
                    frame[3].get<std::vector<std::string>>(), {}, frame[2].get<bool>() };
                if( !frame[4].is_null() ) value.beginNs = std::stoll( frame[4].get<std::string>() );
                if( !frame[5].is_null() ) value.endNs = std::stoll( frame[5].get<std::string>() );
                timeline.frames.push_back( std::move( value ) );
            }
            products.frameTimelines.push_back( std::move( timeline ) );
        }
        for( const auto& item : document.at( "capacity_facts" ) )
        {
            PolicyCapacityFact value;
            value.domain = item.at( "domain" ).get<std::string>();
            value.signatureId = item.at( "signature_id" ).get<std::string>();
            value.familyId = item.value( "family_id", std::string() );
            value.metric = item.at( "metric" ).get<std::string>();
            value.valueBytes = JsonU64( item.at( "value_bytes" ) );
            if( !item.at( "frame_index" ).is_null() ) value.frameIndex = JsonU64( item.at( "frame_index" ) );
            value.exact = item.value( "exact", true );
            value.description = item.value( "description", std::string() );
            products.capacityFacts.push_back( std::move( value ) );
        }
        return true;
    }
    catch( const std::exception& exception )
    {
        error = "analysis_scan_context_parse_failed:" + std::string( exception.what() );
        return false;
    }
}

size_t CursorOffset( const std::string& cursor )
{
    if( cursor.empty() ) return 0;
    size_t consumed = 0;
    const auto value = std::stoull( cursor, &consumed );
    if( consumed != cursor.size() || value > std::numeric_limits<size_t>::max() )
        throw std::runtime_error( "analysis_scan_invalid_cursor" );
    return size_t( value );
}

}

bool ExecuteDefaultAnalysisScan( const AnalysisScanExecutionRequest& request,
    std::stop_token stopToken, const AnalysisScanProgressCallback& progress,
    AnalysisScanProducts& products, std::string& error )
{
    products = {};
    error.clear();
    if( !request.source ) { error = "trace_source_unavailable"; return false; }
    const auto cancelled = [&] {
        if( !stopToken.stop_requested() ) return false;
        error = "cancelled";
        return true;
    };
    try
    {
        const auto statisticsRoot = request.temporaryRoot / "statistics";
        std::filesystem::create_directories( statisticsRoot );
        NeutralStatisticsStreamBuilder statistics( statisticsRoot, 65536, 65536 );
        std::map<std::string, size_t> contexts;
        const auto contextKey = []( std::string_view domain, std::string_view signature,
            std::string_view frameScope ) {
            return std::string( domain ) + '\n' + std::string( signature ) + '\n' +
                std::string( frameScope );
        };
        const auto ensureContext = [&]( std::string domain, std::string signature,
            std::string family, std::string parent, std::string name, std::string path,
            std::string frameScope, std::string metric, bool frameRoot, bool critical ) -> PolicySignatureContext& {
            const auto key = contextKey( domain, signature, frameScope );
            const auto found = contexts.find( key );
            if( found != contexts.end() ) return products.signatureContexts[found->second];
            PolicySignatureContext value;
            value.domain = std::move( domain ); value.signatureId = std::move( signature );
            value.familyId = std::move( family ); value.parentSignatureId = std::move( parent );
            value.name = std::move( name ); value.path = std::move( path );
            value.frameScope = std::move( frameScope ); value.metricPreference = std::move( metric );
            value.frameRoot = frameRoot; value.provenCriticalPath = critical;
            contexts.emplace( key, products.signatureContexts.size() );
            products.signatureContexts.push_back( std::move( value ) );
            return products.signatureContexts.back();
        };
        // Scan DTOs (notably Jobs/relations and memory/sampling facts) must not
        // coexist with the external merge's complete aggregate output. All
        // needed numeric facts/context have been copied into the bounded store
        // before this scope ends. The source Worker itself remains unchanged.
        {
        using ScopedSignature = std::pair<std::string, std::string>;
        std::set<ScopedSignature> cpuOutputs, gpuOutputs, jobOutputs;
        std::unordered_map<uint64_t, NeutralStatisticsStreamBuilder::SignatureToken> cpuTokens;

        progress( ScanState::Scanning, 0, 4, "cpu_frame_zone_scan" );
        CpuFrameScanOptions cpuOptions;
        cpuOptions.includeExactSignatures = true;
        cpuOptions.includeLogicalSignatures = false;
        cpuOptions.logicalSignatureMode = CpuLogicalSignatureMode::FullPath;
        std::map<std::string, std::vector<PolicyFrameEvidence>> frameTimelines;
        cpuOptions.completeFrameSink = [&]( const auto& scope, uint64_t index, int64_t begin, int64_t end ) {
            frameTimelines[scope].push_back( { index, end-begin, {}, {}, true } );
        };
        const auto cpu = ExactFrameCpuScanner( *request.source ).ScanView(
            [&]( const CpuSignatureFrameRunView& run ) {
                if( stopToken.stop_requested() ) return false;
                // Preserve unknown-frame membership. Skipping a source-invalid
                // run here would turn it into an implicit known-zero later.
                const auto wait = run.workClass == CpuWorkClass::Wait ||
                    run.workClass == CpuWorkClass::IntentionalPacing;
                const auto key = ( uint64_t( run.signatureOrdinal ) << 32 ) | run.frameSetOrdinal;
                auto found = cpuTokens.find( key );
                if( found == cpuTokens.end() )
                {
                    const auto token = statistics.RegisterSignature( "cpu", run.signatureId, run.frameSetRef );
                    if( token == NeutralStatisticsStreamBuilder::InvalidSignatureToken )
                        throw std::runtime_error( "neutral_cpu_signature_register_failed" );
                    found = cpuTokens.emplace( key, token ).first;
                    cpuOutputs.emplace( std::string( run.signatureId ), std::string( run.frameSetRef ) );
                }
                if( !statistics.AddRegisteredRun( found->second, uint64_t( run.frameIndex ),
                    run.inclusiveNs, run.exclusiveNs, wait ? run.inclusiveNs : 0, 0,
                    run.occurrenceCount, run.exact, false ) )
                    throw std::runtime_error( "neutral_cpu_run_ingest_failed" );
                return true;
            }, cpuOptions );
        if( cancelled() ) return false;
        for( const auto& set : cpu.frameSetDenominators )
        {
            products.frameTimelines.push_back( { set.frameSetRef, set.name, std::move( frameTimelines[set.frameSetRef] ) } );
            auto& root = ensureContext( "cpu", "frame-wall:" + set.frameSetRef, "frame-wall:" + set.frameSetRef,
                {}, set.name, set.name, set.frameSetRef, "inclusive", true, false );
            root.frames = products.frameTimelines.back().frames;
            root.frameSeriesComplete = true;
        }
        std::map<std::string, CpuSignatureDefinition> cpuDefinitions;
        for( const auto& value : cpu.signatures ) cpuDefinitions.emplace( value.signatureId, value );
        std::set<std::tuple<std::string, std::string, std::string>> denominators;
        for( const auto& value : cpu.denominators )
        {
            if( !cpuOutputs.contains( { value.signatureId, value.frameSetRef } ) ) continue;
            if( denominators.emplace( "cpu", value.signatureId, value.frameSetRef ).second )
                statistics.AddDenominator( { "cpu", value.signatureId, value.frameSetRef,
                    value.completeFrameDenominator } );
            const auto definition = cpuDefinitions.find( value.signatureId );
            const auto wait = definition != cpuDefinitions.end() &&
                ( definition->second.workClass == CpuWorkClass::Wait ||
                  definition->second.workClass == CpuWorkClass::IntentionalPacing );
            auto& context = ensureContext( "cpu", value.signatureId,
                "cpu:" + value.signatureId + ":" + value.frameSetRef,
                definition == cpuDefinitions.end() ? std::string() : definition->second.parentSignatureId,
                definition == cpuDefinitions.end() ? value.signatureId : definition->second.name,
                definition == cpuDefinitions.end() ? value.signatureId : definition->second.path,
                value.frameSetRef, wait ? "wait" : "exclusive", false, false );
            if( definition != cpuDefinitions.end() ) context.threadOrQueue = definition->second.threadRef;
        }

        progress( ScanState::Scanning, 1, 4, "gpu_job_managed_relation_scan" );
        uint64_t playerFrameDenominator = 0;
        for( const auto& frameSet : cpu.frameSetDenominators )
            if( frameSet.name == "Player.Frame" )
                playerFrameDenominator += frameSet.completeFrameCount;
        // Job originFrameId is a process-wide correlated frame identity, not
        // an index into the Player.Frame FrameSet. Present, Editor and other
        // canonical domains can therefore allocate valid identities of their
        // own. Only identities with a proven Player Begin/End pair belong in
        // per-Player-frame job statistics.
        std::unordered_map<uint64_t, uint8_t> playerFrameBoundaryMask;
        for( const auto& frame : request.source->GetCorrelatedFrameEvents() )
        {
            if( frame.frameId == 0 || frame.domain != uint8_t( tracy::JnFrameDomain::Player ) ) continue;
            auto& mask = playerFrameBoundaryMask[frame.frameId];
            if( frame.phase == uint8_t( tracy::JnFramePhase::Begin ) ) mask |= 1;
            else if( frame.phase == uint8_t( tracy::JnFramePhase::End ) ) mask |= 2;
        }
        std::unordered_set<uint64_t> completePlayerFrameIds;
        completePlayerFrameIds.reserve( playerFrameBoundaryMask.size() );
        for( const auto& [frameId, mask] : playerFrameBoundaryMask )
            if( mask == 3 ) completePlayerFrameIds.emplace( frameId );
        const auto jobPlayerFrameDenominator = uint64_t( completePlayerFrameIds.size() );
        std::map<std::string, std::string> gpuPresent;
        std::map<std::string, PolicyFrameTimeline> gpuTimelines;
        std::set<std::string> jobPresent;
        GpuJobManagedScanOptions gpuOptions;
        gpuOptions.retainDetails = false;
        gpuOptions.includeExactGpuSignatures = true;
        gpuOptions.includeLogicalGpuSignatures = false;
        gpuOptions.logicalSignatureMode = GpuLogicalSignatureMode::FullPath;
        gpuOptions.gpuZoneSink = [&]( const GpuZoneScanFact& value ) {
            if( stopToken.stop_requested() ) return false;
            if( !value.physicalTimingExact ) return true;
            const auto scope = "GPU.L0Segment:" + value.contextRef;
            if( !statistics.AddRun( { "gpu", value.signatureId, scope, value.l0SegmentOrdinal,
                value.inclusiveNs, value.exclusiveNs, 0, 0, 1, true, false } ) )
                throw std::runtime_error( "neutral_gpu_run_ingest_failed" );
            gpuPresent.emplace( value.signatureId, scope );
            gpuOutputs.emplace( value.signatureId, scope );
            if( value.depth == 0 )
            {
                auto& timeline = gpuTimelines[scope];
                timeline.frameScope = scope; timeline.name = "Physical GPU L0 segments (not Player frames)";
                timeline.observationUnit = "l0_segment";
                timeline.frames.push_back( { value.l0SegmentOrdinal, value.inclusiveNs, { value.zoneRef }, {}, true, value.beginNs, value.endNs } );
            }
            return true;
        };
        gpuOptions.jobSink = [&]( const JobLifecycleFact& job ) {
            if( stopToken.stop_requested() ) return false;
            if( job.originFrameId == 0 || !job.exact ||
                !completePlayerFrameIds.contains( job.originFrameId ) ) return true;
            const auto signature = "job:type:" + std::to_string( job.typeId ) + ":" + job.name;
            if( !statistics.AddRun( { "job", signature, "Player.Frame", job.originFrameId,
                job.executionNs + job.waitNs, job.executionNs, job.waitNs,
                0, 1, true, false } ) )
                throw std::runtime_error( "neutral_job_run_ingest_failed" );
            jobPresent.emplace( signature );
            jobOutputs.emplace( signature, "Player.Frame" );
            ensureContext( "job", signature, "job:type:" + std::to_string( job.typeId ),
                {}, job.name, job.name, "Player.Frame",
                job.waitNs > job.executionNs ? "wait" : "exclusive", false,
                false ); // A continuation is not proof that this job blocked a frame.
            return true;
        };
        const auto gpuJob = GpuJobManagedScanner( *request.source ).Scan( gpuOptions );
        if( cancelled() ) return false;
        if( !jobPresent.empty() && jobPlayerFrameDenominator == 0 )
            throw std::runtime_error( "job_player_frame_identity_unavailable" );
        std::map<std::string, GpuSignatureDefinition> gpuDefinitions;
        for( const auto& value : gpuJob.gpuSignatures ) gpuDefinitions.emplace( value.signatureId, value );
        for( const auto& [signature, scope] : gpuPresent )
        {
            statistics.AddDenominator( { "gpu", signature, scope, gpuTimelines[scope].frames.size() } );
            const auto definition = gpuDefinitions.find( signature );
            auto& context = ensureContext( "gpu", signature,
                "gpu:" + signature + ":" + scope,
                definition == gpuDefinitions.end() ? std::string() : definition->second.parentSignatureId,
                definition == gpuDefinitions.end() ? signature : definition->second.name,
                definition == gpuDefinitions.end() ? signature : definition->second.path,
                scope, "exclusive", false, false );
            context.threadOrQueue = definition == gpuDefinitions.end() ? scope : definition->second.contextRef;
            context.observationUnit = "l0_segment";
        }
        for( auto& [_, timeline] : gpuTimelines ) products.frameTimelines.push_back( std::move( timeline ) );

        for( const auto& signature : jobPresent )
            statistics.AddDenominator( { "job", signature, "Player.Frame", jobPlayerFrameDenominator } );

        progress( ScanState::Scanning, 2, 4, "memory_io_sampling_telemetry_scan" );
        const auto system = MemoryIoSamplingTelemetryScanner( *request.source ).Scan();
        if( cancelled() ) return false;

        const auto summarizeFindings = []( const auto& findings,
            std::initializer_list<std::string_view> prefixes ) {
            std::string summary;
            for( const auto& finding : findings )
            {
                const auto matches = prefixes.size() == 0 || std::any_of( prefixes.begin(), prefixes.end(),
                    [&]( std::string_view prefix ) { return std::string_view( finding.code ).starts_with( prefix ); } );
                if( !matches ) continue;
                if( !summary.empty() ) summary += ';';
                summary += finding.code + "=" + std::to_string( finding.count );
            }
            return summary;
        };
        const auto sourceStatus = []( bool present, const std::string& reason ) {
            return !present ? std::string( "absent" ) :
                reason.empty() ? std::string( "complete" ) : std::string( "invalid" );
        };
        const auto audit = [&]( std::string domain, bool present, std::string status,
            uint64_t inputCount, uint64_t outputCount, std::string reason = {} ) {
            const auto checksum = HashText( domain + "\n" + std::to_string( inputCount ) + "\n" +
                std::to_string( outputCount ) + "\n" + status );
            // qualityComplete describes this scanner's lossless transfer into
            // the neutral aggregate. Source-domain degradation is represented
            // by status=invalid plus an explicit reason, so CandidatePolicy can
            // retain a quality appendix without publishing corrupt numbers.
            statistics.AddDomainAudit( { std::move( domain ), present, std::move( status ),
                inputCount, inputCount, outputCount, checksum, checksum, true, std::move( reason ) } );
        };
        const auto cpuReason = summarizeFindings( cpu.qualityFindings, {} );
        const auto gpuReason = summarizeFindings( gpuJob.qualityFindings,
            { "gpu_", "missing_gpu_" } );
        const auto jobReason = summarizeFindings( gpuJob.qualityFindings, { "job_" } );
        const auto managedReason = summarizeFindings( gpuJob.qualityFindings, { "script_" } );
        const auto relationReason = summarizeFindings( gpuJob.qualityFindings, { "relation_" } );
        const auto memoryReason = summarizeFindings( system.qualityFindings, { "cpu_memory_" } );
        const auto gpuMemoryReason = summarizeFindings( system.qualityFindings, { "gpu_memory_" } );
        const auto ioReason = summarizeFindings( system.qualityFindings, { "io_" } );
        const auto samplingReason = summarizeFindings( system.qualityFindings, { "sampling_" } );
        const auto schedulingReason = summarizeFindings( system.qualityFindings, { "scheduling_" } );
        const auto telemetryReason = summarizeFindings( system.qualityFindings, { "telemetry_" } );

        audit( "cpu", cpu.inputZoneCount != 0,
            sourceStatus( cpu.inputZoneCount != 0, cpuReason ),
            cpu.inputZoneCount, cpuOutputs.size(), cpuReason );
        audit( "gpu", gpuJob.gpuZoneCount != 0,
            sourceStatus( gpuJob.gpuZoneCount != 0, gpuReason ),
            gpuJob.gpuZoneCount, gpuOutputs.size(), gpuReason );
        audit( "job", gpuJob.jobCount != 0,
            sourceStatus( gpuJob.jobCount != 0, jobReason ),
            gpuJob.jobCount, jobOutputs.size(), jobReason );
        audit( "managed", gpuJob.managedZoneCount != 0,
            sourceStatus( gpuJob.managedZoneCount != 0, managedReason ),
            gpuJob.managedZoneCount, 0, managedReason );
        audit( "relation", gpuJob.relationCount != 0,
            sourceStatus( gpuJob.relationCount != 0, relationReason ),
            gpuJob.relationCount, 0, relationReason );
        // Audit source records, not compact analysis outputs.  A pool, a
        // unique allocation, a scheduling role or a producer is a summary and
        // cannot prove that every source record was consumed.
        audit( "memory", system.inputMemoryEventCount != 0,
            sourceStatus( system.inputMemoryEventCount != 0, memoryReason ),
            system.inputMemoryEventCount, 0, memoryReason );
        const bool gpuMemoryPresent = system.gpuMemory.physicalFactsAvailable ||
            system.inputGpuAllocationCount != 0;
        audit( "gpu_memory", gpuMemoryPresent,
            sourceStatus( gpuMemoryPresent, gpuMemoryReason ),
            system.inputGpuAllocationCount, 0, gpuMemoryReason );
        const uint64_t gpuCatalogInputs = system.inputGpuResourceCount +
            system.inputGpuPassCount + system.inputGpuRangeCount;
        const bool gpuCatalogPresent = system.gpuMemory.catalogState == GpuCatalogState::Complete ||
            system.gpuMemory.catalogState == GpuCatalogState::Invalid;
        const auto gpuCatalogStatus = !gpuCatalogPresent ? std::string( "absent" ) :
            system.gpuMemory.catalogState == GpuCatalogState::Complete ?
                std::string( "complete" ) : std::string( "invalid" );
        const auto gpuCatalogReason = system.gpuMemory.catalogState == GpuCatalogState::Complete ?
            std::string() : system.gpuMemory.catalogReason;
        audit( "gpu_catalog", gpuCatalogPresent, gpuCatalogStatus,
            gpuCatalogInputs, 0, gpuCatalogReason );
        audit( "io", system.inputIoRequestCount != 0,
            sourceStatus( system.inputIoRequestCount != 0, ioReason ),
            system.inputIoRequestCount, 0, ioReason );
        audit( "sampling", system.sampling.available,
            sourceStatus( system.sampling.available, samplingReason ),
            system.inputSampleCount, 0,
            system.sampling.available ? samplingReason : system.sampling.unavailableReason );
        audit( "scheduling", system.scheduling.available,
            sourceStatus( system.scheduling.available, schedulingReason ),
            system.inputContextSwitchCount, 0,
            system.scheduling.available ? schedulingReason : system.scheduling.unavailableReason );
        audit( "telemetry", system.telemetry.producerQualityPresent,
            sourceStatus( system.telemetry.producerQualityPresent, telemetryReason ),
            system.inputTelemetryRecordCount, 0, telemetryReason );

        for( const auto& pool : system.cpuMemoryPools ) if( pool.peakLiveBytes != 0 )
            products.capacityFacts.push_back( { "memory.cpu", pool.poolRef,
                "cpu-memory-pool:" + pool.poolRef, "cpu_memory", pool.peakLiveBytes,
                std::nullopt, !pool.hasAccountingGap, pool.name } );
        if( system.gpuMemory.physicalFactsAvailable )
            products.capacityFacts.push_back( { "memory.gpu", "gpu-physical-peak",
                "gpu-physical-capacity", "gpu_memory", system.gpuMemory.physicalBytes,
                std::nullopt, system.gpuMemory.physicalPeakExact,
                "Engine-known concurrently live root-allocation peak" } );

        }
        progress( ScanState::Aggregating, 3, 4, "exact_statistics" );
        products.frameSeriesPath = request.temporaryRoot / "frame-series-v1.bin";
        std::ofstream seriesOutput( products.frameSeriesPath, std::ios::binary | std::ios::trunc );
        if( !seriesOutput ) throw std::runtime_error( "frame_series_open_failed" );
        const auto selectEvidence = [&]( const NeutralSignatureAggregate& aggregate,
            const std::vector<NeutralMergedFrameValues>& frames ) {
            const auto found = contexts.find( contextKey( aggregate.domain,
                aggregate.signatureId, aggregate.frameScope ) );
            if( found == contexts.end() || frames.empty() ) return;
            auto& context = products.signatureContexts[found->second];
            // Preserve all numeric rows on disk before choosing UI representatives.
            // Job origin IDs are not Player.Frame indexes. CPU uses actual
            // FrameSet boundaries; GPU uses explicitly typed L0 segments.
            if( context.domain == "cpu" || context.domain == "gpu" ) WritePolicyFrameSeries( seriesOutput, context, frames );
            const auto value = [&]( const NeutralMergedFrameValues& frame ) {
                if( context.metricPreference == "wait" ) return frame.waitNs;
                if( context.metricPreference == "inclusive" ) return frame.inclusiveNs;
                if( context.metricPreference == "critical" ) return frame.criticalPathNs;
                return frame.exclusiveNs;
            };
            const NeutralMetricAggregate* metric = &aggregate.exclusive;
            if( context.metricPreference == "wait" ) metric = &aggregate.wait;
            else if( context.metricPreference == "inclusive" ) metric = &aggregate.inclusive;
            else if( context.metricPreference == "critical" ) metric = &aggregate.criticalPath;
            std::map<uint64_t, PolicyFrameEvidence> selected;
            const auto add = [&]( const NeutralMergedFrameValues& frame ) {
                selected.emplace( frame.frameIndex, PolicyFrameEvidence { frame.frameIndex,
                    value( frame ), {}, context.path } );
            };
            add( frames.front() ); add( frames.back() );
            std::vector<const NeutralMergedFrameValues*> ordered;
            ordered.reserve( frames.size() );
            for( const auto& frame : frames ) ordered.push_back( &frame );
            std::sort( ordered.begin(), ordered.end(), [&]( const auto* left, const auto* right ) {
                if( value( *left ) != value( *right ) ) return value( *left ) > value( *right );
                return left->frameIndex < right->frameIndex;
            } );
            for( size_t index = 0; index < std::min<size_t>( 3, ordered.size() ); ++index ) add( *ordered[index] );
            const auto p95 = metric->perCompleteFrame.p95;
            std::sort( ordered.begin(), ordered.end(), [&]( const auto* left, const auto* right ) {
                const auto lhs = std::abs( double( value( *left ) ) - p95 );
                const auto rhs = std::abs( double( value( *right ) ) - p95 );
                if( lhs != rhs ) return lhs < rhs;
                return left->frameIndex < right->frameIndex;
            } );
            for( size_t index = 0; index < std::min<size_t>( 2, ordered.size() ); ++index ) add( *ordered[index] );
            if( metric->longestBurstFrames >= 3 && metric->longestBurstStartFrame &&
                metric->longestBurstEndFrame && metric->longestBurstPeakFrame )
            {
                const auto addFrameIndex = [&]( uint64_t frameIndex ) {
                    const auto item = std::lower_bound( frames.begin(), frames.end(), frameIndex,
                        []( const auto& frame, uint64_t index ) { return frame.frameIndex < index; } );
                    if( item != frames.end() && item->frameIndex == frameIndex ) add( *item );
                };
                addFrameIndex( *metric->longestBurstStartFrame );
                addFrameIndex( *metric->longestBurstPeakFrame );
                addFrameIndex( *metric->longestBurstEndFrame );
            }
            else if( metric->longestBurstFrames >= 3 && !metric->anomalies.empty() )
            {
                // Backward-compatible reconstruction for an aggregate that
                // predates explicit bounded longest-burst representatives.
                size_t begin = 0, bestBegin = 0, bestEnd = 0;
                for( size_t index = 1; index <= metric->anomalies.size(); ++index )
                {
                    if( index < metric->anomalies.size() && metric->anomalies[index].frameIndex ==
                        metric->anomalies[index-1].frameIndex + 1 ) continue;
                    if( index - begin > bestEnd - bestBegin + 1 )
                    { bestBegin = begin; bestEnd = index - 1; }
                    begin = index;
                }
                const auto addFrameIndex = [&]( uint64_t frameIndex ) {
                    const auto item = std::lower_bound( frames.begin(), frames.end(), frameIndex,
                        []( const auto& frame, uint64_t index ) { return frame.frameIndex < index; } );
                    if( item != frames.end() && item->frameIndex == frameIndex ) add( *item );
                };
                addFrameIndex( metric->anomalies[bestBegin].frameIndex );
                addFrameIndex( metric->anomalies[bestEnd].frameIndex );
                const auto peak = std::max_element( metric->anomalies.begin() + bestBegin,
                    metric->anomalies.begin() + bestEnd + 1, []( const auto& left, const auto& right ) {
                        return left.valueNs < right.valueNs;
                    } );
                addFrameIndex( peak->frameIndex );
            }
            for( auto& [frame, evidence] : selected ) context.frames.push_back( std::move( evidence ) );
        };
        if( !statistics.Finish( products.aggregate, error, selectEvidence,
            [&] { return stopToken.stop_requested(); } ) ) return false;
        seriesOutput.flush();
        if( !seriesOutput ) throw std::runtime_error( "frame_series_flush_failed" );
        seriesOutput.close();
        if( !products.aggregate.qualityComplete )
        {
            error = "neutral_statistics_quality_incomplete";
            if( !products.aggregate.qualityFindings.empty() )
                error += ":" + products.aggregate.qualityFindings.front();
            return false;
        }
        progress( ScanState::Aggregating, 4, 4, "exact_statistics_complete" );
        return !cancelled();
    }
    catch( const std::exception& exception )
    {
        if( stopToken.stop_requested() ) { error = "cancelled"; return false; }
        error = "analysis_scan_executor_failed:" + std::string( exception.what() );
        return false;
    }
}

struct AnalysisScanManager::Entry
{
    mutable std::mutex mutex;
    AnalysisScanSnapshot snapshot;
    AnalysisScanStartRequest request;
    NeutralAggregateIdentity aggregateIdentity;
    std::filesystem::path scanRoot;
    std::filesystem::path aggregateRoot;
    std::jthread worker;
};

AnalysisScanManager::AnalysisScanManager( std::filesystem::path root,
    std::filesystem::path cacheRoot, std::string queryExecutableSha256,
    AnalysisScanSourceResolver resolver, AnalysisScanExecutor executor )
    : m_root( std::move( root ) )
    , m_cacheRoot( std::move( cacheRoot ) )
    , m_queryExecutableSha256( std::move( queryExecutableSha256 ) )
    , m_resolver( std::move( resolver ) )
    , m_executor( std::move( executor ) )
{
    if( m_root.empty() || m_cacheRoot.empty() || m_queryExecutableSha256.size() != 64 || !m_executor )
        throw std::invalid_argument( "analysis_scan_manager_invalid_configuration" );
    std::filesystem::create_directories( m_root / "scans" );
    std::filesystem::create_directories( m_cacheRoot );
}

AnalysisScanManager::~AnalysisScanManager()
{
    std::vector<std::shared_ptr<Entry>> entries;
    {
        std::lock_guard lock( m_mutex );
        for( const auto& [_, entry] : m_entries ) entries.push_back( entry );
    }
    for( const auto& entry : entries ) if( entry->worker.joinable() ) entry->worker.request_stop();
}

bool AnalysisScanManager::SaveStateLocked( const Entry& entry, std::string& error ) const
{
    const auto& value = entry.snapshot;
    return WriteJson( entry.scanRoot / "scan-state.json", {
        { "schema_version", 1 }, { "scan_id", value.scanId },
        { "state", ScanStateName( value.state ) }, { "resumable", value.resumable },
        { "completed", value.completed }, { "closed", value.closed },
        { "progress_completed", std::to_string( value.progressCompleted ) },
        { "progress_total", std::to_string( value.progressTotal ) }, { "stage", value.stage },
        { "error", value.error.empty() ? json( nullptr ) : json( value.error ) },
        { "trace_session_id", entry.request.traceSessionId },
        { "trace_path", entry.request.tracePath.generic_string() },
        { "trace_strong_id", entry.aggregateIdentity.traceStrongId },
        { "profile_identity", entry.request.profileIdentity },
        { "aggregate_identity", ComputeNeutralAggregateIdentity( entry.aggregateIdentity ) },
        { "aggregate_root", entry.aggregateRoot.generic_string() }
    }, error );
}

void AnalysisScanManager::Update( const std::shared_ptr<Entry>& entry, ScanState state,
    uint64_t completed, uint64_t total, std::string_view stage, std::string_view error ) const
{
    std::lock_guard lock( entry->mutex );
    if( entry->snapshot.state == ScanState::CancelledResumable && state != ScanState::CancelledResumable ) return;
    entry->snapshot.state = state;
    entry->snapshot.progressCompleted = completed;
    entry->snapshot.progressTotal = total;
    entry->snapshot.stage.assign( stage );
    entry->snapshot.error.assign( error );
    entry->snapshot.completed = state == ScanState::Complete;
    entry->snapshot.resumable = state == ScanState::CancelledResumable;
    std::string ignored;
    SaveStateLocked( *entry, ignored );
}

AnalysisScanSnapshot AnalysisScanManager::Start( const AnalysisScanStartRequest& request )
{
    if( request.tracePath.empty() || !request.source || request.profileIdentity.size() != 64 ||
        !request.normalizedProfile.is_object() )
        throw std::invalid_argument( "analysis_scan_start_invalid" );
    const auto traceStrongId = request.source->GetTraceInfo().fingerprint;
    if( traceStrongId.empty() ) throw std::invalid_argument( "analysis_scan_trace_identity_missing" );
    NeutralAggregateIdentity aggregateIdentity { traceStrongId, m_queryExecutableSha256,
        "1.35.0", NeutralScanAlgorithmId, NeutralAggregateSchemaVersion };
    const auto aggregateId = ComputeNeutralAggregateIdentity( aggregateIdentity );
    const auto scanId = "scan-" + HashText( aggregateId + "\n" + request.profileIdentity +
        "\n" + CandidatePolicyAlgorithmId ).substr( 0, 32 );
    {
        std::lock_guard lock( m_mutex );
        const auto found = m_entries.find( scanId );
        if( found != m_entries.end() )
        {
            std::lock_guard entryLock( found->second->mutex );
            return found->second->snapshot;
        }
    }
    if( auto existing = LoadEntry( scanId ) )
    {
        std::lock_guard lock( m_mutex );
        m_entries.emplace( scanId, existing );
        std::lock_guard entryLock( existing->mutex );
        return existing->snapshot;
    }

    auto entry = std::make_shared<Entry>();
    entry->snapshot.scanId = scanId;
    entry->snapshot.state = ScanState::Queued;
    entry->snapshot.stage = "queued";
    entry->request = request;
    entry->aggregateIdentity = std::move( aggregateIdentity );
    entry->scanRoot = m_root / "scans" / scanId;
    entry->aggregateRoot = NeutralAggregateCachePath( m_cacheRoot, entry->aggregateIdentity );
    std::filesystem::create_directories( entry->scanRoot );
    std::string error;
    if( !WriteJson( entry->scanRoot / "profile.json", request.normalizedProfile, error ) ||
        !SaveStateLocked( *entry, error ) ) throw std::runtime_error( error );
    {
        std::lock_guard lock( m_mutex );
        m_entries.emplace( scanId, entry );
    }
    StartWorker( entry );
    std::lock_guard lock( entry->mutex );
    return entry->snapshot;
}

void AnalysisScanManager::StartWorker( const std::shared_ptr<Entry>& entry )
{
    entry->worker = std::jthread( [this, entry]( std::stop_token stopToken ) {
        try { Run( entry, stopToken ); }
        catch( const std::exception& exception )
        { Update( entry, ScanState::Failed, 0, 4, "unhandled_exception", exception.what() ); }
        catch( ... )
        { Update( entry, ScanState::Failed, 0, 4, "unhandled_exception", "unknown_exception" ); }
    } );
}

void AnalysisScanManager::Run( const std::shared_ptr<Entry>& entry, std::stop_token stopToken )
{
    Update( entry, ScanState::Validating, 0, 4, "validating" );
    AnalysisScanProducts products;
    std::string error;
    NeutralAggregateManifest manifest;
    bool reused = false;
    if( const auto existing = LoadNeutralAggregateManifest( entry->aggregateRoot, error );
        existing && CanReuseNeutralAggregate( *existing, entry->aggregateIdentity ) &&
        VerifyNeutralAggregate( entry->aggregateRoot, *existing, error ) )
    {
        manifest = *existing;
        if( manifest.runs.empty() ) error = "neutral_aggregate_run_missing";
        else
        {
            std::vector<uint8_t> payload;
            if( ReadNeutralAggregateRun( entry->aggregateRoot, manifest.runs.front(), payload, error ) &&
                DeserializeNeutralStatisticsResult( std::string( payload.begin(), payload.end() ),
                    products.aggregate, error ) &&
                ParseContextDocument( ReadJson( entry->aggregateRoot / "policy-context-v2.json" ),
                    products, error ) )
            {
                products.frameSeriesPath = entry->aggregateRoot / "frame-series-v1.bin";
                reused = true;
            }
        }
    }
    if( stopToken.stop_requested() )
    { Update( entry, ScanState::CancelledResumable, 0, 4, "cancelled", "cancelled" ); return; }

    if( !reused )
    {
        products = {};
        NeutralAggregateWriterLease lease;
        while( !AcquireNeutralAggregateWriterLease( entry->aggregateRoot, lease, error ) )
        {
            if( stopToken.stop_requested() )
            { Update( entry, ScanState::CancelledResumable, 0, 4, "cancelled", "cancelled" ); return; }
            const auto existing = LoadNeutralAggregateManifest( entry->aggregateRoot, error );
            if( existing && CanReuseNeutralAggregate( *existing, entry->aggregateIdentity ) )
            {
                Run( entry, stopToken );
                return;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
        }
        auto source = entry->request.source;
        if( !source && m_resolver ) source = m_resolver( entry->request.tracePath, stopToken );
        if( !source ) { Update( entry, ScanState::Failed, 0, 4, "source", "trace_source_unavailable" ); return; }
        AnalysisScanExecutionRequest request;
        request.scanId = entry->snapshot.scanId;
        request.traceSessionId = entry->request.traceSessionId;
        request.tracePath = entry->request.tracePath;
        request.source = std::move( source );
        request.normalizedProfile = entry->request.normalizedProfile;
        request.profileIdentity = entry->request.profileIdentity;
        request.aggregateIdentity = entry->aggregateIdentity;
        request.aggregateRoot = entry->aggregateRoot;
        request.temporaryRoot = entry->scanRoot / "temporary";
        std::filesystem::create_directories( request.temporaryRoot );
        const auto progress = [this, entry, stopToken]( ScanState state, uint64_t complete,
            uint64_t total, std::string_view stage ) {
            if( !stopToken.stop_requested() ) Update( entry, state, complete, total, stage );
        };
        if( !m_executor( request, stopToken, progress, products, error ) )
        {
            if( stopToken.stop_requested() || error == "cancelled" )
                Update( entry, ScanState::CancelledResumable, 0, 4, "cancelled", "cancelled" );
            else Update( entry, ScanState::Failed, 0, 4, "scan_failed", error );
            return;
        }
        if( stopToken.stop_requested() )
        { Update( entry, ScanState::CancelledResumable, 0, 4, "cancelled", "cancelled" ); return; }
        Update( entry, ScanState::Aggregating, 2, 4, "publishing_neutral_aggregate" );
        if( !products.frameSeriesPath.empty() )
        {
            const auto destination = entry->aggregateRoot / "frame-series-v1.bin";
            auto temporary = destination; temporary += ".tmp";
            const auto limit = entry->request.normalizedProfile.at( "limits" ).at( "cache_max_bytes" ).get<uint64_t>();
            if( std::filesystem::file_size( products.frameSeriesPath ) > limit )
            { Update( entry, ScanState::Failed, 2, 4, "aggregate_failed", "frame_series_disk_budget" ); return; }
            std::filesystem::copy_file( products.frameSeriesPath, temporary, std::filesystem::copy_options::overwrite_existing );
            if( !ReplaceFile( temporary, destination, error ) )
            { Update( entry, ScanState::Failed, 2, 4, "aggregate_failed", error ); return; }
            products.frameSeriesPath = destination;
        }
        if( !WriteJson( entry->aggregateRoot / "policy-context-v2.json",
            ContextDocument( products ), error ) ||
            !PublishNeutralStatistics( entry->aggregateRoot, entry->aggregateIdentity,
                HashText( entry->snapshot.scanId + "\nneutral" ).substr( 0, 24 ),
                products.aggregate, manifest, error ) )
        { Update( entry, ScanState::Failed, 2, 4, "aggregate_failed", error ); return; }
    }

    if( stopToken.stop_requested() )
    { Update( entry, ScanState::CancelledResumable, 2, 4, "cancelled", "cancelled" ); return; }
    Update( entry, ScanState::EvaluatingPolicy, 3, 4, "evaluating_candidate_policy" );
    CandidatePolicyInput policy;
    policy.aggregateIdentity = ComputeNeutralAggregateIdentity( entry->aggregateIdentity );
    policy.profileIdentity = entry->request.profileIdentity;
    policy.normalizedProfile = entry->request.normalizedProfile;
    policy.aggregate = std::move( products.aggregate );
    policy.signatures = std::move( products.signatureContexts );
    policy.capacityFacts = std::move( products.capacityFacts );
    policy.frameTimelines = std::move( products.frameTimelines );
    policy.cancelled = [stopToken] { return stopToken.stop_requested(); };
    const auto frameSeriesPath = products.frameSeriesPath;
    const auto seriesBudget = entry->request.normalizedProfile.at( "limits" ).at( "query_memory_target_bytes" ).get<uint64_t>() / 4;
    policy.readFrameSeries = [frameSeriesPath, seriesBudget, stopToken]( const PolicySignatureContext& context ) {
        return ReadPolicyFrameSeries( frameSeriesPath, context, seriesBudget,
            [stopToken] { return stopToken.stop_requested(); } );
    };
    const auto candidates = EvaluateCandidatePolicy( policy );
    if( !candidates.valid || !WriteCandidatePolicyManifest(
        entry->scanRoot / "candidate-manifest.json", candidates, error ) )
    { Update( entry, ScanState::Failed, 3, 4, "policy_failed", candidates.error.empty() ? error : candidates.error ); return; }
    Update( entry, ScanState::Auditing, 3, 4, "auditing" );
    if( stopToken.stop_requested() )
    { Update( entry, ScanState::CancelledResumable, 3, 4, "cancelled", "cancelled" ); return; }
    Update( entry, ScanState::Complete, 4, 4, "complete" );
}

std::shared_ptr<AnalysisScanManager::Entry> AnalysisScanManager::LoadEntry( const std::string& scanId ) const
{
    const auto root = m_root / "scans" / scanId;
    const auto statePath = root / "scan-state.json";
    if( !std::filesystem::exists( statePath ) ) return {};
    try
    {
        const auto state = ReadJson( statePath );
        if( state.value( "schema_version", 0u ) != 1 || state.value( "scan_id", std::string() ) != scanId ) return {};
        auto entry = std::make_shared<Entry>();
        entry->scanRoot = root;
        entry->snapshot.scanId = scanId;
        entry->snapshot.state = ParseState( state.at( "state" ).get<std::string>() );
        entry->snapshot.resumable = state.value( "resumable", false );
        entry->snapshot.completed = state.value( "completed", false );
        entry->snapshot.closed = state.value( "closed", false );
        entry->snapshot.progressCompleted = JsonU64( state.at( "progress_completed" ) );
        entry->snapshot.progressTotal = JsonU64( state.at( "progress_total" ) );
        entry->snapshot.stage = state.value( "stage", std::string() );
        if( state.contains( "error" ) && !state.at( "error" ).is_null() ) entry->snapshot.error = state.at( "error" ).get<std::string>();
        entry->request.traceSessionId = state.value( "trace_session_id", std::string() );
        entry->request.tracePath = std::filesystem::path( state.at( "trace_path" ).get<std::string>() );
        entry->request.profileIdentity = state.at( "profile_identity" ).get<std::string>();
        entry->request.normalizedProfile = ReadJson( root / "profile.json" );
        entry->aggregateIdentity = { state.at( "trace_strong_id" ).get<std::string>(),
            m_queryExecutableSha256, "1.35.0", NeutralScanAlgorithmId, NeutralAggregateSchemaVersion };
        if( state.at( "aggregate_identity" ).get<std::string>() !=
            ComputeNeutralAggregateIdentity( entry->aggregateIdentity ) ) return {};
        const auto expectedScanId = "scan-" + HashText(
            ComputeNeutralAggregateIdentity( entry->aggregateIdentity ) + "\n" +
            entry->request.profileIdentity + "\n" + CandidatePolicyAlgorithmId ).substr( 0, 32 );
        if( scanId != expectedScanId ) return {}; // Do not resume an old candidate policy.
        entry->aggregateRoot = std::filesystem::path( state.at( "aggregate_root" ).get<std::string>() );
        if( entry->snapshot.state != ScanState::Complete &&
            entry->snapshot.state != ScanState::CancelledResumable &&
            entry->snapshot.state != ScanState::Failed )
        {
            entry->snapshot.state = ScanState::CancelledResumable;
            entry->snapshot.resumable = true;
            entry->snapshot.completed = false;
            entry->snapshot.stage = "query_process_restart";
            entry->snapshot.error = "query_process_restart";
            std::string ignored;
            SaveStateLocked( *entry, ignored );
        }
        return entry;
    }
    catch( ... ) { return {}; }
}

std::shared_ptr<AnalysisScanManager::Entry> AnalysisScanManager::FindOrLoad( const std::string& scanId ) const
{
    {
        std::lock_guard lock( m_mutex );
        const auto found = m_entries.find( scanId );
        if( found != m_entries.end() ) return found->second;
    }
    auto entry = LoadEntry( scanId );
    if( !entry ) throw std::runtime_error( "analysis_scan_not_found" );
    std::lock_guard lock( m_mutex );
    return m_entries.emplace( scanId, entry ).first->second;
}

AnalysisScanSnapshot AnalysisScanManager::Status( const std::string& scanId ) const
{
    const auto entry = FindOrLoad( scanId );
    std::lock_guard lock( entry->mutex );
    return entry->snapshot;
}

AnalysisScanSnapshot AnalysisScanManager::Cancel( const std::string& scanId )
{
    const auto entry = FindOrLoad( scanId );
    if( entry->worker.joinable() ) entry->worker.request_stop();
    uint64_t completed = 0;
    uint64_t total = 0;
    {
        std::lock_guard lock( entry->mutex );
        completed = entry->snapshot.progressCompleted;
        total = entry->snapshot.progressTotal;
    }
    Update( entry, ScanState::CancelledResumable, completed, total, "cancelled", "cancelled" );
    NeutralAggregateCheckpoint checkpoint;
    checkpoint.generation = scanId;
    checkpoint.stage = "cancelled";
    checkpoint.progressNumerator = completed;
    checkpoint.progressDenominator = total;
    checkpoint.resumable = true;
    std::string ignored;
    SaveNeutralAggregateCheckpoint( entry->aggregateRoot, checkpoint, ignored );
    return Status( scanId );
}

AnalysisScanSnapshot AnalysisScanManager::Resume( const std::string& scanId )
{
    const auto entry = FindOrLoad( scanId );
    if( entry->worker.joinable() )
    {
        entry->worker.request_stop();
        entry->worker.join();
    }
    {
        std::lock_guard lock( entry->mutex );
        if( entry->snapshot.state != ScanState::CancelledResumable && entry->snapshot.state != ScanState::Failed )
            return entry->snapshot;
        entry->snapshot.state = ScanState::Queued;
        entry->snapshot.resumable = false;
        entry->snapshot.completed = false;
        entry->snapshot.error.clear();
        entry->snapshot.stage = "queued";
        std::string error;
        SaveStateLocked( *entry, error );
    }
    StartWorker( entry );
    return Status( scanId );
}

AnalysisScanSnapshot AnalysisScanManager::Close( const std::string& scanId )
{
    const auto entry = FindOrLoad( scanId );
    std::lock_guard lock( entry->mutex );
    entry->snapshot.closed = true;
    if( entry->snapshot.completed ) entry->request.source.reset();
    std::string error;
    if( !SaveStateLocked( *entry, error ) ) throw std::runtime_error( error );
    return entry->snapshot;
}

json AnalysisScanManager::ReadAggregateDocument( const Entry& entry ) const
{
    std::string error;
    const auto manifest = LoadNeutralAggregateManifest( entry.aggregateRoot, error );
    if( !manifest || !CanReuseNeutralAggregate( *manifest, entry.aggregateIdentity ) || manifest->runs.empty() )
        throw std::runtime_error( error.empty() ? "analysis_scan_aggregate_unavailable" : error );
    std::vector<uint8_t> payload;
    if( !ReadNeutralAggregateRun( entry.aggregateRoot, manifest->runs.front(), payload, error ) )
        throw std::runtime_error( error );
    return json::parse( payload.begin(), payload.end() );
}

json AnalysisScanManager::ReadCandidateDocument( const Entry& entry ) const
{
    return ReadJson( entry.scanRoot / "candidate-manifest.json" );
}

json PaginateAnalysisScanItems( const json& source, size_t limit, const std::string& cursor,
    const std::vector<std::string>& fields, const json& filter )
{
    if( !source.is_array() || limit == 0 || limit > 1000 ) throw std::runtime_error( "analysis_scan_invalid_page" );
    const auto offset = CursorOffset( cursor );
    json items = json::array();
    size_t total = 0;
    size_t payloadBytes = 2;
    bool byteLimited = false;
    for( const auto& item : source )
    {
        bool match = true;
        if( filter.is_object() ) for( const auto& [key, expected] : filter.items() )
            if( !item.contains( key ) || item.at( key ) != expected ) { match = false; break; }
        if( !match ) continue;
        const auto matchIndex = total++;
        if( matchIndex < offset || items.size() >= limit || byteLimited ) continue;

        json projected;
        if( fields.empty() ) projected = item;
        else
        {
            projected = json::object();
            for( const auto& field : fields ) if( item.contains( field ) ) projected[field] = item[field];
        }
        const auto itemBytes = projected.dump().size() + ( items.empty() ? 0 : 1 );
        if( itemBytes > AnalysisScanPagePayloadBudgetBytes && items.empty() )
            throw std::runtime_error( "analysis_scan_page_item_too_large" );
        if( payloadBytes + itemBytes > AnalysisScanPagePayloadBudgetBytes )
        {
            byteLimited = true;
            continue;
        }
        payloadBytes += itemBytes;
        items.push_back( std::move( projected ) );
    }
    const auto end = std::min( total, offset + items.size() );
    return { { "items", std::move( items ) }, { "page", {
        { "cursor", std::to_string( offset ) },
        { "next_cursor", end < total ? json( std::to_string( end ) ) : json( nullptr ) },
        { "done", end >= total }, { "total", std::to_string( total ) },
        { "byte_limited", byteLimited }, { "payload_bytes", std::to_string( payloadBytes ) } } } };
}

json AnalysisScanManager::Summary( const std::string& scanId ) const
{
    const auto entry = FindOrLoad( scanId );
    const auto aggregate = ReadAggregateDocument( *entry );
    const auto candidates = ReadCandidateDocument( *entry );
    return { { "scan_id", scanId }, { "quality", aggregate.at( "quality" ) },
        { "capture_quality", candidates.at( "capture_quality" ) },
        { "policy_algorithm", candidates.at( "policy_algorithm" ) },
        { "domains", aggregate.at( "domains" ) }, { "backlog", candidates.at( "backlog" ) },
        { "aggregate_content_sha256", aggregate.at( "content_sha256" ) },
        { "candidate_content_sha256", candidates.at( "content_sha256" ) } };
}

json AnalysisScanManager::Signatures( const std::string& scanId, size_t limit,
    const std::string& cursor, const std::vector<std::string>& fields, const json& filter ) const
{
    const auto entry = FindOrLoad( scanId );
    return PaginateAnalysisScanItems( ReadAggregateDocument( *entry ).at( "signatures" ),
        limit, cursor, fields, filter );
}

json AnalysisScanManager::Candidates( const std::string& scanId, size_t limit,
    const std::string& cursor, const std::vector<std::string>& fields, const json& filter ) const
{
    const auto entry = FindOrLoad( scanId );
    return PaginateAnalysisScanItems( ReadCandidateDocument( *entry ).at( "candidates" ),
        limit, cursor, fields, filter );
}

json AnalysisScanManager::Candidate( const std::string& scanId, const std::string& candidateId ) const
{
    const auto entry = FindOrLoad( scanId );
    const auto document = ReadCandidateDocument( *entry );
    for( const auto& candidate : document.at( "candidates" ) )
        if( candidate.at( "candidate_id" ).get<std::string>() == candidateId ) return candidate;
    throw std::runtime_error( "analysis_scan_candidate_not_found" );
}

json AnalysisScanManager::RepresentativeFrames( const std::string& scanId,
    const std::string& candidateId ) const
{
    const auto candidate = Candidate( scanId, candidateId );
    return { { "scan_id", scanId }, { "candidate_id", candidateId },
        { "frames", candidate.at( "representative_frame_details" ) } };
}

json AnalysisScanManager::Quality( const std::string& scanId ) const
{
    const auto entry = FindOrLoad( scanId );
    const auto aggregate = ReadAggregateDocument( *entry );
    auto quality = aggregate.at( "quality" );
    quality["scan_id"] = scanId;
    quality["domains"] = aggregate.at( "domains" );
    quality["capture_quality"] = ReadCandidateDocument( *entry ).at( "capture_quality" );
    return quality;
}

}
