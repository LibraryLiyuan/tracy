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
#include <unordered_map>

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

    if( argc == 7 && std::string( argv[1] ) == "--capacity" )
    {
        try {
            PagedRecordOptions options;
            options.workspace = std::make_shared<AnalysisWorkspaceBudget>( 2ull << 30, 2ull << 30 );
            QuerySampleStoreReader samples( argv[2], argv[6], options );
            options.readerCacheBytes = 1ull << 30;
            QueryCallstackStoreReader stacks( argv[3], argv[6], options );
            QueryCallstackFrameStoreReader frames( argv[4], argv[6], options );
            QueryStringStoreReader strings( argv[5], argv[6], options );
            struct Node { uint64_t key, bytes; bool reusable; };
            constexpr uint64_t cacheBytes = 16ull << 20;
            AnalysisWorkspaceReservation cacheMemory( options.workspace, 2 * cacheBytes + ( 4ull << 20 ) );
            std::unordered_map<uint64_t, std::vector<Node>> definitions;
            std::unordered_map<uint32_t, std::vector<Node>> paths;
            uint64_t definitionBytes = 0, pathBytes = 0;
            auto spans = samples.MainSpans();
            std::sort( spans.begin(), spans.end(), []( const auto& a, const auto& b ) { return a.thread < b.thread; } );
            uint64_t visited = 0, ghosts = 0, stringBytes = 0, total = 0;
            for( const auto& span : spans ) total += span.count;
            const auto started = std::chrono::steady_clock::now();
            const auto report = [&]( bool complete ) {
                std::cout << "capacity_complete=" << complete << " samples=" << visited << '/' << total
                    << " ghosts=" << ghosts << " string_bytes=" << stringBytes
                    << " legacy_ghost_only_peak_lower_bound_bytes=" << ghosts * 191 + stringBytes * 2
                    << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count() << '\n';
                std::cout.flush();
            };
            for( const auto& span : spans ) {
                std::vector<Node> active;
                const bool skipZero = span.count && samples.ReadPhysical( span.first, span.thread, QuerySampleKind::Main ).timeNs == 0;
                for( uint64_t i = 0; i < span.count; ++i ) {
                    if( visited % 65536 == 0 && std::chrono::steady_clock::now() - started > std::chrono::seconds(120) ) { report(false); return 2; }
                    const auto sample = samples.ReadPhysical( span.first + i, span.thread, QuerySampleKind::Main );
                    ++visited;
                    if( skipZero && sample.timeNs == 0 ) continue;
                    std::vector<Node> uncached;
                    auto foundPath = paths.find( sample.callstack );
                    if( foundPath == paths.end() ) {
                        const auto stack = stacks.Frames( sample.callstack );
                        for( size_t n = stack.size(); n != 0; --n ) {
                            const auto id = stack[n-1];
                            auto found = definitions.find( id.data );
                            std::vector<Node> definition;
                            if( found == definitions.end() ) {
                                const uint64_t key = ( id.idx & 0x3FFFFFFFFFFFFFFF ) | ( ( id.idx & 0x3000000000000000 ) << 2 );
                                uint64_t digits = 1; for( auto value = key; value >= 16; value >>= 4 ) ++digits;
                                const auto entry = frames.Find( id );
                                if( !entry ) definition.push_back( { key, 2 + digits, false } );
                                else for( size_t j = entry->frames.size(); j != 0; --j ) {
                                    const auto& frame = entry->frames[j-1];
                                    const auto nameBytes = frame.name.Active() ? strings.StringLength(frame.name.Idx()) : 0;
                                    const auto fileBytes = frame.file.Active() ? strings.StringLength(frame.file.Idx()) : 0;
                                    if( nameBytes > (1<<20) || fileBytes > (1<<20) ) throw std::runtime_error("preflight string exceeds Ghost limit");
                                    definition.push_back( { key, nameBytes + fileBytes + 2 + digits, true } );
                                }
                                const auto footprint = 256 + definition.capacity() * sizeof(Node);
                                if( definitionBytes + footprint > cacheBytes ) { definitions.clear(); definitionBytes = 0; }
                                definitionBytes += footprint;
                                found = definitions.emplace( id.data, std::move(definition) ).first;
                            }
                            if( uncached.size() + found->second.size() > 65536 ) throw std::runtime_error("preflight path depth limit");
                            uncached.insert( uncached.end(), found->second.begin(), found->second.end() );
                        }
                        const auto footprint = 256 + uncached.capacity() * sizeof(Node);
                        if( pathBytes + footprint > cacheBytes ) { paths.clear(); pathBytes = 0; }
                        pathBytes += footprint;
                        foundPath = paths.emplace( sample.callstack, std::move(uncached) ).first;
                    }
                    const auto& path = foundPath->second;
                    // All currently active records have the same end after each sample.
                    // Therefore period/clamping cannot alter common-prefix cardinality.
                    size_t common = 0;
                    while( common < active.size() && common < path.size() && active[common].reusable && active[common].key == path[common].key ) ++common;
                    active.resize(common);
                    for( size_t n = common; n < path.size(); ++n ) { ++ghosts; stringBytes += path[n].bytes; active.push_back(path[n]); }
                    if( visited % 10000000 == 0 ) report(false);
                }
            }
            report(true); return 0;
        } catch( const std::exception& e ) { std::cerr << e.what() << '\n'; return 1; }
    }

    // Bounded stage replay: reads existing sidecars, writes only a new output directory.
    // A one-nanosecond period is deliberate: this measures storage work, not acceptance semantics.
    const bool slice = argc == 11 && std::string( argv[1] ) == "--slice";
    if( ( argc == 9 && std::string( argv[1] ) == "--replay" ) || slice )
    {
        QueryGhostZoneBuildStatistics stats;
        const auto maximum = std::stoull( argv[8] );
        if( maximum == 0 || maximum > 1000000 || fs::exists( argv[6] ) ) return 2;
        PagedRecordOptions options;
        options.workspace = std::make_shared<AnalysisWorkspaceBudget>( 512ull * 1024 * 1024, 256ull * 1024 * 1024 );
        auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ argv[6] } );
        options.disk = std::make_shared<AnalysisDiskBudget>( AnalysisDiskBudget{ usage, 2ull * 1024 * 1024 * 1024, {} } );
        std::filesystem::path samplesPath = argv[2];
        if( slice ) {
            auto skip = std::stoull( argv[9] );
            const auto cacheMiB = std::stoull( argv[10] );
            if( cacheMiB < 4 || cacheMiB > 128 ) return 2;
            QuerySampleStoreReader sourceSamples( samplesPath, argv[7], options );
            auto spans = sourceSamples.MainSpans();
            std::sort( spans.begin(), spans.end(), []( const auto& a, const auto& b ) { return a.thread < b.thread; } );
            samplesPath = std::string( argv[6] ) + "-samples";
            if( fs::exists( samplesPath ) ) return 2;
            QuerySampleStoreWriter selected( samplesPath, argv[7], options );
            uint64_t remaining = maximum;
            for( const auto& span : spans ) {
                if( skip >= span.count ) { skip -= span.count; continue; }
                const auto take = std::min<uint64_t>( remaining, span.count - skip );
                selected.Begin( span.thread, QuerySampleKind::Main, take );
                for( uint64_t i = 0; i < take; ++i ) {
                    const auto row = sourceSamples.ReadPhysical( span.first + skip + i, span.thread, QuerySampleKind::Main );
                    selected.Append( i, row.timeNs, row.callstack );
                }
                selected.End(); remaining -= take; skip = 0;
                if( remaining == 0 ) break;
            }
            if( remaining ) throw std::runtime_error( "diagnostic slice exceeds source" );
            selected.Commit();
            options.readerCacheBytes = cacheMiB * 1024 * 1024;
        } else options.cancelled = [&] { return stats.pathRequests >= maximum; };
        const auto start = std::chrono::steady_clock::now();
        bool boundedStop = false;
        try { BuildQueryGhostZoneStore( argv[6], samplesPath, argv[3], argv[4], argv[5], argv[7], 1, options, &stats ); }
        catch( const std::exception& e )
        {
            if( slice || stats.pathRequests < maximum || std::string( e.what() ).find( "cancel" ) == std::string::npos )
            { std::cerr << e.what() << '\n'; return 1; }
            boundedStop = true;
        }
        std::cout << "bounded_stop=" << boundedStop << " requests=" << stats.pathRequests
            << " resolutions=" << stats.pathResolutions << " hits=" << stats.pathCacheHits
            << " scratch_writes=" << stats.scratchRecordWrites
            << " scratch_write_calls=" << stats.scratchWriteCalls
            << " string_blocks=" << stats.stringBlockLoads << " frame_blocks=" << stats.frameBlockLoads
            << " callstack_blocks=" << stats.callstackBlockLoads
            << " decoded_bytes=" << stats.stringDecodedBytes + stats.frameDecodedBytes + stats.callstackDecodedBytes
            << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start ).count() << '\n';
        return 0;
    }
    const bool capacityProbe = ( argc == 3 || argc == 4 ) && std::string( argv[1] ) == "--peak-capacity";
    if( argc != 1 && !capacityProbe ) return 2;
    const uint64_t capacitySamples = argc == 4 ? std::stoull( argv[3] ) : 5000;
    if( capacityProbe && ( capacitySamples < 5000 || capacitySamples > 5000000 ) ) return 2;

    const auto root = fs::temp_directory_path() /
        ( "query-ghost-zone-store-test-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    fs::create_directories( root );
    const std::string fingerprint( 64, 'g' );
    auto usage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ root } );
    auto disk = std::make_shared<AnalysisDiskBudget>( AnalysisDiskBudget{
        usage, capacityProbe ? 20ull * 1024 * 1024 * 1024 : 64ull * 1024 * 1024, {} } );
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
        Require( buildStatistics.scratchWriteCalls == 1, "adjacent finalized Ghost records must share one write" );
        if( count != 2 ) throw std::runtime_error( "streamed Ghost Zone count changed: " + std::to_string( count ) );
        Require( !fs::exists( root / "ghost" / "build-scratch.bin" ), "Ghost Zone scratch was not removed" );
        Require( !fs::exists( root / "ghost" / "strings" ), "Ghost records must reuse the global dictionary instead of duplicating three strings per zone" );

        QueryGhostZoneStoreReader reader( root / "ghost", fingerprint, root / "strings", options );
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
        bool rejectedMissingDictionary = false;
        try { QueryGhostZoneStoreReader missing( root / "ghost", fingerprint, root / "missing-dictionary", options ); }
        catch( const std::exception& ) { rejectedMissingDictionary = true; }
        Require( rejectedMissingDictionary, "Ghost opened without its manifest-bound dictionary" );
        {
            QueryStringStoreWriter other( root / "other-strings", std::string(64,'x'), options );
            other.StringsBegin(0); other.StringsEnd();
            for(size_t kind=0;kind<3;++kind){other.PointerTableBegin(QueryStringPointerKind(kind),0);other.PointerTableEnd(QueryStringPointerKind(kind));}
            other.Commit();
            bool rejectedIdentity = false;
            try { QueryGhostZoneStoreReader mismatch( root / "ghost", fingerprint, root / "other-strings", options ); }
            catch( const std::exception& ) { rejectedIdentity = true; }
            Require( rejectedIdentity, "Ghost accepted a dictionary from another source" );
        }
        {
            QueryGhostZoneStoreWriter invalid( root / "invalid-global-ref", fingerprint, options );
            invalid.Begin(1); QueryGhostZoneFact fact; fact.name=999; invalid.Append(0,fact); invalid.Commit();
            QueryGhostZoneStoreReader invalidReader( root / "invalid-global-ref", fingerprint, root / "strings", options );
            bool rejectedReference=false;
            try { invalidReader.ValidateAll(); } catch(const std::exception&) { rejectedReference=true; }
            Require(rejectedReference,"deep validation accepted an invalid global name reference");
        }

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
        QueryGhostZoneStoreReader repeatedReader( root / "repeated-ghost", fingerprint, root / "strings", options );
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
        QueryGhostZoneStoreReader unresolvedReader( root / "unresolved-ghost", fingerprint, root / "strings", options );
        const auto unresolvedRows = unresolvedReader.Scan( 0, 30, 0, 10 );
        Require( unresolvedRows.size() == 5 && !unresolvedRows[0].parent && unresolvedRows[1].parent == 0 &&
            !unresolvedRows[2].parent && unresolvedRows[3].parent == 2 && !unresolvedRows[4].parent,
            "missing Frame definitions changed Ghost Zone tree boundaries" );
        unresolvedReader.ValidateAll();
        QuerySampleStoreWriter batchSamples( root / "batch-samples", fingerprint, options );
        batchSamples.Begin( 77, QuerySampleKind::Main, capacitySamples );
        for( uint64_t i = 0; i < capacitySamples; ++i ) batchSamples.Append( i, 5 + i * 5, 1 );
        batchSamples.End(); batchSamples.Commit();
        auto batchUsage = std::make_shared<AnalysisDiskUsage>( std::vector<fs::path>{ root / "batch-ghost" } );
        auto batchOptions = options;
        batchOptions.disk = std::make_shared<AnalysisDiskBudget>(
            AnalysisDiskBudget{ batchUsage, 20ull * 1024 * 1024 * 1024, {} } );
        const auto batchStarted = std::chrono::steady_clock::now();
        batchOptions.cancelled = [&] {
            return std::chrono::steady_clock::now() - batchStarted > std::chrono::minutes( 10 );
        };
        QueryGhostZoneBuildStatistics batchStats;
        Require( BuildQueryGhostZoneStore( root / "batch-ghost", root / "batch-samples", root / "callstacks",
            root / "unresolved-frames", root / "strings", fingerprint, 5, batchOptions, &batchStats ) == 2 * capacitySamples,
            "batched Ghost write lost records" );
        if( capacityProbe )
        {
            const auto observed = batchUsage->Snapshot();
            std::cout << "samples=" << capacitySamples << " ghost_records=" << 2 * capacitySamples
                << " peak_bytes=" << observed.peakBytes << " final_bytes=" << observed.currentBytes
                << " build_elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - batchStarted ).count()
                << " scratch_write_calls=" << batchStats.scratchWriteCalls << '\n';
            if( observed.peakBytes > std::stoull( argv[2] ) )
                throw std::runtime_error( "ghost scratch exceeded the stage capacity target" );
        }
        if( !capacityProbe )
            Require( batchStats.scratchWriteCalls < 10, "batched Ghost writes retained per-record system calls" );
        if( !capacityProbe )
        {
            QueryGhostZoneStoreReader batchReader( root / "batch-ghost", fingerprint, root / "strings", options );
            const auto boundary = batchReader.Scan( 0, 100000, 8190, 6 );
            Require( boundary.size() == 6, "batch boundary page is incomplete" );
            for( size_t i = 0; i < boundary.size(); ++i ) {
                const auto ordinal = 8190 + i;
                const auto& row = boundary[i];
                Require( row.ordinal == ordinal && row.startNs == int64_t( 5 + ( ordinal / 2 ) * 5 ) &&
                    row.endNs == row.startNs + 5 && row.depth == ordinal % 2 &&
                    ( ordinal % 2 ? row.parent == ordinal - 1 : !row.parent ), "batched Ghost record semantics changed" );
            }
            batchReader.ValidateAll();
        }
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
