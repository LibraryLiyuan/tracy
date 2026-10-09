#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace tracy;
using namespace tracy::stream;
namespace
{
void Check( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}
template<class T>
void Bytes( std::vector<uint8_t>& b, const T& v )
{
    auto p = reinterpret_cast<const uint8_t*>( &v );
    b.insert( b.end(), p, p + sizeof( v ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType t, const T& v )
{
    b.push_back( uint8_t( t ) );
    Bytes( b, v );
}
void Fixture( const std::filesystem::path& path, bool combine = false, int fault = 0 )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t seq = 0;
    auto append = [&]( RecordType t, uint32_t flags, std::span<const uint8_t> bytes ) { Check( writer->Append( t, flags, bytes, ++seq, error ), error.c_str() ); };
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
    welcome.flags = combine ? WelcomeFlag::CombineSamples : 0;
    strcpy_s( welcome.programName, "Scheduler fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    b.clear();
    auto frame = [&] {std::vector<uint8_t>out(LZ4Size+4);int n=LZ4_compress_default(reinterpret_cast<char*>(b.data()),reinterpret_cast<char*>(out.data()+4),int(b.size()),LZ4Size);Check(n>0,"compress scheduler frame");memcpy(out.data(),&n,4);out.resize(n+4);append(RecordType::ClientToServer,RecordFlagCompressedFrame,out);b.clear(); };
    Event( b, QueueType::CallstackSampleDictionary, QueueStringTransfer{ 1 } );
    Bytes( b, uint16_t( 8 ) );
    Bytes( b, uint64_t( 0x12345 ) );
    Event( b, QueueType::ThreadWakeup, QueueThreadWakeup{ 1100, 7, 1, 0, 0 } );
    Event( b, QueueType::ContextSwitch, QueueContextSwitch{ 10, 0, 7, 1, 0, 0, 0, 0, 0 } );
    Event( b, QueueType::CallstackSampleRef, QueueCallstackSampleRef{ { 0, 7 }, 1 } );
    frame();
    // Wakeup arrives while the thread still appears to be running.
    Event( b, QueueType::ThreadWakeup, QueueThreadWakeup{ 10, 7, 2, 0, 0 } );
    Event( b, QueueType::ContextSwitch, QueueContextSwitch{ 10, 7, 8, 1, 6, 5, 0, 0, 0 } );
    Event( b, QueueType::CallstackPayload, QueueStringTransfer{ 2 } );
    Bytes( b, uint16_t( 8 ) );
    Bytes( b, uint64_t( 0x67890 ) );
    Event( b, QueueType::CallstackSample, QueueCallstackSample{ 0, 8 } );
    Event( b, QueueType::CallstackSampleRef, QueueCallstackSampleRef{ { 0, 8 }, 1 } );
    Event( b, QueueType::ContextSwitch, QueueContextSwitch{ 10, 0, 7, 2, 0, 0, 0, 0, 0 } );
    Event( b, QueueType::CallstackSampleContextSwitchRef, QueueCallstackSampleRef{ { 0, 7 }, 1 } );
    Event( b, QueueType::ContextSwitch, QueueContextSwitch{ 10, 7, 0, 2, 7, 6, 0, 0, 0 } );
    Event( b, QueueType::ContextSwitch, QueueContextSwitch{ 0, 8, 0, 1, 6, 5, 0, 0, 0 } );
    Event( b, QueueType::CallstackSampleRef, QueueCallstackSampleRef{ { 10, 7 }, 1 } );
    if( fault == 1 ) Event( b, QueueType::CallstackSampleRef, QueueCallstackSampleRef{ { 0, 7 }, 99 } );
    if( fault == 2 )
    {
        Event( b, QueueType::CallstackSampleDictionary, QueueStringTransfer{ 1 } );
        Bytes( b, uint16_t( 8 ) );
        Bytes( b, uint64_t( 0x12345 ) );
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
    auto root = std::filesystem::temp_directory_path() / std::filesystem::path( "tracy-store-scheduler-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        auto p = root / "scheduler.tracy-stream";
        Fixture( p );
        auto info = EventStore::Build( p );
        Check( info.samples == 4, "four projected samples including merged equal timestamps" );
        auto store = EventStore::Open( p );
        auto samples = store->Samples( 0, 1000 );
        Check( samples.values.size() == 4, "all scheduler samples" );
        Check( samples.values[0].time == 110 && samples.values[0].contextSwitch, "sample at switch-in is classified without statistics cache" );
        Check( store->Stack( samples.values[1].stack ) == std::vector<uint64_t>{ 0x67890, 0x12345 }, "equal timestamp sample stacks concatenate in protocol order" );
        Check( samples.values[2].explicitContextSwitch && samples.values[2].time == 140, "dictionary context sample and shared scheduling clock" );
        auto regions = store->ContextRegions( 115, 145, 7 );
        Check( regions.values.size() == 2, "context interval overlap and migration" );
        Check( regions.values[0].start == 110 && regions.values[0].end == 130 && regions.values[0].wakeup == 100, "wakeup placeholder retains identity and original wakeup" );
        Check( regions.values[1].start == 140 && regions.values[1].end == 150 && regions.values[1].wakeup == 120 && regions.values[1].wakeupCpu == 2, "early wakeup applied after switch-out" );
        Check( store->ContextRegions( 130, 140, 7 ).values.empty(), "scheduler half-open boundaries" );
        auto cpus = store->CpuRegions( 0, 1000 );
        Check( cpus.values.size() == 3 && cpus.values[0].cpu == 1 && cpus.values[0].thread == 7 && cpus.values[1].thread == 8, "per-CPU thread ownership intervals" );
        auto page = store->Samples( 0, 1000, {}, 0, 1 );
        uint64_t count = page.values.size();
        while( page.more )
        {
            page = store->Samples( 0, 1000, {}, page.next, 1 );
            count += page.values.size();
        }
        Check( count == 4, "sample paging has no loss" );
        const auto stable = regions.values[0].id;
        store.reset();
        store = EventStore::Open( p );
        Check( store->ContextRegions( 115, 125, 7 ).values[0].id == stable, "wakeup-derived interval identity survives reopening" );
        auto combined = root / "combined.tracy-stream";
        Fixture( combined, true );
        auto ci = EventStore::Build( combined );
        auto cs = EventStore::Open( combined );
        Check( ci.pendingSamples > 0 && cs->Samples( 0, 1000 ).values.size() > cs->Samples( 0, 1000, {}, 0, 256, false ).values.size(), "combined sampling preserves last pending packet explicitly" );
        for( int fault = 1; fault <= 2; ++fault )
        {
            auto bad = root / ( "bad-" + std::to_string( fault ) + ".tracy-stream" );
            Fixture( bad, false, fault );
            bool rejected = false;
            try
            {
                EventStore::Build( bad );
            }
            catch( const std::exception& )
            {
                rejected = true;
            }
            Check( rejected && !std::filesystem::exists( EventStore::Directory( bad ) ), "missing or duplicate sample dictionary rejects publication" );
        }
        std::cout << "Scheduler tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
