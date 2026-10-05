#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include "sqlite3.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace tracy;
using namespace tracy::stream;
void Check( bool v, const char* m )
{
    if( !v ) throw std::runtime_error( m );
}
template<class T>
void Bytes( std::vector<uint8_t>& b, const T& v )
{
    const auto p = reinterpret_cast<const uint8_t*>( &v );
    b.insert( b.end(), p, p + sizeof( v ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType type, const T& v )
{
    b.push_back( uint8_t( type ) );
    Bytes( b, v );
}
void Fixture( const std::filesystem::path& path, int invalid = 0 )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t sequence = 0;
    const auto append = [&]( RecordType t, uint32_t flags, std::span<const uint8_t> b ) { Check( writer->Append( t, flags, b, ++sequence, error ), error.c_str() ); };
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
    welcome.flags=invalid?WelcomeFlag::OnDemand:0;
    welcome.resolution = 123;
    welcome.epoch = 456;
    welcome.exectime = 789;
    welcome.pid = 0x100000001ull;
    welcome.samplingPeriod = 321;
    welcome.cpuArch = CpuArchX64;
    welcome.cpuId = 0x12345678;
    memcpy( welcome.cpuManufacturer, "Manufacturer", 12 );
    strcpy_s( welcome.hostInfo, "Fixture host" );
    strcpy_s( welcome.programName, "Export metadata fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    if(invalid){OnDemandPayloadMessage payload{100,1050};append(RecordType::ClientToServer,RecordFlagHandshake,{reinterpret_cast<const uint8_t*>(&payload),sizeof(payload)});}
    const auto flush = [&] { std::vector<uint8_t> out( LZ4Size + 4 ); const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size ); Check( n > 0, "compress references" ); memcpy( out.data(), &n, 4 ); out.resize( n + 4 ); append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear(); };
    b.clear();
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    Event( b, QueueType::CpuTopology, QueueCpuTopology{ 1, 2, 3, 4 } );
    Event( b, QueueType::ThreadGroupHint, QueueThreadGroupHint{ 7, -9 } );
    Event( b, QueueType::JnJobType, QueueJnJobType{ 0, 9, 1, 2 } );
    Event( b, QueueType::JnJobType, QueueJnJobType{ 0, 9, 3, 4 } );
    Event( b, QueueType::JnJobSchedule, QueueJnJobSchedule{ 1100, 10, 100, 1, 1, 2 } );
    Event( b, QueueType::JnJobSchedule, QueueJnJobSchedule{ 1200, 10, 101, 2, 3, 4 } );
    Event( b, QueueType::JnJobConfig, QueueJnJobConfig{ 10, 9, 32, 4, 1, 1, 1, 2 } );
    Event( b, QueueType::JnJobConfig, QueueJnJobConfig{ 10, 9, 64, 8, 2, 2, 3, 4 } );
    Event( b, QueueType::JnIoRequest, QueueJnIoRequest{ 1100, 20, 200, 1, 2, 3, 4, 5 } );
    Event( b, QueueType::JnIoRequest, QueueJnIoRequest{ 1200, 20, 201, 2, 3, 4, 5, 6 } );
    Event( b, QueueType::JnIoConfig, QueueJnIoConfig{ 20, 10, 1024, 1, 1, 2 } );
    Event( b, QueueType::JnIoConfig, QueueJnIoConfig{ 20, 11, 2048, 2, 3, 4 } );
    Event( b, QueueType::CrashReport, QueueCrashReport{ 1210, 0x9876 } );
    b.push_back( uint8_t( QueueType::Crash ) );
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-export-metadata-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto path = root / "metadata.tracy-stream";
        Fixture( path );
        EventStore::Build( path );
        sqlite3* db = nullptr;
        Check( sqlite3_open( ( EventStore::Directory( path ) / "events.sqlite" ).string().c_str(), &db ) == SQLITE_OK, "open metadata database" );
        sqlite3_stmt* q = nullptr;
        const auto rc = sqlite3_prepare_v2( db, "SELECT COALESCE(SUM(count),0) FROM blocks WHERE stream=3", -1, &q, nullptr );
        const bool complete = rc == SQLITE_OK && sqlite3_step( q ) == SQLITE_ROW && sqlite3_column_int64( q, 0 ) == 10;
        sqlite3_finalize( q );
        sqlite3_close( db );
        Check( complete, "export needs every JN definition/config/schedule occurrence, not only final projections" );
        auto store = EventStore::Open( path );
        const auto header = store->CaptureMetadata();
        Check( header.available && header.initBegin == 1000 && header.initEnd == 1010 && header.timerMultiplier == 1 && header.resolution == 123 && header.epoch == 456 && header.executableTime == 789 && header.pid == 0x100000001ull && header.samplingPeriod == 321, "complete capture clock/process header" );
        Check( header.architecture == CpuArchX64 && header.cpuId == 0x12345678 && std::string( header.cpuManufacturer.data(), 12 ) == "Manufacturer" && header.host == "Fixture host", "architecture, CPU identity and host header" );
        const auto topology = store->CpuTopology();
        Check( topology.values.size() == 1 && topology.values[0].package == 1 && topology.values[0].die == 2 && topology.values[0].core == 3 && topology.values[0].thread == 4, "CPU topology fields" );
        const auto hint = store->ThreadHints().values[0];
        Check( hint.thread == 7 && hint.hint == -9, "signed physical thread group hint" );
        const auto crash = store->Crash();
        Check( crash && crash->thread == 7 && crash->time == 210 && crash->message == 0x9876 && crash->stack == 0, "crash clock/thread/message/stack" );
        const auto schedules = store->PacketsAt( uint8_t( QueueType::JnJobSchedule ), 0, 3 );
        Check( schedules.size() == 2, "raw schedules retain repeated logical IDs" );
        QueueJnJobSchedule first{};
        memcpy( &first, schedules[0].payload.data(), sizeof( first ) );
        Check( first.time == 1100 && first.jobId == 10 && first.packedHandle == 100 && first.dependencyCount == 1 && first.kind == 1 && first.flags == 2 && schedules[0].thread == 7, "first raw schedule is byte exact" );
        QueueJnJobSchedule second{};
        memcpy( &second, schedules[1].payload.data(), sizeof( second ) );
        Check( second.time == 1200 && second.packedHandle == 101 && second.dependencyCount == 2 && second.kind == 3 && second.flags == 4, "later raw schedule remains distinct from the projection" );
        const auto configs = store->PacketsAt( uint8_t( QueueType::JnJobConfig ), 0, 256 );
        Check( configs.size() == 2 && configs[0].event < configs[1].event, "raw configuration order" );
        auto live = LiveEventStore::Create( path );
        Check( live->Update( 3 ), "welcome revision" );
        auto before = live->Snapshot();
        Check( before->CaptureMetadata().pid == header.pid && !before->Crash(), "header precedes crash data" );
        Check( live->Update( 1 ), "raw event revision" );
        auto after = live->Snapshot();
        Check( after->Crash().has_value() && !before->Crash() && before->PacketsAt( uint8_t( QueueType::JnJobConfig ), 0, 1 ).empty(), "metadata/history snapshots are immutable" );
        store.reset();
        store = EventStore::Open( path );
        Check( store->PacketsAt( uint8_t( QueueType::JnIoConfig ), 0, 256 ).size() == 2, "raw history survives reopen" );
        const auto onDemand=root/"on-demand.tracy-stream";Fixture(onDemand,1);EventStore::Build(onDemand);auto demand=EventStore::Open(onDemand);const auto d=demand->CaptureMetadata();Check(d.frameOffset==100 && (d.flags&WelcomeFlag::OnDemand)!=0,"on-demand frame offset is persisted");
        std::cout << "Export metadata tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
