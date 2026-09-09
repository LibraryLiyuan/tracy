#include "TracyAnalysisScanManager.hpp"
#include "TracyAnalysisScanCache.hpp"
#include "TracyAnalysisCacheQuery.hpp"
#include "TracyAnalysisCacheSort.hpp"
#include "TracyAnalysisCacheKey.hpp"
#include "TracyCandidatePolicyCache.hpp"

#include "TracyExactStatistics.hpp"
#include "TracyFrameCpuScanner.hpp"
#include "TracyPolicyFrameSeries.hpp"
#include "TracyGpuJobManagedScanner.hpp"
#include "TracyHash.hpp"
#include "TracyMemoryIoSamplingTelemetryScanner.hpp"
#include "TracyNeutralAggregateStore.hpp"
#include "TracyNeutralStatisticsCache.hpp"
#include "../../public/common/TracyQueue.hpp"

#include <algorithm>
#include <array>
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

AnalysisProcessMemoryOptions ProfileMemoryOptions(AnalysisProcessMemoryOptions options,const json& profile)
{
    // A Profile may tighten the process limit, never raise the Manager's
    // configured limit (which itself cannot exceed 16 GiB).
    options.maximumBytes=std::min(options.maximumBytes,
        profile.at("limits").at("query_memory_hard_bytes").get<uint64_t>());
    return options;
}

// Cooperative free-space checkpoints, not an OS disk quota. Small state writes
// remain possible so a denied execution can durably record its pause reason.
class DiskSpaceCheck
{
public:
    DiskSpaceCheck(std::array<std::filesystem::path,2> roots,uint64_t minimum,
        std::function<uint64_t(const std::filesystem::path&)> available)
        :m_roots(std::move(roots)),m_minimum(minimum),m_available(std::move(available)) {}
    void Check(bool force=false)
    {
        std::lock_guard lock(m_mutex);
        const auto now=std::chrono::steady_clock::now();
        if(!force && now<m_next) return;
        for(const auto& root:m_roots)
        {
            uint64_t available=0;
            try { available=m_available(root); }
            catch(...) { throw std::runtime_error("analysis_scan_disk_space_unavailable"); }
            if(available==std::numeric_limits<uint64_t>::max())
                throw std::runtime_error("analysis_scan_disk_space_unavailable");
            if(available<m_minimum) throw std::runtime_error("analysis_scan_minimum_free_disk");
        }
        m_next=now+std::chrono::milliseconds(100);
    }
private:
    std::array<std::filesystem::path,2> m_roots;
    uint64_t m_minimum;
    std::function<uint64_t(const std::filesystem::path&)> m_available;
    std::chrono::steady_clock::time_point m_next{};
    std::mutex m_mutex;
};

