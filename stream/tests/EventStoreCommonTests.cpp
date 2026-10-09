#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace tracy;
using namespace tracy::stream;
namespace
{
void Check( bool v, const char* m )
{
    if( !v ) throw std::runtime_error( m );
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
void String( std::vector<uint8_t>& b, QueueType type, const char* text )
{
    b.push_back( uint8_t( type ) );
    Bytes( b, uint16_t( strlen( text ) ) );
    b.insert( b.end(), text, text + strlen( text ) );
}
void Fixture( const std::filesystem::path& path, int fault = 0 )
{
    std::string error;
    auto w = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( w ), error.c_str() );
    uint64_t seq = 0;
    auto append = [&]( RecordType t, uint32_t flags, std::span<const uint8_t> b ) { Check( w->Append( t, flags, b, ++seq, error ), error.c_str() ); };
    std::vector<uint8_t> b( 24 );
    b[0] = 2;
    b[2] = 24;
    const uint32_t protocol = ProtocolVersion;
    memcpy( b.data() + 4, &protocol, 4 );
    append( RecordType::SessionBegin, 0, b );
    const uint8_t handshake = HandshakeWelcome;
    append( RecordType::ClientToServer, RecordFlagHandshake, { &handshake, 1 } );
    WelcomeMessage welcome{};
    welcome.timerMul = 1;
    welcome.initBegin = 1000;
    welcome.initEnd = 1010;
    strcpy_s( welcome.programName, "Common domains" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    for( auto [t, key] : { std::pair{ ServerQuerySourceLocation, uint64_t( 0x123 ) }, std::pair{ ServerQueryString, uint64_t( 0x234 ) }, std::pair{ ServerQueryPlotName, uint64_t( 0x345 ) }, std::pair{ ServerQueryPlotName, uint64_t( 0x456 ) } } )
    {
        ServerQueryPacket q{ t, key, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &q ), sizeof( q ) } );
    }
    b.clear();
    auto frame = [&] {std::vector<uint8_t>out(LZ4Size+4);int n=LZ4_compress_default(reinterpret_cast<char*>(b.data()),reinterpret_cast<char*>(out.data()+4),int(b.size()),LZ4Size);Check(n>0,"compress common fixture");memcpy(out.data(),&n,4);out.resize(n+4);append(RecordType::ClientToServer,RecordFlagCompressedFrame,out);b.clear(); };
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    String( b, QueueType::SingleStringData, "inline message" );
    Event( b, QueueType::MessageColor, QueueMessageColor{ { 1100 }, 1, 2, 3 } );
    Event( b, QueueType::MessageLiteral, QueueMessageLiteral{ { 1110 }, 0x234 } );
    String( b, QueueType::SingleStringData, "app info" );
    Event( b, QueueType::MessageAppInfo, QueueMessage{ 1120 } );
    Event( b, QueueType::PlotDataInt, QueuePlotDataInt{ { 0x345, 1100 }, 5 } );
    Event( b, QueueType::PlotDataDouble, QueuePlotDataDouble{ { 0x456, 10 }, 2.5 } );
    Event( b, QueueType::PlotDataFloat, QueuePlotDataFloat{ { 0x345, 5000 }, std::bit_cast<float>( uint32_t( 0x7fc00000 ) ) } );
    Event( b, QueueType::PlotDataInt, QueuePlotDataInt{ { 0x345, 10 }, 9 } );
    Event( b, QueueType::PlotConfig, QueuePlotConfig{ 0x345, 2, 1, 0, 0x123456 } );
    Event( b, QueueType::LockAnnounce, QueueLockAnnounce{ 1, 1090, 0x123, LockType::Lockable } );
    Event( b, QueueType::LockWait, QueueLockWait{ 7, 1, 1100 } );
    Event( b, QueueType::LockObtain, QueueLockWait{ 7, 1, 10 } );
    Event( b, QueueType::LockWait, QueueLockWait{ 8, 1, 10 } );
    Event( b, QueueType::LockMark, QueueLockMark{ 7, 1, 0x123 } );
    frame();
    Event( b, QueueType::LockRelease, QueueLockRelease{ 1, 10 } );
    Event( b, QueueType::LockObtain, QueueLockWait{ 8, 1, 10 } );
    Event( b, QueueType::LockRelease, QueueLockRelease{ 1, 10 } );
    Event( b, QueueType::LockTerminate, QueueLockTerminate{ 1, 1160 } );
    String( b, QueueType::SingleStringData, "Named lock" );
    Event( b, QueueType::LockName, QueueLockName{ 1 } );
    Event( b, QueueType::LockAnnounce, QueueLockAnnounce{ 2, 1160, 0x123, LockType::SharedLockable } );
    Event( b, QueueType::LockSharedWait, QueueLockWait{ 7, 2, 10 } );
    Event( b, QueueType::LockSharedObtain, QueueLockWait{ 7, 2, 10 } );
    Event( b, QueueType::LockWait, QueueLockWait{ 8, 2, 10 } );
    Event( b, QueueType::LockSharedRelease, QueueLockReleaseShared{ { 2, 10 }, 7 } );
    Event( b, QueueType::LockObtain, QueueLockWait{ 8, 2, 10 } );
    Event( b, QueueType::LockRelease, QueueLockRelease{ 2, 10 } );
    Event( b, QueueType::SourceLocation, QueueSourceLocation{ 0, 0, 0, 42, 0, 0, 255 } );
    Event( b, QueueType::StringData, QueueStringTransfer{ 0x234 } );
    Bytes( b, uint16_t( 15 ) );
    const char literal[] = "delayed literal";
    b.insert( b.end(), literal, literal + 15 );
    for( uint64_t key : { 0x345, 0x456 } )
    {
        Event( b, QueueType::PlotName, QueueStringTransfer{ key } );
        Bytes( b, uint16_t( 6 ) );
        const char text[] = "merged";
        b.insert( b.end(), text, text + 6 );
    }
    if( fault == 1 ) Event( b, QueueType::LockRelease, QueueLockRelease{ 1, 0 } );
    if( fault == 2 ) Event( b, QueueType::LockWait, QueueLockWait{ 7, 99, 0 } );
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
    auto root = std::filesystem::temp_directory_path() / std::filesystem::path( "tracy-store-common-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        auto p = root / "common.tracy-stream";
        Fixture( p );
        auto info = EventStore::Build( p );
        Check( info.messages == 2 && info.plotPoints == 3 && info.lockEvents == 12, "common-domain counts" );
        auto store = EventStore::Open( p );
        auto messages = store->Messages( 100, 111 );
        Check( messages.values.size() == 2 && messages.values[0].text == "inline message" && messages.values[0].color == 0xFF010203 && messages.values[1].text == "delayed literal", "message color, absolute clock and delayed literal definition" );
        Check( store->Messages( 110, 111 ).values.size() == 1, "message half-open boundary" );
        Check( store->Messages( 0, 1000, {}, 0, 256, true ).values[0].text == "app info", "app info remains distinct" );
        auto plots = store->Plots();
        Check( plots.values.size() == 1 && plots.values[0].name == "merged" && plots.values[0].points == 3 && plots.values[0].min == 2.5 && plots.values[0].max == 9 && plots.values[0].sum == 16.5, "plot name aliases merge with exact points and stats" );
        Check( plots.values[0].format == 2 && plots.values[0].steps && !plots.values[0].fill, "plot config" );
        auto points = store->PlotPoints( 0, 1000 );
        Check( points.values.size() == 3 && points.values[2].time == 120, "nonfinite Plot does not advance thread clock" );
        auto locks = store->Locks();
        Check( locks.values.size() == 2 && locks.values[0].name == "Named lock" && locks.values[0].contended && locks.values[0].terminate == 160, "lock lifecycle and contention" );
        auto events = store->LockEvents( 0, 1000, 1 );
        Check( events.values.size() == 6 && events.values[1].line == 42, "lock mark modifies existing acquisition" );
        Check( events.values[2].owner == 7 && events.values[2].waiters == std::vector<uint64_t>{ 8 }, "exclusive owner and waiter snapshot" );
        Check( events.values[4].owner == 8 && events.values[4].waiters.empty(), "ownership transitions" );
        auto shared = store->LockEvents( 0, 1000, 2 );
        Check( shared.values.size() == 6 && shared.values[1].sharedOwners == std::vector<uint64_t>{ 7 } && shared.values[2].waiters == std::vector<uint64_t>{ 8 } && locks.values[1].contended, "shared ownership and exclusive wait" );
        auto first = store->Messages( 0, 1000, {}, 0, 1 );
        Check( first.more && store->Message( first.values[0].id )->text == first.values[0].text, "message stable ID and paging" );
        store.reset();
        store = EventStore::Open( p );
        Check( store->Plots().values[0].points == 3, "reopen common domains" );
        for( int fault = 1; fault <= 2; ++fault )
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
            Check( rejected && !std::filesystem::exists( EventStore::Directory( bad ) ), "invalid lock lifetime/state is not published" );
        }
        std::cout << "Common domain tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
