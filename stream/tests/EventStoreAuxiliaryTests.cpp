#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include "sqlite3.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
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
void Fixture( const std::filesystem::path& path, uint32_t repeats = 1, bool domains = false )
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
    strcpy_s( welcome.programName, "Auxiliary fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    const auto flush = [&] { std::vector<uint8_t> out( LZ4Size + 4 ); const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size ); Check( n > 0, "compress references" ); memcpy( out.data(), &n, 4 ); out.resize( n + 4 ); append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear(); };
    b.clear();
    for( uint32_t n = 0; n < repeats; ++n )
    {
        const int64_t time = 1000 + int64_t( n ) * 1800000000000ll / repeats;
        Event( b, QueueType::ThreadContext, QueueThreadContext{ n % 2 ? 8ull : 7ull } );
        Event( b, QueueType::JnFrame, QueueJnFrame{ time, 0xf000000000000001ull + n, uint64_t( n ), 1, 0, 1 } );
        Event( b, QueueType::JnGfxDispatch, QueueJnGfxDispatch{ time + 1, 100 + n, 0xf000000000000001ull + n, 17, 3, 128 } );
        Event( b, QueueType::JnGfxEntity, QueueJnGfxEntity{ time + 2, 200 + n, 100 + n, 0xf0000001u, 255, 7, 64 } );
        Event( b, QueueType::JnGfxLink, QueueJnGfxLink{ time + 3, 200 + n, 300 + n, 4, 32 } );
        Event( b, QueueType::JnRelation, QueueJnRelation{ time + 4, 200 + n, 400 + n, 4, 3, 2, 5, 16 } );
        Event( b, QueueType::JnRelation, QueueJnRelation{ time + 4, 200 + n, 400 + n, 4, 3, 2, 5, 16 } );
        Event( b, QueueType::JnRuntimeDomainState, QueueJnRuntimeDomainState{ time + 5, 9 + n, 0xf000000000000001ull + n, 3, 1, 2, 255, 8 } );
        flush();
        Event( b, QueueType::ThreadContext, QueueThreadContext{ 8 } );
        Event( b, QueueType::JnGfxEntity, QueueJnGfxEntity{ time + 6, 300 + n, 200 + n, 0xffffu, 1, 2, 4 } );
        Event( b, QueueType::JnFrame, QueueJnFrame{ time + 7, 0xf000000000000001ull + n, uint64_t( n ), 1, 1, 1 } );
        flush();
    }
    if( domains )
    {
        for( uint8_t domain : { uint8_t( 0 ), uint8_t( 3 ), uint8_t( 255 ) } ) Event( b, QueueType::JnRuntimeDomainState, QueueJnRuntimeDomainState{ 1025, 1, 0, domain, 1, 2, 3, 4 } );
        Event( b, QueueType::JnRuntimeDomainState, QueueJnRuntimeDomainState{ 1025, 0xfffffffffffffff0ull, 0, 3, 2, 3, 4, 5 } );
        flush();
    }
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main( int argc, char** )
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-auxiliary-store-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        if( argc > 1 )
        {
            const auto path = root / "auxiliary-scale.tracy-stream";
            Fixture( path, 20000 );
            StoreOptions options;
            options.cacheBytes = 4ull << 20;
            options.resultBytes = 65536;
            const auto started = std::chrono::steady_clock::now();
            EventStore::Build( path, options );
            auto store = EventStore::Open( path, options );
            Check( store->Info().auxiliaryEvents == 180000, "scale auxiliary count" );
            for( int minute : { 1, 10, 20, 29 } )
            {
                auto page = store->AuxiliaryEvents( int64_t( minute ) * 60000000000ll, int64_t( minute ) * 60000000000ll + 1000000000ll );
                const auto state = store->RuntimeStatesAt( int64_t( minute ) * 60000000000ll );
                Check( state.values.size() == 1 && state.values[0].time <= int64_t( minute ) * 60000000000ll && int64_t( minute ) * 60000000000ll - state.values[0].time <= 90000000ll, "effective states use indexed predecessor lookup" );
                Check( page.values.size() >= 99 && page.values.size() <= 108 && !page.more, "indexed 30-minute point-event windows" );
            }
            auto capped = store->AuxiliaryEvents( 0, INT64_MAX, {}, {}, 0, 4096 );
            Check( capped.more && capped.values.size() * sizeof( StoreAuxiliaryRecord ) <= 65536, "auxiliary pages obey byte budget" );
            Check( capped.values.capacity() * sizeof( StoreAuxiliaryRecord ) + sizeof(capped) <= 65536, "auxiliary page allocation capacity obeys the full result budget" );
            const auto id = capped.values.front().id;
            store.reset();
            store = EventStore::Open( path, options );
            Check( store->AuxiliaryEvents( 0, INT64_MAX, {}, StoreEntityKey{ 1, 0xf000000000000001ull } ).values.size() == 4, "typed cross-record references survive page eviction and reopen" );
            Check( store->AuxiliaryEvents( 0, INT64_MAX, {}, {}, id, 1 ).values.front().id > id, "stable auxiliary cursor after reopen" );
            std::cout << "Auxiliary scale passed: 180000 events, 1800-second span; seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - started ).count() << "; store_bytes=" << std::filesystem::file_size( EventStore::Directory( path ) / "events.sqlite" ) << "; evidence=" << root.string() << '\n';
            return 0;
        }
        const auto path = root / "auxiliary.tracy-stream";
        Fixture( path );
        EventStore::Build( path );
        auto store = EventStore::Open( path );
        Check( store->Info().auxiliaryEvents == 9, "auxiliary metadata count" );
        std::set<uint64_t> reachable;
        for( uint8_t kind = 0; kind < 32; ++kind )
            for( const uint64_t object : { 0xf000000000000001ull, 100ull, 200ull, 300ull, 400ull } )
                for( const auto& record : store->AuxiliaryEvents( 0, 8, {}, StoreEntityKey{ kind, object } ).values ) reachable.insert( record.id );
        Check( reachable.size() == 9, "every auxiliary record must be reachable by its typed object identity" );
        std::vector<StoreAuxiliaryRecord> records;
        uint64_t cursor = 0;
        for( ;; )
        {
            auto page = store->AuxiliaryEvents( 0, 8, {}, {}, cursor, 2 );
            records.insert( records.end(), page.values.begin(), page.values.end() );
            if( !page.more ) break;
            Check( page.next > cursor, "cursor advances" );
            cursor = page.next;
        }
        Check( records.size() == 9 && records[0].time == 0 && records[0].thread == 7 && records[7].thread == 8, "record order, clocks and physical threads" );
        const auto& frame = std::get<StoreFrameIdentity>( records[0].data );
        Check( frame.frame == 0xf000000000000001ull && frame.domainIndex == 0 && frame.domain == 1 && frame.phase == 0 && frame.flags == 1, "unsigned Frame identity and all fields" );
        const auto& dispatch = std::get<StoreGfxDispatch>( records[1].data );
        Check( dispatch.dispatch == 100 && dispatch.frame == frame.frame && dispatch.expectedJobs == 17 && dispatch.threadingMode == 3 && dispatch.flags == 128, "Dispatch complete fields" );
        const auto& entity = std::get<StoreGfxEntity>( records[2].data );
        Check( entity.entity == 200 && entity.parent == 100 && entity.gpuQuery == 0xf0000001u && entity.gpuContext == 255 && entity.kind == 7 && entity.flags == 64, "Entity keeps full query width and unknown kinds" );
        const auto& link = std::get<StoreGfxLink>( records[3].data );
        Check( link.source == 200 && link.target == 300 && link.relation == 4 && link.flags == 32, "Link fields" );
        const auto& relation = std::get<StoreRelation>( records[4].data );
        Check( relation.source == 200 && relation.target == 400 && relation.sourceKind == 4 && relation.targetKind == 3 && relation.nameSpace == 2 && relation.relation == 5 && relation.flags == 16, "Relation kinds, namespace and flags" );
        const auto& state = std::get<StoreRuntimeState>( records[6].data );
        Check( state.generation == 9 && state.requestedFrame == frame.frame && state.domain == 3 && state.requestedMode == 1 && state.effectiveMode == 2 && state.reason == 255 && state.flags == 8, "Runtime state preserves unknown reasons and all modes" );
        Check( store->RuntimeStatesAt( 4 ).values.empty() && store->RuntimeStatesAt( 5 ).values[0].id == records[6].id && store->RuntimeStatesAt( INT64_MAX ).values[0].id == records[6].id, "effective runtime state at and between updates" );
        Check( store->RuntimeStatesAt( 5, 4 ).values.empty(), "runtime state domain cursor" );
        Check( store->AuxiliaryEvents( 4, 5 ).values.size() == 2 && store->AuxiliaryEvents( 5, 6 ).values.size() == 1, "duplicate relations and exact half-open windows" );
        Check( store->AuxiliaryEvents( 0, 8, StoreAuxiliaryKind::GfxEntity ).values.size() == 2, "kind filtering" );
        Check( store->AuxiliaryEvents( 0, 8, {}, StoreEntityKey{ 14, 200 } ).values.size() == 1 && store->AuxiliaryEvents( 0, 8, {}, StoreEntityKey{ 15, 300 } ).values.size() == 1, "legacy GfxLink endpoint kinds follow client relation semantics" );
        Check( store->AuxiliaryEvents( 0, 8, {}, StoreEntityKey{ 4, 200 } ).values.size() == 4, "inbound, outbound and late entity relationships" );
        Check( store->AuxiliaryEvents( 0, 8, {}, StoreEntityKey{ 3, 400 } ).values.size() == 2 && store->AuxiliaryEvents( 0, 8, {}, StoreEntityKey{ 1, 400 } ).values.empty(), "identical numeric IDs in different domains never alias" );
        auto live = LiveEventStore::Create( path );
        Check( live->Update( 4 ), "first live auxiliary revision" );
        auto old = live->Snapshot();
        Check( old->AuxiliaryEvents( 0, 8 ).values.size() == 7 && old->AuxiliaryEvents( 0, 8, StoreAuxiliaryKind::GfxEntity ).values.size() == 1, "unresolved target definition is not invented" );
        Check( live->Update( 1 ), "late auxiliary definition revision" );
        auto newer = live->Snapshot();
        Check( newer->AuxiliaryEvents( 0, 8 ).values.size() == 9 && old->AuxiliaryEvents( 0, 8 ).values.size() == 7, "old auxiliary revisions remain unchanged" );
        const auto domainsPath = root / "runtime-domains.tracy-stream";
        Fixture( domainsPath, 1, true );
        EventStore::Build( domainsPath );
        auto domainStore = EventStore::Open( domainsPath );
        auto d0 = domainStore->RuntimeStatesAt( 25, 0, 1 );
        auto d3 = domainStore->RuntimeStatesAt( 25, uint16_t( d0.next ), 1 );
        auto d255 = domainStore->RuntimeStatesAt( 25, uint16_t( d3.next ), 1 );
        Check( d0.more && d0.next == 1 && std::get<StoreRuntimeState>( d0.values[0].data ).domain == 0, "runtime domain zero pages correctly" );
        Check( d3.more && d3.next == 4 && std::get<StoreRuntimeState>( d3.values[0].data ).generation == 0xfffffffffffffff0ull, "same-time state uses latest protocol event and preserves unsigned generation" );
        Check( !d255.more && d255.next == 256 && std::get<StoreRuntimeState>( d255.values[0].data ).domain == 255 && domainStore->RuntimeStatesAt( 25, 256 ).values.empty(), "unknown future domain and end cursor preserved" );
        bool cancel = false;
        StoreOptions options;
        options.cancelled = [&] { return cancel; };
        store = EventStore::Open( path, options );
        cancel = true;
        bool interrupted = false;
        try
        {
            store->AuxiliaryEvents( 0, 8 );
        }
        catch( const std::exception& )
        {
            interrupted = true;
        }
        Check( interrupted, "auxiliary query cancellation" );
        cancel = false;
        Check( store->AuxiliaryEvents( 0, 8, {}, {}, records[3].id, 1 ).values[0].id == records[4].id, "reopen/cancel keeps stable IDs" );
        std::cout << "Auxiliary tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
