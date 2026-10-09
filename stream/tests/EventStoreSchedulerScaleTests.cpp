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
        const auto root = std::filesystem::temp_directory_path() / std::filesystem::path( "tracy-store-scheduler-scale-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
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
        strcpy_s( welcome.programName, "Scheduler scale" );
        append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
        b.clear();
        auto flush = [&] {if(b.empty())return;std::vector<uint8_t>out(LZ4Size+4);int n=LZ4_compress_default(reinterpret_cast<char*>(b.data()),reinterpret_cast<char*>(out.data()+4),int(b.size()),LZ4Size);Check(n>0,"compress scale");memcpy(out.data(),&n,4);out.resize(n+4);append(RecordType::ClientToServer,RecordFlagCompressedFrame,out);b.clear(); };
        constexpr uint32_t Count = 100000;
        int64_t reference = 0;
        for( uint32_t n = 0; n < Count; ++n )
        {
            const uint32_t tid = 1000 + n;
            const int64_t time = 1100 + int64_t( n ) * 1800000000000ll / Count;
            Event( b, QueueType::CallstackSampleDictionary, QueueStringTransfer{ uint64_t( n ) + 1 } );
            Bytes( b, uint16_t( 8 ) );
            Bytes( b, uint64_t( 0x10000 + n ) );
            Event( b, QueueType::ThreadWakeup, QueueThreadWakeup{ time - reference, tid, 0, 0, 0 } );
            reference = time;
            Event( b, QueueType::ContextSwitch, QueueContextSwitch{ 0, 0, tid, 0, 0, 0, 0, 0, 0 } );
            Event( b, QueueType::CallstackSampleRef, QueueCallstackSampleRef{ { 0, tid }, n + 1 } );
            Event( b, QueueType::CallstackSampleRef, QueueCallstackSampleRef{ { 0, tid }, n + 1 } );
            Event( b, QueueType::ContextSwitch, QueueContextSwitch{ 1000, tid, 0, 0, 6, 5, 0, 0, 0 } );
            reference += 1000;
            Event( b, QueueType::CallstackSampleRef, QueueCallstackSampleRef{ { 1000, tid }, n + 1 } );
            reference += 1000;
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
        Check( info.samples == Count * 2 && info.contextRegions == Count && info.cpuRegions == Count, "scheduler high-cardinality counts" );
        uint64_t cursor = 0, total = 0;
        for( ;; )
        {
            auto page = store->Samples( INT64_MIN, INT64_MAX, {}, cursor, 512 );
            total += page.values.size();
            if( !page.more ) break;
            cursor = page.next;
        }
        Check( total == Count * 2, "every high-cardinality sample remains paged" );
        auto first = store->Samples( 0, 1000, 1000 ).values.front();
        Check( first.contextSwitch && store->Stack( first.stack ) == std::vector<uint64_t>{ 0x10000, 0x10000 }, "sample merge and classification at scale" );
        for( int minute : { 1, 10, 20, 29 } )
        {
            auto time = int64_t( minute ) * 60000000000ll;
            Check( !store->Samples( time, time + 1000000000ll ).values.empty() && !store->CpuRegions( time, time + 1000000000ll ).values.empty(), "random long scheduling windows" );
        }
        monitor.request_stop();
        monitor.join();
        Check( peakCommit.load() < 512ull << 20, "scheduler sample stays within process experiment budget" );
        std::cout << "Scheduler scale passed; samples=" << info.samples << " dictionaries=" << Count << " threads=" << Count << " seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() << " sampled_peak_commit=" << peakCommit.load() << " sampled_peak_working_set=" << peakWorkingSet.load() << " evidence=" << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
