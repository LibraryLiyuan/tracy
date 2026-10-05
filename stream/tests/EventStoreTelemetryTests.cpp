#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include "../src/TracySnapshotStream.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace tracy;
using namespace tracy::stream;
void Check( bool ok, const char* message )
{
    if( !ok ) throw std::runtime_error( message );
}
template<class T>
void Bytes( std::vector<uint8_t>& b, const T& v )
{
    const auto p = reinterpret_cast<const uint8_t*>( &v );
    b.insert( b.end(), p, p + sizeof( v ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType kind, const T& v )
{
    b.push_back( uint8_t( kind ) );
    Bytes( b, v );
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-telemetry-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto path = root / "telemetry.tracy-stream";
        std::string error;
        auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
        Check( bool( writer ), error.c_str() );
        uint64_t sequence = 0;
        auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> bytes ) { Check( writer->Append( type, flags, bytes, ++sequence, error ), error.c_str() ); };
        std::vector<uint8_t> data( 24 );
        data[0] = 2;
        data[2] = 24;
        const uint32_t version = ProtocolVersion;
        memcpy( data.data() + 4, &version, 4 );
        append( RecordType::SessionBegin, 0, data );
        const uint8_t status = HandshakeWelcome;
        append( RecordType::ClientToServer, RecordFlagHandshake, { &status, 1 } );
        WelcomeMessage welcome{};
        welcome.timerMul = 1;
        welcome.initBegin = 1000;
        welcome.initEnd = 1010;
        append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
        for( uint64_t key : { 401, 402 } )
        {
            const ServerQueryPacket query{ ServerQueryString, key, 0 };
            append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
        }
        data.clear();
        Event( data, QueueType::SysTimeReport, QueueSysTime{ 1100, 10.5f } );
        Event( data, QueueType::SysTimeReport, QueueSysTime{ 1200, 25.25f } );
        Event( data, QueueType::SysPowerReport, QueueSysPower{ 1100, 0, 401 } );
        Event( data, QueueType::SysPowerReport, QueueSysPower{ 1200, 2, 401 } );
        Event( data, QueueType::SysPowerReport, QueueSysPower{ 1300, 1, 401 } );
        Event( data, QueueType::FrameVsync, QueueFrameVsync{ 1100, 11 } );
        Event( data, QueueType::FrameVsync, QueueFrameVsync{ 1200, 22 } );
        Event( data, QueueType::FrameVsync, QueueFrameVsync{ 1300, 11 } );
        Event( data, QueueType::ParamSetup, QueueParamSetup{ 7, 402, 1, -3 } );
        Event( data, QueueType::ParamSetup, QueueParamSetup{ 8, 402, 0, 42 } );
        Event(data,QueueType::FrameImageData,QueueStringTransfer{99});Bytes(data,uint32_t(8));Bytes(data,uint64_t(0xe4e4e4e40000ffffull));Event(data,QueueType::FrameImage,QueueFrameImage{99,4,4,0});
        for( auto [key, value] : { std::pair{ 401ull, "Power rail" }, std::pair{ 402ull, "Parameter" } } )
        {
            Event( data, QueueType::StringData, QueueStringTransfer{ key } );
            Bytes( data, uint16_t( strlen( value ) ) );
            data.insert( data.end(), value, value + strlen( value ) );
        }
        std::vector<uint8_t> compressed( LZ4Size + 4 );
        const int n = LZ4_compress_default( reinterpret_cast<const char*>( data.data() ), reinterpret_cast<char*>( compressed.data() + 4 ), int( data.size() ), LZ4Size );
        Check( n > 0, "compress telemetry" );
        memcpy( compressed.data(), &n, 4 );
        compressed.resize( n + 4 );
        append( RecordType::ClientToServer, RecordFlagCompressedFrame, compressed );
        data.assign( 32, 0 );
        data[0] = 1;
        data[2] = 32;
        data[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, data );
        writer.reset();
        EventStore::Build( path );
        auto store = EventStore::Open( path );
        const auto plots = store->Plots();
        Check( plots.values.size() == 2, "system CPU and power plots are retained" );
        Check( plots.values[0].kind == 2 && plots.values[0].points == 2 && plots.values[0].min == 10.5 && plots.values[0].max == 25.25 && plots.values[0].sum == 35.75, "system time plot metadata" );
        Check( plots.values[1].kind == 3 && plots.values[1].name == "Power rail" && plots.values[1].points == 2 && plots.values[1].min == 10 && plots.values[1].max == 20 && plots.values[1].sum == 30, "power plot uses microjoules and nanosecond intervals" );
        const auto points = store->PlotPoints( 0, 500 );
        Check( points.values.size() == 4 && points.values[2].time == 200 && points.values[2].value == 20, "system plot timestamps and values" );
        const auto vsync = store->FrameSet( 0x8000000000000000ull );
        Check( vsync && vsync->frames == 2 && vsync->completeFrames == 1, "vsync sets use native discovery order and retain frame boundaries" );
        const auto parameters = store->Parameters( 0, 1 );
        Check( parameters.more && parameters.values[0].index == 7 && parameters.values[0].boolean && parameters.values[0].value == -3 && parameters.values[0].name == "Parameter", "parameter definitions retain names, type, signed values and pagination" );
        Check( store->Parameters( parameters.next, 1 ).values[0].value == 42, "parameter stable cursor" );
        auto result = store->ExportSnapshot( root / "telemetry.tracy" );
        Check( result.decodedBytes > 0, "telemetry is exported with bounded validation" );
        Check(store->FrameImages().values.size()==1 && !store->FrameImages().values[0].frame,"image and parameter footers can coexist");
        {
            SnapshotStreamReader input(root/"telemetry.tracy");std::vector<uint8_t> bytes(size_t(result.decodedBytes));input.ReadExact(bytes);input.Finish();
            const std::array<uint8_t,4> magic{'J','P','M','1'};const auto at=std::search(bytes.begin(),bytes.end(),magic.begin(),magic.end());Check(at!=bytes.end(),"parameter footer exists");
            bytes[size_t(at-bytes.begin())+14+4+8+1]=2;SnapshotStreamWriter output(root/"telemetry-invalid.tracy");output.Write(bytes);output.Finish();
        }
        std::cout << "Telemetry tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
