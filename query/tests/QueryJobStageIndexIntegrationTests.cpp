#include "TracyQueryIndex.hpp"
#include "TracyWorkerTraceSource.hpp"
#include "TracyQueryService.hpp"
#include "TracyQueryCorrelationStore.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

void RequireJobEqual( const tracy::analysis::JobDto& expected, const tracy::analysis::JobDto& actual )
{
    const auto check = [&]( bool condition, const char* field ) {
        if( !condition ) throw std::runtime_error( "indexed Job field changed: " + std::string( field ) +
            " job=" + std::to_string( expected.jobId ) );
    };
#define CHECK_JOB_FIELD(field) check( actual.field == expected.field, #field )
    CHECK_JOB_FIELD( ref ); CHECK_JOB_FIELD( jobId ); CHECK_JOB_FIELD( packedHandle ); CHECK_JOB_FIELD( name );
    CHECK_JOB_FIELD( typeId ); CHECK_JOB_FIELD( kind ); CHECK_JOB_FIELD( flags ); CHECK_JOB_FIELD( scheduleNs );
    CHECK_JOB_FIELD( scheduleThreadRef ); CHECK_JOB_FIELD( count ); CHECK_JOB_FIELD( grainSize ); CHECK_JOB_FIELD( unityFlowId );
    CHECK_JOB_FIELD( originFrameSequence ); CHECK_JOB_FIELD( originFrameId ); CHECK_JOB_FIELD( scheduleCallstack );
    CHECK_JOB_FIELD( scheduleCallsiteId ); CHECK_JOB_FIELD( scheduleStackProvenance ); CHECK_JOB_FIELD( scheduleStackUnavailableReason );
    CHECK_JOB_FIELD( jobSchemaVersion ); CHECK_JOB_FIELD( expectedDependencyCount ); CHECK_JOB_FIELD( readyNs );
    CHECK_JOB_FIELD( queueEnterNs ); CHECK_JOB_FIELD( firstRunNs ); CHECK_JOB_FIELD( completedNs );
    CHECK_JOB_FIELD( dependencyReadyLatencyNs ); CHECK_JOB_FIELD( readyLane ); CHECK_JOB_FIELD( queueLane );
    CHECK_JOB_FIELD( readyFlags ); CHECK_JOB_FIELD( executionNs ); CHECK_JOB_FIELD( waitNs );
    CHECK_JOB_FIELD( waitActiveHelpNs ); CHECK_JOB_FIELD( waitSpinYieldNs ); CHECK_JOB_FIELD( waitSleepNs );
    CHECK_JOB_FIELD( dispatchCount ); CHECK_JOB_FIELD( schedulerStealCount ); CHECK_JOB_FIELD( rangeStealSliceCount );
    CHECK_JOB_FIELD( activeHelpDispatchCount ); CHECK_JOB_FIELD( queueRetryCount ); CHECK_JOB_FIELD( waitEndCount );
    CHECK_JOB_FIELD( continuationCount ); CHECK_JOB_FIELD( captureBoundary ); CHECK_JOB_FIELD( cancelled );
    CHECK_JOB_FIELD( incomplete ); CHECK_JOB_FIELD( orphan ); CHECK_JOB_FIELD( truncated );
#undef CHECK_JOB_FIELD
    Require( actual.dependencies.size() == expected.dependencies.size() && actual.executionLanes == expected.executionLanes &&
        actual.waitCallstacks.size() == expected.waitCallstacks.size() && actual.stages.size() == expected.stages.size(),
        "indexed Job collection sizes changed" );
    for( size_t index = 0; index < expected.dependencies.size(); index++ )
        Require( actual.dependencies[index].prerequisiteJobId == expected.dependencies[index].prerequisiteJobId &&
            actual.dependencies[index].prerequisiteHandle == expected.dependencies[index].prerequisiteHandle &&
            actual.dependencies[index].flags == expected.dependencies[index].flags, "indexed Job dependency changed" );
    for( size_t index = 0; index < expected.waitCallstacks.size(); index++ )
    {
        const auto& lhs = expected.waitCallstacks[index]; const auto& rhs = actual.waitCallstacks[index];
        Require( rhs.timeNs == lhs.timeNs && rhs.threadRef == lhs.threadRef && rhs.waitSpanId == lhs.waitSpanId &&
            rhs.callstack == lhs.callstack && rhs.callsiteId == lhs.callsiteId && rhs.stackProvenance == lhs.stackProvenance &&
            rhs.stackUnavailableReason == lhs.stackUnavailableReason, "indexed Job wait callstack changed" );
    }
    for( size_t index = 0; index < expected.stages.size(); index++ )
    {
        const auto& lhs = expected.stages[index]; const auto& rhs = actual.stages[index];
        Require( rhs.timeNs == lhs.timeNs && rhs.threadRef == lhs.threadRef && rhs.spanId == lhs.spanId &&
            rhs.arg0 == lhs.arg0 && rhs.arg1 == lhs.arg1 && rhs.stage == lhs.stage && rhs.flags == lhs.flags &&
            rhs.callsiteId == lhs.callsiteId && rhs.callstack == lhs.callstack && rhs.stackProvenance == lhs.stackProvenance &&
            rhs.stackUnavailableReason == lhs.stackUnavailableReason, "indexed Job stage changed" );
    }
}

}

