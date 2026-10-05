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
void Check( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}
template<class T>
void Bytes( std::vector<uint8_t>& b, const T& value )
{
    const auto p = reinterpret_cast<const uint8_t*>( &value );
    b.insert( b.end(), p, p + sizeof( value ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType type, const T& value )
{
    b.push_back( uint8_t( type ) );
    Bytes( b, value );
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-live-store-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto path = root / "live.tracy-stream";
        std::string error;
        auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
        Check( bool( writer ), error.c_str() );
        bool cancel = false, abortConsumer = false;
        StoreOptions liveOptions;
        liveOptions.cancelled = [&] { return cancel; };
        liveOptions.progress = [&]( uint64_t, uint64_t ) { if( abortConsumer ) throw std::runtime_error( "injected consumer rollback" ); };
        auto live = LiveEventStore::Create( path, liveOptions );
        Check( !live->Update(), "header alone has no records to decode" );
        uint64_t sequence = 0;
        const auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> b ) { Check( writer->Append( type, flags, b, ++sequence, error ), error.c_str() ); };
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
        strcpy_s( welcome.programName, "Live fixture" );
        append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
        ServerQueryPacket query{ ServerQuerySourceLocation, 99, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
        for( uint64_t key : { 1, 2, 3 } )
        {
            query = { ServerQueryString, key, 0 };
            append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
        }
        const auto flush = [&] {
            std::vector<uint8_t> out( LZ4Size + 4 );
            const int size = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size );
            Check( size > 0, "compress live frame" );
            memcpy( out.data(), &size, 4 );
            out.resize( size + 4 );
            append( RecordType::ClientToServer, RecordFlagCompressedFrame, out );
            b.clear();
        };
        b.clear();
        Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
        Event( b, QueueType::SourceLocation, QueueSourceLocation{ 1, 2, 3, 42, 0, 0, 0 } );
        Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { 1100 }, 99 } );
        flush();
        Check( live->Update(), "first input batch publishes a revision" );
        auto old = live->Snapshot();
        const auto first = old->Zones( 0, 1000, {} );
        Check( first.values.size() == 1 && !first.values[0].end && first.values[0].name == "???", "first revision exposes unfinished zone and unresolved definition" );
        const auto id = first.values[0].id, revision = old->Info().revision, decoded = live->DecodedRecords();
        Check( !live->Update() && live->DecodedRecords() == decoded, "temporary EOF never replays the decoder" );
        cancel = true;
        bool cancelled = false;
        try
        {
            live->Update();
        }
        catch( const std::exception& )
        {
            cancelled = true;
        }
        cancel = false;
        Check( cancelled && live->DecodedRecords() == decoded && old->Zone( id )->end == std::nullopt, "cancelled update preserves the published revision" );
        for( auto [key, value] : { std::pair{ uint64_t( 1 ), "Delayed name" }, std::pair{ uint64_t( 2 ), "Function" }, std::pair{ uint64_t( 3 ), "Source.cpp" } } )
        {
            Event( b, QueueType::StringData, QueueStringTransfer{ key } );
            Bytes( b, uint16_t( strlen( value ) ) );
            b.insert( b.end(), value, value + strlen( value ) );
        }
        b.push_back( uint8_t( QueueType::SingleStringData ) );
        Bytes( b, uint16_t( 9 ) );
        const char text[] = "late text";
        b.insert( b.end(), text, text + 9 );
        b.push_back( uint8_t( QueueType::ZoneText ) );
        Event( b, QueueType::ZoneEnd, QueueZoneEnd{ { 50 } } );
        flush();
        abortConsumer = true;
        bool aborted = false;
        try
        {
            live->Update();
        }
        catch( const std::exception& )
        {
            aborted = true;
        }
        abortConsumer = false;
        Check( aborted && live->DecodedRecords() == decoded && live->Snapshot()->Zone( id )->end == std::nullopt, "consumer rollback restores SQL and parser state" );
        Check( live->Update(), "second batch publishes new data" );
        auto next = live->Snapshot();
        auto completed = next->Zone( id );
        Check( completed && completed->end == 150 && completed->name == "Delayed name" && completed->text == "late text", "parser clocks, stack and delayed definitions survive a poll boundary" );
        Check( next->Info().revision > revision && next->Info().openZones == 0 && live->DecodedRecords() == decoded + 1, "only new input records were decoded" );
        auto original = old->Zone( id );
        Check( original && !original->end && original->name == "???" && original->text.empty() && old->Info().revision == revision, "old revision remains immutable" );
        b.assign( 32, 0 );
        b[0] = 1;
        b[2] = 32;
        b[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, b );
        writer.reset();
        Check( live->Update( 1 ) && live->Snapshot()->Info().cleanEnd, "terminal batch is published" );
        Check( !live->Update(), "terminal EOF is stable" );
        const auto last = live->Snapshot();
        {
            StoreOptions bounded;bounded.cacheBytes=4ull<<20;bounded.diskBytes=1ull<<20;
            auto limited=LiveEventStore::Create(path,bounded);const auto before=limited->Snapshot();bool budgetRejected=false;
            try{limited->Update(128);}catch(const std::exception& e){budgetRejected=std::string_view(e.what()).find("budget")!=std::string_view::npos;}
            Check(budgetRejected && limited->Snapshot()->Info().revision==before->Info().revision && before->Info().cpuZones==0,"live aggregate disk budget failure preserves the preceding snapshot");
        }
        std::filesystem::resize_file( path, FileHeaderSize );
        bool rejected = false;
        try
        {
            live->Update();
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected && last->Zone( id )->end == 150 && old->Zone( id )->end == std::nullopt, "source truncation preserves every published revision" );
        std::cout << "Live store tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