std::string_view DiskBudgetReason(std::string_view error)
{
    for(const std::string_view reason:{"analysis_scan_minimum_free_disk",
        "analysis_scan_disk_space_unavailable","analysis_scan_cache_disk_budget"})
        if(error.find(reason)!=std::string_view::npos) return reason;
    return {};
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

bool WriteJson( const std::filesystem::path& path, const json& value, std::string& error,
    const std::shared_ptr<AnalysisDiskUsage>& disk={},uint64_t limit=UINT64_MAX )
{
    std::error_code ec;
    std::filesystem::create_directories( path.parent_path(), ec );
    if( ec ) { error = "analysis_scan_directory_failed:" + ec.message(); return false; }
    auto temporary = path;
    temporary += ".tmp";
    const auto payload=value.dump()+"\n";
    const auto oldTemporary=disk?AnalysisDiskUsage::Bytes(temporary):0;
    const auto replaced=disk?AnalysisDiskUsage::Bytes(path):0;
    // State/checkpoint writes can use the control space reserved by Run. They
    // are still counted; profiles and publication pointers supply a hard limit.
    if(disk) disk->Grow(payload.size(),limit,limit!=UINT64_MAX);
    std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
    if( !output ) { error = "analysis_scan_open_failed"; return false; }
    if(disk) disk->Release(oldTemporary);
    output << payload;
    output.flush();
    if( !output ) { error = "analysis_scan_write_failed"; return false; }
    output.close();
    if(!ReplaceFile( temporary, path, error )) return false;
    if(disk) disk->Release(replaced);
    return true;
}

// Walk the already validated small profile without first dumping or copying
// it. Covers retained nodes, concurrent request copies and escaped encoding.
void ReserveControlJson(const json& value,AnalysisWorkspaceReservation& workspace)
{
    workspace.Add(512);
    if(value.is_string()) workspace.Add(32ull*value.get_ref<const std::string&>().size());
    if(value.is_object()) for(auto item=value.begin();item!=value.end();++item) {
        workspace.Add(32ull*item.key().size()); ReserveControlJson(item.value(),workspace);
    }
    else if(value.is_array()) for(const auto& item:value) ReserveControlJson(item,workspace);
}

json ReadJson( const std::filesystem::path& path,AnalysisWorkspaceReservation& workspace )
{
    std::ifstream input( path, std::ios::binary );
    if( !input ) throw std::runtime_error( "analysis_scan_file_missing:" + path.string() );
    input.seekg(0,std::ios::end); const auto bytes=input.tellg();
    if(bytes<0) throw std::runtime_error("analysis_scan_small_document_read_failed");
    if(uint64_t(bytes)>4*1024*1024) throw std::runtime_error("analysis_scan_small_document_budget");
    // The caller owns this charge for as long as the returned DOM or strings
    // copied from it survive, including cold Entries retained by the Manager.
    workspace.Add(65536+32ull*uint64_t(bytes));
    input.seekg(0); std::string payload(size_t(bytes),'\0');
    input.read(payload.data(),std::streamsize(payload.size()));
    if(input.gcount()!=std::streamsize(payload.size()) || input.peek()!=std::char_traits<char>::eof())
        throw std::runtime_error("analysis_scan_small_document_changed");
    return json::parse(payload);
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


size_t CursorOffset( const std::string& cursor )
{
    if( cursor.empty() ) return 0;
    size_t consumed = 0;
    const auto value = std::stoull( cursor, &consumed );
    if( consumed != cursor.size() || value > std::numeric_limits<size_t>::max() )
        throw std::runtime_error( "analysis_scan_invalid_cursor" );
    return size_t( value );
}

bool GenerationName( const std::string& value )
{
    return value.starts_with( "gen-" ) && value.size()>4 &&
        std::all_of( value.begin()+4,value.end(),[](char c){return c>='0' && c<='9';} );
}

std::filesystem::path AnalysisPath( const std::filesystem::path& value )
{
#ifdef _WIN32
    // Hash identities and immutable generations can exceed MAX_PATH even for
    // ordinary user cache roots. Extended absolute paths do not depend on a
    // machine-wide long-path setting or the embedding executable's manifest.
    auto path=std::filesystem::absolute(value).lexically_normal(); path.make_preferred();
    const auto native=path.native();
    if(native.starts_with(L"\\\\?\\")) return path;
    if(native.starts_with(L"\\\\")) return std::filesystem::path(L"\\\\?\\UNC\\"+native.substr(2));
    return std::filesystem::path(L"\\\\?\\"+native);
#else
    return value;
#endif
}

std::string NewGeneration( const std::filesystem::path& parent )
{
    std::filesystem::create_directories( parent );
    for( uint64_t i=1;i<1000000;++i )
    {
        const auto name="gen-"+std::to_string(i);
        if( std::filesystem::create_directory(parent/name) ) return name;
    }
    throw std::runtime_error("analysis_scan_generation_limit");
}

void CheckGenerationDiskBudget( const std::filesystem::path& root,uint64_t limit,
    std::stop_token stopToken )
{
    uint64_t bytes=0;
    for( const auto& file:std::filesystem::recursive_directory_iterator(root) )
    {
        if(stopToken.stop_requested()) throw std::runtime_error("cancelled");
        if(!file.is_regular_file()) continue;
        const auto size=file.file_size();
        if(size>limit-bytes) throw std::runtime_error("analysis_scan_cache_disk_budget");
        bytes+=size;
    }
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
        if(request.checkDiskSpace) request.checkDiskSpace();
        if( !stopToken.stop_requested() ) return false;
        error = "cancelled";
        return true;
    };
    try
    {
        const auto workspace=request.workspace ? request.workspace : std::make_shared<AnalysisWorkspaceBudget>();
        products.metadataWorkspace=AnalysisWorkspaceReservation(workspace);
        products.contextWorkspace=AnalysisWorkspaceReservation(workspace);
        const auto temporaryRoot=AnalysisPath(request.temporaryRoot);
        const auto statisticsRoot = temporaryRoot / "statistics";
        std::filesystem::create_directories( statisticsRoot );
        NeutralStatisticsStreamBuilder statistics( statisticsRoot, 65536, 65536, workspace, request.disk );
        NeutralStatisticsCacheOptions cacheOptions;
        cacheOptions.table.cancelled = cancelled;
        cacheOptions.table.workspace = workspace;
        cacheOptions.table.disk = request.disk;
        products.neutralCacheIdentity = ComputeNeutralAggregateIdentity( request.aggregateIdentity );
        const auto cpuDefinitionsPath = temporaryRoot / "cpu-definitions-v1";
        const auto cpuContextsPath = temporaryRoot / "cpu-context-input-v2";
        const auto gpuDefinitionsPath = temporaryRoot / "gpu-definitions-v1";
        const auto gpuContextsPath = temporaryRoot / "gpu-context-input-v2";
        std::unique_ptr<AnalysisCacheTableReader> cpuContextInput;
        std::unique_ptr<AnalysisCacheTableReader> gpuContextInput;
        uint64_t nextContextOrdinal = 0;
        std::map<std::string, size_t> contexts;
        const auto contextKey = []( std::string_view domain, std::string_view signature,
            std::string_view frameScope ) {
            return std::string( domain ) + '\n' + std::string( signature ) + '\n' +
                std::string( frameScope );
        };
        const auto ensureContext = [&]( std::string_view domain, std::string_view signature,
            std::string_view family, std::string_view parent, std::string_view name, std::string_view path,
            std::string_view frameScope, std::string_view metric, bool frameRoot, bool critical ) -> PolicySignatureContext& {
            AnalysisWorkspaceReservation lookupWorkspace(workspace,512+4ull*(domain.size()+signature.size()+frameScope.size()));
            const auto key = contextKey( domain, signature, frameScope );
            const auto found = contexts.find( key );
            if( found != contexts.end() ) return products.signatureContexts[found->second];
            // Context vector capacity, lookup map, retained strings and bounded
            // representative copies. Charge full strings; no path truncation.
            products.contextWorkspace.Add(2048+8ull*(domain.size()+signature.size()+family.size()+parent.size()+
                name.size()+path.size()+frameScope.size()+metric.size()));
            PolicySignatureContext value;
            value.domain = domain; value.signatureId = signature;
            value.familyId = family; value.parentSignatureId = parent;
            value.name = name; value.path = path;
            value.frameScope = frameScope; value.metricPreference = metric;
            value.frameRoot = frameRoot; value.provenCriticalPath = critical;
            value.sourceOrdinal = nextContextOrdinal++;
            contexts.emplace( key, products.signatureContexts.size() );
            products.signatureContexts.push_back( std::move( value ) );
            return products.signatureContexts.back();
        };
        // Scan DTOs (notably Jobs/relations and memory/sampling facts) must not
        // coexist with the external merge's complete aggregate output. All
        // needed numeric facts/context have been copied into the bounded store
        // before this scope ends. The source Worker itself remains unchanged.
        {
        AnalysisWorkspaceReservation scanRegistryWorkspace( workspace );
        std::unordered_map<uint64_t, NeutralStatisticsStreamBuilder::SignatureToken> cpuTokens;

        progress( ScanState::Scanning, 0, 4, "cpu_frame_zone_scan" );
        CpuFrameScanOptions cpuOptions;
        cpuOptions.cancelled = cancelled;
        cpuOptions.workspace = workspace;
        cpuOptions.includeExactSignatures = true;
        cpuOptions.includeLogicalSignatures = false;
        cpuOptions.logicalSignatureMode = CpuLogicalSignatureMode::FullPath;
        auto cpuDefinitionOutput = std::make_unique<AnalysisCacheSortedWriter>( cpuDefinitionsPath,
            products.neutralCacheIdentity, "cpu-definitions-v1", cacheOptions.table );
        cpuOptions.definitionSink = [&]( const CpuSignatureDefinition& definition ) {
            if( stopToken.stop_requested() ) return false;
            AnalysisWorkspaceReservation currentDefinition( workspace, 65536 + 32ull *
                ( definition.signatureId.size() + definition.parentSignatureId.size() + definition.name.size() +
                  definition.path.size() + definition.threadRef.size() ) );
            PolicySignatureContext context;
            context.domain = "cpu"; context.signatureId = definition.signatureId;
            context.parentSignatureId = definition.parentSignatureId;
            context.name = definition.name; context.path = definition.path;
            context.threadOrQueue = definition.threadRef;
            context.metricPreference = definition.workClass == CpuWorkClass::Wait ||
                definition.workClass == CpuWorkClass::IntentionalPacing ? "wait" : "exclusive";
            cpuDefinitionOutput->Append( definition.signatureId, SerializePolicySignatureContextRecord( context ) );
            return true;
        };
        std::map<std::string, std::vector<PolicyFrameEvidence>> frameTimelines;
        cpuOptions.completeFrameSink = [&]( const auto& scope, uint64_t index, int64_t begin, int64_t end ) {
            // Timeline capacity and the frame-root Context copy coexist until
            // neutral publication. Keep ownership of the surviving Timeline.
            products.metadataWorkspace.Add(1024+8ull*scope.size());
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
                    scanRegistryWorkspace.Add( 128 );
                    found = cpuTokens.emplace( key, token ).first;
                }
                if( !statistics.AddRegisteredRun( found->second, uint64_t( run.frameIndex ),
                    run.inclusiveNs, run.exclusiveNs, wait ? run.inclusiveNs : 0, 0,
                    run.occurrenceCount, run.exact, false ) )
                    throw std::runtime_error( "neutral_cpu_run_ingest_failed" );
                return true;
            }, cpuOptions );
        if( cancelled() ) return false;
        cpuDefinitionOutput->Commit();
        cpuDefinitionOutput.reset();
        for( const auto& set : cpu.frameSetDenominators )
        {
            products.metadataWorkspace.Add(1024+8ull*(set.frameSetRef.size()+set.name.size()));
            products.frameTimelines.push_back( { set.frameSetRef, set.name, std::move( frameTimelines[set.frameSetRef] ) } );
            auto& root = ensureContext( "cpu", "frame-wall:" + set.frameSetRef, "frame-wall:" + set.frameSetRef,
                {}, set.name, set.name, set.frameSetRef, "inclusive", true, false );
            root.frames = products.frameTimelines.back().frames;
            root.frameSeriesComplete = true;
        }
        {
        AnalysisCacheTableReader cpuDefinitions( cpuDefinitionsPath,
            products.neutralCacheIdentity, "cpu-definitions-v1", cacheOptions.table );
        AnalysisCacheSortedWriter cpuContexts( cpuContextsPath,
            products.neutralCacheIdentity, "cpu-context-input-v2", cacheOptions.table );
        for( const auto& value : cpu.denominators )
        {
            // Streamed scanner denominators are emitted only for accepted
            // signature/FrameSet runs; a second string-key set is redundant.
            statistics.AddDenominator( { "cpu", value.signatureId, value.frameSetRef,
                value.completeFrameDenominator } );
            const auto definition = cpuDefinitions.Find( value.signatureId );
            if( !definition ) throw std::runtime_error( "neutral_cpu_definition_missing" );
            AnalysisWorkspaceReservation currentContext( workspace, 65536 + 32ull * definition->payload.size() );
            PolicySignatureContext context;
            if( !DeserializePolicySignatureContextRecord( definition->payload, context, error ) )
                throw std::runtime_error( error );
            context.familyId = "cpu:" + value.signatureId + ":" + value.frameSetRef;
            context.frameScope = value.frameSetRef;
            context.sourceOrdinal = nextContextOrdinal++;
            const auto token=statistics.FindSignatureToken("cpu",value.signatureId,value.frameSetRef);
            if(token==NeutralStatisticsStreamBuilder::InvalidSignatureToken)
                throw std::runtime_error("neutral_cpu_context_signature_missing");
            cpuContexts.Append( cache_key::Unsigned(token),
                SerializePolicySignatureContextRecord( context ) );
        }
        cpuContexts.Commit();
        }
        cpuContextInput = std::make_unique<AnalysisCacheTableReader>( cpuContextsPath,
            products.neutralCacheIdentity, "cpu-context-input-v2", cacheOptions.table );

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
        {
        AnalysisWorkspaceReservation correlatedWorkspace( workspace );
        const auto correlatedFrames = request.source->GetCorrelatedFrameEvents();
        if( cancelled() ) return false;
        for( const auto& frame : correlatedFrames )
            correlatedWorkspace.Add( 1024 + 4ull * ( frame.ref.size() + frame.threadRef.size() ) );
        for( const auto& frame : correlatedFrames )
        {
            if( cancelled() ) return false;
            if( frame.frameId == 0 || frame.domain != uint8_t( tracy::JnFrameDomain::Player ) ) continue;
            if( !playerFrameBoundaryMask.contains( frame.frameId ) ) scanRegistryWorkspace.Add( 256 );
            auto& mask = playerFrameBoundaryMask[frame.frameId];
            if( frame.phase == uint8_t( tracy::JnFramePhase::Begin ) ) mask |= 1;
            else if( frame.phase == uint8_t( tracy::JnFramePhase::End ) ) mask |= 2;
        }
        }
        std::unordered_set<uint64_t> completePlayerFrameIds;
        scanRegistryWorkspace.Add( 32ull * playerFrameBoundaryMask.size() );
        completePlayerFrameIds.reserve( playerFrameBoundaryMask.size() );
        for( const auto& [frameId, mask] : playerFrameBoundaryMask )
            if( mask == 3 ) completePlayerFrameIds.emplace( frameId );
        const auto jobPlayerFrameDenominator = uint64_t( completePlayerFrameIds.size() );
        std::map<std::string, std::string> gpuPresent;
        std::map<std::string, PolicyFrameTimeline> gpuTimelines;
        std::set<std::string> jobPresent;
        GpuJobManagedScanOptions gpuOptions;
        gpuOptions.cancelled = cancelled;
        gpuOptions.workspace = workspace;
        gpuOptions.retainDetails = false;
        gpuOptions.includeExactGpuSignatures = true;
        gpuOptions.includeLogicalGpuSignatures = false;
        gpuOptions.logicalSignatureMode = GpuLogicalSignatureMode::FullPath;
        auto gpuDefinitionOutput = std::make_unique<AnalysisCacheSortedWriter>( gpuDefinitionsPath,
            products.neutralCacheIdentity, "gpu-definitions-v1", cacheOptions.table );
        gpuOptions.definitionSink = [&]( const GpuSignatureDefinition& definition ) {
            if( stopToken.stop_requested() ) return false;
            AnalysisWorkspaceReservation currentDefinition( workspace, 65536 + 32ull *
                ( definition.signatureId.size() + definition.parentSignatureId.size() + definition.name.size() +
                  definition.path.size() + definition.contextRef.size() ) );
            PolicySignatureContext context;
            context.domain = "gpu"; context.signatureId = definition.signatureId;
            context.parentSignatureId = definition.parentSignatureId;
            context.name = definition.name; context.path = definition.path;
            context.threadOrQueue = definition.contextRef;
            context.metricPreference = "exclusive"; context.observationUnit = "l0_segment";
            gpuDefinitionOutput->Append( definition.signatureId, SerializePolicySignatureContextRecord( context ) );
            return true;
        };
        gpuOptions.gpuZoneSink = [&]( const GpuZoneScanFact& value ) {
            if( stopToken.stop_requested() ) return false;
            const auto scope = "GPU.L0Segment:" + value.contextRef;
            if( value.depth == 0 )
            {
                products.metadataWorkspace.Add(2048+8ull*(scope.size()+value.zoneRef.size()));
                auto& timeline = gpuTimelines[scope];
                timeline.frameScope = scope; timeline.name = "Physical GPU L0 segments (not Player frames)";
                timeline.observationUnit = "l0_segment";
                timeline.frames.push_back( { value.l0SegmentOrdinal, value.physicalTimingExact?value.inclusiveNs:0,
                    { value.zoneRef }, {}, value.l0Complete,
                    value.beginNs>=0?std::optional<int64_t>(value.beginNs):std::nullopt,
                    value.endNs>=0?std::optional<int64_t>(value.endNs):std::nullopt } );
            }
            if( !value.physicalTimingExact ) return true;
            if( !statistics.AddRun( { "gpu", value.signatureId, scope, value.l0SegmentOrdinal,
                value.inclusiveNs, value.exclusiveNs, 0, 0, 1, true, false } ) )
                throw std::runtime_error( "neutral_gpu_run_ingest_failed" );
            if( !gpuPresent.contains( value.signatureId ) )
            {
                scanRegistryWorkspace.Add( 256 + 4ull * ( value.signatureId.size() + scope.size() ) );
                gpuPresent.emplace( value.signatureId, scope );
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
            if( !jobPresent.contains( signature ) )
            {
                scanRegistryWorkspace.Add( 256 + 4ull * signature.size() );
                jobPresent.emplace( signature );
            }
            ensureContext( "job", signature, "job:type:" + std::to_string( job.typeId ),
                {}, job.name, job.name, "Player.Frame",
                job.waitNs > job.executionNs ? "wait" : "exclusive", false,
                false ); // A continuation is not proof that this job blocked a frame.
            return true;
        };
        const auto gpuJob = GpuJobManagedScanner( *request.source ).Scan( gpuOptions );
        if( cancelled() ) return false;
        gpuDefinitionOutput->Commit();
        gpuDefinitionOutput.reset();
        if( !jobPresent.empty() && jobPlayerFrameDenominator == 0 )
            throw std::runtime_error( "job_player_frame_identity_unavailable" );
        {
        AnalysisCacheTableReader gpuDefinitions( gpuDefinitionsPath,
            products.neutralCacheIdentity, "gpu-definitions-v1", cacheOptions.table );
        AnalysisCacheSortedWriter gpuContexts( gpuContextsPath,
            products.neutralCacheIdentity, "gpu-context-input-v2", cacheOptions.table );
        for( const auto& [signature, scope] : gpuPresent )
        {
            const auto& segments=gpuTimelines[scope].frames;
            statistics.AddDenominator( { "gpu", signature, scope,
                uint64_t(std::count_if(segments.begin(),segments.end(),[](const auto& frame){return frame.exact;})) } );
            const auto definition = gpuDefinitions.Find( signature );
            if( !definition ) throw std::runtime_error( "neutral_gpu_definition_missing" );
            AnalysisWorkspaceReservation currentContext( workspace, 65536 + 32ull * definition->payload.size() );
            PolicySignatureContext context;
            if( !DeserializePolicySignatureContextRecord( definition->payload, context, error ) )
                throw std::runtime_error( error );
            context.familyId = "gpu:" + signature + ":" + scope;
            context.frameScope = scope; context.sourceOrdinal = nextContextOrdinal++;
            const auto token=statistics.FindSignatureToken("gpu",signature,scope);
            if(token==NeutralStatisticsStreamBuilder::InvalidSignatureToken)
                throw std::runtime_error("neutral_gpu_context_signature_missing");
            gpuContexts.Append( cache_key::Unsigned(token),
                SerializePolicySignatureContextRecord( context ) );
        }
        gpuContexts.Commit();
        }
        gpuContextInput = std::make_unique<AnalysisCacheTableReader>( gpuContextsPath,
            products.neutralCacheIdentity, "gpu-context-input-v2", cacheOptions.table );
        for( auto& [_, timeline] : gpuTimelines ) products.frameTimelines.push_back( std::move( timeline ) );

        for( const auto& signature : jobPresent )
            statistics.AddDenominator( { "job", signature, "Player.Frame", jobPlayerFrameDenominator } );

        progress( ScanState::Scanning, 2, 4, "memory_io_sampling_telemetry_scan" );
        const auto system = MemoryIoSamplingTelemetryScanner( *request.source ).ScanSummary(
            cancelled, workspace );
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
            { "gpu_", "missing_gpu_", "invalid_gpu_" } );
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
            cpu.inputZoneCount, cpuTokens.size(), cpuReason );
        audit( "gpu", gpuJob.gpuZoneCount != 0,
            sourceStatus( gpuJob.gpuZoneCount != 0, gpuReason ),
            gpuJob.gpuZoneCount, gpuPresent.size(), gpuReason );
        audit( "job", gpuJob.jobCount != 0,
            sourceStatus( gpuJob.jobCount != 0, jobReason ),
            gpuJob.jobCount, jobPresent.size(), jobReason );
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
        {
            products.metadataWorkspace.Add(2048+8ull*(pool.poolRef.size()+pool.name.size()));
            products.capacityFacts.push_back( { "memory.cpu", pool.poolRef,
                "cpu-memory-pool:" + pool.poolRef, "cpu_memory", pool.peakLiveBytes,
                std::nullopt, !pool.hasAccountingGap, pool.name } );
        }
        if( system.gpuMemory.physicalFactsAvailable )
        {
            products.metadataWorkspace.Add(4096);
            products.capacityFacts.push_back( { "memory.gpu", "gpu-physical-peak",
                "gpu-physical-capacity", "gpu_memory", system.gpuMemory.physicalBytes,
                std::nullopt, system.gpuMemory.physicalPeakExact,
                "Engine-known concurrently live root-allocation peak" } );
        }

        }
        progress( ScanState::Aggregating, 3, 4, "exact_statistics" );
        products.frameSeriesPath = temporaryRoot / "frame-series-v1.bin";
        std::ofstream seriesOutput( products.frameSeriesPath, std::ios::binary | std::ios::trunc );
        if( !seriesOutput ) throw std::runtime_error( "frame_series_open_failed" );
        const auto selectEvidence = [&]( const NeutralSignatureAggregate& aggregate,
            const std::vector<NeutralMergedFrameValues>& frames, PolicySignatureContext& context ) {
            if( frames.empty() ) return;
            // Preserve all numeric rows on disk before choosing UI representatives.
            // Job origin IDs are not Player.Frame indexes. CPU uses actual
            // FrameSet boundaries; GPU uses explicitly typed L0 segments.
            if( context.domain == "cpu" || context.domain == "gpu" ) WritePolicyFrameSeries( seriesOutput, context, frames,request.disk );
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
                    value( frame ), {}, context.path, frame.exact } );
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
        products.neutralCachePath = temporaryRoot / "neutral-statistics-v1";
        NeutralStatisticsCacheWriter cache( products.neutralCachePath,
            products.neutralCacheIdentity, cacheOptions );
        NeutralStatisticsStreamSummary summary;
        const auto persistSignature = [&]( const NeutralSignatureAggregate& aggregate,
            const std::vector<NeutralMergedFrameValues>& frames ) {
            AnalysisWorkspaceReservation decodedWorkspace( workspace );
            PolicySignatureContext decoded;
            const auto found = contexts.find( contextKey( aggregate.domain,
                aggregate.signatureId, aggregate.frameScope ) );
            if( found == contexts.end() )
            {
                auto* input = aggregate.domain == "cpu" ? cpuContextInput.get() :
                    aggregate.domain == "gpu" ? gpuContextInput.get() : nullptr;
                if( !input ) throw std::runtime_error( "neutral_statistics_context_missing" );
                // Numeric runs are merged by registration token, not the
                // lexical signature hash. Match the temporary Context key to
                // that order so wide payload blocks are visited sequentially.
                const auto token=statistics.FindSignatureToken(aggregate.domain,aggregate.signatureId,aggregate.frameScope);
                if(token==NeutralStatisticsStreamBuilder::InvalidSignatureToken)
                    throw std::runtime_error("neutral_statistics_context_signature_missing");
                const auto record = input->Find( cache_key::Unsigned(token) );
                if( !record ) throw std::runtime_error( "neutral_statistics_context_missing" );
                decodedWorkspace.Resize( 65536 + 32ull * record->payload.size() );
                if( !DeserializePolicySignatureContextRecord( record->payload, decoded, error ) )
                    throw std::runtime_error( error );
            }
            auto& context = found == contexts.end() ? decoded : products.signatureContexts[found->second];
            AnalysisWorkspaceReservation evidenceWorkspace(workspace,65536+frames.size()*16ull+64ull*context.path.size());
            selectEvidence( aggregate, frames, context );
            cache.AppendStatistics( aggregate );
            cache.AppendContext( SerializePolicySignatureContextRecord( context ) );
            // Only the lookup key/ordinal is needed after this signature. Do
            // not retain every representative/path beside the cache writer.
            context = {};
        };
        statistics.SetCompleteFrameFilter([&](std::string_view domain,std::string_view scope,uint64_t index) {
            if(domain!="gpu") return true;
            for(const auto& timeline:products.frameTimelines) if(timeline.frameScope==scope) {
                const auto found=std::lower_bound(timeline.frames.begin(),timeline.frames.end(),index,
                    [](const auto& frame,uint64_t ordinal){return frame.frameIndex<ordinal;});
                if(found==timeline.frames.end() || found->frameIndex!=index)
                    throw std::runtime_error("neutral_gpu_l0_identity_missing");
                return found->exact;
            }
            throw std::runtime_error("neutral_gpu_frame_scope_missing");
        });
        if( !statistics.FinishToSink( summary, error, persistSignature,
            cancelled ) ) return false;
        seriesOutput.flush();
        if( !seriesOutput ) throw std::runtime_error( "frame_series_flush_failed" );
        seriesOutput.close();
        if( !summary.qualityComplete )
        {
            error = "neutral_statistics_quality_incomplete";
            if( !summary.qualityFindings.empty() )
                error += ":" + summary.qualityFindings.front();
            return false;
        }
        for( const auto& context : products.signatureContexts )
            if( !context.domain.empty() ) cache.AppendContext( SerializePolicySignatureContextRecord( context ) );
        std::vector<PolicySignatureContext>().swap( products.signatureContexts );
        contexts.clear();
        products.contextWorkspace.Resize(0);
        cpuContextInput.reset();
        gpuContextInput.reset();
        cache.Commit( summary );
        for(const auto& domain:summary.domains)
            products.metadataWorkspace.Add(4096+32ull*(domain.domain.size()+domain.status.size()+domain.inputChecksum.size()+
                domain.consumedChecksum.size()+domain.unavailableReason.size()));
        for(const auto& finding:summary.qualityFindings) products.metadataWorkspace.Add(1024+32ull*finding.size());
        products.aggregate.qualityComplete = summary.qualityComplete;
        products.aggregate.unreportedGapCount = summary.unreportedGapCount;
        products.aggregate.qualityFindings = std::move( summary.qualityFindings );
        products.aggregate.domains = std::move( summary.domains );
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
    AnalysisWorkspaceReservation controlWorkspace;
    mutable std::mutex mutex;
    mutable std::mutex readMutex;
    AnalysisScanSnapshot snapshot;
    AnalysisScanStartRequest request;
    NeutralAggregateIdentity aggregateIdentity;
    std::filesystem::path scanRoot;
    std::filesystem::path aggregateRoot;
    std::string neutralGeneration,neutralHeaderSha256,policyGeneration,policyIdentity;
    // Readers outlive one request. Their callbacks consult this replaceable
    // request guard under readMutex, so a denied request does not poison later
    // reads after memory pressure drops. It outlives the owned readers.
    std::shared_ptr<AnalysisProcessMemoryGuard> readMemory;
    std::unique_ptr<NeutralStatisticsCacheReader> neutralReader;
    std::unique_ptr<CandidatePolicyCacheReader> policyReader;
    std::unique_ptr<AnalysisCacheQuery> signatureQuery,candidateQuery;
    std::shared_ptr<AnalysisProcessMemoryGuard> processMemory;
    std::jthread worker;
};

