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
void Check( bool v, const char* m ) { if( !v ) throw std::runtime_error( m ); }
template<class T> void Bytes( std::vector<uint8_t>& b, const T& v )
{
    const auto p = reinterpret_cast<const uint8_t*>( &v );
    b.insert( b.end(), p, p + sizeof( v ) );
}
template<class T> void Event( std::vector<uint8_t>& b, QueueType t, const T& v )
{
    b.push_back( uint8_t( t ) );
    Bytes( b, v );
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-store-task-scale-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto path = root / "tasks.tracy-stream";
        std::string error;
        auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
        Check( bool( writer ), error.c_str() );
        uint64_t seq = 0;
        auto append = [&]( RecordType t, uint32_t flags, std::span<const uint8_t> b ) { Check( writer->Append( t, flags, b, ++seq, error ), error.c_str() ); };
        std::vector<uint8_t> b( 24 );
        b[0] = 2; b[2] = 24;
        const uint32_t protocol = ProtocolVersion;
        memcpy( b.data() + 4, &protocol, 4 );
        append( RecordType::SessionBegin, 0, b );
        const uint8_t handshake = HandshakeWelcome;
        append( RecordType::ClientToServer, RecordFlagHandshake, { &handshake, 1 } );
        WelcomeMessage welcome{};
        welcome.initBegin = 1000; welcome.initEnd = 1010; welcome.timerMul = 1;
        strcpy_s( welcome.programName, "Task scale" );
        append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
        b.clear();
        auto flush = [&] {
            if( b.empty() ) return;
            std::vector<uint8_t> out( LZ4Size + 4 );
            const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size );
            Check( n > 0, "compress task scale" );
            memcpy( out.data(), &n, 4 ); out.resize( n + 4 );
            append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear();
        };
        constexpr uint32_t Count = 50000;
        constexpr uint64_t JobBase = 0xf000000000000000ull, IoBase = 0xe000000000000000ull;
        const auto timestamp = []( uint32_t n ) { return int64_t( 1100 ) + int64_t( n ) * 1800000000000ll / Count; };
        for( uint32_t n = 0; n < Count; ++n )
        {
            const uint64_t key = 0x10000 + n, job = JobBase + n, io = IoBase + n;
            const auto time = timestamp( n );
            ServerQueryPacket query{ ServerQueryString, key, 0 };
            append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
            Event( b, QueueType::StringData, QueueStringTransfer{ key } );
            const auto name = "task-" + std::to_string( n );
            Bytes( b, uint16_t( name.size() ) ); b.insert( b.end(), name.begin(), name.end() );
            Event( b, QueueType::ThreadContext, QueueThreadContext{ n + 1 } );
            Event( b, QueueType::JnFrame, QueueJnFrame{ time, 0x100000000ull + n + 1, n, 1, 0, 1 } );
            Event( b, QueueType::JnJobType, QueueJnJobType{ key, n + 1, 1, 0 } );
            const QueueJnJobConfig config{ job, n + 1, 64, 8, n, n + 1, 7, 64 };
            if( n % 2 == 0 ) Event( b, QueueType::JnJobConfig, config );
            Event( b, QueueType::JnJobSchedule, QueueJnJobSchedule{ time, job, n * 10, uint16_t( n != 0 ), 1, 0 } );
            if( n % 2 != 0 ) Event( b, QueueType::JnJobConfig, config );
            if( n ) Event( b, QueueType::JnJobDependency, QueueJnJobDependency{ job, job - 1, ( n - 1 ) * 10ull, 0 } );
            Event( b, QueueType::JnJobStage, QueueJnJobStage{ time + 60, job, 0, n % 16, 0, uint8_t( JnJobStage::Ready ), 3 } );
            Event( b, QueueType::JnJobStage, QueueJnJobStage{ time + 70, job, 0, n % 16, 0, uint8_t( JnJobStage::QueueEnter ), 64 } );
            Event( b, QueueType::JnJobStage, QueueJnJobStage{ time + 80, job, 0, n % 16, 0, uint8_t( JnJobStage::Dispatch ), 2 } );
            constexpr JnJobStage begin[] = { JnJobStage::WorkerSliceBegin, JnJobStage::WaitBegin, JnJobStage::WaitActiveHelpBegin, JnJobStage::WaitSpinYieldBegin, JnJobStage::WaitSleepBegin };
            for( uint32_t span = 0; span < std::size( begin ); ++span ) Event( b, QueueType::JnJobStage, QueueJnJobStage{ time + 100 + span, job, span, 0, 0, uint8_t( begin[span] ), uint8_t( span == 0 ? 128 : 0 ) } );
            Event( b, QueueType::JnIoConfig, QueueJnIoConfig{ io, job, ( 1ull << 40 ) + n, n + 1, 3, 0 } );
            Event( b, QueueType::JnIoStage, QueueJnIoStage{ time + 110, io, 0, 0, 0, 0, 0 } );
            Event( b, QueueType::JnIoRequest, QueueJnIoRequest{ time + 90, io, n, 0, 0, 1, 0, 0 } );
            Event( b, QueueType::JnScriptFrame, QueueJnScriptFrame{ key, key, n + 1, n, 0, 0 } );
            Event( b, QueueType::JnScriptStack, QueueJnScriptStack{ time + 120, job, 0, n + 1, 0, 0, uint8_t( JnScriptRecordKind::ZoneBegin ) } );
            if( b.size() > 100000 ) flush();
        }
        flush();
        // All 250,000 span starts and 50,000 I/O starts remain active across
        // records until this second pass; none may require an in-memory map.
        for( uint32_t n = 0; n < Count; ++n )
        {
            const auto time = timestamp( n );
            const uint64_t job = JobBase + n, io = IoBase + n;
            Event( b, QueueType::ThreadContext, QueueThreadContext{ n + 1 } );
            constexpr JnJobStage end[] = { JnJobStage::WorkerSliceEnd, JnJobStage::WaitEnd, JnJobStage::WaitActiveHelpEnd, JnJobStage::WaitSpinYieldEnd, JnJobStage::WaitSleepEnd };
            for( uint32_t span = 0; span < std::size( end ); ++span ) Event( b, QueueType::JnJobStage, QueueJnJobStage{ time + 150 + span, job, span, 0, 0, uint8_t( end[span] ), 0 } );
            Event( b, QueueType::JnJobStage, QueueJnJobStage{ time + 200, job, 0, 0, 0, uint8_t( JnJobStage::Completed ), 0 } );
            Event( b, QueueType::JnIoStage, QueueJnIoStage{ time + 210, io, ( 1ull << 40 ) + n, 0, 1, 1, 0 } );
            Event( b, QueueType::JnScriptStack, QueueJnScriptStack{ time + 220, job, 0, 0, 0, 0, uint8_t( JnScriptRecordKind::ZoneEnd ) } );
            if( b.size() > 100000 ) flush();
        }
        Event( b, QueueType::JnFrame, QueueJnFrame{ 1800000001000ll, 0x300000000ull, Count, 1, 2, 0 } );
        flush();
        b.assign( 32, 0 ); b[0] = 1; b[2] = 32; b[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, b ); writer.reset();
        std::atomic<uint64_t> peakCommit{ 0 }, peakWorkingSet{ 0 };
        std::jthread monitor( [&]( std::stop_token stop ) {
            while( !stop.stop_requested() )
            {
                const auto memory = GetProcessMemoryState();
                peakCommit = std::max( peakCommit.load(), memory.commit );
                peakWorkingSet = std::max( peakWorkingSet.load(), memory.workingSet );
                std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            }
        } );
        const auto start = std::chrono::steady_clock::now();
        StoreOptions options; options.cacheBytes = 4ull << 20; options.resultBytes = 65536;
        const auto info = EventStore::Build( path, options );
        auto store = EventStore::Open( path, options );
        Check( info.jobs == Count && info.ioRequests == Count && info.scriptEvents == Count * 2, "task high-cardinality counts" );
        uint64_t cursor = 0, seen = 0;
        for( ;; )
        {
            const auto page = store->Jobs( INT64_MIN, INT64_MAX, cursor, 4096 );
            Check( page.values.size() * sizeof( StoreJob ) <= options.resultBytes, "Job result byte budget" );
            for( const auto& job : page.values )
            {
                const auto n = uint32_t( job.id - JobBase );
                Check( job.name == "task-" + std::to_string( n ) && job.originFrame == 0x100000000ull + n + 1 && job.kind == 7 && job.flags == 64 && !job.orphan, "high-cardinality definitions and early/late config identity" );
                Check( job.execution == 50 && job.wait == 50 && job.activeHelp == 50 && job.spin == 50 && job.sleep == 50 && job.rangeSteals == 1 && job.retries == 1 && job.waitEnds == 1, "all active span classes and counters" );
                if( n ) Check( job.dependencyLatency == 1800000000000ll / Count - 140, "indexed dependency latency" );
            }
            seen += page.values.size();
            if( !page.more ) break;
            Check( page.next > cursor, "Job pagination progresses" ); cursor = page.next;
        }
        Check( seen == Count, "every Job remains accessible under result budget" );
        for( int minute : { 1, 10, 20, 29 } )
        {
            const int64_t time = int64_t( minute ) * 60000000000ll;
            Check( !store->Jobs( time, time + 1000000000ll ).values.empty() && !store->IoRequests( time, time + 1000000000ll ).values.empty() && !store->ScriptEvents( time, time + 1000000000ll ).values.empty(), "30-minute task random windows" );
        }
        const auto lastIo = store->IoRequest( IoBase + Count - 1 );
        Check( lastIo && lastIo->requested == ( 1ull << 40 ) + Count - 1 && lastIo->transferred == lastIo->requested && lastIo->parent == JobBase + Count - 1, "64-bit I/O bytes and parent identity" );
        cursor = 0; seen = 0;
        for( ;; )
        {
            const auto page = store->ScriptFrames( cursor, 4096 ); seen += page.values.size();
            if( !page.more ) break;
            Check( page.next > cursor, "Script definition pagination progresses" ); cursor = page.next;
        }
        Check( seen == Count, "all script definitions remain paged" );
        store.reset(); store = EventStore::Open( path, options );
        Check( store->Job( JobBase )->execution == 50 && store->JobLanes( JobBase ).values == std::vector<uint32_t>{ 0 }, "reopen after cache eviction keeps unsigned Job identity and lane zero" );
        monitor.request_stop(); monitor.join();
        Check( peakCommit.load() < 128ull << 20, "task scale process experiment budget" );
        std::cout << "Task scale passed; jobs=" << info.jobs << " io=" << info.ioRequests << " scripts=" << info.scriptEvents << " seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() << " sampled_peak_commit=" << peakCommit.load() << " sampled_peak_working_set=" << peakWorkingSet.load() << " evidence=" << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e ) { std::cerr << e.what() << "\nEvidence: " << root.string() << '\n'; return 1; }
}
