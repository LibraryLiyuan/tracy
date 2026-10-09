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
void Fixture( const std::filesystem::path& p )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( p, MakeFileHeader( ProtocolVersion ), false, {}, error );
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
    strcpy_s( welcome.programName, "Task fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    for( uint64_t key : { 1, 2, 3, 4 } )
    {
        ServerQueryPacket q{ ServerQueryString, key, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &q ), sizeof( q ) } );
    }
    auto flush = [&] {std::vector<uint8_t>out(LZ4Size+4);int n=LZ4_compress_default(reinterpret_cast<char*>(b.data()),reinterpret_cast<char*>(out.data()+4),int(b.size()),LZ4Size);Check(n>0,"compress task");memcpy(out.data(),&n,4);out.resize(n+4);append(RecordType::ClientToServer,RecordFlagCompressedFrame,out);b.clear(); };
    b.clear();
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    Event( b, QueueType::JnJobType, QueueJnJobType{ 1, 9, 0, 0 } );
    Event( b, QueueType::JnJobType, QueueJnJobType{ 1, 9, 1, 0 } );
    Event( b, QueueType::JnJobConfig, QueueJnJobConfig{ 12, 9, 32, 4, 2000, 1, 7, 192 } );
    Event( b, QueueType::JnJobSchedule, QueueJnJobSchedule{ 1090, 12, 120, 0, 1, 0 } );
    Event( b, QueueType::JnJobSchedule, QueueJnJobSchedule{ 1095, 12, 121, 0, 2, 0 } );
    Event( b, QueueType::JnJobSchedule, QueueJnJobSchedule{ 1100, 10, 100, 1, 0, 0 } );
    Event( b, QueueType::JnJobConfig, QueueJnJobConfig{ 10, 9, 64, 8, 1000, 1, 0, 0 } );
    Event( b, QueueType::JnJobDependency, QueueJnJobDependency{ 10, 11, 110, 0 } );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1110, 11, 0, 0, 0, uint8_t( JnJobStage::Completed ), 0 } );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1120, 10, 0, 3, 0, uint8_t( JnJobStage::Ready ), 0 } );
    for( uint32_t lane : { 0u, 3u, 3u, UINT32_MAX } ) Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1121, 10, 0, lane, 0, uint8_t( JnJobStage::Dispatch ), 2 } );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1130, 10, 1, 0, 0, uint8_t( JnJobStage::WorkerSliceBegin ), 0 } );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1140, 10, 2, 0, 0, uint8_t( JnJobStage::WaitBegin ), 0 } );
    Event( b, QueueType::JnIoStage, QueueJnIoStage{ 1100, 20, 0, 0, uint8_t( JnIoStage::Start ), 0, 0 } );
    Event( b, QueueType::JnIoConfig, QueueJnIoConfig{ 20, 10, 4096, 1, 3, 0 } );
    Event( b, QueueType::JnScriptFrame, QueueJnScriptFrame{ 2, 3, 30, 42, 0, 0 } );
    Event( b, QueueType::JnScriptStack, QueueJnScriptStack{ 1110, 40, 0, 1, 0, 0, uint8_t( JnScriptRecordKind::ZoneBegin ) } );
    flush();
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 8 } );
    Event( b, QueueType::JnFrame, QueueJnFrame{ 1125, 0x100000001ull, 100, uint8_t( JnFrameDomain::Player ), uint8_t( JnFramePhase::Begin ), uint8_t( JnFrameFlags::Canonical ) } );
    Event( b, QueueType::JnFrame, QueueJnFrame{ 1126, 0x200000001ull, 101, uint8_t( JnFrameDomain::Render ), uint8_t( JnFramePhase::Begin ), uint8_t( JnFrameFlags::Alias ) } );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1127, 12, 31, 0, 0, uint8_t( JnJobStage::ScheduleCallsite ), 0 } );
    Event( b, QueueType::JnCallsiteDefinition, QueueJnCallsiteDefinition{ 0, 31, 8, 0, 3, 0, 3 } );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1128, 11, 31, 0, 0, uint8_t( JnJobStage::ScheduleCallsite ), 0 } );
    Event( b, QueueType::CallstackPayload, QueueStringTransfer{ 1 } );
    Bytes( b, uint16_t( 8 ) );
    Bytes( b, uint64_t( 0x12345678 ) );
    b.push_back( uint8_t( QueueType::CallstackSerial ) );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1129, 12, 0, 8, 0, uint8_t( JnJobStage::ScheduleCallstack ), 0 } );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1150, 10, 2, 0, 0, uint8_t( JnJobStage::WaitEnd ), 0 } );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1160, 10, 1, 0, 0, uint8_t( JnJobStage::WorkerSliceEnd ), 0 } );
    Event( b, QueueType::JnJobStage, QueueJnJobStage{ 1170, 10, 0, 0, 0, uint8_t( JnJobStage::Completed ), 0 } );
    Event( b, QueueType::JnIoRequest, QueueJnIoRequest{ 1090, 20, 200, 0, 0, 1, 0, 0 } );
    Event( b, QueueType::JnIoStage, QueueJnIoStage{ 1180, 20, 4096, 0, uint8_t( JnIoStage::Complete ), 1, 0 } );
    Event( b, QueueType::JnScriptStack, QueueJnScriptStack{ 1180, 40, 0, 0, 0, 0, uint8_t( JnScriptRecordKind::ZoneEnd ) } );
    Event( b, QueueType::JnScriptStack, QueueJnScriptStack{ 1150, 50, 4, 30, 0, 0, uint8_t( JnScriptRecordKind::Marker ) } );
    for( auto [key, text] : { std::pair{ uint64_t( 1 ), "Task" }, std::pair{ uint64_t( 2 ), "ScriptFn" }, std::pair{ uint64_t( 3 ), "script.lua" }, std::pair{ uint64_t( 4 ), "Marker" } } )
    {
        Event( b, QueueType::StringData, QueueStringTransfer{ key } );
        Bytes( b, uint16_t( strlen( text ) ) );
        b.insert( b.end(), text, text + strlen( text ) );
    }
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main()
{
    auto root = std::filesystem::temp_directory_path() / std::filesystem::path( "tracy-store-task-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        auto p = root / "task.tracy-stream";
        Fixture( p );
        auto info = EventStore::Build( p );
        Check( info.jobs == 3 && info.ioRequests == 1 && info.scriptEvents == 3, "task-domain counts" );
        auto store = EventStore::Open( p );
        auto job = store->Job( 10 );
        Check( job && job->name == "Task" && job->schedule == 100 && job->completed == 170 && job->execution == 30 && job->wait == 10, "cross-block Job lifecycle and span accounting" );
        Check( job->ready == 120 && job->readyLane == 3 && job->dependencyLatency == 10 && job->count == 64 && !job->orphan, "Job configuration and dependency readiness" );
        Check( job->originFrame == 0x100000001ull, "late canonical Frame keeps 64-bit identity and excludes aliases" );
        auto early = store->Job( 12 );
        Check( early && early->packedHandle == 121 && early->schedule == 95 && early->kind == 7 && early->flags == 192 && early->schema == 3 && early->orphan && early->captureBoundary, "early Job config survives repeated later schedules" );
        auto unavailable = store->Job( 11 );
        Check( unavailable->callsite == 31 && unavailable->stack == 0 && unavailable->provenance == 3 && unavailable->reason == 3, "late unavailable Job callsite retains provenance and reason" );
        Check( early->callsite == 31 && early->stack != 0 && early->provenance == 2 && early->reason == 3, "later exact Job stack retains native callsite identity and prior reason" );
        Check( job->dispatches == 4 && job->activeDispatches == 4, "scheduler dispatch counts include duplicate lanes" );
        auto lanes = store->JobLanes( 10, 0, 1 );
        Check( lanes.values == std::vector<uint32_t>{ 0 } && lanes.more && lanes.next == 1, "lane zero and unique paged lanes" );
        auto lastLanes = store->JobLanes( 10, lanes.next, 3 );
        Check( lastLanes.values == std::vector<uint32_t>{ 3, UINT32_MAX } && !lastLanes.more, "unsigned lane maximum and page continuation" );
        auto overlapping = store->Jobs( 135, 145 );
        Check( overlapping.values.size() == 2, "long and unfinished Job window overlap" );
        Check( store->JobDependencies( 10 ).values[0].prerequisite == 11, "paged dependencies" );
        auto stages = store->JobStages( 10, 0, 1 );
        Check( stages.more, "Job stage paging" );
        auto io = store->IoRequest( 20 );
        Check( io && io->queued == 90 && io->start == 100 && io->end == 180 && io->parent == 10 && io->requested == 4096 && io->transferred == 4096 && !io->orphan, "IO stage before request and delayed config" );
        auto frames = store->ScriptFrames();
        Check( frames.values[0].function == "ScriptFn" && frames.values[0].file == "script.lua", "script frame delayed strings" );
        auto script = store->ScriptEvents( 0, 1000 );
        Check( script.values.size() == 3 && script.values[2].text == "Marker", "script events and marker definition" );
        store.reset();
        store = EventStore::Open( p );
        Check( store->Job( 10 )->execution == 30 && store->IoStages( 20 ).values.size() == 2, "task reopen and stable details" );
        std::cout << "Task tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