json AnalysisProcessMemorySnapshotJson(const AnalysisProcessMemorySnapshot& value)
{
    return {{"maximum_bytes",std::to_string(value.maximumBytes)},{"samples",std::to_string(value.samples)},
        {"resident_bytes",std::to_string(value.residentBytes)},{"private_bytes",std::to_string(value.privateBytes)},
        {"peak_resident_bytes",std::to_string(value.peakResidentBytes)},
        {"peak_private_bytes",std::to_string(value.peakPrivateBytes)},
        {"error",value.error.empty()?json(nullptr):json(value.error)}};
}

AnalysisScanManager::AnalysisScanManager( std::filesystem::path root,
    std::filesystem::path cacheRoot, std::string queryExecutableSha256,
    AnalysisScanSourceResolver resolver, AnalysisScanExecutor executor,
    std::shared_ptr<AnalysisWorkspaceBudget> workspace, AnalysisProcessMemoryOptions processMemory,
    std::function<uint64_t(const std::filesystem::path&)> diskAvailable )
    : m_root( std::move( root ) )
    , m_cacheRoot( std::move( cacheRoot ) )
    , m_queryExecutableSha256( std::move( queryExecutableSha256 ) )
    , m_resolver( std::move( resolver ) )
    , m_executor( std::move( executor ) )
    , m_workspace( workspace ? std::move(workspace) : std::make_shared<AnalysisWorkspaceBudget>() )
    , m_processMemory( std::move(processMemory) )
    , m_diskAvailable( diskAvailable ? std::move(diskAvailable) : [](const auto& root) {
        return std::filesystem::space(root).available;
    } )
{
    if( m_root.empty() || m_cacheRoot.empty() || m_queryExecutableSha256.size() != 64 || !m_executor )
        throw std::invalid_argument( "analysis_scan_manager_invalid_configuration" );
    AnalysisProcessMemoryGuard validateMemory(m_processMemory);
    m_root=AnalysisPath(m_root); m_cacheRoot=AnalysisPath(m_cacheRoot);
    std::filesystem::create_directories( m_root / "scans" );
    std::filesystem::create_directories( m_cacheRoot );
    m_diskUsage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{
        m_root/"scans",m_cacheRoot/AnalysisScanCacheFormatId});
}

