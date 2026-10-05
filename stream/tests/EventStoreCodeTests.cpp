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
void Fixture( const std::filesystem::path& path, int invalid = 0, bool large = false, uint32_t definitions = 0 )
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
    strcpy_s( welcome.programName, "Hardware and code fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    const auto flush = [&] { std::vector<uint8_t> out( LZ4Size + 4 ); const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size ); Check( n > 0, "compress references" ); memcpy( out.data(), &n, 4 ); out.resize( n + 4 ); append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear(); };
    b.clear();
    if( definitions )
    {
        const auto query = [&]( ServerQuery type, uint64_t key, uint32_t extra = 0 ) {ServerQueryPacket q{type,key,extra};append(RecordType::ServerToClient,RecordFlagServerQuery,{reinterpret_cast<const uint8_t*>(&q),sizeof(q)}); };
        const auto text = [&]( QueueType type, const std::string& value ) {b.push_back(uint8_t(type));Bytes(b,uint16_t(value.size()));b.insert(b.end(),value.begin(),value.end()); };
        for( uint32_t n = 0; n < definitions; ++n )
        {
            const uint64_t address = 0x10000000ull + uint64_t( n ) * 16;
            const std::string name = "Symbol-" + std::to_string( n ), path = "source-" + std::to_string( n ) + ".cpp";
            Event( b, QueueType( uint8_t( QueueType::HwSampleCpuCycle ) + n % 6 ), QueueHwSample{ address + 1, 1000 + int64_t( n ) * 1800000000000ll / definitions } );
            query( ServerQueryCallstackFrame, address + 1 );
            text( QueueType::SingleStringData, "image.exe" );
            Event( b, QueueType::CallstackFrameSize, QueueCallstackFrameSize{ address + 1, 1 } );
            text( QueueType::SingleStringData, name );
            text( QueueType::SecondStringData, path );
            Event( b, QueueType::CallstackFrame, QueueCallstackFrame{ 42, address, 4 } );
            query( ServerQuerySymbol, address );
            text( QueueType::SingleStringData, path );
            Event( b, QueueType::SymbolInformation, QueueSymbolInformation{ 21, address } );
            query( ServerQuerySymbolCode, address, 4 );
            Event( b, QueueType::SymbolCode, QueueStringTransfer{ address } );
            Bytes( b, uint32_t( 4 ) );
            Bytes( b, uint32_t( 0x00c39090 ) );
            const size_t length = path.size() + 1;
            query( ServerQueryDataTransfer, length );
            for( size_t offset = 0; offset < length; offset += 12 )
            {
                uint8_t part[12]{};
                memcpy( part, path.c_str() + offset, std::min<size_t>( 12, length - offset ) );
                uint64_t lo;
                uint32_t hi;
                memcpy( &lo, part, 8 );
                memcpy( &hi, part + 8, 4 );
                query( ServerQueryDataTransferPart, lo, hi );
            }
            query( ServerQuerySourceCode, n + 1 );
            Event( b, QueueType::SourceCode, QueueStringTransfer{ n + 1 } );
            Bytes( b, uint32_t( 1024 ) );
            b.insert( b.end(), 1024, uint8_t( n % 251 ) );
            flush();
        }
        b.assign( 32, 0 );
        b[0] = 1;
        b[2] = 32;
        b[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, b );
        return;
    }
    for( uint8_t kind = 0; kind < 6; ++kind )
    {
        Event( b, QueueType( uint8_t( QueueType::HwSampleCpuCycle ) + kind ), QueueHwSample{ 0xf000000000001000ull, kind == 0 ? 0 : int64_t( 1100 + kind ) } );
        Event( b, QueueType( uint8_t( QueueType::HwSampleCpuCycle ) + kind ), QueueHwSample{ 0xf000000000001000ull, int64_t( 1200 + kind ) } );
    }
    flush();
    const auto query = [&]( ServerQuery type, uint64_t key, uint32_t extra = 0 ) { ServerQueryPacket q{type,key,extra}; append(RecordType::ServerToClient,RecordFlagServerQuery,{reinterpret_cast<const uint8_t*>(&q),sizeof(q)}); };
    query( ServerQueryCallstackFrame, 0x1001 );
    const auto text = [&]( QueueType type, const char* value ) {b.push_back(uint8_t(type));Bytes(b,uint16_t(strlen(value)));b.insert(b.end(),value,value+strlen(value)); };
    text( QueueType::SingleStringData, "fixture.exe" );
    Event( b, QueueType::CallstackFrameSize, QueueCallstackFrameSize{ 0x1001, 1 } );
    const auto symbolName = large ? std::string( 6000, 's' ) : std::string( "FixtureSymbol" );
    text( QueueType::SingleStringData, symbolName.c_str() );
    text( QueueType::SecondStringData, "call.cpp" );
    Event( b, QueueType::CallstackFrame, QueueCallstackFrame{ 42, 0x1000, 4 } );
    flush();
    query( ServerQuerySymbol, 0x1000 );
    text( QueueType::SingleStringData, "source.cpp" );
    Event( b, QueueType::SymbolInformation, QueueSymbolInformation{ 21, 0x1000 } );
    flush();
    query( ServerQueryCallstackFrame, 0x2001 );
    text( QueueType::SingleStringData, "fixture.exe" );
    Event( b, QueueType::CallstackFrameSize, QueueCallstackFrameSize{ 0x2001, 1 } );
    text( QueueType::SingleStringData, "UnavailableSymbol" );
    text( QueueType::SecondStringData, "other-call.cpp" );
    Event( b, QueueType::CallstackFrame, QueueCallstackFrame{ 43, 0x2000, 4 } );
    flush();
    query( ServerQuerySymbol, 0x2000 );
    text( QueueType::SingleStringData, "other-source.cpp" );
    Event( b, QueueType::SymbolInformation, QueueSymbolInformation{ 22, 0x2000 } );
    flush();
    query( ServerQuerySymbolCode, 0x1000, 4 );
    query( ServerQuerySymbolCode, 0x2000, 4 );
    b.push_back( uint8_t( QueueType::AckSymbolCodeNotAvailable ) );
    if( invalid == 4 ) b.push_back( uint8_t( QueueType::AckSymbolCodeNotAvailable ) );
    flush(); // Keyless failure can overtake a successful asynchronous response.
    Event( b, QueueType::SymbolCode, QueueStringTransfer{ 0x1000 } );
    Bytes( b, uint32_t( 4 ) );
    for( uint8_t value : { 0x90, 0x90, 0xc3, 0x00 } ) b.push_back( value );
    flush();
    const auto sourceQuery = [&]( uint32_t id, const char* path ) {
        if( invalid == 1 )
        {
            query( ServerQuerySourceCode, id );
            return;
        }
        const size_t length = strlen( path ) + 1;
        query( ServerQueryDataTransfer, length );
        for( size_t offset = 0; offset < length; offset += 12 )
        {
            if( invalid == 2 && offset + 12 >= length ) break;
            uint8_t part[12]{};
            memcpy( part, path + offset, std::min<size_t>( 12, length - offset ) );
            uint64_t lo;
            uint32_t hi;
            memcpy( &lo, part, 8 );
            memcpy( &hi, part + 8, 4 );
            query( ServerQueryDataTransferPart, lo, hi );
        }
        query( ServerQuerySourceCode, id );
    };
    const auto sourcePath = large ? std::string( 9000, 'p' ) + "/source.cpp" : std::string( "Z:/fixture/source.cpp" );
    sourceQuery( 7, sourcePath.c_str() );
    Event( b, QueueType::SourceCode, QueueStringTransfer{ invalid == 3 ? 99ull : 7ull } );
    Bytes( b, uint32_t( large ? 300000 : 9 ) );
    if( large )
    {
        for( uint32_t n = 0; n < 300000; ++n ) b.push_back( uint8_t( ( n * 17 ) % 251 ) );
    }
    else
        for( uint8_t value : std::initializer_list<uint8_t>{ 'a', 'b', 0, 'c', 'd', '\n', 0xef, 0xbb, 0xbf } ) b.push_back( value );
    flush();
    sourceQuery( 8, "Z:/fixture/missing.cpp" );
    Event( b, QueueType::AckSourceCodeNotAvailable, QueueSourceCodeNotAvailable{ 8 } );
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main( int argc, char** )
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-code-store-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        if( argc > 1 )
        {
            const auto path = root / "code-scale.tracy-stream";
            Fixture( path, 0, false, 20000 );
            StoreOptions options;
            options.cacheBytes = 4ull << 20;
            options.resultBytes = 65536;
            const auto started = std::chrono::steady_clock::now();
            EventStore::Build( path, options );
            auto store = EventStore::Open( path, options );
            Check( store->Info().hardwareSamples == 20000 && store->Info().hardwareAddresses == 20000 && store->Info().symbols == 20000 && store->Info().sourceFiles == 20000, "paged high-cardinality resource and address counts" );
            for( int minute : { 1, 10, 20, 29 } )
            {
                const auto page = store->HardwareSamples( int64_t( minute ) * 60000000000ll, int64_t( minute ) * 60000000000ll + 1000000000ll );
                Check( page.values.size() >= 11 && page.values.size() <= 12, "30-minute hardware windows" );
            }
            const auto symbols = store->SymbolResources( 0, 4096 );
            Check( symbols.more && !symbols.values.empty() && symbols.values[0].name == "Symbol-0", "byte-limited symbol metadata paging" );
            auto source = store->SourceResources( 19999, 1 ).values[0];
            Check( source.path == "source-19999.cpp" && store->CodeBytes( StoreCodeKind::Source, source.id, 1000, 64 ).data == std::vector<uint8_t>( 24, uint8_t( 19999 % 251 ) ), "late source bytes stay on disk until requested" );
            store.reset();
            store = EventStore::Open( path, options );
            Check( store->CodeBytes( StoreCodeKind::Symbol, symbols.values[0].id, 0, 64 ).data == std::vector<uint8_t>( { 0x90, 0x90, 0xc3, 0 } ), "resource bytes and IDs survive page eviction/reopen" );
            std::cout << "Code scale passed: 20000 addresses/symbols/sources; 1800-second span; seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - started ).count() << "; store_bytes=" << std::filesystem::file_size( EventStore::Directory( path ) / "events.sqlite" ) << "; evidence=" << root.string() << '\n';
            return 0;
        }
        const auto path = root / "code.tracy-stream";
        Fixture( path );
        EventStore::Build( path );
        sqlite3* db = nullptr;
        Check( sqlite3_open( ( EventStore::Directory( path ) / "events.sqlite" ).string().c_str(), &db ) == SQLITE_OK, "open database" );
        sqlite3_stmt* q = nullptr;
        const auto rc = sqlite3_prepare_v2( db, "SELECT COUNT(*) FROM hardware_samples", -1, &q, nullptr );
        const bool stored = rc == SQLITE_OK && sqlite3_step( q ) == SQLITE_ROW && sqlite3_column_int64( q, 0 ) == 12;
        sqlite3_finalize( q );
        sqlite3_close( db );
        Check( stored, "all six hardware counters and zero-time samples must be retained" );
        StoreOptions options;
        options.cacheBytes = 4ull << 20;
        options.resultBytes = 65536;
        auto store = EventStore::Open( path, options );
        Check( store->Info().hardwareSamples == 12 && store->Info().hardwareAddresses == 1 && store->Info().symbols == 2 && store->Info().sourceFiles == 2, "hardware and resource metadata counts" );
        auto address = store->HardwareAddresses().values[0];
        Check( address.address == 0xf000000000001000ull, "hardware address preserves 64 bits" );
        for( auto count : address.counts ) Check( count == 2, "six independent counter counts" );
        Check( store->HardwareSamples( 0, 1 ).values.size() == 1 && store->HardwareSamples( 100, 200 ).values.size() == 5, "hardware zero timestamp and absolute half-open windows" );
        auto counter = store->HardwareSamples( 0, 300, 3, address.address, 0, 1 );
        Check( counter.more && counter.values[0].time == 103 && counter.values[0].kind == 3, "counter/address filtering and byte-independent timestamp" );
        Check( store->HardwareSamples( 0, 300, 3, address.address, counter.next, 1 ).values[0].time == 203, "hardware cursor continues same counter" );
        auto symbols = store->SymbolResources();
        Check( symbols.values.size() == 2, "two explicit symbol definitions" );
        const auto symbol = symbols.values[0], missing = symbols.values[1];
        Check( symbol.address == 0x1000 && symbol.name == "FixtureSymbol" && symbol.file == "source.cpp" && symbol.line == 21 && symbol.callFile == "call.cpp" && symbol.callLine == 42 && symbol.image == "fixture.exe" && symbol.size == 4 && !symbol.inlineFrame, "symbol definition and call-site fields" );
        Check( symbol.status == StoreCodeStatus::Available && missing.status == StoreCodeStatus::Unavailable, "keyless code failure does not consume an overtaken successful query" );
        Check( store->CodeBytes( StoreCodeKind::Symbol, symbol.id, 0, 2 ).data == std::vector<uint8_t>( { 0x90, 0x90 } ) && store->CodeBytes( StoreCodeKind::Symbol, symbol.id, 2, 8 ).data == std::vector<uint8_t>( { 0xc3, 0 } ), "symbol bytes including NUL and chunk boundary" );
        auto sources = store->SourceResources();
        Check( sources.values.size() == 2 && sources.values[0].path == "Z:/fixture/source.cpp" && sources.values[0].bytes == 9 && sources.values[1].status == StoreCodeStatus::Unavailable, "source queries associate complete multi-part paths and unavailable files" );
        const std::vector<uint8_t> sourceBytes = { 'a', 'b', 0, 'c', 'd', '\n', 0xef, 0xbb, 0xbf };
        Check( store->CodeBytes( StoreCodeKind::Source, sources.values[0].id, 0, 64 ).data == sourceBytes, "embedded source bytes are exact rather than C strings" );
        Check( store->CodeTextBytes( StoreCodeKind::Source, sources.values[0].id, 0, 0, 64 ).totalBytes == sources.values[0].path.size(), "source path available separately as bytes" );
        bool rejected = false;
        try
        {
            store->CodeBytes( StoreCodeKind::Symbol, missing.id, 0, 64 );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected, "unavailable resource is not an empty successful read" );
        auto live = LiveEventStore::Create( path, options );
        Check( live->Update( 15 ), "live keyless-ack revision" );
        auto before = live->Snapshot();
        Check( before->SymbolResources().values[0].status == StoreCodeStatus::Pending && before->SymbolResources().values[1].status == StoreCodeStatus::Pending, "ambiguous live failure stays pending until completion" );
        Check( live->Update( 1 ), "live successful-code revision" );
        auto available = live->Snapshot();
        Check( available->SymbolResources().values[0].status == StoreCodeStatus::Available && available->SymbolResources().values[1].status == StoreCodeStatus::Pending, "successful payload resolves its exact symbol" );
        Check( live->Update( 100 ) && live->Snapshot()->SymbolResources().values[1].status == StoreCodeStatus::Unavailable && before->SymbolResources().values[0].status == StoreCodeStatus::Pending, "completed unavailable status and old snapshots remain exact" );
        for( int invalid = 1; invalid <= 4; ++invalid )
        {
            const auto bad = root / ( "invalid-" + std::to_string( invalid ) + ".tracy-stream" );
            Fixture( bad, invalid );
            bool failed = false;
            try
            {
                EventStore::Build( bad );
            }
            catch( const std::exception& )
            {
                failed = true;
            }
            Check( failed && !std::filesystem::exists( EventStore::Directory( bad ) ), "invalid code transcript never publishes" );
        }
        const auto largePath = root / "large-code.tracy-stream";
        Fixture( largePath, 0, true );
        EventStore::Build( largePath, options );
        auto largeStore = EventStore::Open( largePath, options );
        const auto largeSymbol = largeStore->SymbolResources().values[0];
        const auto largeSource = largeStore->SourceResources().values[0];
        Check( largeSymbol.textMore && largeSymbol.name.size() == 4096 && largeSymbol.textBytes[0] == 6000, "oversized symbol names have explicit previews" );
        Check( largeStore->CodeTextBytes( StoreCodeKind::Symbol, largeSymbol.id, 0, 4096, 4096 ).data == std::vector<uint8_t>( 1904, 's' ), "full symbol text remains accessible" );
        Check( largeSource.pathMore && largeSource.path.size() == 4096 && largeSource.pathBytes == 9011 && largeSource.bytes == 300000, "long transfer paths and large code stay bounded" );
        uint64_t offset = 0;
        while( offset < 300000 )
        {
            const auto chunk = largeStore->CodeBytes( StoreCodeKind::Source, largeSource.id, offset, 32749 );
            for( size_t n = 0; n < chunk.data.size(); ++n ) Check( chunk.data[n] == uint8_t( ( ( offset + n ) * 17 ) % 251 ), "large source exact byte chunks" );
            offset += chunk.data.size();
            Check( chunk.more == ( offset < 300000 ), "large resource EOF" );
        }
        bool cancel = false;
        options.cancelled = [&] { return cancel; };
        store = EventStore::Open( path, options );
        cancel = true;
        rejected = false;
        try
        {
            store->CodeBytes( StoreCodeKind::Source, sources.values[0].id, 0, 64 );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected, "code read cancellation" );
        cancel = false;
        Check( store->CodeBytes( StoreCodeKind::Source, sources.values[0].id, 0, 64 ).data == sourceBytes, "code data and IDs survive reopen/cancel" );
        std::cout << "Code tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
