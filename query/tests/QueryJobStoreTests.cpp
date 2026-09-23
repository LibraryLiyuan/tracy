#include "TracyQueryStringStore.hpp"
#include "TracyQueryJobStore.hpp"

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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

int CapacityProbe( uint64_t stageCount, uint64_t jobCount, bool inventory,
    uint64_t maximumBytes = 20ull * 1024 * 1024 * 1024 )
{
    namespace fs = std::filesystem;
    using namespace tracy;
    using namespace tracy::analysis;
    using namespace tracy::query;
    if( stageCount < 100000 || stageCount > 10000000 || maximumBytes == 0 ||
        maximumBytes > 20ull * 1024 * 1024 * 1024 ||
        jobCount == 0 || jobCount > std::min<uint64_t>( stageCount, 1000000 ) ) return 2;
    const auto root = fs::temp_directory_path() /
        ( "query-job-capacity-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        fs::create_directories( root );
        const std::string fingerprint( 64, 'j' );
        auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ root } );
        PagedRecordOptions options;
        options.disk = std::make_shared<AnalysisDiskBudget>(
            AnalysisDiskBudget{ usage, maximumBytes, {} } );
        options.workspace = std::make_shared<AnalysisWorkspaceBudget>(
            4ull * 1024 * 1024 * 1024, 2ull * 1024 * 1024 * 1024 );
        const auto started = std::chrono::steady_clock::now();
        options.cancelled = [started] {
            return std::chrono::steady_clock::now() - started > std::chrono::minutes( 10 );
        };
        {
            QueryStringStoreWriter strings( root / "strings", fingerprint, options );
            strings.StringsBegin( 1 );
            WriteString( strings, 0, 100, "Job" );
            strings.StringsEnd();
            strings.PointerTableBegin( QueryStringPointerKind::String, 1 );
            strings.PointerRecord( QueryStringPointerKind::String, 0, 100, 100, 0 );
            strings.PointerTableEnd( QueryStringPointerKind::String );
            for( size_t kind = 1; kind < 3; ++kind )
            {
                strings.PointerTableBegin( QueryStringPointerKind( kind ), 0 );
                strings.PointerTableEnd( QueryStringPointerKind( kind ) );
            }
            strings.Commit();
        }
        {
        const auto jobs = jobCount;
        QueryJobStoreWriter writer( root / "jobs", fingerprint, options );
        JnJobTypeData type{}; type.typeId = 1; type.name = 100;
        writer.Begin( QueryJobDomain::Type, 1 );
        writer.Append( QueryJobDomain::Type, 0, &type, 1 );
        writer.End( QueryJobDomain::Type );
        AnalysisWorkspaceReservation scheduleMemory( options.workspace, jobs * sizeof( JnJobScheduleData ) );
        std::vector<JnJobScheduleData> schedules( static_cast<size_t>( jobs ) );
        for( uint64_t i = 0; i < jobs; ++i )
        {
            schedules[size_t( i )].jobId = i + 1;
            schedules[size_t( i )].time = int64_t( i );
        }
        writer.Begin( QueryJobDomain::Schedule, jobs );
        writer.Append( QueryJobDomain::Schedule, 0, schedules.data(), jobs );
        writer.End( QueryJobDomain::Schedule );
        writer.Begin( QueryJobDomain::Stage, stageCount );
        std::array<JnJobStageData, 4096> block{};
        for( uint64_t offset = 0; offset < stageCount; )
        {
            const auto count = std::min<uint64_t>( block.size(), stageCount - offset );
            for( uint64_t index = 0; index < count; ++index )
            {
                auto& row = block[size_t( index )];
                row.jobId = ( offset + index ) % jobs + 1;
                row.time = int64_t( offset + index );
                row.stage = uint8_t( JnJobStage::Ready );
            }
            writer.Append( QueryJobDomain::Stage, offset, block.data(), count );
            offset += count;
        }
        writer.End( QueryJobDomain::Stage );
        writer.FinishMissing();
        const auto sourceBytes = usage->Snapshot().currentBytes;
        std::vector<std::string> inventoryRows;
        uint64_t nextSample = sourceBytes + 8 * 1024 * 1024;
        if( inventory )
        {
            options.disk->checkSpace = [&] {
                const auto current = usage->Current();
                if( current < nextSample ) return;
                nextSample = current + 8 * 1024 * 1024;
                const auto scratch = root / "jobs" / "build-scratch";
                uint64_t raw = 0, source = 0, sorted = 0, runs = 0, other = 0;
                if( fs::exists( scratch ) )
                    for( const auto& item : fs::directory_iterator( scratch ) )
                    {
                        if( !item.is_regular_file() ) continue;
                        const auto bytes = item.file_size();
                        const auto name = item.path().filename().string();
                        if( name.ends_with( ".raw" ) ) raw += bytes;
                        else if( name == "jobs-source.work" ) source += bytes;
                        else if( name == "jobs-sorted.work" ) sorted += bytes;
                        else if( name.starts_with( "jobs-run-" ) ) runs += bytes;
                        else other += bytes;
                    }
                std::ostringstream line;
                line << "job_inventory ledger=" << current << " raw=" << raw
                    << " source=" << source << " sorted=" << sorted
                    << " runs=" << runs << " other=" << other;
                inventoryRows.emplace_back( line.str() );
            };
        }
        const auto buildStarted = std::chrono::steady_clock::now();
        const auto statistics = writer.BuildIndexes( root / "strings" );
        const auto buildMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - buildStarted ).count();
        QueryJobStoreReader reader( root / "jobs", root / "strings", fingerprint, options );
        Require( reader.GetJobCount() == jobs && !fs::exists( root / "jobs" / "build-scratch" ),
            "Job capacity probe did not publish and clean its index" );
        for( const auto jobId : { uint64_t( 1 ), jobs / 2, jobs } )
        {
            const auto page = reader.ScanJobs( size_t( jobId - 1 ), 1 );
            Require( page.size() == 1 && page.front().jobId == jobId &&
                page.front().scheduleNs == int64_t( jobId - 1 ) &&
                page.front().readyNs == int64_t( jobId - 1 ) &&
                !page.front().stages.empty(),
                "Job capacity probe changed sorted Job identity or Stage aggregation" );
        }
        if( stageCount <= 1000000 ) reader.ValidateAll();
        const auto observed = usage->Snapshot();
        std::cout << "job_stage_records=" << stageCount << " jobs=" << jobs << " source_bytes=" << sourceBytes
            << " peak_bytes=" << observed.peakBytes << " final_bytes=" << observed.currentBytes
            << " build_elapsed_ms=" << buildMilliseconds
            << " mapped_fact_reads=" << statistics.aggregationMappedFactReads << '\n';
        for( const auto& line : inventoryRows ) std::cout << line << '\n';
        }
        std::error_code cleanup;
        fs::remove_all( root, cleanup );
        if( cleanup ) throw std::runtime_error( "Job capacity probe cleanup: " + cleanup.message() );
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all( root, ignored );
        return 1;
    }
}

}

