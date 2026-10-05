#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace tracy;
using namespace tracy::stream;
void Check( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}
template<class T>
void Bytes( std::vector<uint8_t>& b, const T& value )
{
    const auto p = reinterpret_cast<const uint8_t*>( &value );
    b.insert( b.end(), p, p + sizeof( value ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType type, const T& value )
{
    b.push_back( uint8_t( type ) );
    Bytes( b, value );
}
void String( std::vector<uint8_t>& b, uint64_t key, const char* value )
{
    Event( b, QueueType::StringData, QueueStringTransfer{ key } );
    Bytes( b, uint16_t( strlen( value ) ) );
    b.insert( b.end(), value, value + strlen( value ) );
}
void Fixture( const std::filesystem::path& path, uint32_t count, int64_t step, uint32_t depth = 0, bool overlap = false, const std::string& rootName = "Root", bool allocated = false, bool uniqueRoots = false )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t sequence = 0;
    const auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> data ) {
        Check( writer->Append( type, flags, data, ++sequence, error ), error.c_str() );
    };
    std::vector<uint8_t> b( 24 );
    b[0] = 2;
    b[2] = 24;
    const uint32_t protocol = ProtocolVersion;
    memcpy( b.data() + 4, &protocol, 4 );
    append( RecordType::SessionBegin, 0, b );
    const uint8_t status = HandshakeWelcome;
    append( RecordType::ClientToServer, RecordFlagHandshake, { &status, 1 } );
    WelcomeMessage welcome{};
    welcome.initBegin = 1000;
    welcome.initEnd = 1010;
    welcome.timerMul = 1;
    strcpy_s( welcome.programName, "Global analysis fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    for( auto key : { uint64_t( 0x100 ), uint64_t( 0x200 ) } )
    {
        ServerQueryPacket query{ ServerQuerySourceLocation, key, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    }
    for( auto key : { uint64_t( 0x1000 ), uint64_t( 0x2000 ), uint64_t( 0x3000 ) } )
    {
        ServerQueryPacket query{ ServerQueryString, key, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    }
    b.clear();
    Event( b, QueueType::SourceLocation, QueueSourceLocation{ 0x1000, 0x1000, 0x3000, 10, 0, 0, 0 } );
    Event( b, QueueType::SourceLocation, QueueSourceLocation{ 0x2000, 0x2000, 0x3000, 20, 0, 0, 0 } );
    String( b, 0x1000, rootName.c_str() );
    String( b, 0x2000, "Leaf%_needle" );
    String( b, 0x3000, "analysis.cpp" );
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    const auto flush = [&] {
        std::vector<uint8_t> compressed( LZ4Size + 4 );
        const int size = LZ4_compress_default( reinterpret_cast<const char*>( b.data() ), reinterpret_cast<char*>( compressed.data() + 4 ), int( b.size() ), LZ4Size );
        Check( size > 0, "compress analysis fixture" );
        memcpy( compressed.data(), &size, 4 );
        compressed.resize( size + 4 );
        append( RecordType::ClientToServer, RecordFlagCompressedFrame, compressed );
        b.clear();
    };
    if( depth )
    {
        for( uint32_t n = 0; n < depth; ++n )
        {
            Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { n ? 1 : 1100 }, 0x100 } );
            if( b.size() > 400000 ) flush();
        }
        for( uint32_t n = 0; n < depth; ++n )
        {
            Event( b, QueueType::ZoneEnd, QueueZoneEnd{ 1 } );
            if( b.size() > 400000 ) flush();
        }
    }
    for( uint32_t n = 0; !depth && n < count; ++n )
    {
        if( uniqueRoots ) Event( b, QueueType::ThreadContext, QueueThreadContext{ n + 1 } );
        if( allocated )
        {
            std::vector<uint8_t> payload;
            Bytes( payload, uint32_t( 0x010203 ) );
            Bytes( payload, uint32_t( 10 ) );
            payload.insert( payload.end(), rootName.c_str(), rootName.c_str() + rootName.size() + 1 );
            const char file[] = "analysis.cpp";
            payload.insert( payload.end(), file, file + sizeof( file ) );
            Event( b, QueueType::SourceLocationPayload, QueueStringTransfer{ 100 + n } );
            Bytes( b, uint16_t( payload.size() ) );
            b.insert( b.end(), payload.begin(), payload.end() );
            Event( b, QueueType::ZoneBeginAllocSrcLoc, QueueZoneBeginLean{ n ? step / 2 : 1100 } );
        }
        else Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { n && !uniqueRoots ? step / 2 : 1100 }, 0x100 } );
        Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { step / 20 }, 0x200 } );
        if( !n )
        {
            const char* name = "renamed-child";
            b.push_back( uint8_t( QueueType::SingleStringData ) );
            Bytes( b, uint16_t( strlen( name ) ) );
            b.insert( b.end(), name, name + strlen( name ) );
            b.push_back( uint8_t( QueueType::ZoneName ) );
            const char* text = "special annotation";
            b.push_back( uint8_t( QueueType::SingleStringData ) );
            Bytes( b, uint16_t( strlen( text ) ) );
            b.insert( b.end(), text, text + strlen( text ) );
            b.push_back( uint8_t( QueueType::ZoneText ) );
        }
        Event( b, QueueType::ZoneEnd, QueueZoneEnd{ overlap ? step / 4 : step * 3 / 20 } );
        Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { overlap ? -step / 10 : step / 10 }, 0x200 } );
        Event( b, QueueType::ZoneEnd, QueueZoneEnd{ overlap ? step / 5 : step / 10 } );
        Event( b, QueueType::ZoneEnd, QueueZoneEnd{ step / 10 } );
        if( b.size() > 400000 ) flush();
    }
    if( !depth ) Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { step / 2 }, 0x100 } );
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
// The whole-capture call tree and the all-zone traversal agree on every
// statistic, flame node, view box and search hit.
void CompareCallTree( const EventStore& store, const char* what )
{
    auto tree = store.Analyze();
    StoreAnalysisOptions eventsOnly;
    eventsOnly.events = true;
    auto all = store.Analyze( eventsOnly );
    const auto& x = tree->Info();
    const auto& y = all->Info();
    Check( x.callTree && !y.callTree, what );
    Check( std::tie( x.events, x.closed, x.unfinished, x.invalid, x.sources, x.nodes, x.selfUnknown ) == std::tie( y.events, y.closed, y.unfinished, y.invalid, y.sources, y.nodes, y.selfUnknown ), what );
    const auto row = []( const StoreAnalysisRow& r ) { return std::tie( r.id, r.source, r.parent, r.thread, r.context, r.example, r.events, r.closed, r.unfinished, r.invalid, r.selfUnknown, r.subtreeEvents, r.inclusive, r.self, r.minimum, r.maximum, r.name, r.function, r.file, r.line ); };
    const auto samePage = [&]( const StorePage<StoreAnalysisRow>& a, const StorePage<StoreAnalysisRow>& b ) {
        if( a.values.size() != b.values.size() || a.more != b.more || a.next != b.next ) return false;
        for( size_t n = 0; n < a.values.size(); ++n )
            if( row( a.values[n] ) != row( b.values[n] ) ) return false;
        return true;
    };
    for( uint64_t after = 0;; )
    {
        const auto a = tree->Statistics( after, 16 ), b = all->Statistics( after, 16 );
        Check( samePage( a, b ), what );
        if( !a.more ) break;
        after = a.next;
    }
    std::vector<uint64_t> parents{ 0 };
    for( size_t n = 0; n < parents.size(); ++n )
        for( uint64_t after = 0;; )
        {
            const auto a = tree->Flame( parents[n], after, 64 ), b = all->Flame( parents[n], after, 64 );
            Check( samePage( a, b ), what );
            for( const auto& r : a.values ) parents.push_back( r.id );
            if( !a.more ) break;
            after = a.next;
        }
    const auto a = tree->FlameView(), b = all->FlameView();
    Check( a.boxes.size() == b.boxes.size() && a.totalNodes == b.totalNodes && a.omittedNodes == b.omittedNodes && a.totalWeight == b.totalWeight && a.depth == b.depth, what );
    for( size_t n = 0; n < a.boxes.size(); ++n ) Check( row( a.boxes[n].row ) == row( b.boxes[n].row ) && a.boxes[n].begin == b.boxes[n].begin && a.boxes[n].end == b.boxes[n].end && a.boxes[n].weight == b.boxes[n].weight && a.boxes[n].depth == b.boxes[n].depth && a.boxes[n].expandedWeight == b.boxes[n].expandedWeight, what );
    for( const char* text : { "", "root", "Root", "child", "annotation", "renamed", "x", "%_" } )
        for( bool caseSensitive : { false, true } )
            for( uint64_t after = 0;; )
            {
                const auto p = tree->Search( text, caseSensitive, after, 5 ), q = all->Search( text, caseSensitive, after, 5 );
                Check( p.values.size() == q.values.size() && p.more == q.more && p.next == q.next, what );
                for( size_t n = 0; n < p.values.size(); ++n )
                {
                    const auto& l = p.values[n];
                    const auto& r = q.values[n];
                    Check( std::tie( l.id, l.thread, l.context, l.source, l.start, l.end, l.name, l.function, l.file, l.textMatched ) == std::tie( r.id, r.thread, r.context, r.source, r.start, r.end, r.name, r.function, r.file, r.textMatched ), what );
                }
                if( !p.more ) break;
                after = p.next;
            }
}
int main( int argc, char** argv )
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-analysis-store-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        if( argc == 3 && std::string_view( argv[1] ) == "--capture" )
        {
            StoreOptions options;
            options.cacheBytes = 4ull << 20;
            options.resultBytes = 65536;
            auto store = EventStore::Open( std::filesystem::u8path( argv[2] ), options );
            for( bool gpu : { false, true } )
            {
                const auto expected = gpu ? store->Info().gpuZones : store->Info().cpuZones;
                if( !expected ) continue;
                const auto start = std::chrono::steady_clock::now();
                StoreAnalysisOptions settings;
                settings.gpu = gpu;
                auto analysis = store->Analyze( settings );
                Check( analysis->Info().events == expected, "capture analysis must account for every zone" );
                uint64_t events = 0, cursor = 0;
                for( ;; )
                {
                    auto page = analysis->Statistics( cursor, 128 );
                    for( const auto& row : page.values ) events += row.events;
                    if( !page.more ) break;
                    cursor = page.next;
                }
                Check( events == expected, "source aggregates must account for every event" );
                events = 0;
                cursor = 0;
                for( ;; )
                {
                    auto page = analysis->Search( "", false, cursor, 128 );
                    events += page.values.size();
                    if( !page.more ) break;
                    cursor = page.next;
                }
                Check( events == expected, "paged search must reach every event" );
                std::cout << ( gpu ? "GPU" : "CPU" ) << " events=" << expected << " sources=" << analysis->Info().sources << " nodes=" << analysis->Info().nodes << " workspace_bytes=" << analysis->Info().workspaceBytes << " seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() << '\n';
            }
            return 0;
        }
        std::filesystem::create_directory( root );
        if( argc > 1 && std::string_view( argv[1] ) == "--deep" )
        {
            const auto path = root / "deep.tracy-stream";
            constexpr uint32_t depth = 10000;
            Fixture( path, 0, 200, depth );
            StoreOptions read;
            read.cacheBytes = 4ull << 20;
            read.resultBytes = 65536;
            EventStore::Build( path, read );
            auto store = EventStore::Open( path, read );
            auto analysis = store->Analyze();
            StoreTimelineOptions deepOptions;deepOptions.begin=0;deepOptions.end=store->Info().lastNs+1;deepOptions.bins=32;
            const auto deepOverview=store->TimelineOverview(deepOptions);Check(deepOverview.events==depth && deepOverview.lanes.size()==1 && deepOverview.lanes[0].maximumDepth==depth-1,"dense overview preserves deep event count and original depth");
            Check(std::any_of(deepOverview.cells.begin(),deepOverview.cells.end(),[](const auto& cell){return cell.depth==3 && cell.events>100;}),"last overview row aggregates deep levels");
            store.reset();
            Check( analysis->Info().events == depth && analysis->Info().nodes == depth, "deep flame paths stay on disk" );
            const auto stats = analysis->Statistics();
            Check( stats.values.size() == 1 && stats.values[0].inclusive == int64_t( depth ) * depth && stats.values[0].self == depth * 2 - 1, "deep inclusive and self totals" );
            StoreFlameOptions viewOptions;
            viewOptions.depth = 32;
            auto view = analysis->FlameView( viewOptions );
            Check( view.boxes.size() == 32 && view.depth == 32 && view.totalNodes == depth && view.omittedNodes == depth - 32, "deep chart materializes only requested levels" );
            viewOptions.focus = view.boxes.back().row.id;
            const auto zoomed = analysis->FlameView( viewOptions );
            Check( zoomed.boxes.size() == 32 && zoomed.boxes.front().row.id == viewOptions.focus && zoomed.boxes.front().begin == 0 && zoomed.boxes.front().end == 1, "deep chart zoom has fresh normalized coordinates" );
            uint64_t parent = 0;
            for( uint32_t n = 0; n < depth; ++n )
            {
                const auto page = analysis->Flame( parent );
                Check( page.values.size() == 1 && !page.more && page.values[0].subtreeEvents == depth - n, "deep path traversal is complete after source close" );
                parent = page.values[0].id;
            }
            Check( analysis->Flame( parent ).values.empty(), "deep flame leaf" );
            std::cout << "Deep analysis passed. Evidence: " << root.string() << '\n';
            return 0;
        }
        const bool scale = argc > 1;
        const uint32_t count = scale ? 20000 : 40;
        const int64_t step = scale ? 90000000 : 200;
        const auto path = root / "analysis.tracy-stream";
        Fixture( path, count, step );
        bool cancel = false;
        StoreOptions read;
        read.cacheBytes = 4ull << 20;
        read.resultBytes = 65536;
        read.cancelled = [&] {
            return cancel;
        };
        EventStore::Build( path, read );
        auto store = EventStore::Open( path, read );
        StoreTimelineOptions timelineOptions;timelineOptions.begin=100;timelineOptions.end=100+step/2;timelineOptions.bins=4;timelineOptions.depths=2;
        const auto timeline=store->TimelineOverview(timelineOptions);Check(timeline.events==3 && timeline.lanes.size()==1 && timeline.cells.size()==8 && !timeline.more,"dense timeline accounts for every overlapping event");
        for(const auto& cell:timeline.cells)Check(cell.events==1 && cell.closed==1 && !cell.unfinished,"half-open bins include both disjoint child intervals without boundary duplication");
        CompareCallTree( *store, "call tree analysis equals the all-zone traversal" );
        auto analysis = store->Analyze();
        const auto summary = analysis->Info();
        Check( summary.events == uint64_t( count ) * 3 + 1 && summary.closed == uint64_t( count ) * 3 && summary.unfinished == 1 && summary.invalid == 0, "completed global traversal retains every matching event and reports unfinished events" );
        const auto stats = analysis->Statistics();
        Check( stats.values.size() == 2 && !stats.more, "all source statistics available" );
        const auto& a = stats.values[0];
        const auto& b = stats.values[1];
        Check( a.name == "Root" && a.events == count + 1 && a.inclusive == int64_t( count ) * step / 2 && a.self == int64_t( count ) * step / 4 && a.selfUnknown == 1, "root inclusive/self duration and unfinished coverage" );
        Check( b.name == "Leaf%_needle" && b.events == count * 2 && b.inclusive == int64_t( count ) * step / 4 && b.self == b.inclusive, "child source totals include both sibling instances" );
        const auto roots = analysis->Flame();
        Check( roots.values.size() == 1 && roots.values[0].name == "Root", "flame roots merge repeated paths" );
        const auto children = analysis->Flame( roots.values[0].id );
        Check( children.values.size() == 1 && children.values[0].events == count * 2 && children.values[0].parent == roots.values[0].id, "flame children preserve source path and counts" );
        const auto graph = analysis->FlameView();
        Check( graph.boxes.size() == 2 && graph.totalNodes == 2 && graph.omittedNodes == 0 && graph.depth == 2, "multi-layer flame view contains both levels" );
        Check( graph.boxes[0].row.id == roots.values[0].id && graph.boxes[0].begin == 0 && graph.boxes[0].end == 1 && graph.boxes[1].depth == 1 && graph.boxes[1].end == 0.5, "flame geometry preserves parent and child proportions" );
        StoreFlameOptions focused;
        focused.focus = children.values[0].id;
        const auto leafGraph = analysis->FlameView( focused );
        Check( leafGraph.boxes.size() == 1 && leafGraph.boxes[0].begin == 0 && leafGraph.boxes[0].end == 1, "zoomed flame leaf remains visible" );
        focused = {};
        focused.depth = 1;
        const auto shallow = analysis->FlameView( focused );
        Check( shallow.boxes.size() == 1 && shallow.omittedNodes == 1, "depth clipping is reported" );
        focused = {};
        focused.eventCounts = true;
        const auto counted = analysis->FlameView( focused );
        Check( counted.eventCounts && std::abs( counted.boxes[1].end - double( count * 2 ) / double( count * 3 + 1 ) ) < 1e-12, "count mode preserves unfinished paths" );
        focused = {};
        focused.maximumNodes = 1;
        const auto boundedGraph = analysis->FlameView( focused );
        Check( boundedGraph.boxes.size() == 1 && boundedGraph.omittedNodes == 1, "node clipping is explicit" );
        cancel = true;
        bool layoutCancelled = false;
        try
        {
            analysis->FlameView();
        }
        catch( const std::exception& )
        {
            layoutCancelled = true;
        }
        cancel = false;
        Check( layoutCancelled && analysis->FlameView().boxes.size() == 2, "cancelled view leaves immutable layout reusable" );
        uint64_t cursor = 0, hits = 0;
        for( ;; )
        {
            const auto page = analysis->Search( "%_", false, cursor, 7 );
            for( const auto& hit : page.values )
            {
                Check( hit.id > cursor, "search cursors are monotonic stable event IDs" );
                cursor = hit.id;
                ++hits;
            }
            if( !page.more ) break;
        }
        Check( hits == uint64_t( count ) * 2, "search is literal and complete across pages" );
        Check( analysis->Search( "renamed-child" ).values.size() == 1 && analysis->Search( "annotation" ).values.size() == 1, "search includes event overrides and text fragments" );
        const auto foundRoots = analysis->Search( "root" );
        Check( analysis->Search( "root", true ).values.empty() && foundRoots.values.size() == std::min<uint32_t>( count + 1, 128 ) && foundRoots.more == ( count + 1 > 128 ), "case sensitivity and search page completeness are explicit" );
        StoreAnalysisOptions interrupted;
        interrupted.progress = [&]( uint64_t done, uint64_t ) {
            if( done > 4 ) cancel = true;
        };
        bool rejected = false;
        try
        {
            store->Analyze( interrupted );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        cancel = false;
        Check( rejected && analysis->Statistics().values[0].events == count + 1, "cancelled analysis preserves preceding published result" );
        interrupted.progress = {};
        interrupted.temporaryBytes = 1;
        rejected = false;
        try
        {
            store->Analyze( interrupted );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected && analysis->Statistics().values[0].events == count + 1, "temporary disk budget preserves preceding analysis" );
        store.reset();
        Check( analysis->Statistics().values[0].events == count + 1 && analysis->Search( "annotation" ).values.size() == 1, "analysis result outlives the source revision handle" );
        Check( analysis->FlameParent( children.values[0].id ) == roots.values[0].id, "flame navigation resolves parent without an unbounded UI breadcrumb stack" );
        store = EventStore::Open( path, read );
        if( !scale )
        {
            const auto widePath = root / "wide.tracy-stream";
            Fixture( widePath, 40, 200, 0, false, "Root", false, true );
            EventStore::Build( widePath, read );
            auto wideStore = EventStore::Open( widePath, read );
            auto wide = wideStore->Analyze();
            StoreTimelineOptions laneOptions;laneOptions.begin=0;laneOptions.end=wideStore->Info().lastNs+1;laneOptions.bins=16;laneOptions.maximumLanes=3;uint64_t laneEvents=0,laneCount=0;
            for(;;){const auto page=wideStore->TimelineOverview(laneOptions);Check(page.lanes.size()<=3,"timeline lane pagination is bounded");laneEvents+=page.events;laneCount+=page.lanes.size();if(!page.more)break;laneOptions.afterLane=page.nextLane;}
            Check(laneCount==40 && laneEvents==121,"lane pagination accounts for all events without overlap duplication");
            StoreFlameOptions wideOptions;
            wideOptions.pixels = 1;
            const auto narrow = wide->FlameView( wideOptions );
            Check( narrow.totalNodes == 80 && narrow.boxes.empty() && narrow.omittedNodes == 80, "pixel clipping is explicit for dense roots" );
            wideOptions = {};
            wideOptions.focus = wide->Flame().values.front().id;
            const auto zoom = wide->FlameView( wideOptions );
            Check( zoom.totalNodes == 2 && zoom.boxes.size() == 2 && zoom.omittedNodes == 0, "dense roots remain reachable through path browser and zoom" );
            wideOptions = {};
            wideOptions.maximumNodes = 3;
            const auto small = wide->FlameView( wideOptions );
            Check( small.boxes.size() == 3 && small.omittedNodes == 77, "wide node budget preserves total accounting" );
            const auto allocatedPath = root / "allocated.tracy-stream";
            Fixture( allocatedPath, 2, 200, 0, false, "Root", true );
            EventStore::Build( allocatedPath, read );
            auto allocatedStore = EventStore::Open( allocatedPath, read );
            auto allocatedAnalysis = allocatedStore->Analyze();
            Check( allocatedAnalysis->Info().sources == 3 && allocatedAnalysis->Statistics().values[0].events == 2, "equivalent allocated source locations share native statistics identity" );
            const auto overlapPath = root / "overlap.tracy-stream";
            Fixture( overlapPath, 1, 200, 0, true );
            EventStore::Build( overlapPath, read );
            auto overlapStore = EventStore::Open( overlapPath, read );
            CompareCallTree( *overlapStore, "call tree of overlapping siblings equals the all-zone traversal" );
            CompareCallTree( *allocatedStore, "call tree of allocated sources equals the all-zone traversal" );
            const auto overlapRows = overlapStore->Analyze()->Statistics();
            Check( overlapRows.values[0].inclusive == 100 && overlapRows.values[0].self == 30 && overlapRows.values[1].inclusive == 90, "overlapping siblings are unioned before subtracting self time" );
            const auto overlapGraph = overlapStore->Analyze()->FlameView();
            Check( overlapGraph.totalWeight == 120 && overlapGraph.boxes[0].expandedWeight && overlapGraph.boxes[1].end == 0.75, "overlap flame widths report aggregate cost without changing measured inclusive time" );
            const auto longPath = root / "long-name.tracy-stream";
            Fixture( longPath, 2, 200, 0, false, std::string( 30000, 'x' ) + "TAIL-NEEDLE" );
            EventStore::Build( longPath, read );
            auto longStore = EventStore::Open( longPath, read );
            CompareCallTree( *longStore, "call tree of long names equals the all-zone traversal" );
            auto longAnalysis = longStore->Analyze();
            const auto longMatches = longAnalysis->Search( "TAIL-NEEDLE", true, 0, 4096 );
            Check( longMatches.values.size() == 3 && longMatches.values[0].name.size() == 2048, "search matches complete names beyond bounded display previews" );
            size_t resultBytes = sizeof( longMatches ) + longMatches.values.capacity() * sizeof( StoreAnalysisMatch );
            for( const auto& hit : longMatches.values ) resultBytes += hit.name.capacity() + hit.function.capacity() + hit.file.capacity() + 3;
            Check( resultBytes <= read.resultBytes, "large requested pages reserve room for variable-length fields" );
            StoreFlameOptions longViewOptions;
            longViewOptions.maximumNodes = 4096;
            const auto longGraph = longAnalysis->FlameView( longViewOptions );
            size_t graphBytes = sizeof( longGraph ) + longGraph.boxes.capacity() * sizeof( StoreFlameBox );
            for( const auto& box : longGraph.boxes ) graphBytes += box.row.name.capacity() + box.row.function.capacity() + box.row.file.capacity() + 3;
            Check( graphBytes <= read.resultBytes, "flame metadata allocation respects result bytes" );
            const auto zeroPath = root / "zero.tracy-stream";
            Fixture( zeroPath, 1, 0 );
            EventStore::Build( zeroPath, read );
            auto zeroStore = EventStore::Open( zeroPath, read );
            const auto zeroGraph = zeroStore->Analyze()->FlameView();
            Check( zeroGraph.eventCounts && zeroGraph.totalWeight == 4 && zeroGraph.boxes.size() == 2, "zero durations fall back to event widths without losing open paths" );
            StoreTimelineOptions point;point.begin=100;point.end=101;point.bins=2048;point.depths=2;const auto pointOverview=zeroStore->TimelineOverview(point);Check(pointOverview.bins==1 && pointOverview.events==4 && pointOverview.cells.size()==2 && pointOverview.cells[0].events==2 && pointOverview.cells[0].unfinished==1,"nanosecond overview keeps zero-duration and open events");
            cancel=true;bool overviewCancelled=false;try{store->TimelineOverview(timelineOptions);}catch(const std::exception&){overviewCancelled=true;}cancel=false;Check(overviewCancelled && store->TimelineOverview(timelineOptions).events==3,"cancelled aggregate query remains reusable");
            StoreAnalysisOptions range;
            range.begin = 125;
            range.end = 175;
            analysis = store->Analyze( range );
            const auto clipped = analysis->Info();
            Check( clipped.events == 3 && clipped.closed == 3, "half-open selected interval counts overlapping zones" );
            const auto rows = analysis->Statistics();
            Check( rows.values[0].inclusive == 50 && rows.values[0].self == 20 && rows.values[1].inclusive == 30, "self time subtracts clipped child coverage" );
            range.thread = 999;
            analysis = store->Analyze( range );
            Check( analysis->Info().events == 0 && analysis->Statistics().values.empty() && analysis->Flame().values.empty(), "empty filtered analysis is successful" );
        }
        std::cout << "Global analysis tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
