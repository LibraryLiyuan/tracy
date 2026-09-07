#include "TracyAnalysisScanManager.hpp"
#include "TracyAnalysisProfile.hpp"
#include "TracyBoundedScanCursor.hpp"
#include "TracyExactStatistics.hpp"
#include "TracyQueryService.hpp"
#include "FakeTraceSource.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>

using namespace std::chrono_literals;
using nlohmann::json;
using namespace tracy::analysis;
using namespace tracy::query;

namespace
{

json Profile( uint64_t topN = 10 )
{
    json result = json::object();
    result["schema_version"] = 1;
    result["profile_name"] = "scan-manager-test";
    result["frame_budget"] = { { "target_fps", 60.0 }, { "frame_ms", 16.666667 } };
    result["resource_budgets"] = {
        { "cpu_memory", { { "value", 16.0 }, { "unit", "GB" }, { "status", "provisional" } } },
        { "gpu_memory", { { "value", 6.4 }, { "unit", "GB" }, { "status", "fixed" } } } };
    result["candidate_policy"] = { { "top_n", topN }, { "cumulative_contribution", 0.8 },
        { "per_domain_limit", 50 }, { "priorities", json::array( { "P0", "P1", "P2", "P3", "P4" } ) } };
    result["limits"] = { { "query_memory_target_bytes", 8589934592ULL },
        { "query_memory_hard_bytes", 17179869184ULL }, { "cache_max_bytes", 137438953472ULL },
        { "minimum_free_disk_bytes", 68719476736ULL } };
    result["user_focus"] = json::array( { "Update" } );
    return result;
}

AnalysisScanProducts Products( const std::filesystem::path& temporary )
{
    NeutralStatisticsInput input;
    input.temporaryRoot = temporary;
    input.maximumBufferedValues = 2;
    input.runs = { { "cpu", "cpu:update", "Player.Frame", 7,
        20'000'000, 20'000'000, 0, 0, 1, true, false } };
    input.denominators = { { "cpu", "cpu:update", "Player.Frame", 1 } };
    input.domainAudit = { { "cpu", true, "complete", 1, 1, 1,
        std::string( 64, 'a' ), std::string( 64, 'a' ), true, {} } };

    AnalysisScanProducts products;
    products.aggregate = BuildNeutralStatistics( input );
    PolicySignatureContext context;
    context.domain = "cpu"; context.signatureId = "cpu:update";
    context.name = "Update"; context.path = "PlayerLoop/Update";
    context.frameScope = "Player.Frame"; context.frameRoot = true;
    context.frames = { { 7, 20'000'000, { "cpu-zone:7" }, "player-loop" } };
    products.signatureContexts.push_back( std::move( context ) );
    products.capacityFacts.push_back( { "gpu.memory", "gpu-local-peak", "gpu-capacity",
        "gpu_memory", 7'000'000'000ULL, 7, true, "DXGI Local peak" } );
    return products;
}

AnalysisScanSnapshot WaitFor( AnalysisScanManager& manager, const std::string& scanId,
    ScanState state, std::chrono::milliseconds timeout = 2s )
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    AnalysisScanSnapshot snapshot;
    do
    {
        snapshot = manager.Status( scanId );
        if( snapshot.state == state ) return snapshot;
        std::this_thread::sleep_for( 5ms );
    }
    while( std::chrono::steady_clock::now() < deadline );
    assert( snapshot.state == state );
    return snapshot;
}

class SourceDegradedTrace final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource
{
public:
    bool nestedCpu = false;
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        if( !nestedCpu ) return FakeTraceSource::ScanCpuZones( range );
        std::vector<CpuZoneDto> values;
        const auto add = [&]( const char* name, size_t id, std::optional<size_t> parent,
            int64_t begin, int64_t end ) {
            CpuZoneDto zone;
            zone.ref = MakeEntityRef( "cpu-zone", id );
            zone.threadRef = MakeEntityRef( "thread", 1 );
            zone.sourceLocationRef = name == std::string( "Work" ) ? "source:work" : std::string( "source:" ) + name;
            zone.name = name; zone.startNs = begin; zone.endNs = end;
            zone.complete = true; zone.timingValid = true;
            if( parent ) zone.parentRef = MakeEntityRef( "cpu-zone", *parent );
            values.push_back( zone );
        };
        add( "ParentA", 10, {}, 0, 40 ); add( "Work", 11, 10, 1, 20 );
        add( "ParentB", 12, {}, 40, 80 ); add( "Work", 13, 12, 41, 60 );
        const auto first = std::min( range.offset, values.size() );
        const auto last = first + std::min( range.limit, values.size() - first );
        return { values.begin() + first, values.begin() + last };
    }
    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    {
        return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 1, 100, true };
    }
    std::vector<FrameSetDto> GetFrameSets() const override
    {
        return {
            { MakeEntityRef( "frame-set", 0 ), 0, "Player.Frame", true, 2, 2 },
            { MakeEntityRef( "frame-set", 1 ), 1, "Render.Frame", false, 1, 1 }
        };
    }
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override
    {
        const std::vector<FrameDto> values = {
            { MakeEntityRef( "frame", 0 ), MakeEntityRef( "frame-set", 0 ), 0, 0, 100, {}, true },
            // This second FrameMark proves the first continuous Player frame;
            // its own synthetic tail boundary is intentionally excluded.
            { MakeEntityRef( "frame", 1 ), MakeEntityRef( "frame-set", 0 ), 1, 100, 200, {}, true },
            { MakeEntityRef( "frame", 2 ), MakeEntityRef( "frame-set", 1 ), 0, 0, 100, {}, true }
        };
        const auto begin = std::min( range.offset, values.size() );
        const auto end = begin + std::min( range.limit, values.size() - begin );
        return { values.begin() + begin, values.begin() + end };
    }
    std::vector<JobDto> GetJobs() const override
    {
        auto values = tracy::query::test::FakeTraceSource::GetJobs();
        assert( !values.empty() );
        auto presentJob = values.back();
        presentJob.ref = MakeEntityRef( "job", 3 );
        presentJob.jobId = 3;
        presentJob.packedHandle = ( uint64_t( 2 ) << 32 ) | 7;
        presentJob.originFrameSequence = 2;
        presentJob.originFrameId = ( uint64_t( 1 ) << 48 ) | 2;
        values.emplace_back( std::move( presentJob ) );
        return values;
    }
    std::vector<CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const override
    {
        auto values = tracy::query::test::FakeTraceSource::GetCorrelatedFrameEvents();
        const uint64_t presentFrameId = ( uint64_t( 1 ) << 48 ) | 2;
        values.push_back( { MakeEntityRef( "frame-identity-event", 4 ), presentFrameId, 2, 120,
            MakeEntityRef( "thread", 1 ), uint8_t( tracy::JnFrameDomain::Present ),
            uint8_t( tracy::JnFramePhase::Begin ), uint8_t( tracy::JnFrameFlags::Canonical ) } );
        values.push_back( { MakeEntityRef( "frame-identity-event", 5 ), presentFrameId, 2, 130,
            MakeEntityRef( "thread", 1 ), uint8_t( tracy::JnFrameDomain::Present ),
            uint8_t( tracy::JnFramePhase::End ), uint8_t( tracy::JnFrameFlags::Canonical ) } );
        return values;
    }
};

}

