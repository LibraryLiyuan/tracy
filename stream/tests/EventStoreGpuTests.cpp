#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace tracy;
using namespace tracy::stream;
namespace
{
bool contextZero=false;
void Check( bool v, const char* message )
{
    if( !v ) throw std::runtime_error( message );
}
template<class T>
void Bytes( std::vector<uint8_t>& b, const T& value )
{
    auto p = reinterpret_cast<const uint8_t*>( &value );
    b.insert( b.end(), p, p + sizeof( value ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType type, const T& value )
{
    b.push_back( uint8_t( type ) );
    auto adjusted=value;if constexpr(requires{adjusted.context;})if(contextZero && adjusted.context==1)adjusted.context=0;Bytes( b, adjusted );
}
void Fixture( const std::filesystem::path& path, int fault = 0, bool zero = false )
{
    contextZero=zero;
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t sequence = 0;
    auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> data ) { Check( writer->Append( type, flags, data, ++sequence, error ), error.c_str() ); };
    std::vector<uint8_t> b( 24 );
    b[0] = 2;
    b[2] = 24;
    const uint32_t protocol = ProtocolVersion;
    memcpy( b.data() + 4, &protocol, 4 );
    append( RecordType::SessionBegin, 0, b );
    const uint8_t handshake = HandshakeWelcome;
    append( RecordType::ClientToServer, RecordFlagHandshake, { &handshake, 1 } );
    WelcomeMessage welcome{};
    welcome.initBegin = 1000;
    welcome.initEnd = 1010;
    welcome.timerMul = 1;
    strcpy_s( welcome.programName, "GPU fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    ServerQueryPacket query{ ServerQuerySourceLocation, 0x123, 0 };
    append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    auto frame = [&] { std::vector<uint8_t> out(LZ4Size+4); int n=LZ4_compress_default(reinterpret_cast<const char*>(b.data()),reinterpret_cast<char*>(out.data()+4),int(b.size()),LZ4Size); Check(n>0,"compression"); memcpy(out.data(),&n,4);out.resize(n+4);append(RecordType::ClientToServer,RecordFlagCompressedFrame,out);b.clear(); };
    b.clear();
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    Event( b, QueueType::GpuNewContext, QueueGpuNewContext{ 1000, 10, 0, 1.f, 1, GpuContextFlags( 0 ), GpuContextType::Vulkan } );
    b.push_back( uint8_t( QueueType::SingleStringData ) );
    Bytes( b, uint16_t( 14 ) );
    const char name[] = "Vulkan fixture";
    b.insert( b.end(), name, name + 14 );
    Event( b, QueueType::GpuContextName, QueueGpuContextName{ 1 } );
    Event( b, QueueType::GpuZoneBegin, QueueGpuZoneBegin{ { 1100, 7, 1, 1 }, 0x123 } );
    Event( b, QueueType::GpuZoneBegin, QueueGpuZoneBegin{ { 10, 7, 2, 1 }, 0x123 } );
    frame();
    Event( b, QueueType::GpuZoneEnd, QueueGpuZoneEnd{ 10, 7, 3, 1 } );
    Event( b, QueueType::GpuZoneEnd, QueueGpuZoneEnd{ 80, 7, 4, 1 } );
    for( auto [time, id] : std::array<std::pair<int64_t, uint16_t>, 4>{ { { 110, 1 }, { 10, 2 }, { 10, 3 }, { 80, 4 } } } ) Event( b, QueueType::GpuTime, QueueGpuTime{ time, id, 1 } );
    Event( b, QueueType::GpuZoneBegin, QueueGpuZoneBegin{ { 10, 7, 5, 1 }, 0x123 } );
    Event( b, QueueType::GpuNewContext, QueueGpuNewContext{ 1000, 0, 0, 2.f, 2, GpuContextCalibration, GpuContextType::Direct3D12 } );
    Event( b, QueueType::GpuCalibration, QueueGpuCalibration{ 50, 1200, 200, 2 } );
    Event( b, QueueType::LockAnnounce, QueueLockAnnounce{ 1, 900, 0x123, LockType::Lockable } );
    Event( b, QueueType::LockWait, QueueLockWait{ 8, 1, 1000 } );
    Event( b, QueueType::MemAlloc, QueueMemAlloc{ 100, 8, 0x999, { 1, 0, 0, 0, 0, 0 } } );
    Event( b, QueueType::GpuZoneBeginSerial, QueueGpuZoneBegin{ { 300, 8, 6, 2 }, 0x123 } );
    Event( b, QueueType::GpuZoneEndSerial, QueueGpuZoneEnd{ 100, 8, 7, 2 } );
    Event( b, QueueType::GpuTime, QueueGpuTime{ 50, 6, 2 } );
    Event( b, QueueType::GpuTime, QueueGpuTime{ 10, 7, 2 } );
    Event( b, QueueType::GpuNewContext, QueueGpuNewContext{ 1000, 0, 9, 1.f, 3, GpuContextFlags( 0 ), GpuContextType::OpenGl } );
    Event( b, QueueType::GpuZoneBegin, QueueGpuZoneBegin{ { 10, 9, 8, 3 }, 0x123 } );
    Event( b, QueueType::GpuZoneEnd, QueueGpuZoneEnd{ 10, 0, 9, 3 } );
    Event( b, QueueType::GpuTime, QueueGpuTime{ 4294967006ll, 8, 3 } );
    Event( b, QueueType::GpuTime, QueueGpuTime{ -4294967271ll, 9, 3 } );
    Event( b, QueueType::SourceLocation, QueueSourceLocation{ 0, 0, 0, 42, 0, 0, 255 } );
    Event( b, QueueType::GpuZoneAnnotation, QueueGpuZoneAnnotation{ -7, 1.5, 7, 1, 1 } );
    Event( b, QueueType::GpuZoneAnnotation, QueueGpuZoneAnnotation{ 0, std::bit_cast<double>( uint64_t( 0x7ff8000000000042ull ) ), 7, 1, 1 } );
    Event( b, QueueType::GpuZoneAnnotation, QueueGpuZoneAnnotation{ -7, 2.5, 7, 1, 1 } );
    b.push_back( uint8_t( QueueType::SingleStringData ) );
    Bytes( b, uint16_t( 4 ) );
    b.insert( b.end(), { 'n', 'o', 't', 'e' } );
    Event( b, QueueType::GpuAnnotationName, QueueGpuAnnotationName{ -7, 1 } );
    if( fault == 1 ) Event( b, QueueType::GpuTime, QueueGpuTime{ 0, 65535, 1 } );
    if( fault == 2 ) Event( b, QueueType::GpuCalibration, QueueGpuCalibration{ 50, 1300, 100, 2 } );
    if( fault == 3 ) Event( b, QueueType::GpuNewContext, QueueGpuNewContext{ 1000, 0, 0, 1.f, 1, GpuContextFlags( 0 ), GpuContextType::Vulkan } );
    if( fault == 4 ) Event( b, QueueType::GpuZoneEnd, QueueGpuZoneEnd{ 0, 999, 1, 1 } );
    if( fault == 5 )
    {
        Event( b, QueueType::GpuTimeSync, QueueGpuTimeSync{ 100, 5000, 3 } );
        Event( b, QueueType::GpuZoneBeginSerial, QueueGpuZoneBegin{ { 3600, 9, 10, 3 }, 0x123 } );
        Event( b, QueueType::GpuZoneEndSerial, QueueGpuZoneEnd{ 100, 0, 11, 3 } );
        Event( b, QueueType::GpuTime, QueueGpuTime{ 105, 10, 3 } );
        Event( b, QueueType::GpuTime, QueueGpuTime{ 20, 11, 3 } );
    }
    if( fault == 6 )
    {
        Event( b, QueueType::JnGpuZoneBeginCallsite, QueueJnGpuZoneBeginCallsite{ { { 10, 8, 8, 2 }, 0x123 }, 7 } );
        Event( b, QueueType::GpuZoneEndSerial, QueueGpuZoneEnd{ 10, 8, 9, 2 } );
        Event( b, QueueType::GpuTime, QueueGpuTime{ 275, 8, 2 } );
        Event( b, QueueType::GpuTime, QueueGpuTime{ 10, 9, 2 } );
        Event( b, QueueType::JnCallsiteDefinition, QueueJnCallsiteDefinition{ 0x123, 7, 8, 1, 3, 0, 1 } );
    }
    frame();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
}
int main()
{
    auto root = std::filesystem::temp_directory_path() / std::filesystem::path( "tracy-store-gpu-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        auto path = root / "gpu.tracy-stream";
        Fixture( path );
        auto info = EventStore::Build( path );
        Check( info.gpuZones == 5, "five GPU zones must be indexed directly from journal" );
        auto store = EventStore::Open( path );
        auto contexts = store->GpuContexts();
        Check( contexts.values.size() == 3, "three GPU contexts" );
        Check( contexts.values[0].name == "Vulkan fixture" && contexts.values[2].thread == 9 && contexts.values[2].overflow == 4294967296ull, "context names, bound thread and wrap state" );
        auto all = store->GpuZones( INT64_MIN, INT64_MAX );
        Check( all.values.size() == 5, "all resolved and unresolved GPU zones remain accessible" );
        const auto& parent = all.values[0];
        const auto& child = all.values[1];
        const auto& unresolved = all.values[2];
        const auto& calibrated = all.values[3];
        const auto& wrapped = all.values[4];
        Check( parent.gpuStart == 100 && parent.gpuEnd == 200 && parent.cpuStart == 100 && parent.cpuEnd == 200, "GPU and CPU time projection" );
        Check( child.parent == parent.id && child.depth == 1 && child.gpuStart == 110 && child.gpuEnd == 120, "cross-frame GPU parent and asynchronous timestamps" );
        Check( !unresolved.gpuStart && !unresolved.gpuEnd && !unresolved.cpuEnd && info.incompleteGpuZones == 1, "unresolved GPU time remains explicit" );
        Check( calibrated.gpuStart == 1040 && calibrated.gpuEnd == 1080 && calibrated.cpuStart == 400 && calibrated.cpuEnd == 500, "period and calibration with independent serial clock" );
        Check( wrapped.thread == 0 && wrapped.gpuStart == 4294967276ll && wrapped.gpuEnd == 4294967301ll, "32-bit GPU wrap and context-bound thread semantics" );
        // Cursor batches must retain exact child ownership across contexts.
        // These child fields feed Query self time and the windowed analyzer.
        const auto groups = store->GpuZoneGroups();
        auto cursor = store->GpuZoneCursor();
        uint64_t cursorCount = 0;
        for( size_t group = 0; group < groups.size(); ++group )
        {
            cursor->Seek( group, 0 );
            StoreGpuZoneRecord record;
            while( cursor->Next( record ) )
            {
                ++cursorCount;
                const auto children = store->GpuChildren( record.id );
                Check( !children.more && record.children == children.values.size(), "GPU cursor child counts match direct parent query" );
                Check( record.childTime == ( record.id == parent.id ? 10 : 0 ), "GPU cursor child intervals do not include other contexts" );
            }
        }
        Check( cursorCount == 5, "GPU cursor retains every context and unfinished record" );
        StoreAnalysisOptions globalGpu;globalGpu.gpu=true;
        auto analysis=store->Analyze(globalGpu);
        Check(analysis->Info().events==5 && analysis->Info().closed==4 && analysis->Info().unfinished==1,"GPU analysis distinguishes missing timestamps");
        const auto statistics=analysis->Statistics();
        Check(statistics.values.size()==1 && statistics.values[0].inclusive==175 && statistics.values[0].self==165 && statistics.values[0].selfUnknown==1,"GPU global duration and direct-child self totals use calibrated times");
        globalGpu.context=1;globalGpu.begin=115;globalGpu.end=120;analysis=store->Analyze(globalGpu);
        Check(analysis->Info().events==3 && analysis->Info().closed==2 && analysis->Statistics().values[0].inclusive==10 && analysis->Statistics().values[0].self==5,"GPU analysis clips intervals and retains unknown-time coverage");
        StoreTimelineOptions dense;dense.gpu=true;dense.begin=115;dense.end=120;dense.context=1;dense.bins=2;
        const auto overview=store->TimelineOverview(dense);Check(overview.events==3 && overview.lanes.size()==1 && overview.lanes[0].unplaced==1 && overview.lanes[0].unfinished==1,"GPU overview preserves unplaced timestamp counts");
        const auto zeroPath=root/"context-zero.tracy-stream";Fixture(zeroPath,0,true);EventStore::Build(zeroPath);auto zeroStore=EventStore::Open(zeroPath);dense.context=0;const auto zeroOverview=zeroStore->TimelineOverview(dense);Check(zeroOverview.events==3 && zeroOverview.lanes[0].id==0,"GPU context zero remains visible in overview and filtered queries");
        Check( store->GpuZones( 115, 120, 1 ).values.size() == 3, "overlap includes long parents and unknown timestamps" );
        Check( store->GpuZones( 120, 121, 1 ).values.size() == 2, "half-open query excludes ended child" );
        auto page = store->GpuZones( 0, 5000000000ll, {}, 0, 1 );
        uint64_t count = page.values.size();
        while( page.more )
        {
            page = store->GpuZones( 0, 5000000000ll, {}, page.next, 1 );
            count += page.values.size();
        }
        Check( count == 5, "GPU pagination retains every event" );
        Check( store->GpuChildren( parent.id ).values[0].id == child.id, "indexed GPU child navigation" );
        auto notes = store->GpuNotes( 1 );
        Check( notes.values.size() == 1 && notes.values[0].note == -7 && notes.values[0].name == "note", "negative note identifiers and delayed names are retained" );
        auto annotations = store->GpuAnnotations( 1, 1, 0, 1 );
        Check( annotations.more && annotations.values[0].name == "note" && std::bit_cast<double>( annotations.values[0].valueBits ) == 2.5, "annotation updates preserve identity and current value" );
        annotations = store->GpuAnnotations( 1, 1, annotations.next, 1 );
        Check( !annotations.more && annotations.values[0].note == 0 && annotations.values[0].valueBits == 0x7ff8000000000042ull, "annotation paging preserves zero IDs and NaN payload bits" );
        const auto stable = parent.id;
        store.reset();
        store = EventStore::Open( path );
        Check( store->GpuZone( stable )->gpuEnd == 200, "GPU identity stable after reopening" );
        store.reset();
        for( int fault = 1; fault <= 4; ++fault )
        {
            auto bad = root / ( "bad-" + std::to_string( fault ) + ".tracy-stream" );
            Fixture( bad, fault );
            bool rejected = false;
            try
            {
                EventStore::Build( bad );
            }
            catch( const std::exception& )
            {
                rejected = true;
            }
            Check( rejected && !std::filesystem::exists( EventStore::Directory( bad ) ), "invalid GPU query, calibration, context or stack must not publish" );
        }
        auto syncPath = root / "sync.tracy-stream";
        Fixture( syncPath, 5 );
        EventStore::Build( syncPath );
        auto syncStore = EventStore::Open( syncPath );
        auto sync = syncStore->GpuZones( 4009, 4030, 3 );
        Check( sync.values.size() == 1 && sync.values[0].gpuStart == 4010 && sync.values[0].gpuEnd == 4030 && sync.values[0].cpuStart == 4100, "GPU sync updates offset and resets wrap state" );
        Check( syncStore->GpuContexts().values[2].overflow == 0, "GPU sync clears previous wrap state" );
        auto callsitePath = root / "callsite.tracy-stream";
        Fixture( callsitePath, 6 );
        EventStore::Build( callsitePath );
        auto callsiteStore = EventStore::Open( callsitePath );
        auto callsite = callsiteStore->GpuZones( 1119, 1161, 2 );
        Check( callsite.values.size() == 1 && callsite.values[0].callsite == 7 && callsite.values[0].cpuStart == 510 && callsite.values[0].cpuEnd == 520 && callsite.values[0].provenance == 3 && callsite.values[0].unavailableReason == 1, "JN GPU callsite begin uses serial clock and resolves delayed definition and provenance" );
        bool cancelled = false;
        StoreOptions cancellable;
        cancellable.cancelled = [&] { return cancelled; };
        store = EventStore::Open( path, cancellable );
        cancelled = true;
        bool rejected = false;
        try
        {
            store->GpuZones( 0, 1000 );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        cancelled = false;
        Check( rejected && store->GpuZones( 0, 1000 ).values.size() == 3, "GPU query cancellation and subsequent reuse" );
        std::cout << "GPU event store tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