int main( int argc, char** argv )
{
    if( argc == 3 && std::string( argv[1] ) == "--capacity" )
        return CapacityProbe( std::stoull( argv[2] ), 1000, false );
    if( argc == 4 && std::string( argv[1] ) == "--capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), false );
    if( argc == 5 && std::string( argv[1] ) == "--capacity" )
        return CapacityProbe( std::stoull( argv[2] ), std::stoull( argv[3] ), false, std::stoull( argv[4] ) );
    if( argc == 3 && std::string( argv[1] ) == "--capacity-inventory" )
        return CapacityProbe( std::stoull( argv[2] ), 1000, true );
    if( argc != 1 ) return 2;
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
        const auto buildStatistics = writer.BuildIndexes( root / "strings" );
        Require( buildStatistics.aggregationCompressedFactPageLoads == 0,
            "Job aggregation reloaded compressed fact pages" );
        Require( buildStatistics.aggregationMappedFactReads == 14,
            "Job aggregation mapped fact count changed" );

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

        QueryJobStoreWriter shuffledWriter( root / "shuffled-jobs", fingerprint, options );
        constexpr size_t ShuffledJobs = 120;
        constexpr size_t StagesPerJob = 1000;
        std::vector<JnJobScheduleData> shuffledSchedules( ShuffledJobs );
        for( size_t job = 0; job < ShuffledJobs; job++ )
        {
            shuffledSchedules[job].jobId = job + 1;
            shuffledSchedules[job].time = int64_t( job );
        }
        shuffledWriter.Begin( QueryJobDomain::Schedule, shuffledSchedules.size() );
        shuffledWriter.Append( QueryJobDomain::Schedule, 0, shuffledSchedules.data(), shuffledSchedules.size() );
        shuffledWriter.End( QueryJobDomain::Schedule );
        std::vector<JnJobStageData> shuffledStages( ShuffledJobs * StagesPerJob );
        for( size_t index = 0; index < shuffledStages.size(); index++ )
        {
            shuffledStages[index].jobId = index % ShuffledJobs + 1;
            shuffledStages[index].time = int64_t( index );
            shuffledStages[index].stage = uint8_t( JnJobStage::Ready );
        }
        shuffledWriter.Begin( QueryJobDomain::Stage, shuffledStages.size() );
        shuffledWriter.Append( QueryJobDomain::Stage, 0, shuffledStages.data(), shuffledStages.size() );
        shuffledWriter.End( QueryJobDomain::Stage );
        shuffledWriter.FinishMissing();
        const auto shuffledBuild = shuffledWriter.BuildIndexes( root / "strings" );
        Require( shuffledBuild.aggregationCompressedFactPageLoads == 0,
            "jobId order thrashed compressed fact pages" );
        Require( shuffledBuild.aggregationMappedFactReads == ShuffledJobs * ( StagesPerJob + 1 ),
            "shuffled Job aggregation did not consume every mapped fact" );

        QueryJobStoreWriter dependencyWriter( root / "dependency-jobs", fingerprint, options );
        constexpr size_t DependencyJobs = 2048;
        std::vector<JnJobScheduleData> dependencySchedules( DependencyJobs );
        std::vector<JnJobConfigData> dependencyConfigs( DependencyJobs );
        std::vector<JnJobDependencyData> dependencyEdges( DependencyJobs );
        std::vector<JnJobStageData> dependencyStages( DependencyJobs * 2 );
        for( size_t index = 0; index < DependencyJobs; index++ )
        {
            const auto jobId = uint64_t( index + 1 );
            const auto prerequisite = index + 1 == DependencyJobs ? uint64_t( DependencyJobs + 99 ) :
                uint64_t( ( index * 40503 ) % DependencyJobs + 1 );
            dependencySchedules[index].jobId = jobId;
            dependencySchedules[index].time = int64_t( jobId * 10 );
            dependencyConfigs[index].jobId = jobId;
            dependencyConfigs[index].typeId = uint32_t( index % 2 + 1 );
            dependencyEdges[index].jobId = jobId;
            dependencyEdges[index].prerequisiteJobId = prerequisite;
            dependencyStages[index * 2].jobId = jobId;
            dependencyStages[index * 2].time = int64_t( jobId * 10 + 5 );
            dependencyStages[index * 2].stage = uint8_t( JnJobStage::Completed );
            dependencyStages[index * 2 + 1].jobId = jobId;
            dependencyStages[index * 2 + 1].time = 1000000 + int64_t( jobId );
            dependencyStages[index * 2 + 1].stage = uint8_t( JnJobStage::Ready );
        }
        dependencyWriter.Begin( QueryJobDomain::Type, 2 );
        dependencyWriter.Append( QueryJobDomain::Type, 0, types, 2 );
        dependencyWriter.End( QueryJobDomain::Type );
        dependencyWriter.Begin( QueryJobDomain::Schedule, dependencySchedules.size() );
        dependencyWriter.Append( QueryJobDomain::Schedule, 0, dependencySchedules.data(), dependencySchedules.size() );
        dependencyWriter.End( QueryJobDomain::Schedule );
        dependencyWriter.Begin( QueryJobDomain::Config, dependencyConfigs.size() );
        dependencyWriter.Append( QueryJobDomain::Config, 0, dependencyConfigs.data(), dependencyConfigs.size() );
        dependencyWriter.End( QueryJobDomain::Config );
        dependencyWriter.Begin( QueryJobDomain::Dependency, dependencyEdges.size() );
        dependencyWriter.Append( QueryJobDomain::Dependency, 0, dependencyEdges.data(), dependencyEdges.size() );
        dependencyWriter.End( QueryJobDomain::Dependency );
        dependencyWriter.Begin( QueryJobDomain::Stage, dependencyStages.size() );
        dependencyWriter.Append( QueryJobDomain::Stage, 0, dependencyStages.data(), dependencyStages.size() );
        dependencyWriter.End( QueryJobDomain::Stage );
        dependencyWriter.FinishMissing();
        const auto dependencyBuild = dependencyWriter.BuildIndexes( root / "strings" );
        Require( dependencyBuild.completionLookupBlockLoads == 0,
            "Dependency aggregation repeated compressed completion lookup blocks" );
        Require( dependencyBuild.jobNameRequests == DependencyJobs && dependencyBuild.jobNameResolutions == 2 &&
            dependencyBuild.jobNameCacheHits == DependencyJobs - 2,
            "Job names were resolved more than once per unique type" );
        QueryJobStoreReader dependencyReader( root / "dependency-jobs", root / "strings", fingerprint, options );
        const auto firstDependencyJob = dependencyReader.ScanJobs( 0, 1 );
        if( firstDependencyJob.size() != 1 || firstDependencyJob[0].name != "RootJob" ||
            firstDependencyJob[0].dependencyReadyLatencyNs != 999986 )
            throw std::runtime_error( "Dependency completion/name result changed: name=" +
                ( firstDependencyJob.empty() ? std::string( "<missing>" ) : firstDependencyJob[0].name ) +
                " latency=" + ( firstDependencyJob.empty() || !firstDependencyJob[0].dependencyReadyLatencyNs ?
                    std::string( "<missing>" ) : std::to_string( *firstDependencyJob[0].dependencyReadyLatencyNs ) ) );
        const auto missingDependencyJob = dependencyReader.ScanJobs( DependencyJobs - 1, 1 );
        Require( missingDependencyJob.size() == 1 && !missingDependencyJob[0].dependencyReadyLatencyNs,
            "missing prerequisite completion was reported as available" );
        Require( !fs::exists( root / "dependency-jobs" / "build-scratch" ) &&
            !fs::exists( root / "shuffled-jobs" / "build-scratch" ),
            "Job build scratch survived successful aggregation" );

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

        QueryJobStoreWriter oversizedWriter( root / "oversized-jobs", fingerprint, options );
        JnJobScheduleData oversizedSchedule{};
        oversizedSchedule.jobId = 77;
        oversizedWriter.Begin( QueryJobDomain::Schedule, 1 );
        oversizedWriter.Append( QueryJobDomain::Schedule, 0, &oversizedSchedule, 1 );
        oversizedWriter.End( QueryJobDomain::Schedule );
        std::vector<JnJobStageData> oversizedStages( 32769 );
        for( size_t index = 0; index < oversizedStages.size(); index++ )
        {
            oversizedStages[index].jobId = 77;
            oversizedStages[index].time = int64_t( index );
            oversizedStages[index].stage = uint8_t( JnJobStage::Ready );
        }
        oversizedWriter.Begin( QueryJobDomain::Stage, oversizedStages.size() );
        oversizedWriter.Append( QueryJobDomain::Stage, 0, oversizedStages.data(), oversizedStages.size() );
        oversizedWriter.End( QueryJobDomain::Stage );
        oversizedWriter.FinishMissing();
        oversizedWriter.BuildIndexes( root / "strings" );
        QueryJobStoreReader oversizedReader( root / "oversized-jobs", root / "strings", fingerprint, options );
        bool oversizedRejected = false;
        try { (void)oversizedReader.ScanJobs( 0, 1 ); }
        catch( const std::runtime_error& error )
        {
            oversizedRejected = std::string( error.what() ).find( "workspace_budget" ) != std::string::npos;
        }
        Require( oversizedRejected, "single Job detail exceeded its bounded collection limit" );

        Require( usage->Snapshot().currentBytes == AnalysisDiskUsage::Bytes( root ),
            "Job scratch cleanup did not release the disk ledger" );
        {
            const auto limitedRoot = root / "limited-jobs";
            auto limitedUsage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ limitedRoot } );
            auto limitedDisk = std::make_shared<AnalysisDiskBudget>( AnalysisDiskBudget{ limitedUsage, 2048, {} } );
            auto limitedOptions = options;
            limitedOptions.disk = limitedDisk;
            QueryJobStoreWriter limitedWriter( limitedRoot, fingerprint, limitedOptions );
            std::vector<JnJobScheduleData> limitedSchedules( 100 );
            for( size_t index = 0; index < limitedSchedules.size(); index++ ) limitedSchedules[index].jobId = index + 1;
            limitedWriter.Begin( QueryJobDomain::Schedule, limitedSchedules.size() );
            bool limitedRejected = false;
            try { limitedWriter.Append( QueryJobDomain::Schedule, 0, limitedSchedules.data(), limitedSchedules.size() ); }
            catch( const std::runtime_error& error )
            {
                limitedRejected = std::string( error.what() ).find( "disk_budget" ) != std::string::npos;
            }
            Require( limitedRejected && limitedUsage->Snapshot().rejectedGrowths == 1,
                "temporary flat Job facts bypassed the disk budget" );
        }

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