AnalysisScanManager::~AnalysisScanManager()
{
    std::vector<std::shared_ptr<Entry>> entries;
    {
        std::lock_guard lock( m_mutex );
        for( const auto& [_, entry] : m_entries ) entries.push_back( entry );
    }
    for( const auto& entry : entries ) if( entry->worker.joinable() ) entry->worker.request_stop();
    // Workers capture this manager and their Entry. Keep both alive until all
    // cancellation/stream cleanup has finished; an Entry must not join itself.
    for( const auto& entry : entries ) if( entry->worker.joinable() ) entry->worker.join();
}

bool AnalysisScanManager::SaveStateLocked( const Entry& entry, std::string& error ) const
{
    const auto& value = entry.snapshot;
    return WriteJson( entry.scanRoot / "scan-state.json", {
        { "schema_version", 2 }, { "cache_format", AnalysisScanCacheFormatId }, { "scan_id", value.scanId },
        { "state", ScanStateName( value.state ) }, { "resumable", value.resumable },
        { "completed", value.completed }, { "closed", value.closed },
        { "progress_completed", std::to_string( value.progressCompleted ) },
        { "progress_total", std::to_string( value.progressTotal ) }, { "stage", value.stage },
        { "error", value.error.empty() ? json( nullptr ) : json( value.error ) },
        { "process_memory", AnalysisProcessMemorySnapshotJson(value.processMemory) },
        { "trace_session_id", entry.request.traceSessionId },
        { "trace_path", entry.request.tracePath.generic_string() },
        { "trace_strong_id", entry.aggregateIdentity.traceStrongId },
        { "profile_identity", entry.request.profileIdentity },
        { "aggregate_identity", ComputeNeutralAggregateIdentity( entry.aggregateIdentity ) },
        { "aggregate_root", entry.aggregateRoot.generic_string() },
        { "neutral_generation", entry.neutralGeneration }, { "neutral_header_sha256", entry.neutralHeaderSha256 },
        { "policy_generation", entry.policyGeneration }, { "policy_identity", entry.policyIdentity }
    }, error, m_diskUsage );
}

