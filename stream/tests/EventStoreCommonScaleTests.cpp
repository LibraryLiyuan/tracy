#include "../../capture/src/ProcessMemory.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace tracy;
using namespace tracy::stream;
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
void Event( std::vector<uint8_t>& b, QueueType type, const T& v )
{
    b.push_back( uint8_t( type ) );
    Bytes( b, v );
}
int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() / std::filesystem::path( "tracy-store-common-scale-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        std::filesystem::create_directory( root );
        const auto path = root / "scheduler.tracy-stream";
        std::string error;
        auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
        Check( bool( writer ), error.c_str() );
        uint64_t seq = 0;
        auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> b ) { Check( writer->Append( type, flags, b, ++seq, error ), error.c_str() ); };
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
        strcpy_s( welcome.programName, "Common scale" );
        append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
        b.clear();
        auto flush = [&] {if(b.empty())return;std::vector<uint8_t>out(LZ4Size+4);int n=LZ4_compress_default(reinterpret_cast<char*>(b.data()),reinterpret_cast<char*>(out.data()+4),int(b.size()),LZ4Size);Check(n>0,"compress scale");memcpy(out.data(),&n,4);out.resize(n+4);append(RecordType::ClientToServer,RecordFlagCompressedFrame,out);b.clear(); };
        constexpr uint32_t Count = 50000;
        int64_t threadRef = 0, serialRef = 0;
        ServerQueryPacket query{ ServerQuerySourceLocation, 0x123, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
        Event( b, QueueType::SourceLocation, QueueSourceLocation{ 0, 0, 0, 42, 0, 0, 255 } );
        flush();
        for( uint32_t n = 0; n < Count; ++n )
        {
            const uint64_t key = 0x10000 + n;
            ServerQueryPacket q{ ServerQueryPlotName, key, 0 };
            append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &q ), sizeof( q ) } );
            const int64_t time = 1100 + int64_t( n ) * 1800000000000ll / Count;
            Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
            threadRef = 0;
            Event( b, QueueType::PlotDataInt, QueuePlotDataInt{ { key, time - threadRef }, int64_t( n ) } );
            threadRef = time;
            Event( b, QueueType::PlotName, QueueStringTransfer{ key } );
            const auto name = "plot-" + std::to_string( n );
            Bytes( b, uint16_t( name.size() ) );
            b.insert( b.end(), name.begin(), name.end() );
            b.push_back( uint8_t( QueueType::SingleStringData ) );
            Bytes( b, uint16_t( 7 ) );
            b.insert( b.end(), { 'm', 'e', 's', 's', 'a', 'g', 'e' } );
            Event( b, QueueType::Message, QueueMessage{ time } );
            Event( b, QueueType::LockAnnounce, QueueLockAnnounce{ n, time, 0x123, LockType::Lockable } );
            Event( b, QueueType::LockWait, QueueLockWait{ 7, n, time - serialRef } );
            serialRef = time;
            Event( b, QueueType::LockObtain, QueueLockObtain{ 7, n, 1 } );
            ++serialRef;
            Event( b, QueueType::LockRelease, QueueLockRelease{ n, 1 } );
            ++serialRef;
            Event( b, QueueType::LockTerminate, QueueLockTerminate{ n, time + 3 } );
            if( b.size() > 65536 ) flush();
        }
        flush();
        b.assign( 32, 0 );
        b[0] = 1;
        b[2] = 32;
        b[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, b );
        writer.reset();
        std::atomic<uint64_t> peakCommit{ 0 }, peakWorkingSet{ 0 };
        std::jthread monitor( [&]( std::stop_token stop ) {while(!stop.stop_requested()){auto m=GetProcessMemoryState();peakCommit=std::max(peakCommit.load(),m.commit);peakWorkingSet=std::max(peakWorkingSet.load(),m.workingSet);std::this_thread::sleep_for(std::chrono::milliseconds(50));} } );
        const auto start = std::chrono::steady_clock::now();
        StoreOptions options;
        options.cacheBytes = 4ull << 20;
        auto info = EventStore::Build( path, options );
        auto store = EventStore::Open( path, options );
        Check( info.messages == Count && info.plotPoints == Count && info.lockEvents == Count * 3, "common high-cardinality counts" );
        uint64_t cursor = 0, plots = 0;
        for( ;; )
        {
            auto page = store->Plots( cursor, 64 );
            plots += page.values.size();
            if( !page.more ) break;
            cursor = page.next;
        }
        Check( plots == Count, "all independent plot definitions remain paged" );
        cursor = 0;
        uint64_t locks = 0;
        for( ;; )
        {
            auto page = store->Locks( cursor, 64 );
            locks += page.values.size();
            if( !page.more ) break;
            cursor = page.next;
        }
        Check( locks == Count, "all lock definitions including zero ID remain paged" );
        for( int minute : { 1, 10, 20, 29 } )
        {
            auto time = int64_t( minute ) * 60000000000ll;
            Check( !store->Messages( time, time + 1000000000ll ).values.empty() && !store->PlotPoints( time, time + 1000000000ll ).values.empty() && !store->LockEvents( time, time + 1000000000ll ).values.empty(), "long common-domain random windows" );
        }
        monitor.request_stop();
        monitor.join();
        Check( peakCommit.load() < 512ull << 20, "scheduler sample stays within process experiment budget" );
        std::cout << "Common scale passed; messages=" << info.messages << " plots=" << Count << " locks=" << Count << " seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() << " sampled_peak_commit=" << peakCommit.load() << " sampled_peak_working_set=" << peakWorkingSet.load() << " evidence=" << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