int main()
{
    {
        json large = json::array();
        for( size_t index = 0; index < 100; ++index )
            large.push_back( { { "index", std::to_string( index ) },
                { "payload", std::string( 64 * 1024, char( 'a' + index % 26 ) ) } } );
        size_t observed = 0;
        std::string cursor;
        bool observedByteLimit = false;
        for( ;; )
        {
            const auto page = PaginateAnalysisScanItems( large, 1000, cursor, {}, json::object() );
            assert( page.dump().size() < 4 * 1024 * 1024 );
            observed += page.at( "items" ).size();
            observedByteLimit = observedByteLimit || page.at( "page" ).at( "byte_limited" ).get<bool>();
            if( page.at( "page" ).at( "done" ).get<bool>() ) break;
            cursor = page.at( "page" ).at( "next_cursor" ).get<std::string>();
            assert( !cursor.empty() );
        }
        assert( observed == large.size() );
        assert( observedByteLimit );
    }

    const auto root = std::filesystem::temp_directory_path() /
        ( "jn-tracy-analysis-scan-manager-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count() ) );
    const auto cache = root / "cache";
    const auto tracePath = root / "fixture.tracy";
    std::filesystem::create_directories( cache );
    { std::ofstream trace( tracePath, std::ios::binary ); trace << "fixture"; }

    const auto validation = ValidateAndNormalizeAnalysisProfile( Profile() );
    assert( validation.valid );

    // The default fake trace intentionally persists a producer drop. A full
    // native scan must remain usable, preserve the invalid source domain, and
    // leave the neutral aggregate's own integrity audit complete.
    {
        auto degraded = std::make_shared<SourceDegradedTrace>();
        AnalysisScanExecutionRequest request;
        request.source = degraded;
        request.temporaryRoot = root / "source-degraded-default-scan";
        request.normalizedProfile = validation.normalized;
        request.profileIdentity = validation.profileSha256;
        AnalysisScanProducts products;
        std::string scanError;
        std::stop_source stop;
        assert( ExecuteDefaultAnalysisScan( request, stop.get_token(),
            []( ScanState, uint64_t, uint64_t, std::string_view ) {},
            products, scanError ) );
        assert( scanError.empty() );
        assert( products.aggregate.qualityComplete );
        assert( products.aggregate.unreportedGapCount == 0 );
        assert( std::count_if( products.aggregate.signatures.begin(),
            products.aggregate.signatures.end(), []( const auto& value ) {
                return value.domain == "cpu";
            } ) >= 2 );
        const auto telemetry = std::find_if( products.aggregate.domains.begin(),
            products.aggregate.domains.end(), []( const auto& value ) {
                return value.domain == "telemetry";
            } );
        assert( telemetry != products.aggregate.domains.end() );
        assert( telemetry->status == "invalid" );
        assert( telemetry->qualityComplete );
        const auto memory = std::find_if( products.aggregate.domains.begin(),
            products.aggregate.domains.end(), []( const auto& value ) {
                return value.domain == "memory";
            } );
        assert( memory != products.aggregate.domains.end() );
        assert( memory->present );
        assert( memory->status == "complete" );
        assert( memory->inputCount == 1 );
        assert( memory->consumedInputCount == 1 );
        const auto jobAggregate = std::find_if( products.aggregate.signatures.begin(),
            products.aggregate.signatures.end(), []( const auto& value ) {
                return value.domain == "job" && value.frameScope == "Player.Frame";
            } );
        assert( jobAggregate != products.aggregate.signatures.end() );
        // SourceDegradedTrace contains one proven continuous Player frame and
        // independent Render and Present frames.  The Present frame schedules
        // another exact occurrence of the same job signature. Job per-frame
        // statistics must use only Player.Frame rather than treating every
        // correlated origin identity as a Player frame.
        assert( jobAggregate->completeFrameCount == 1 );
        assert( jobAggregate->presentFrameCount == 1 );
        if( jobAggregate->criticalPath.perCompleteFrame.total != 0 )
        { std::cerr << "Job execution is not a proven critical path\n"; return 1; }
        degraded->nestedCpu = true;
        request.temporaryRoot = root / "nested-cpu-paths";
        AnalysisScanProducts nested;
        assert( ExecuteDefaultAnalysisScan( request, stop.get_token(),
            []( ScanState, uint64_t, uint64_t, std::string_view ) {}, nested, scanError ) );
        size_t workPaths = 0;
        for( const auto& context : nested.signatureContexts )
            if( context.domain == "cpu" && context.name == "Work" )
            {
                ++workPaths;
                if( !context.frameSeriesComplete || context.seriesCount == 0 || context.seriesSha256.size() != 64 || context.threadOrQueue.empty() )
                { std::cerr << "Real CPU scan must persist a complete checked frame series, not representatives\n"; return 1; }
                if( context.path.find( "Parent" ) == std::string::npos ||
                    context.familyId == context.parentSignatureId )
                { std::cerr << "Candidate identity must preserve path, not merge sibling families\n"; return 1; }
            }
        if( workPaths != 4 )
        { std::cerr << "Expected two distinct Work paths in each of two FrameSets\n"; return 1; }
        if( std::none_of( nested.signatureContexts.begin(), nested.signatureContexts.end(), []( const auto& c ) {
            return c.domain == "gpu" && c.frameScope.starts_with( "GPU.L0Segment:" ) && c.frameSeriesComplete;
        } ) ) { std::cerr << "Physical GPU work must survive missing origin-frame linkage\n"; return 1; }
    }

    auto source = std::make_shared<tracy::query::test::FakeTraceSource>();
    std::atomic<uint32_t> executions = 0;
    std::atomic<bool> release = false;
    AnalysisScanExecutor executor = [&]( const AnalysisScanExecutionRequest& request,
        std::stop_token stopToken, const AnalysisScanProgressCallback& progress,
        AnalysisScanProducts& products, std::string& error ) {
        ++executions;
        progress( ScanState::Scanning, 1, 4, "fixture_scan" );
        while( !release.load() && !stopToken.stop_requested() ) std::this_thread::sleep_for( 2ms );
        if( stopToken.stop_requested() ) { error = "cancelled"; return false; }
        progress( ScanState::Aggregating, 3, 4, "fixture_aggregate" );
        products = Products( request.temporaryRoot );
        return true;
    };
    AnalysisScanSourceResolver resolver = [source]( const std::filesystem::path&,
        std::stop_token ) { return source; };

    std::string scanId;
    {
        AnalysisScanManager manager( root, cache, std::string( 64, 'e' ), resolver, executor );
        AnalysisScanStartRequest request;
        request.traceSessionId = "trace-session-1";
        request.tracePath = tracePath;
        request.source = source;
        request.normalizedProfile = validation.normalized;
        request.profileIdentity = validation.profileSha256;

        const auto startedAt = std::chrono::steady_clock::now();
        const auto started = manager.Start( request );
        assert( std::chrono::steady_clock::now() - startedAt < 2s );
        assert( started.state == ScanState::Queued || started.state == ScanState::Validating ||
            started.state == ScanState::Scanning );
        scanId = started.scanId;
        assert( scanId.starts_with( "scan-" ) );

        const auto duplicate = manager.Start( request );
        assert( duplicate.scanId == scanId );
        std::this_thread::sleep_for( 20ms );
        assert( executions == 1 );

        const auto cancelAt = std::chrono::steady_clock::now();
        manager.Cancel( scanId );
        const auto cancelled = WaitFor( manager, scanId, ScanState::CancelledResumable );
        assert( std::chrono::steady_clock::now() - cancelAt < 2s );
        assert( cancelled.resumable && !cancelled.completed );

        release = true;
        const auto resumed = manager.Resume( scanId );
        assert( resumed.scanId == scanId );
        const auto complete = WaitFor( manager, scanId, ScanState::Complete );
        assert( complete.completed && complete.progressCompleted == complete.progressTotal );
        assert( executions == 2 );

        const auto summary = manager.Summary( scanId );
        assert( summary.at( "quality" ).at( "complete" ) == true );
        assert( summary.at( "policy_algorithm" ) == "candidate-policy-v3" );
        assert( summary.at( "capture_quality" ).is_array() );
        const auto signatures = manager.Signatures( scanId, 1, "", {}, json::object() );
        assert( signatures.at( "items" ).size() == 1 && signatures.at( "page" ).at( "done" ) == true );
        const auto candidates = manager.Candidates( scanId, 1, "", {}, json::object() );
        assert( candidates.at( "items" ).size() == 1 );
        const auto candidateId = candidates.at( "items" ).at( 0 ).at( "candidate_id" ).get<std::string>();
        const auto candidate = manager.Candidate( scanId, candidateId );
        assert( candidate.at( "candidate_id" ) == candidateId );
        const auto representative = manager.RepresentativeFrames( scanId, candidateId );
        assert( !representative.at( "frames" ).empty() );
        const auto quality = manager.Quality( scanId );
        assert( quality.at( "complete" ) == true );
        assert( quality.at( "capture_quality" ) == summary.at( "capture_quality" ) );
        assert( manager.Close( scanId ).closed );
    }

    // A new Query process discovers completed state from disk and can query it.
    {
        AnalysisScanManager restarted( root, cache, std::string( 64, 'e' ), resolver, executor );
        const auto status = restarted.Status( scanId );
        assert( status.state == ScanState::Complete && status.completed );
        assert( !restarted.Candidates( scanId, 100, "", {}, json::object() ).at( "items" ).empty() );

        // Changing only policy creates a new scan but reuses the completed neutral aggregate.
        const auto changedValidation = ValidateAndNormalizeAnalysisProfile( Profile( 3 ) );
        assert( changedValidation.valid );
        AnalysisScanStartRequest changed;
        changed.traceSessionId = "trace-session-2"; changed.tracePath = tracePath; changed.source = source;
        changed.normalizedProfile = changedValidation.normalized;
        changed.profileIdentity = changedValidation.profileSha256;
        const auto before = executions.load();
        const auto changedStart = restarted.Start( changed );
        const auto changedComplete = WaitFor( restarted, changedStart.scanId, ScanState::Complete );
        assert( changedComplete.completed && executions == before );
    }

    // Query dispatch remains responsive while a persistent scan runs; the scan
    // does not occupy QueryService's legacy global cache mutex.
    {
        const auto analysisRoot = root / "query-analysis";
        const auto analysisCache = analysisRoot / "cache";
        std::filesystem::create_directories( analysisCache );
        std::atomic<bool> queryRelease = false;
        AnalysisScanExecutor queryExecutor = [&]( const AnalysisScanExecutionRequest& request,
            std::stop_token token, const AnalysisScanProgressCallback& progress,
            AnalysisScanProducts& output, std::string& error ) {
            progress( ScanState::Scanning, 1, 2, "query_fixture" );
            while( !queryRelease.load() && !token.stop_requested() ) std::this_thread::sleep_for( 2ms );
            if( token.stop_requested() ) { error = "cancelled"; return false; }
            output = Products( request.temporaryRoot );
            return true;
        };
        SessionManager sessions( { root }, 2,
            []( const std::filesystem::path&, SessionManager::StateCallback callback ) {
                callback( tracy::analysis::TraceSourceState::Ready );
                return std::make_unique<tracy::query::test::FakeTraceSource>();
            } );
        QueryService service( sessions, DefaultAnalysisCacheBytes, analysisRoot,
            analysisCache, queryExecutor, std::string( 64, 'd' ) );
        const auto opened = sessions.Open( tracePath );
        assert( sessions.WaitReady( opened.id, 2s ).state == tracy::analysis::TraceSourceState::Ready );
        const auto started = service.Execute( {
            { "protocol", QueryProtocol }, { "id", "scan-start" },
            { "method", "analysis.scan.start" },
            { "params", { { "trace_id", opened.id }, { "profile", Profile() } } }
        } );
        assert( started.at( "ok" ) == true );
        const auto queryScanId = started.at( "data" ).at( "scan_id" ).get<std::string>();
        const auto ordinaryAt = std::chrono::steady_clock::now();
        const auto ordinary = service.Execute( {
            { "protocol", QueryProtocol }, { "id", "ordinary" },
            { "method", "trace.info" }, { "params", { { "trace_id", opened.id } } }
        } );
        assert( ordinary.at( "ok" ) == true );
        assert( std::chrono::steady_clock::now() - ordinaryAt < 2s );
        const auto status = service.Execute( {
            { "protocol", QueryProtocol }, { "id", "scan-status" },
            { "method", "analysis.scan.status" }, { "params", { { "scan_id", queryScanId } } }
        } );
        assert( status.at( "ok" ) == true && status.at( "data" ).at( "completed" ) == false );
        queryRelease = true;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        json completed;
        do
        {
            completed = service.Execute( {
                { "protocol", QueryProtocol }, { "id", "scan-complete" },
                { "method", "analysis.scan.status" }, { "params", { { "scan_id", queryScanId } } }
            } );
            if( completed.at( "data" ).at( "completed" ) == true ) break;
            std::this_thread::sleep_for( 5ms );
        }
        while( std::chrono::steady_clock::now() < deadline );
        assert( completed.at( "data" ).at( "completed" ) == true );
    }

    std::error_code ignored;
    std::filesystem::remove_all( root, ignored );
    return 0;
}