void AnalysisScanManager::Update( const std::shared_ptr<Entry>& entry, ScanState state,
    uint64_t completed, uint64_t total, std::string_view stage, std::string_view error ) const
{
    std::lock_guard lock( entry->mutex );
    const auto memory=entry->processMemory ? entry->processMemory->Snapshot() : AnalysisProcessMemorySnapshot{};
    if(!memory.error.empty())
    {
        state=ScanState::CancelledResumable;
        stage=memory.error=="analysis_process_memory_budget" ? "process_memory_budget" : "process_memory_unavailable";
        error=memory.error;
    }
    if( entry->snapshot.state == ScanState::Complete && state != ScanState::Complete ) return;
    if( entry->snapshot.state == ScanState::CancelledResumable && state != ScanState::CancelledResumable ) return;
    entry->snapshot.state = state;
    entry->snapshot.progressCompleted = completed;
    entry->snapshot.progressTotal = total;
    entry->snapshot.stage.assign( stage );
    entry->snapshot.error.assign( error );
    entry->snapshot.processMemory=memory;
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
        "\n" + CandidatePolicyAlgorithmId + "\n" + AnalysisScanCacheFormatId ).substr( 0, 32 );
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

    AnalysisWorkspaceReservation controlWorkspace(m_workspace,65536+32ull*
        (request.traceSessionId.size()+request.tracePath.native().size()*sizeof(std::filesystem::path::value_type)+
            request.profileIdentity.size()+traceStrongId.size()));
    ReserveControlJson(request.normalizedProfile,controlWorkspace);
    auto entry = std::make_shared<Entry>();
    entry->controlWorkspace=std::move(controlWorkspace);
    AnalysisDiskActivity diskActivity(m_diskUsage);
    entry->snapshot.scanId = scanId;
    entry->snapshot.state = ScanState::Queued;
    entry->snapshot.stage = "queued";
    entry->request = request;
    entry->aggregateIdentity = std::move( aggregateIdentity );
    entry->scanRoot = m_root / "scans" / scanId;
    entry->aggregateRoot = m_cacheRoot / AnalysisScanCacheFormatId / aggregateId;
    std::filesystem::create_directories( entry->scanRoot );
    std::string error;
    if( !WriteJson( entry->scanRoot / "profile.json", request.normalizedProfile, error,m_diskUsage,
        request.normalizedProfile.at("limits").at("cache_max_bytes").get<uint64_t>() ) ||
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
    auto memory=std::make_shared<AnalysisProcessMemoryGuard>(ProfileMemoryOptions(m_processMemory,entry->request.normalizedProfile));
    { std::lock_guard lock(entry->mutex); entry->processMemory=memory; }
    entry->worker = std::jthread( [this, entry,memory]( std::stop_token userStop ) {
        std::stop_source executionStop;
        std::stop_callback forward(userStop,[&]{executionStop.request_stop();});
        try {
            memory->Check();
            auto monitor=memory->Monitor(executionStop);
            Run( entry, executionStop.get_token(), *memory );
        }
        catch( const std::exception& exception )
        {
            const std::string error=exception.what();
            if(error.find("analysis_workspace_budget")!=std::string::npos)
                Update(entry,ScanState::CancelledResumable,0,4,"workspace_budget",error);
            else if(const auto reason=DiskBudgetReason(error); !reason.empty())
                Update(entry,ScanState::CancelledResumable,0,4,"disk_budget",reason);
            else Update( entry, ScanState::Failed, 0, 4, "unhandled_exception", error );
        }
        catch( ... )
        { Update( entry, ScanState::Failed, 0, 4, "unhandled_exception", "unknown_exception" ); }
    } );
}

