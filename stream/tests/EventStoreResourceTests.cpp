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
    strcpy_s( welcome.programName, "Resource set fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    const auto flush = [&] { std::vector<uint8_t> out( LZ4Size + 4 ); const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size ); Check( n > 0, "compress references" ); memcpy( out.data(), &n, 4 ); out.resize( n + 4 ); append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear(); };
    b.clear();
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    const auto definition = [&] {
        Event( b, QueueType::JnGpuReferenceSetDefinition, QueueStringTransfer{ 7 } );
        Bytes( b, uint32_t( 3 * sizeof( JnGpuReferenceSetEntry ) ) );
        Bytes( b, JnGpuReferenceSetEntry{ 0xf00000000000000aull, 1 } );
        Bytes( b, JnGpuReferenceSetEntry{ invalid == 3 ? 0ull : 0xbull, 2 } );
        Bytes( b, JnGpuReferenceSetEntry{ 0xf00000000000000aull, 4 } );
    };
    definition();
    if( invalid == 2 ) definition();
    Event( b, QueueType::JnGpuReferencePass, QueueJnGpuReferencePass{ 1100, 1, 0x100000002ull, 42, 3, 5 } );
    Event( b, QueueType::JnGpuReferenceUse, QueueJnGpuReferenceUse{ 1105, 1, 0xc, 8, 6 } );
    Event( b, QueueType::JnGpuReferenceSetUse, QueueJnGpuReferenceSetUse{ 1, uint32_t( invalid == 5 ? 8 : 7 ), uint16_t( invalid == 1 ? 2 : 3 ), 9, uint8_t( invalid == 6 ? 1 : 2 ) } );
    flush();
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 8 } );
    Event( b, QueueType::JnGpuReferenceSetUse, QueueJnGpuReferenceSetUse{ 1, 7, 3, 10, 2 } );
    Event( b, QueueType::JnGpuReferenceEnd, QueueJnGpuReferenceEnd{ 1150, 1, 0x200000003ull, 7, 2, 11 } );
    if( invalid == 4 ) Event( b, QueueType::JnGpuReferenceSetUse, QueueJnGpuReferenceSetUse{ 1, 7, 3, 0, 2 } );
    Event( b, QueueType::JnGpuReferencePass, QueueJnGpuReferencePass{ 1200, 1, 0x100000003ull, 43, 4, 0 } );
    Event( b, QueueType::JnGpuReferenceSetUse, QueueJnGpuReferenceSetUse{ 1, 7, 3, 12, 2 } );
    Event( b, QueueType::JnGpuReferenceEnd, QueueJnGpuReferenceEnd{ 1210, 1, 0x200000004ull, 3, 0, 0 } );
    for( uint64_t n = 0; n < 3; ++n ) Event( b, QueueType::JnGpuReferenceUse, QueueJnGpuReferenceUse{ int64_t( 1220 + n ), 1, 0xd1 + n, 1, 0 } );
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-resource-store-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto path = root / "references.tracy-stream";
        Fixture( path );
        EventStore::Build( path );
        auto store = EventStore::Open( path );
        const auto passes = store->GpuReferencePasses( 0, 1000 );
        Check( passes.values.size() == 2, "GPU reference pass records are indexed" );
        Check( passes.values[0].time == 100 && passes.values[0].thread == 7 && passes.values[0].frame == 0x100000002ull && passes.values[0].taxonomy == 42 && passes.values[0].level == 3 && passes.values[0].flags == 5, "all pass metadata fields" );
        std::vector<StoreGpuReference> uses;
        uint64_t cursor = 0;
        for( ;; )
        {
            auto page = store->GpuReferences( 0, 1000, {}, cursor, 2 );
            uses.insert( uses.end(), page.values.begin(), page.values.end() );
            if( !page.more ) break;
            Check( page.next > cursor, "expanded cursor progresses inside a set" );
            cursor = page.next;
        }
        Check( uses.size() == 13, "every repeated resource set and legacy use is expanded" );
        Check( uses[0].encoding == 1 && uses[0].resource == 0xc && uses[0].time == 105 && uses[0].mask == 8, "legacy use remains in stream order" );
        for( size_t n = 1; n < 10; ++n )
        {
            const auto entry = ( n - 1 ) % 3;
            Check( uses[n].resource == ( entry == 1 ? 0xb : 0xf00000000000000aull ) && uses[n].mask == ( 1u << entry ) && uses[n].ordinal == entry && uses[n].set == 7 && uses[n].encoding == 2, "resource duplicates, masks and definition order are exact" );
            Check( uses[n].time == ( n < 7 ? 100 : 200 ) && uses[n].thread == ( n < 4 ? 7 : 8 ), "set use keeps pass timestamp and use thread across blocks" );
        }
        Check( store->GpuReferences( 100, 101 ).values.size() == 6 && store->GpuReferences( 101, 105 ).values.empty(), "resource use half-open windows" );
        auto tail = store->GpuReferences( 0, 1000, {}, uses[9].id, 1 );
        Check( tail.more && tail.values[0].resource == 0xd1, "exhausted set cursor must not hide later legacy packets" );
        const auto ends = store->GpuReferenceEnds( 0, 1000 );
        Check( ends.values.size() == 2 && ends.values[0].total == 7 && ends.values[0].dropped == 2 && ends.values[0].commandList == 0x200000003ull && ends.values[0].flags == 11, "reference end and drop accounting" );
        store.reset();
        store = EventStore::Open( path );
        Check( store->GpuReferences( 0, 1000, {}, uses[4].id, 1 ).values[0].id == uses[5].id, "expanded IDs survive reopen" );
        bool cancelled = false;
        StoreOptions options;
        options.cancelled = [&] { return cancelled; };
        store = EventStore::Open( path, options );
        cancelled = true;
        bool interrupted = false;
        try
        {
            store->GpuReferences( 0, 1000 );
        }
        catch( const std::exception& )
        {
            interrupted = true;
        }
        Check( interrupted, "reference query honors cancellation" );
        cancelled = false;
        Check( store->GpuReferences( 0, 1000, 1, uses[4].id, 1 ).values[0].id == uses[5].id, "reference cursor remains usable after cancellation" );
        for( int invalid = 1; invalid <= 6; ++invalid )
        {
            const auto input = root / ( "invalid-" + std::to_string( invalid ) + ".tracy-stream" );
            Fixture( input, invalid );
            bool rejected = false;
            try
            {
                EventStore::Build( input );
            }
            catch( const std::exception& )
            {
                rejected = true;
            }
            Check( rejected && !std::filesystem::exists( EventStore::Directory( input ) ), "invalid set references never publish" );
        }
        std::cout << "Resource reference tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
