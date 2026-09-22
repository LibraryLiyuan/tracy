#include "TracyQueryGhostZoneStore.hpp"

#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"
#include "TracyQueryCallstackStore.hpp"
#include "TracyQuerySampleStore.hpp"
#include "TracyQueryStringStore.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

void WriteString( tracy::query::QueryStringStoreWriter& writer, uint64_t ordinal, const std::string& value )
{
    writer.StringBegin( ordinal, ordinal, value.size() );
    if( !value.empty() ) writer.StringBytes( ordinal, 0, value.data(), value.size() );
    writer.StringEnd( ordinal );
}

}

int main( int argc, char** argv )
{
    namespace fs = std::filesystem;
    using namespace tracy;
    using namespace tracy::analysis;
    using namespace tracy::query;

    // Bounded stage replay: reads existing sidecars, writes only a new output directory.
    // A one-nanosecond period is deliberate: this measures storage work, not acceptance semantics.
    if( argc == 9 && std::string( argv[1] ) == "--replay" )
    {
        QueryGhostZoneBuildStatistics stats;
        const auto maximum = std::stoull( argv[8] );
        if( maximum == 0 || maximum > 1000000 || fs::exists( argv[6] ) ) return 2;
        PagedRecordOptions options;
        options.workspace = std::make_shared<AnalysisWorkspaceBudget>( 256ull * 1024 * 1024, 128ull * 1024 * 1024 );
        auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ argv[6] } );
        options.disk = std::make_shared<AnalysisDiskBudget>( AnalysisDiskBudget{ usage, 256ull * 1024 * 1024, {} } );
        options.cancelled = [&] { return stats.pathRequests >= maximum; };
        const auto start = std::chrono::steady_clock::now();
        bool boundedStop = false;
        try { BuildQueryGhostZoneStore( argv[6], argv[2], argv[3], argv[4], argv[5], argv[7], 1, options, &stats ); }
        catch( const std::exception& e )
        {
            if( stats.pathRequests < maximum ) { std::cerr << e.what() << '\n'; return 1; }
            boundedStop = true;
        }
        std::cout << "bounded_stop=" << boundedStop << " requests=" << stats.pathRequests
            << " resolutions=" << stats.pathResolutions << " hits=" << stats.pathCacheHits
            << " scratch_writes=" << stats.scratchRecordWrites
            << " string_blocks=" << stats.stringBlockLoads << " frame_blocks=" << stats.frameBlockLoads
            << " callstack_blocks=" << stats.callstackBlockLoads
            << " decoded_bytes=" << stats.stringDecodedBytes + stats.frameDecodedBytes + stats.callstackDecodedBytes
            << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start ).count() << '\n';
        return 0;
    }
    if( argc != 1 ) return 2;

    const auto root = fs::temp_directory_path() /
        ( "query-ghost-zone-store-test-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    fs::create_directories( root );
    const std::string fingerprint( 64, 'g' );
    auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ root } );
    auto disk = std::make_shared<AnalysisDiskBudget>( AnalysisDiskBudget{ usage, 64 * 1024 * 1024, {} } );
    auto workspace = std::make_shared<AnalysisWorkspaceBudget>( 64 * 1024 * 1024, 32 * 1024 * 1024 );
    PagedRecordOptions options;
    options.blockBytes = 4096;
    options.disk = disk;
    options.workspace = workspace;

    try
    {
        QueryStringStoreWriter strings( root / "strings", fingerprint, options );
        strings.StringsBegin( 3 );
        WriteString( strings, 0, "root" );
        WriteString( strings, 1, "sample.cpp" );
        WriteString( strings, 2, "leaf" );
        strings.StringsEnd();
        for( size_t kind = 0; kind < 3; kind++ )
        {
            strings.PointerTableBegin( QueryStringPointerKind( kind ), 0 );
            strings.PointerTableEnd( QueryStringPointerKind( kind ) );
        }
        strings.Commit();

        CallstackFrameId leafId{};
        leafId.data = 0x2000;
        CallstackFrameId rootId{};
        rootId.data = 0x1000;
        QueryCallstackFrameStoreWriter frameWriter( root / "frames", fingerprint, options );
        frameWriter.Begin( 2 );
        CallstackFrame leaf{};
        leaf.name = StringIdx( 2 ); leaf.file = StringIdx( 1 ); leaf.line = 20; leaf.symAddr = 0x2200;
        CallstackFrame rootFrame{};
        rootFrame.name = StringIdx( 0 ); rootFrame.file = StringIdx( 1 ); rootFrame.line = 10; rootFrame.symAddr = 0x1100;
        frameWriter.FrameBegin( 0, leafId, 1, StringIdx{} );
        frameWriter.FrameRecords( 0, 0, &leaf, 1 );
        frameWriter.FrameEnd( 0 );
        frameWriter.FrameBegin( 1, rootId, 1, StringIdx{} );
        frameWriter.FrameRecords( 1, 0, &rootFrame, 1 );
        frameWriter.FrameEnd( 1 );
        frameWriter.Commit();

        QueryCallstackStoreWriter callstacks( root / "callstacks", fingerprint, options );
        callstacks.Begin( 2 );
        const CallstackFrameId deep[] = { leafId, rootId };
        callstacks.CallstackBegin( 1, 2 );
        callstacks.CallstackFrames( 1, 0, deep, 2 );
        callstacks.CallstackEnd( 1 );
        callstacks.CallstackBegin( 2, 1 );
        callstacks.CallstackFrames( 2, 0, &rootId, 1 );
        callstacks.CallstackEnd( 2 );
        callstacks.Commit();

        QuerySampleStoreWriter samples( root / "samples", fingerprint, options );
        samples.Begin( 77, QuerySampleKind::Main, 4 );
        samples.Append( 0, 0, 1 );
        samples.Append( 1, 5, 1 );
        samples.Append( 2, 10, 1 );
        samples.Append( 3, 20, 2 );
        samples.End();
        samples.Commit();

        QueryGhostZoneBuildStatistics buildStatistics;
        const auto count = BuildQueryGhostZoneStore( root / "ghost", root / "samples", root / "callstacks",
            root / "frames", root / "strings", fingerprint, 5, options, &buildStatistics );
        Require( buildStatistics.pathRequests == 3 && buildStatistics.pathResolutions == 2 &&
            buildStatistics.pathCacheHits == 1,
            "Ghost Zone repeated callstack paths were not reused" );
        Require( buildStatistics.scratchRecordReads == 0 && buildStatistics.scratchRecordWrites == count,
            "active Ghost Zones must stay in memory and be written to scratch once" );
        if( count != 2 ) throw std::runtime_error( "streamed Ghost Zone count changed: " + std::to_string( count ) );
        Require( !fs::exists( root / "ghost" / "build-scratch.bin" ), "Ghost Zone scratch was not removed" );

        QueryGhostZoneStoreReader reader( root / "ghost", fingerprint, options );
        const auto rows = reader.Scan( 0, 30, 0, 10 );
        Require( rows.size() == 2, "Ghost Zone page size changed" );
        Require( rows[0].thread == 77 && !rows[0].parent && rows[0].startNs == 5 && rows[0].endNs == 25 &&
            rows[0].name == "root" && rows[0].file == "sample.cpp" && rows[0].line == 10 &&
            rows[0].address == "0x1000" && rows[0].depth == 0 && rows[0].childCount == 1 && !rows[0].inlineFrame,
            "root Ghost Zone changed" );
        Require( rows[1].thread == 77 && rows[1].parent == 0 && rows[1].startNs == 5 && rows[1].endNs == 15 &&
            rows[1].name == "leaf" && rows[1].file == "sample.cpp" && rows[1].line == 20 &&
            rows[1].address == "0x2000" && rows[1].depth == 1 && rows[1].childCount == 0 && !rows[1].inlineFrame,
            "child Ghost Zone changed" );
        reader.ValidateAll();

        // A long unchanged stack must extend two zones, not rewrite them per sample.
        constexpr uint64_t repeatedCount = 100000;
        QuerySampleStoreWriter repeated( root / "repeated-samples", fingerprint, options );
        repeated.Begin( 77, QuerySampleKind::Main, repeatedCount );
        for( uint64_t i = 0; i < repeatedCount; i++ ) repeated.Append( i, 5 + i * 5, 1 );
        repeated.End();
        repeated.Commit();
        QueryGhostZoneBuildStatistics repeatedStatistics;
        const auto repeatedZones = BuildQueryGhostZoneStore( root / "repeated-ghost", root / "repeated-samples",
            root / "callstacks", root / "frames", root / "strings", fingerprint, 5, options, &repeatedStatistics );
        Require( repeatedZones == 2 && repeatedStatistics.scratchRecordReads == 0 &&
            repeatedStatistics.scratchRecordWrites == 2, "repeated samples amplified scratch I/O" );
        QueryGhostZoneStoreReader repeatedReader( root / "repeated-ghost", fingerprint, options );
        const auto repeatedRows = repeatedReader.Scan( 0, 1000000, 0, 10 );
        Require( repeatedRows.size() == 2 && repeatedRows[0].endNs == 500005 &&
            repeatedRows[1].endNs == 500005, "repeated samples changed zone endpoints" );

        QueryCallstackFrameStoreWriter unresolvedFrames( root / "unresolved-frames", fingerprint, options );
        unresolvedFrames.Begin( 0 );
        unresolvedFrames.Commit();
        const auto unresolvedCount = BuildQueryGhostZoneStore( root / "unresolved-ghost", root / "samples",
            root / "callstacks", root / "unresolved-frames", root / "strings", fingerprint, 5, options );
        Require( unresolvedCount == 5,
            "missing Frame definitions must prevent Ghost Zone reuse across samples" );
        QueryGhostZoneStoreReader unresolvedReader( root / "unresolved-ghost", fingerprint, options );
        const auto unresolvedRows = unresolvedReader.Scan( 0, 30, 0, 10 );
        Require( unresolvedRows.size() == 5 && !unresolvedRows[0].parent && unresolvedRows[1].parent == 0 &&
            !unresolvedRows[2].parent && unresolvedRows[3].parent == 2 && !unresolvedRows[4].parent,
            "missing Frame definitions changed Ghost Zone tree boundaries" );
        unresolvedReader.ValidateAll();
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all( root, ignored );
        return 1;
    }

    std::error_code ignored;
    fs::remove_all( root, ignored );
    std::cout << "query Ghost Zone store tests passed\n";
    return 0;
}