void AnalysisScanManager::Run( const std::shared_ptr<Entry>& entry, std::stop_token stopToken,
    AnalysisProcessMemoryGuard& memory )
{
    // Reserve bounded state-journal headroom before bulk growth. An already
    // over-quota namespace is never evicted; only its pause state may be saved.
    AnalysisDiskActivity diskActivity(m_diskUsage,65536);
    Update( entry, ScanState::Validating, 0, 4, "validating" );
    auto disk=std::make_shared<DiskSpaceCheck>(std::array{m_root,m_cacheRoot},
        entry->request.normalizedProfile.at("limits").at("minimum_free_disk_bytes").get<uint64_t>(),m_diskAvailable);
    disk->Check(true);
    {
        std::lock_guard lock(entry->readMutex);
        entry->signatureQuery.reset(); entry->candidateQuery.reset();
        entry->neutralReader.reset(); entry->policyReader.reset();
    }
    AnalysisScanProducts products;
    std::string error;
    const auto identity=ComputeNeutralAggregateIdentity(entry->aggregateIdentity);
    AnalysisCacheTableOptions options;
    options.cancelled=[stopToken,disk] { disk->Check(); return stopToken.stop_requested(); };
    options.workspace=m_workspace;
    options.disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{m_diskUsage,
        entry->request.normalizedProfile.at("limits").at("cache_max_bytes").get<uint64_t>(),
        [disk]{disk->Check();}});
    const auto recordNeutral=[&](const std::string& generation,const std::string& sha) {
        std::lock_guard lock(entry->mutex);
        entry->neutralGeneration=generation; entry->neutralHeaderSha256=sha;
    };
    const auto loadCurrent=[&] {
        const auto pointer=entry->aggregateRoot/"current.json";
        if(!std::filesystem::exists(pointer)) return false;
        AnalysisWorkspaceReservation pointerWorkspace(m_workspace);
        const auto current=ReadJson(pointer,pointerWorkspace);
        const auto generation=current.at("generation").get<std::string>();
        const auto sha=current.at("header_sha256").get<std::string>();
        if(current.at("schema")!=1 || current.at("identity")!=identity || !GenerationName(generation))
            throw std::runtime_error("analysis_scan_neutral_pointer_identity");
        products=OpenAnalysisNeutralBundle(entry->aggregateRoot/"generations"/generation,identity,sha,options);
        recordNeutral(generation,sha); return true;
    };
    bool reused=loadCurrent();
    if( stopToken.stop_requested() )
    { Update( entry, ScanState::CancelledResumable, 0, 4, "cancelled", "cancelled" ); return; }

    if( !reused )
    {
        NeutralAggregateWriterLease lease;
        while( !AcquireNeutralAggregateWriterLease( entry->aggregateRoot, lease, error ) )
        {
            if( stopToken.stop_requested() )
            { Update( entry, ScanState::CancelledResumable, 0, 4, "cancelled", "cancelled" ); return; }
            if(loadCurrent()) { reused=true; break; }
            std::this_thread::sleep_for( std::chrono::milliseconds( 25 ) );
        }
        // A completed generation can appear between the first read and lease
        // acquisition. Recheck under the lease before scanning the source.
        if(!reused) reused=loadCurrent();
        if(!reused)
        {
        products = {};
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
        request.workspace=m_workspace;
        request.checkDiskSpace=[disk]{disk->Check();};
        request.disk=options.disk;
        const auto generation=NewGeneration(entry->aggregateRoot/"generations");
        request.temporaryRoot = entry->aggregateRoot/"generations"/generation;
        const auto progress = [this, entry, stopToken,&memory,disk]( ScanState state, uint64_t complete,
            uint64_t total, std::string_view stage ) {
            memory.Check();
            disk->Check(true);
            if( !stopToken.stop_requested() ) Update( entry, state, complete, total, stage );
        };
        if( !m_executor( request, stopToken, progress, products, error ) )
        {
            if(error.find("analysis_workspace_budget")!=std::string::npos)
                Update(entry,ScanState::CancelledResumable,0,4,"workspace_budget",error);
            else if(const auto reason=DiskBudgetReason(error); !reason.empty())
                Update(entry,ScanState::CancelledResumable,0,4,"disk_budget",reason);
            else if( stopToken.stop_requested() || error == "cancelled" )
                Update( entry, ScanState::CancelledResumable, 0, 4, "cancelled", "cancelled" );
            else Update( entry, ScanState::Failed, 0, 4, "scan_failed", error );
            return;
        }
        if( stopToken.stop_requested() )
        { Update( entry, ScanState::CancelledResumable, 0, 4, "cancelled", "cancelled" ); return; }
        memory.Check();
        Update( entry, ScanState::Aggregating, 2, 4, "publishing_neutral_cache" );
        const auto limit=entry->request.normalizedProfile.at("limits").at("cache_max_bytes").get<uint64_t>();
        CheckGenerationDiskBudget(request.temporaryRoot,limit,stopToken);
        disk->Check(true);
        const auto sha=PublishAnalysisNeutralBundle(request.temporaryRoot,identity,products,options);
        products={}; // Reopen the persisted metadata; never trust only the writer's copy.
        products=OpenAnalysisNeutralBundle(request.temporaryRoot,identity,sha,options);
        memory.Check();
        disk->Check(true);
        if(!WriteJson(entry->aggregateRoot/"current.json",{{"schema",1},{"identity",identity},
            {"generation",generation},{"header_sha256",sha}},error,m_diskUsage,limit)) throw std::runtime_error(error);
        recordNeutral(generation,sha);
        }
    }

    if( stopToken.stop_requested() )
    { Update( entry, ScanState::CancelledResumable, 2, 4, "cancelled", "cancelled" ); return; }
    memory.Check();
    Update( entry, ScanState::EvaluatingPolicy, 3, 4, "evaluating_candidate_policy" );
    CandidatePolicyInput policy;
    policy.aggregateIdentity = identity;
    policy.profileIdentity = entry->request.profileIdentity;
    policy.normalizedProfile = entry->request.normalizedProfile;
    NeutralStatisticsCacheOptions neutralOptions; neutralOptions.table=options;
    NeutralStatisticsCacheReader neutral(products.neutralCachePath,identity,neutralOptions);
    {
        AnalysisWorkspaceReservation summaryWorkspace(m_workspace,65536+32ull*neutral.SummaryBytes());
        const auto neutralSummary=json::parse(neutral.SummaryJson());
        policy.aggregate.qualityComplete=neutralSummary.at("quality").at("complete").get<bool>();
        policy.aggregate.unreportedGapCount=JsonU64(neutralSummary.at("quality").at("unreported_gap_count"));
    }
    policy.aggregate.contentSha256=neutral.ContentSha256();
    policy.capacityFacts = std::move( products.capacityFacts );
    policy.frameTimelines = std::move( products.frameTimelines );
    policy.cancelled = options.cancelled;
    const auto frameSeriesPath = products.frameSeriesPath;
    const uint64_t seriesBudget=32*1024*1024;
    policy.readFrameSeries = [frameSeriesPath, seriesBudget, cancelled=options.cancelled]( const PolicySignatureContext& context ) {
        return ReadPolicyFrameSeries( frameSeriesPath, context, seriesBudget,
            cancelled );
    };
    const auto generation=NewGeneration(entry->scanRoot/"policy-generations");
    CandidatePolicyCacheOptions policyOptions; policyOptions.table=options;
    const auto candidateRoot=entry->scanRoot/"policy-generations"/generation/"policy-cache";
    const auto policyIdentity=BuildCandidatePolicyCache(candidateRoot,neutral,policy,policyOptions);
    CandidatePolicyCacheReader verified(candidateRoot,policyIdentity,policyOptions);
    {
        AnalysisWorkspaceReservation summaryWorkspace(m_workspace,65536+32ull*verified.SummaryBytes());
        const auto checked=json::parse(verified.SummaryJson());
        if(checked.at("neutral_cache_content_sha256")!=neutral.ContentSha256())
            throw std::runtime_error("analysis_scan_policy_neutral_identity");
    }
    CheckGenerationDiskBudget(candidateRoot,entry->request.normalizedProfile.at("limits").at("cache_max_bytes").get<uint64_t>(),stopToken);
    disk->Check(true);
    {
        std::lock_guard lock(entry->mutex);
        entry->policyGeneration=generation; entry->policyIdentity=policyIdentity;
    }
    Update( entry, ScanState::Auditing, 3, 4, "auditing" );
    if( stopToken.stop_requested() )
    { Update( entry, ScanState::CancelledResumable, 3, 4, "cancelled", "cancelled" ); return; }
    memory.Check();
    Update( entry, ScanState::Complete, 4, 4, "complete" );
}

