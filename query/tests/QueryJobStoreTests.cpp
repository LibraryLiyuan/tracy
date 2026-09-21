#include "TracyQueryStringStore.hpp"
#include "TracyQueryJobStore.hpp"

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

void WriteString( tracy::query::QueryStringStoreWriter& writer, uint64_t ordinal,
    uint64_t token, const std::string& value )
{
    writer.StringBegin( ordinal, token, value.size() );
    if( !value.empty() ) writer.StringBytes( ordinal, 0, value.data(), value.size() );
    writer.StringEnd( ordinal );
}

}

int main()
{
    try
    {
        namespace fs = std::filesystem;
        using namespace tracy;
        using namespace tracy::analysis;
        using namespace tracy::query;

        const auto root = fs::temp_directory_path() /
            ( "query-job-store-test-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        fs::create_directories( root );
        const std::string fingerprint( 64, 'j' );
        auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ root } );
        auto disk = std::make_shared<AnalysisDiskBudget>( AnalysisDiskBudget{ usage, 64 * 1024 * 1024, {} } );
        auto workspace = std::make_shared<AnalysisWorkspaceBudget>( 64 * 1024 * 1024, 32 * 1024 * 1024 );
        PagedRecordOptions options;
        options.blockBytes = 4096;
        options.disk = disk;
        options.workspace = workspace;

        {
            QueryStringStoreWriter strings( root / "strings", fingerprint, options );
            strings.StringsBegin( 2 );
            WriteString( strings, 0, 100, "RootJob" );
            WriteString( strings, 1, 200, "ChildJob" );
            strings.StringsEnd();
            strings.PointerTableBegin( QueryStringPointerKind::String, 2 );
            strings.PointerRecord( QueryStringPointerKind::String, 0, 100, 100, 0 );
            strings.PointerRecord( QueryStringPointerKind::String, 1, 200, 200, 0 );
            strings.PointerTableEnd( QueryStringPointerKind::String );
            for( size_t kind = 1; kind < 3; kind++ )
            {
                strings.PointerTableBegin( QueryStringPointerKind( kind ), 0 );
                strings.PointerTableEnd( QueryStringPointerKind( kind ) );
            }
            strings.Commit();
        }

        QueryJobStoreWriter writer( root / "jobs", fingerprint, options );
        JnJobTypeData types[2]{};
        types[0].name = 100; types[0].typeId = 1;
        types[1].name = 200; types[1].typeId = 2;
        writer.Begin( QueryJobDomain::Type, 2 ); writer.Append( QueryJobDomain::Type, 0, types, 2 ); writer.End( QueryJobDomain::Type );

        JnJobScheduleData schedules[2]{};
        schedules[0].time = 1; schedules[0].jobId = 1; schedules[0].packedHandle = 0x100000001; schedules[0].thread = 10;
        schedules[1].time = 2; schedules[1].jobId = 2; schedules[1].packedHandle = 0x100000002; schedules[1].thread = 11; schedules[1].dependencyCount = 1;
        writer.Begin( QueryJobDomain::Schedule, 2 ); writer.Append( QueryJobDomain::Schedule, 0, schedules, 2 ); writer.End( QueryJobDomain::Schedule );

        JnJobConfigData configs[2]{};
        configs[0].jobId = 1; configs[0].typeId = 1; configs[0].count = 1; configs[0].grainSize = 1;
        configs[1].jobId = 2; configs[1].typeId = 2; configs[1].count = 4; configs[1].grainSize = 2; configs[1].originFrameSequence = 7;
        writer.Begin( QueryJobDomain::Config, 2 ); writer.Append( QueryJobDomain::Config, 0, configs, 2 ); writer.End( QueryJobDomain::Config );

        JnJobDependencyData dependency{};
        dependency.jobId = 2; dependency.prerequisiteJobId = 1; dependency.prerequisiteHandle = schedules[0].packedHandle;
        writer.Begin( QueryJobDomain::Dependency, 1 ); writer.Append( QueryJobDomain::Dependency, 0, &dependency, 1 ); writer.End( QueryJobDomain::Dependency );

        JnJobStageData stages[9]{};
        stages[0].time = 10; stages[0].jobId = 1; stages[0].thread = 20; stages[0].spanId = 1; stages[0].stage = uint8_t( JnJobStage::WorkerSliceBegin );
        stages[1].time = 20; stages[1].jobId = 1; stages[1].thread = 20; stages[1].spanId = 1; stages[1].stage = uint8_t( JnJobStage::WorkerSliceEnd );
        stages[2].time = 21; stages[2].jobId = 1; stages[2].thread = 20; stages[2].stage = uint8_t( JnJobStage::Completed );
        stages[3].time = 22; stages[3].jobId = 2; stages[3].thread = 21; stages[3].arg0 = 3; stages[3].stage = uint8_t( JnJobStage::Ready );
        stages[4].time = 23; stages[4].jobId = 2; stages[4].thread = 21; stages[4].arg0 = 3; stages[4].stage = uint8_t( JnJobStage::QueueEnter );
        stages[5].time = 24; stages[5].jobId = 2; stages[5].thread = 21; stages[5].spanId = 2; stages[5].stage = uint8_t( JnJobStage::WorkerSliceBegin );
        stages[6].time = 30; stages[6].jobId = 2; stages[6].thread = 21; stages[6].spanId = 2; stages[6].stage = uint8_t( JnJobStage::WorkerSliceEnd );
        stages[7].time = 31; stages[7].jobId = 2; stages[7].thread = 21; stages[7].stage = uint8_t( JnJobStage::Completed );
        stages[8].time = 3; stages[8].jobId = 2; stages[8].thread = 11; stages[8].spanId = 9; stages[8].stage = uint8_t( JnJobStage::ScheduleCallsite );
        writer.Begin( QueryJobDomain::Stage, 9 ); writer.Append( QueryJobDomain::Stage, 0, stages, 9 ); writer.End( QueryJobDomain::Stage );

        JnCallsiteData callsites[2]{};
        callsites[0].thread = 11; callsites[0].callsiteId = 9; callsites[0].callstack = 111;
        callsites[0].provenance = uint8_t( JnStackProvenance::ExactSource );
        callsites[1] = callsites[0]; callsites[1].callstack = 222;
        writer.Begin( QueryJobDomain::Callsite, 2 ); writer.Append( QueryJobDomain::Callsite, 0, callsites, 2 ); writer.End( QueryJobDomain::Callsite );

        const uint64_t frameId = ( uint64_t( 1 ) << 48 ) | 7;
        JnFrameData frame{};
        frame.time = 0; frame.frameId = frameId; frame.domainIndex = 7; frame.thread = 10;
        frame.domain = uint8_t( JnFrameDomain::Present ); frame.phase = uint8_t( JnFramePhase::Begin ); frame.flags = uint8_t( JnFrameFlags::Canonical );
        writer.Begin( QueryJobDomain::Frame, 1 ); writer.Append( QueryJobDomain::Frame, 0, &frame, 1 ); writer.End( QueryJobDomain::Frame );

        JnGfxDispatchData dispatch{};
        dispatch.time = 5; dispatch.dispatchId = 50; dispatch.frameIndex = frameId; dispatch.thread = 10; dispatch.expectedJobs = 2;
        writer.Begin( QueryJobDomain::GfxDispatch, 1 ); writer.Append( QueryJobDomain::GfxDispatch, 0, &dispatch, 1 ); writer.End( QueryJobDomain::GfxDispatch );
        writer.FinishMissing();
        writer.BuildIndexes( root / "strings" );

        QueryJobStoreReader reader( root / "jobs", root / "strings", fingerprint, options );
        Require( reader.GetJobCount() == 2 && reader.GetGfxDispatchCount() == 1, "Job derived counts changed" );
        const auto jobs = reader.ScanJobs( 0, 10 );
        Require( reader.FindJobRef( 1, {} ) == jobs.front().ref, "Job ID lookup changed reference" );
        Require( !reader.FindJobRef( 999, {} ), "unknown Job ID was reported present" );
        Require( jobs.size() == 2 && jobs[0].jobId == 1 && jobs[0].name == "RootJob" && jobs[0].executionNs == 10 &&
            jobs[0].completedNs == 21 && jobs[1].jobId == 2 && jobs[1].name == "ChildJob" &&
            jobs[1].dependencies.size() == 1 && jobs[1].dependencies[0].prerequisiteJobId == 1 &&
            jobs[1].dependencyReadyLatencyNs == 1 && jobs[1].originFrameId == frameId &&
            jobs[1].scheduleCallsiteId == 9 && jobs[1].scheduleCallstack == 111,
            "Job aggregation changed" );
        const auto directed = reader.GetDirectedJobs( 2, true, {} );
        Require( directed.size() == 2 && directed[0].jobId == 1 && directed[1].jobId == 2,
            "Job dependency direction index changed" );
        const auto evidence = reader.GetEvidenceJobs( frameId, {} );
        Require( evidence.size() == 2 && evidence[0].jobId == 1 && evidence[1].jobId == 2,
            "Job Frame evidence closure changed" );
        const auto dispatches = reader.GetGfxDispatches();
        Require( dispatches.size() == 1 && dispatches[0].dispatchId == 50 && dispatches[0].frameIndex == frameId,
            "Job Gfx dispatch changed" );
        reader.ValidateAll();
        QueryJobStoreWriter orderedWriter( root / "ordered-jobs", fingerprint, options );
        JnJobScheduleData orderedSchedules[3]{};
        orderedSchedules[0].jobId = 1; orderedSchedules[0].time = 50;
        orderedSchedules[1].jobId = 3; orderedSchedules[1].time = -5;
        orderedSchedules[2].jobId = 2; orderedSchedules[2].time = -5;
        orderedWriter.Begin( QueryJobDomain::Schedule, 3 );
        orderedWriter.Append( QueryJobDomain::Schedule, 0, orderedSchedules, 3 );
        orderedWriter.End( QueryJobDomain::Schedule );
        orderedWriter.FinishMissing();
        orderedWriter.BuildIndexes( root / "strings" );
        QueryJobStoreReader orderedReader( root / "ordered-jobs", root / "strings", fingerprint, options );
        orderedReader.ValidateAll();
        const uint64_t expectedOrder[] = { 2, 3, 1 };
        for( size_t position = 0; position < 3; position++ )
        {
            const auto page = orderedReader.ScanJobsBySchedule( position, 1, {} );
            Require( page.size() == 1 && page.front().jobId == expectedOrder[position],
                "Job schedule ordering or ordinal continuation changed" );
        }
        Require( orderedReader.ScanJobsBySchedule( 3, 1, {} ).empty(), "Job schedule page exceeded end" );

        bool cancelled = false;
        auto cancelOptions = options;
        cancelOptions.cancelled = [&] { return cancelled; };
        QueryJobStoreReader cancelReader( root / "jobs", root / "strings", fingerprint, cancelOptions );
        cancelled = true;
        bool rejectedCancelledEmptyPage = false;
        try { cancelReader.ScanJobs( 0, 0 ); }
        catch( const std::runtime_error& error )
        {
            rejectedCancelledEmptyPage = std::string( error.what() ).find( "cancelled" ) != std::string::npos;
        }
        Require( rejectedCancelledEmptyPage, "cancelled Job page returned success" );
        const auto requireCancelled = [&]( const auto& operation, const char* message ) {
            bool rejected = false;
            try { operation(); }
            catch( const std::runtime_error& error )
            {
                rejected = std::string( error.what() ).find( "cancelled" ) != std::string::npos;
            }
            Require( rejected, message );
        };
        requireCancelled( [&] { cancelReader.ScanGfxDispatches( 0, 0 ); }, "cancelled Gfx page returned success" );
        requireCancelled( [&] { cancelReader.ScanCallsites( 0, 0 ); }, "cancelled Callsite page returned success" );
        requireCancelled( [&] { cancelReader.FindJobRef( 999, {} ); }, "cancelled Job identity lookup returned success" );
        bool directedChecked = false;
        try { reader.GetDirectedJobs( 999, true, [&] { directedChecked = true; throw std::runtime_error( "cancelled" ); } ); }
        catch( const std::runtime_error& ) {}
        Require( directedChecked, "missing directed Job bypassed request cancellation" );
        bool evidenceChecked = false;
        try { reader.GetEvidenceJobs( 999, [&] { evidenceChecked = true; throw std::runtime_error( "cancelled" ); } ); }
        catch( const std::runtime_error& ) {}
        Require( evidenceChecked, "empty Job evidence bypassed request cancellation" );
        cancelled = false;
        const auto resumedPage = cancelReader.ScanJobs( 1, 1 );
        Require( resumedPage.size() == 1 && resumedPage.front().jobId == 2, "Job page did not resume at its ordinal" );

        std::error_code ignored;
        fs::remove_all( root, ignored );
        std::cout << "query Job store tests passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
