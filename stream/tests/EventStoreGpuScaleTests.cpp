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
#include <tuple>

using namespace tracy;
using namespace tracy::stream;
void Check( bool b, const char* m )
{
    if( !b ) throw std::runtime_error( m );
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
int main()
{
    try
    {
        auto root = std::filesystem::temp_directory_path() / std::filesystem::path( "tracy-store-gpu-scale-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        std::filesystem::create_directory( root );
        auto path = root / "scale.tracy-stream";
        std::string error;
        auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
        Check( bool( writer ), error.c_str() );
        uint64_t recordTime = 0;
        auto append = [&]( RecordType t, uint32_t f, std::span<const uint8_t> p ) { Check( writer->Append( t, f, p, ++recordTime, error ), error.c_str() ); };
        std::vector<uint8_t> bytes( 24 );
        bytes[0] = 2;
        bytes[2] = 24;
        uint32_t version = ProtocolVersion;
        memcpy( bytes.data() + 4, &version, 4 );
        append( RecordType::SessionBegin, 0, bytes );
        uint8_t status = HandshakeWelcome;
        append( RecordType::ClientToServer, RecordFlagHandshake, { &status, 1 } );
        WelcomeMessage welcome{};
        welcome.timerMul = 1;
        welcome.initBegin = 1000;
        welcome.initEnd = 1010;
        strcpy_s( welcome.programName, "GpuThirtyMinuteScaleFixture" );
        append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<uint8_t*>( &welcome ), sizeof( welcome ) } );
        constexpr uint64_t Definitions = 100000, Depth = 10000;
        bytes.clear();
        for( uint64_t n = 0; n < Definitions; ++n )
        {
            Bytes( bytes, ServerQueryPacket{ ServerQuerySourceLocation, 0x10000 + n, 0 } );
            if( bytes.size() > 65536 )
            {
                append( RecordType::ServerToClient, RecordFlagServerQuery, bytes );
                bytes.clear();
            }
        }
        if( !bytes.empty() ) append( RecordType::ServerToClient, RecordFlagServerQuery, bytes );
        bytes.clear();
        auto flush = [&] {if(bytes.empty())return;std::vector<uint8_t> output(LZ4Size+4);int n=LZ4_compress_default(reinterpret_cast<char*>(bytes.data()),reinterpret_cast<char*>(output.data()+4),int(bytes.size()),LZ4Size);Check(n>0,"compress scale frame");memcpy(output.data(),&n,4);output.resize(n+4);append(RecordType::ClientToServer,RecordFlagCompressedFrame,output);bytes.clear(); };
        for( uint64_t n = 0; n < Definitions; ++n )
        {
            Event( bytes, QueueType::SourceLocation, QueueSourceLocation{ 0, 0, 0, uint32_t( n ), 0, 0, 255 } );
            if( bytes.size() > 65536 ) flush();
        }
        flush();
        Event( bytes, QueueType::GpuNewContext, QueueGpuNewContext{ 1000, 0, 0, 1.f, 1, GpuContextFlags( 0 ), GpuContextType::Direct3D12 } );
        for( uint64_t n = 0; n < Depth; ++n )
        {
            Event( bytes, QueueType::GpuZoneBeginSerial, QueueGpuZoneBegin{ { n ? 1 : 1100, 1, uint16_t( n + 1 ), 1 }, 0x10000 + n } );
            if( bytes.size() > 65536 ) flush();
        }
        for( uint64_t n = 0; n < Depth; ++n )
        {
            Event( bytes, QueueType::GpuZoneEndSerial, QueueGpuZoneEnd{ 1, 1, uint16_t( Depth + n + 1 ), 1 } );
            if( bytes.size() > 65536 ) flush();
        }
        flush();
        for( uint64_t n = 0; n < Depth * 2; ++n )
        {
            Event( bytes, QueueType::GpuTime, QueueGpuTime{ n ? 1 : 100, uint16_t( n + 1 ), 1 } );
            if( bytes.size() > 65536 ) flush();
        }
        flush();
        int64_t cpuReference = 1100 + int64_t( Depth ) * 2 - 1;
        int64_t gpuReference = 100 + int64_t( Depth ) * 2 - 1;
        // Independent zones span 30 minutes without a 30-minute wait.
        bytes.clear();
        for( uint64_t n = 0; n < Definitions; ++n )
        {
            int64_t time = 1100 + int64_t( n ) * 1800000000000ll / Definitions;

            Event( bytes, QueueType::GpuZoneBeginSerial, QueueGpuZoneBegin{ { time - cpuReference, uint32_t( 1000 + n ), 1, 1 }, 0x10000 + n } );
            Event( bytes, QueueType::GpuZoneEndSerial, QueueGpuZoneEnd{ 1000000, uint32_t( 1000 + n ), 2, 1 } );
            cpuReference = time + 1000000;
            Event( bytes, QueueType::GpuTime, QueueGpuTime{ time - 1000 - gpuReference, 1, 1 } );
            Event( bytes, QueueType::GpuTime, QueueGpuTime{ 1000000, 2, 1 } );
            gpuReference = time - 1000 + 1000000;

            if( bytes.size() > 65536 ) flush();
        }
        flush();
        Event( bytes, QueueType::FrameMarkMsg, QueueFrameMark{ 1800000001000ll, 0 } );
        flush();
        bytes.assign( 32, 0 );
        bytes[0] = 1;
        bytes[2] = 32;
        bytes[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, bytes );
        writer.reset();
        auto begin = std::chrono::steady_clock::now();
        std::atomic<uint64_t> peakCommit{ 0 }, peakWorkingSet{ 0 };
        std::jthread monitor( [&]( std::stop_token stop ) {while(!stop.stop_requested()){auto m=GetProcessMemoryState();peakCommit=std::max(peakCommit.load(),m.commit);peakWorkingSet=std::max(peakWorkingSet.load(),m.workingSet);std::this_thread::sleep_for(std::chrono::milliseconds(50));} } );
        StoreOptions options;
        options.cacheBytes = 4ull << 20;
        auto info = EventStore::Build( path, options );
        auto store = EventStore::Open( path, options );
        Check( info.gpuZones == Definitions + Depth, "scale event count" );
        Check( info.lastNs >= 1800000000000ll, "30-minute event-time coverage" );
        auto nested = store->GpuZones( 100, 20000, 1, 0, 1 );
        Check( nested.more && store->GpuZone( nested.values.front().id )->depth == 0, "GPU deep parents remain accessible" );
        auto rootId = nested.values.front().id;
        auto saved = *store->GpuZone( rootId );
        uint64_t scanned = 0, cursor = 0;
        for( ;; )
        {
            auto page = store->GpuZones( INT64_MIN, INT64_MAX, {}, cursor, 512 );
            scanned += page.values.size();
            if( !page.more ) break;
            cursor = page.next;
        }
        Check( scanned == info.gpuZones, "all GPU details are paged without loss" );
        Check( store->GpuZone( rootId )->gpuEnd == saved.gpuEnd, "GPU stable identity after cache turnover" );
        for( int minute : { 1, 10, 20, 29 } )
        {
            const auto time = int64_t( minute ) * 60000000000ll;
            Check( !store->GpuZones( time, time + 1000000000ll ).values.empty(), "GPU random 30-minute window" );
        }
        auto context = store->GpuContexts().values.front();
        Check( context.zones == info.gpuZones && context.incomplete == 0, "GPU aggregate context counts" );
        auto memory = GetProcessMemoryState();
        monitor.request_stop();
        monitor.join();
        Check( peakCommit.load() < ( 512ull << 20 ), "small-cache scale sample remains within the experimental process budget" );
        std::cout << "GPU scale tests passed; zones=" << info.gpuZones << " definitions=" << Definitions << " seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - begin ).count() << " private_commit=" << memory.commit << " working_set=" << memory.workingSet << " sampled_peak_commit=" << peakCommit.load() << " sampled_peak_working_set=" << peakWorkingSet.load() << " evidence=" << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