std::shared_ptr<AnalysisScanManager::Entry> AnalysisScanManager::LoadEntry( const std::string& scanId ) const
{
    const auto root = m_root / "scans" / scanId;
    const auto statePath = root / "scan-state.json";
    if( !std::filesystem::exists( statePath ) ) return {};
    try
    {
        auto entry = std::make_shared<Entry>();
        entry->controlWorkspace=AnalysisWorkspaceReservation(m_workspace);
        const auto state = ReadJson( statePath,entry->controlWorkspace );
        if( state.value( "schema_version", 0u ) != 2 || state.value("cache_format",std::string())!=AnalysisScanCacheFormatId ||
            state.value( "scan_id", std::string() ) != scanId ) return {};
        entry->scanRoot = root;
        entry->snapshot.scanId = scanId;
        entry->snapshot.state = ParseState( state.at( "state" ).get<std::string>() );
        entry->snapshot.resumable = state.value( "resumable", false );
        entry->snapshot.completed = state.value( "completed", false );
        entry->snapshot.closed = state.value( "closed", false );
        entry->snapshot.progressCompleted = JsonU64( state.at( "progress_completed" ) );
        entry->snapshot.progressTotal = JsonU64( state.at( "progress_total" ) );
        entry->snapshot.stage = state.value( "stage", std::string() );
        if(state.contains("process_memory"))
        {
            const auto& memory=state.at("process_memory");
            auto& value=entry->snapshot.processMemory;
            value.maximumBytes=JsonU64(memory.at("maximum_bytes")); value.samples=JsonU64(memory.at("samples"));
            value.residentBytes=JsonU64(memory.at("resident_bytes")); value.privateBytes=JsonU64(memory.at("private_bytes"));
            value.peakResidentBytes=JsonU64(memory.at("peak_resident_bytes")); value.peakPrivateBytes=JsonU64(memory.at("peak_private_bytes"));
            if(!memory.at("error").is_null()) value.error=memory.at("error").get<std::string>();
        }
        if( state.contains( "error" ) && !state.at( "error" ).is_null() ) entry->snapshot.error = state.at( "error" ).get<std::string>();
        entry->request.traceSessionId = state.value( "trace_session_id", std::string() );
        entry->request.tracePath = std::filesystem::path( state.at( "trace_path" ).get<std::string>() );
        entry->request.profileIdentity = state.at( "profile_identity" ).get<std::string>();
        entry->request.normalizedProfile = ReadJson( root / "profile.json",entry->controlWorkspace );
        entry->aggregateIdentity = { state.at( "trace_strong_id" ).get<std::string>(),
            m_queryExecutableSha256, "1.35.0", NeutralScanAlgorithmId, NeutralAggregateSchemaVersion };
        if( state.at( "aggregate_identity" ).get<std::string>() !=
            ComputeNeutralAggregateIdentity( entry->aggregateIdentity ) ) return {};
        const auto expectedScanId = "scan-" + HashText(
            ComputeNeutralAggregateIdentity( entry->aggregateIdentity ) + "\n" +
            entry->request.profileIdentity + "\n" + CandidatePolicyAlgorithmId + "\n" + AnalysisScanCacheFormatId ).substr( 0, 32 );
        if( scanId != expectedScanId ) return {}; // Do not resume an old candidate policy.
        entry->aggregateRoot=m_cacheRoot/AnalysisScanCacheFormatId/ComputeNeutralAggregateIdentity(entry->aggregateIdentity);
        if(AnalysisPath(std::filesystem::path(state.at("aggregate_root").get<std::string>()))!=entry->aggregateRoot) return {};
        entry->neutralGeneration=state.value("neutral_generation",std::string());
        entry->neutralHeaderSha256=state.value("neutral_header_sha256",std::string());
        entry->policyGeneration=state.value("policy_generation",std::string());
        entry->policyIdentity=state.value("policy_identity",std::string());
        if((!entry->neutralGeneration.empty() && !GenerationName(entry->neutralGeneration)) ||
            (!entry->policyGeneration.empty() && !GenerationName(entry->policyGeneration))) return {};
        const auto expectedPolicy=HashText(std::string(CandidatePolicyAlgorithmId)+"\n"+
            ComputeNeutralAggregateIdentity(entry->aggregateIdentity)+"\n"+entry->request.profileIdentity);
        if(!entry->policyIdentity.empty() && entry->policyIdentity!=expectedPolicy) return {};
        if(entry->snapshot.completed && (entry->neutralGeneration.empty() || entry->neutralHeaderSha256.size()!=64 ||
            entry->policyGeneration.empty() || entry->policyIdentity.empty())) return {};
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
    catch(const std::exception& e) {
        // Resource pressure is retryable, not evidence that the scan is absent.
        if(std::string_view(e.what())=="analysis_workspace_budget") throw;
        return {};
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
    auto snapshot=entry->snapshot;
    if(entry->processMemory) snapshot.processMemory=entry->processMemory->Snapshot();
    return snapshot;
}

AnalysisScanSnapshot AnalysisScanManager::Cancel( const std::string& scanId )
{
    const auto entry = FindOrLoad( scanId );
    {
        std::lock_guard lock(entry->mutex);
        if(entry->snapshot.completed) return entry->snapshot;
    }
    if( entry->worker.joinable() ) entry->worker.request_stop();
    uint64_t completed = 0;
    uint64_t total = 0;
    {
        std::lock_guard lock( entry->mutex );
        completed = entry->snapshot.progressCompleted;
        total = entry->snapshot.progressTotal;
    }
    Update( entry, ScanState::CancelledResumable, completed, total, "cancelled", "cancelled" );
    if(const auto snapshot=Status(scanId); snapshot.completed) return snapshot;
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

void AnalysisScanManager::EnsureReaders( const std::shared_ptr<Entry>& entry ) const
{
    entry->readMemory=std::make_shared<AnalysisProcessMemoryGuard>(ProfileMemoryOptions(m_processMemory,entry->request.normalizedProfile));
    entry->readMemory->Check();
    std::string neutralGeneration,neutralSha,policyGeneration,policyIdentity;
    {
        std::lock_guard lock(entry->mutex);
        if(!entry->snapshot.completed) throw std::runtime_error("analysis_scan_cache_incomplete");
        neutralGeneration=entry->neutralGeneration; neutralSha=entry->neutralHeaderSha256;
        policyGeneration=entry->policyGeneration; policyIdentity=entry->policyIdentity;
    }
    if(entry->signatureQuery && entry->candidateQuery) return;
    const auto identity=ComputeNeutralAggregateIdentity(entry->aggregateIdentity);
    const auto neutralRoot=entry->aggregateRoot/"generations"/neutralGeneration;
    AnalysisCacheTableOptions options; options.workspace=m_workspace;
    options.cancelled=[owner=entry.get()] { owner->readMemory->Check(false); return false; };
    auto disk=std::make_shared<DiskSpaceCheck>(std::array{m_root,m_cacheRoot},
        entry->request.normalizedProfile.at("limits").at("minimum_free_disk_bytes").get<uint64_t>(),m_diskAvailable);
    options.disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{m_diskUsage,
        entry->request.normalizedProfile.at("limits").at("cache_max_bytes").get<uint64_t>(),
        [disk]{disk->Check();}});
    NeutralStatisticsCacheOptions neutralOptions; neutralOptions.table=options;
    CandidatePolicyCacheOptions policyOptions; policyOptions.table=options;
    AnalysisCacheTableReader header(neutralRoot/"bundle-header",identity,"analysis-neutral-bundle-v1",options);
    if(header.RecordCount()!=1 || header.Descriptor().contentSha256!=neutralSha)
        throw std::runtime_error("analysis_scan_neutral_header_identity");
    const auto headerRecord=header.GetAt(0);
    AnalysisWorkspaceReservation headerWorkspace(m_workspace,65536+32ull*headerRecord.payload.size());
    const auto bundle=json::parse(headerRecord.payload);
    auto neutral=std::make_unique<NeutralStatisticsCacheReader>(neutralRoot/"neutral-statistics-v1",identity,neutralOptions);
    if(neutral->ContentSha256()!=bundle.at("neutral_content_sha256").get<std::string>())
        throw std::runtime_error("analysis_scan_neutral_content_identity");
    const auto policyRoot=entry->scanRoot/"policy-generations"/policyGeneration;
    auto policy=std::make_unique<CandidatePolicyCacheReader>(policyRoot/"policy-cache",policyIdentity,policyOptions);
    AnalysisWorkspaceReservation summaryWorkspace(m_workspace,65536+32ull*policy->SummaryBytes());
    const auto summary=json::parse(policy->SummaryJson());
    if(summary.at("neutral_cache_content_sha256")!=neutral->ContentSha256() ||
        summary.at("profile_identity")!=entry->request.profileIdentity)
        throw std::runtime_error("analysis_scan_policy_content_identity");
    entry->signatureQuery.reset(); entry->candidateQuery.reset();
    entry->neutralReader=std::move(neutral); entry->policyReader=std::move(policy);
    const auto n=entry->neutralReader.get(); const auto p=entry->policyReader.get();
    entry->signatureQuery=std::make_unique<AnalysisCacheQuery>(entry->scanRoot/"signature-query-indexes",
        n->ContentSha256(),n->SignatureCount(),[n](uint64_t i){return n->SignatureAt(i);},options);
    entry->candidateQuery=std::make_unique<AnalysisCacheQuery>(policyRoot/"candidate-query-indexes",
        p->ContentSha256(),summary.at("backlog").at("total").get<uint64_t>(),[p](uint64_t i){return p->CandidateAt(i);},options);
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
    std::lock_guard lock(entry->readMutex); EnsureReaders(entry);
    AnalysisWorkspaceReservation responseWorkspace(m_workspace,65536+32ull*
        (entry->neutralReader->SummaryBytes()+entry->policyReader->SummaryBytes()));
    const auto aggregate = json::parse(entry->neutralReader->SummaryJson());
    const auto candidates = json::parse(entry->policyReader->SummaryJson());
    json result={ { "scan_id", scanId }, { "quality", aggregate.at( "quality" ) },
        { "capture_quality", candidates.at( "capture_quality" ) },
        { "policy_algorithm", candidates.at( "policy_algorithm" ) },
        { "domains", aggregate.at( "domains" ) }, { "backlog", candidates.at( "backlog" ) },
        { "aggregate_content_sha256", entry->neutralReader->ContentSha256() },
        { "candidate_content_sha256", entry->policyReader->ContentSha256() } };
    entry->readMemory->Check(); return result;
}

json AnalysisScanManager::Signatures( const std::string& scanId, size_t limit,
    const std::string& cursor, const std::vector<std::string>& fields, const json& filter ) const
{
    const auto entry = FindOrLoad( scanId );
    std::lock_guard lock(entry->readMutex); EnsureReaders(entry);
    auto result=entry->signatureQuery->Page(limit,cursor,fields,filter);
    entry->readMemory->Check(); return result;
}

json AnalysisScanManager::Candidates( const std::string& scanId, size_t limit,
    const std::string& cursor, const std::vector<std::string>& fields, const json& filter ) const
{
    const auto entry = FindOrLoad( scanId );
    std::lock_guard lock(entry->readMutex); EnsureReaders(entry);
    auto result=entry->candidateQuery->Page(limit,cursor,fields,filter);
    entry->readMemory->Check(); return result;
}

json AnalysisScanManager::Candidate( const std::string& scanId, const std::string& candidateId ) const
{
    const auto entry = FindOrLoad( scanId );
    std::lock_guard lock(entry->readMutex); EnsureReaders(entry);
    const auto candidate=entry->policyReader->Candidate(candidateId);
    if(candidate) {
        AnalysisWorkspaceReservation responseWorkspace(m_workspace,65536+32ull*candidate->payload.size());
        auto result=json::parse(candidate->payload);
        entry->readMemory->Check(); return result;
    }
    throw std::runtime_error( "analysis_scan_candidate_not_found" );
}

json AnalysisScanManager::RepresentativeFrames( const std::string& scanId,
    const std::string& candidateId ) const
{
    const auto entry=FindOrLoad(scanId);
    std::lock_guard lock(entry->readMutex); EnsureReaders(entry);
    const auto record=entry->policyReader->Candidate(candidateId);
    if(!record) throw std::runtime_error("analysis_scan_candidate_not_found");
    AnalysisWorkspaceReservation responseWorkspace(m_workspace,65536+32ull*record->payload.size());
    const auto candidate=json::parse(record->payload);
    json result={ { "scan_id", scanId }, { "candidate_id", candidateId },
        { "frames", candidate.at( "representative_frame_details" ) } };
    entry->readMemory->Check(); return result;
}

json AnalysisScanManager::Quality( const std::string& scanId ) const
{
    const auto entry = FindOrLoad( scanId );
    std::lock_guard lock(entry->readMutex); EnsureReaders(entry);
    AnalysisWorkspaceReservation responseWorkspace(m_workspace,65536+32ull*
        (entry->neutralReader->SummaryBytes()+entry->policyReader->SummaryBytes()));
    const auto aggregate = json::parse(entry->neutralReader->SummaryJson());
    auto quality = aggregate.at( "quality" );
    quality["scan_id"] = scanId;
    quality["domains"] = aggregate.at( "domains" );
    quality["capture_quality"] = json::parse(entry->policyReader->SummaryJson()).at("capture_quality");
    entry->readMemory->Check();
    return quality;
}

}
