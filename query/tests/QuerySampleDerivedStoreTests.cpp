#include "TracyQueryCallstackStore.hpp"
#include "TracyQuerySampleStore.hpp"
#include "TracyQuerySampleView.hpp"

#if __has_include( "TracyQuerySampleDerivedStore.hpp" )
#  include "TracyQuerySampleDerivedStore.hpp"
#  define TRACY_HAS_QUERY_SAMPLE_DERIVED_STORE 1
#else
#  define TRACY_HAS_QUERY_SAMPLE_DERIVED_STORE 0
#endif

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

}

int main( int argc, char** argv )
{
    try
    {
#if !TRACY_HAS_QUERY_SAMPLE_DERIVED_STORE
        throw std::runtime_error( "Query sample derived store is not implemented" );
#else
        namespace fs = std::filesystem;
        using namespace tracy;
        using namespace tracy::analysis;
        using namespace tracy::query;

        if( argc == 8 && std::string( argv[1] ) == "--replay" )
        {
            QuerySampleDerivedBuildStatistics stats;
            const auto maximum = std::stoull( argv[7] );
            if( maximum == 0 || maximum > 1000000 || fs::exists( argv[5] ) ) return 2;
            PagedRecordOptions options;
            options.workspace = std::make_shared<AnalysisWorkspaceBudget>( 256ull * 1024 * 1024, 128ull * 1024 * 1024 );
            auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ argv[5] } );
            options.disk = std::make_shared<AnalysisDiskBudget>( AnalysisDiskBudget{ usage, 256ull * 1024 * 1024, {} } );
            options.cancelled = [&] { return stats.callstackRequests >= maximum; };
            uint64_t contributionBytes = 0;
            options.beforeFileGrowth = [&]( const fs::path& file, uint64_t bytes ) {
                if( file.filename() == "contributions.work" ) contributionBytes = bytes;
            };
            const auto start = std::chrono::steady_clock::now();
            bool boundedStop = false;
            try { BuildQuerySampleDerivedStore( argv[5], argv[2], argv[3], argv[4], argv[6], options, &stats ); }
            catch( const std::exception& e ) {
                if( stats.callstackRequests < maximum ) { std::cerr << e.what() << '\n'; return 1; }
                boundedStop = true;
            }
            std::cout << "bounded_stop=" << boundedStop << " requests=" << stats.callstackRequests
                << " resolutions=" << stats.callstackResolutions << " hits=" << stats.callstackCacheHits
                << " contribution_bytes=" << contributionBytes << " elapsed_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start ).count() << '\n';
            return 0;
        }
        if( argc != 1 ) return 2;

        const auto root = fs::temp_directory_path() /
            ( "query-sample-derived-store-test-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        fs::create_directories( root );
        const std::string fingerprint( 64, 'd' );
        auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ root } );
        auto disk = std::make_shared<AnalysisDiskBudget>( AnalysisDiskBudget{ usage, 64 * 1024 * 1024, {} } );
        auto workspace = std::make_shared<AnalysisWorkspaceBudget>( 64 * 1024 * 1024, 32 * 1024 * 1024 );
        PagedRecordOptions options;
        options.blockBytes = 4096;
        options.disk = disk;
        options.workspace = workspace;

        CallstackFrameId leafId{}; leafId.data = 0x1000;
        CallstackFrameId rootId{}; rootId.data = 0x2000;
        CallstackFrameId unresolvedId{}; unresolvedId.data = 0x3000;

        {
            QueryCallstackFrameStoreWriter frames( root / "frames", fingerprint, options );
            frames.Begin( 2 );
            CallstackFrame leaf[2]{};
            leaf[0].symAddr = 0x1100;
            leaf[1].symAddr = 0x1200;
            CallstackFrame parent{};
            parent.symAddr = 0x2000;
            frames.FrameBegin( 0, leafId, 2, StringIdx{} );
            frames.FrameRecords( 0, 0, leaf, 2 );
            frames.FrameEnd( 0 );
            frames.FrameBegin( 1, rootId, 1, StringIdx{} );
            frames.FrameRecords( 1, 0, &parent, 1 );
            frames.FrameEnd( 1 );
            frames.Commit();
        }
        {
            QueryCallstackStoreWriter callstacks( root / "callstacks", fingerprint, options );
            callstacks.Begin( 2 );
            const CallstackFrameId resolved[] = { leafId, rootId };
            callstacks.CallstackBegin( 1, 2 );
            callstacks.CallstackFrames( 1, 0, resolved, 2 );
            callstacks.CallstackEnd( 1 );
            callstacks.CallstackBegin( 2, 1 );
            callstacks.CallstackFrames( 2, 0, &unresolvedId, 1 );
            callstacks.CallstackEnd( 2 );
            callstacks.Commit();
        }
        {
            QuerySampleStoreWriter samples( root / "samples", fingerprint, options );
            samples.Begin( 7, QuerySampleKind::Main, 3 );
            samples.Append( 0, 10, 1 );
            samples.Append( 1, 20, 1 );
            samples.Append( 2, 30, 2 );
            samples.End();
            samples.Commit();
        }
        BuildQuerySampleView( root / "samples", fingerprint,
            []( uint64_t thread ) { return thread == 7 ? std::vector<int64_t>{ 20 } : std::vector<int64_t>{}; }, options );

        auto derivedWorkspace = std::make_shared<AnalysisWorkspaceBudget>( 16 * 1024 * 1024, 8 * 1024 * 1024 );
        auto derivedOptions = options;
        derivedOptions.workspace = derivedWorkspace;
        uint64_t contributionBytes = 0;
        derivedOptions.beforeFileGrowth = [&]( const fs::path& file, uint64_t bytes ) {
            if( file.filename() == "contributions.work" ) contributionBytes = bytes;
        };
        uint64_t count = 0;
        QuerySampleDerivedBuildStatistics buildStatistics;
        try
        {
            count = BuildQuerySampleDerivedStore( root / "derived", root / "samples", root / "callstacks",
                root / "frames", fingerprint, derivedOptions, &buildStatistics );
        }
        catch( const std::exception& error )
        {
            const auto snapshot = derivedWorkspace->Snapshot();
            throw std::runtime_error( std::string( error.what() ) + " current=" + std::to_string( snapshot.currentBytes ) +
                " peak=" + std::to_string( snapshot.peakBytes ) + " rejected=" + std::to_string( snapshot.rejectedReservations ) );
        }
        Require( derivedWorkspace->Snapshot().peakBytes <= 16 * 1024 * 1024,
            "sample derived build exceeded its workspace limit" );
        Require( buildStatistics.callstackRequests > buildStatistics.callstackResolutions &&
            buildStatistics.callstackCacheHits != 0,
            "sample derived statistics repeated callstack expansion" );
        Require( count == 3, "sample symbol-statistics record count changed" );
        Require( contributionBytes == 7 * 32, "repeated symbol contributions must be coalesced before disk writes" );
        Require( !fs::exists( root / "derived" / "build-scratch" ), "sample derived scratch was not removed" );

        QuerySampleDerivedStoreReader reader( root / "derived", fingerprint, options );
        const auto leafStats = reader.Find( 0x1100 );
        const auto inlineStats = reader.Find( 0x1200 );
        const auto rootStats = reader.Find( 0x2000 );
        Require( leafStats && leafStats->inclusiveSamples == 0 && leafStats->exclusiveSamples == 1 && leafStats->childSamples == 0,
            "exclusive sample statistics changed" );
        Require( inlineStats && inlineStats->inclusiveSamples == 1 && inlineStats->exclusiveSamples == 0,
            "inline inclusive sample statistics changed" );
        Require( rootStats && rootStats->inclusiveSamples == 1 && rootStats->exclusiveSamples == 0 && rootStats->childSamples == 2,
            "parent symbol statistics or raw child-sample count changed" );
        Require( !reader.Find( 0x3000 ), "missing Frame definition produced symbol statistics" );
        reader.ValidateAll();

        // More unique child addresses than the grouping cap must spill without losing weights.
        {
            constexpr uint32_t childCount = 20000;
            std::vector<CallstackFrameId> stack( childCount + 1 );
            for( uint32_t i = 0; i <= childCount; ++i ) stack[i].data = 0x100000 + i;
            QueryCallstackStoreWriter stacks( root / "spill-stacks", fingerprint, options );
            stacks.Begin( 1 ); stacks.CallstackBegin( 1, uint32_t( stack.size() ) );
            stacks.CallstackFrames( 1, 0, stack.data(), uint32_t( stack.size() ) );
            stacks.CallstackEnd( 1 ); stacks.Commit();
            QuerySampleStoreWriter samples( root / "spill-samples", fingerprint, options );
            samples.Begin( 7, QuerySampleKind::Main, 2 );
            samples.Append( 0, 10, 1 ); samples.Append( 1, 20, 1 ); samples.End(); samples.Commit();
            BuildQuerySampleView( root / "spill-samples", fingerprint,
                []( uint64_t ) { return std::vector<int64_t>{}; }, options );
            const auto records = BuildQuerySampleDerivedStore( root / "spill-derived", root / "spill-samples",
                root / "spill-stacks", root / "frames", fingerprint, derivedOptions );
            Require( records == childCount, "group spill lost child addresses" );
            QuerySampleDerivedStoreReader spilled( root / "spill-derived", fingerprint, options );
            for( uint32_t i = 1; i <= childCount; ++i ) {
                const auto row = spilled.Find( 0x100000 + i );
                Require( row && row->childSamples == 2 && row->inclusiveSamples == 0 && row->exclusiveSamples == 0,
                    "group spill changed exact child weights" );
            }
            spilled.ValidateAll();
        }

        std::error_code ignored;
        fs::remove_all( root, ignored );
        std::cout << "query sample derived store tests passed\n";
        return 0;
#endif
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
