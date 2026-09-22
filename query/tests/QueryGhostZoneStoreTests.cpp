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

int main()
{
    namespace fs = std::filesystem;
    using namespace tracy;
    using namespace tracy::analysis;
    using namespace tracy::query;

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