int main( int argc, char** argv )
{
    try
    {
        if( argc != 3 ) throw std::runtime_error( "expected Job Trace and isolated output directory" );
        namespace fs = std::filesystem;
        const fs::path root = argv[2];
        if( fs::exists( root ) ) throw std::runtime_error( "refusing existing output directory" );
        fs::create_directories( root );
        const auto input = root / "jobs.tracy";
        fs::copy_file( argv[1], input );

        auto original = tracy::analysis::WorkerTraceSource::Open( input );
        const auto expectedStages = original->GetTraceInfo().counts.jobStages;
        Require( expectedStages != 0, "fixture has no Job stages" );
        const auto expectedJobs = original->GetJobs();
        const auto expectedDispatches = original->GetGfxDispatches();
        const auto expectedEntities = original->GetGfxEntities();
        const auto expectedLinks = original->GetGfxLinks();
        const auto expectedFrames = original->GetCorrelatedFrameEvents();
        const auto expectedFrameIdentityRef = expectedFrames.empty() ? std::string() :
            original->MakeEntityRef( "frame-identity", expectedFrames.front().frameId );
        const auto expectedCallsites = original->GetCallsites();
        original.reset();

        const auto manifest = tracy::query::QueryIndex::Build( input );
        Require( manifest.jobStages.count == expectedStages, "Job-stage sidecar count changed" );
        {
            tracy::query::QueryCorrelationStoreReader correlations(
                manifest.jobStages.path / "correlation", manifest.sourceFingerprint );
            correlations.ValidateAll();
            const auto parents = std::count_if( expectedEntities.begin(), expectedEntities.end(),
                []( const auto& value ) { return value.parentId != 0; } );
            Require( correlations.Count() == expectedFrames.size() + expectedDispatches.size() * 2 +
                expectedEntities.size() + parents + expectedLinks.size() * 2,
                "schema44 correlation endpoint count changed" );
        }
        const auto validation = tracy::query::QueryIndex::Validate( input, true );
        Require( validation.manifest.has_value(), "Job schedule deep validation failed" );
        bool rejectedUnboundCompact = false;
        try { auto compact = tracy::analysis::WorkerTraceSource::Open( manifest.dataPath ); }
        catch( const tracy::analysis::TraceLoadError& error )
        { rejectedUnboundCompact = error.code == tracy::analysis::TraceLoadErrorCode::Corrupt; }
        Require( rejectedUnboundCompact, "manifest-bound compact payload opened without its external dictionary" );

        auto indexed = tracy::query::QueryIndex::Open( manifest );
        Require( indexed->GetTraceInfo().counts.jobStages == expectedStages,
            "indexed trace lost Job-stage count" );
        Require( indexed->JobDerivedDataReady(), "indexed trace did not publish Job aggregation" );
        const auto actualJobs = indexed->GetJobs();
        Require( actualJobs.size() == expectedJobs.size(), "indexed Job count changed" );
        for( size_t index = 0; index < expectedJobs.size(); index++ ) RequireJobEqual( expectedJobs[index], actualJobs[index] );
        auto orderedJobs = expectedJobs;
        std::sort( orderedJobs.begin(), orderedJobs.end(), []( const auto& lhs, const auto& rhs ) {
            return lhs.scheduleNs != rhs.scheduleNs ? lhs.scheduleNs < rhs.scheduleNs : lhs.jobId < rhs.jobId;
        } );
        for( size_t position = 0; position < orderedJobs.size(); position++ )
        {
            const auto page = indexed->ScanJobsBySchedule( position, 1, {} );
            Require( page.size() == 1, "indexed schedule page missing" );
            RequireJobEqual( orderedJobs[position], page.front() );
        }
        const auto actualDispatches = indexed->GetGfxDispatches();
        Require( actualDispatches.size() == expectedDispatches.size(), "indexed Gfx dispatch count changed" );
        for( size_t index = 0; index < expectedDispatches.size(); index++ )
            Require( actualDispatches[index].ref == expectedDispatches[index].ref &&
                actualDispatches[index].dispatchId == expectedDispatches[index].dispatchId &&
                actualDispatches[index].frameIndex == expectedDispatches[index].frameIndex &&
                actualDispatches[index].timeNs == expectedDispatches[index].timeNs &&
                actualDispatches[index].threadRef == expectedDispatches[index].threadRef &&
                actualDispatches[index].expectedJobs == expectedDispatches[index].expectedJobs &&
                actualDispatches[index].threadingMode == expectedDispatches[index].threadingMode &&
                actualDispatches[index].flags == expectedDispatches[index].flags, "indexed Gfx dispatch changed" );
        const auto actualFrames = indexed->GetCorrelatedFrameEvents();
        Require( actualFrames.size() == expectedFrames.size(), "indexed correlated Frame count changed" );
        const auto actualCallsites = indexed->GetCallsites();
        Require( actualCallsites.size() == expectedCallsites.size(), "indexed Callsite count changed" );
        for( const auto& expectedJob : expectedJobs )
        {
            const auto directed = indexed->GetDirectedJobs( expectedJob.jobId, true, {} );
            Require( std::any_of( directed.begin(), directed.end(), [&]( const auto& job ) { return job.jobId == expectedJob.jobId; } ),
                "indexed directed Job read lost its seed" );
        }
        for( const auto& expectedJob : expectedJobs ) if( expectedJob.originFrameId != 0 )
        {
            const auto evidence = indexed->GetEvidenceJobs( expectedJob.originFrameId, {} );
            Require( std::any_of( evidence.begin(), evidence.end(), [&]( const auto& job ) { return job.jobId == expectedJob.jobId; } ),
                "indexed Frame Job evidence lost its seed" );
        }
        const auto searchPages = [&]( bool useIndex ) {
            tracy::query::SessionManager sessions( {root}, 1,
                [&]( const fs::path& path, tracy::query::SessionManager::StateCallback ) -> std::unique_ptr<tracy::analysis::TraceSource> {
                    if( useIndex ) return tracy::query::QueryIndex::Open( manifest );
                    return tracy::analysis::WorkerTraceSource::Open( path );
                } );
            const auto session = sessions.Open( input );
            Require( sessions.WaitReady( session.id, std::chrono::seconds( 30 ) ).state == tracy::analysis::TraceSourceState::Ready,
                "Job search session not ready" );
            tracy::query::QueryService service( sessions );
            nlohmann::json request = {{"protocol", "tracy-query/1"}, {"id", 1}, {"method", "job.search"},
                {"params", {{"trace_id", session.id}, {"limit", 1}}}};
            auto jobs = nlohmann::json::array();
            bool complete = false;
            for( size_t page = 0; page <= expectedJobs.size(); page++ )
            {
                const auto response = service.Execute( request );
                Require( response.at("ok"), "real Job search failed" );
                for( const auto& job : response.at("data").at("jobs") ) jobs.push_back( job );
                if( response.at("page").at("next_cursor").is_null() ) { complete = true; break; }
                request["params"]["cursor"] = response.at("page").at("next_cursor");
            }
            Require( complete && jobs.size() == expectedJobs.size(), "real Job search lost rows or did not finish" );
            request["method"] = "job.gfx.statistics";
            request["params"] = {{"trace_id", session.id}};
            const auto statistics = service.Execute( request );
            Require( statistics.at("ok"), "real Job Gfx statistics failed" );
            request["method"] = "job.statistics";
            const auto jobStatistics = service.Execute( request );
            Require( jobStatistics.at("ok"), "real Job statistics failed" );
            nlohmann::json related = nullptr;
            nlohmann::json chain = nullptr;
            nlohmann::json frameIdentity = nullptr;
            nlohmann::json evidence = nullptr;
            nlohmann::json gpuPasses = nullptr;
            if( !expectedFrames.empty() )
            {
                request["method"] = "frame.identity";
                request["params"] = {{"trace_id", session.id}, {"ref", expectedFrameIdentityRef},
                    {"max_scan_events", 10000}};
                const auto frameResponse = service.Execute( request );
                Require( frameResponse.at("ok"), "real Frame identity query failed" );
                frameIdentity = frameResponse.at("data");
                request["method"] = "entity.related";
                request["params"] = {{"trace_id", session.id}, {"ref", expectedFrameIdentityRef},
                    {"max_scan_events", 10000}, {"max_nodes", 1000}, {"max_edges", 2000}};
                const auto response = service.Execute( request );
                Require( response.at("ok"), "real Frame related query failed" );
                related = response.at("data");
                request["method"] = "correlation.chain";
                const auto chainResponse = service.Execute( request );
                Require( chainResponse.at("ok"), "real Frame correlation chain failed" );
                chain = chainResponse.at("data");
                request["method"] = "evidence.graph";
                request["params"] = {{"trace_id", session.id}, {"frame_id", expectedFrames.front().frameId},
                    {"domains", nlohmann::json::array({"frame", "gfx"})},
                    {"max_scan_events", 10000}, {"max_nodes", 1000}, {"max_edges", 2000}};
                const auto evidenceResponse = service.Execute( request );
                Require( evidenceResponse.at("ok"), "real Frame evidence graph failed" );
                evidence = evidenceResponse.at("data");
                evidence["source_read_cost"].erase( "job_dto_count" );
                evidence["source_read_cost"].erase( "job_read_ms" );
            }
            request["method"] = "gpu.pass.search";
            request["params"] = {{"trace_id", session.id}, {"limit", 100},
                {"max_scan_events", 10000}, {"max_nodes", 1000}, {"max_edges", 2000}};
            const auto passResponse = service.Execute( request );
            Require( passResponse.at("ok"), "real Explicit GPU Pass search failed" );
            gpuPasses = passResponse.at("data");
            return nlohmann::json{{"jobs", jobs}, {"gfx_statistics", statistics.at("data")},
                {"job_statistics", jobStatistics.at("data")}, {"related", std::move( related )},
                {"chain", std::move( chain )}, {"frame_identity", std::move( frameIdentity )},
                {"evidence", std::move( evidence )}, {"gpu_passes", std::move( gpuPasses )}};
        };
        const auto workerJson = searchPages( false );
        const auto indexedJson = searchPages( true );
        if( workerJson != indexedJson )
        {
            std::ofstream( root / "worker-query.json" ) << workerJson.dump( 2 );
            std::ofstream( root / "indexed-query.json" ) << indexedJson.dump( 2 );
            throw std::runtime_error( "indexed Job search JSON differs from Worker" );
        }
        std::cout << "Job index integration passed jobs=" << actualJobs.size() << " stages=" << expectedStages << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
