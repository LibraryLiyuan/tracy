#include "../../capture/src/CaptureStatus.hpp"
#include "TracyEventStore.hpp"
#include <charconv>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace tracy::stream;
std::string Q( std::string_view s )
{
    std::string r = "\"";
    for( unsigned char c : s )
    {
        if( c == '"' || c == '\\' )
        {
            r += '\\';
            r += char( c );
        }
        else if( c < 32 )
        {
            char b[7];
            std::snprintf( b, sizeof( b ), "\\u%04x", c );
            r += b;
        }
        else r += char( c );
    }
    return r + '"';
}
int StoreMain( int argc, char** argv )
{
    if( argc < 2 )
    {
        std::puts( "Usage: tracy-stream-index input.tracy-stream [--rebuild] [--verify-transcript] [--query begin_ns end_ns] [--after ID] [--limit N]" );
        return 2;
    }
    try
    {
        int64_t begin = INT64_MIN, end = INT64_MAX;
        uint64_t after = 0, limit = 256;
        bool query = false;
        bool verifyTranscript = false;
        StoreOptions options;
        auto parse = []( const char* text, auto& value ) {std::string_view s=text;auto p=std::from_chars(s.data(),s.data()+s.size(),value);if(p.ec!=std::errc()||p.ptr!=s.data()+s.size())throw std::runtime_error("Invalid integer argument"); };
        for( int a = 2; a < argc; ++a )
        {
            std::string_view key = argv[a];
            if( key == "--query" && a + 2 < argc )
            {
                query = true;
                parse( argv[++a], begin );
                parse( argv[++a], end );
            }
            else if( key == "--after" && a + 1 < argc ) parse( argv[++a], after );
            else if( key == "--limit" && a + 1 < argc ) parse( argv[++a], limit );
            else if( key == "--rebuild" ) options.rebuild = true;
            else if( key == "--verify-transcript" ) verifyTranscript = true;
            else throw std::runtime_error( "Unknown argument" );
        }
        auto path = std::filesystem::u8path( argv[1] );
        auto info = EventStore::Build( path, options );
        if( verifyTranscript && !info.transcriptVerified && !options.rebuild )
        {
            options.rebuild = true;
            info = EventStore::Build( path, options );
        }
        if( verifyTranscript && !info.transcriptVerified ) throw std::runtime_error( "Transcript proof requires a complete deterministic ProtocolOnly session" );
        std::fprintf( stderr, "Independent server transcript verified: %s\n", info.transcriptVerified ? "true" : "false" );
        if( !query )
        {
            std::printf( "{\"schema\":%u,\"session\":%s,\"program\":%s,\"events\":\"%llu\",\"cpu_zones\":\"%llu\",\"gpu_zones\":\"%llu\",\"incomplete_gpu_zones\":\"%llu\",\"frames\":\"%llu\",\"last_ns\":\"%lld\",\"open_zones\":\"%llu\",\"unresolved_definitions\":\"%llu\",\"allocations\":\"%llu\",\"active_allocations\":\"%llu\",\"clean_end\":%s,\"store\":%s}\n", info.schema, Q( info.session ).c_str(), Q( info.program ).c_str(), (unsigned long long)info.events, (unsigned long long)info.cpuZones, (unsigned long long)info.gpuZones, (unsigned long long)info.incompleteGpuZones, (unsigned long long)info.frames, (long long)info.lastNs, (unsigned long long)info.openZones, (unsigned long long)info.unresolvedDefinitions, (unsigned long long)info.allocations, (unsigned long long)info.activeAllocations, info.cleanEnd ? "true" : "false", Q( EventStore::Directory( path ).string() ).c_str() );
            return 0;
        }
        auto store = EventStore::Open( path );
        auto page = store->Zones( begin, end, {}, after, size_t( limit ) );
        std::printf( "{\"more\":%s,\"next\":\"%llu\",\"zones\":[", page.more ? "true" : "false", (unsigned long long)page.next );
        bool first = true;
        for( auto& z : page.values )
        {
            if( !first ) std::putchar( ',' );
            first = false;
            std::printf( "{\"id\":\"%llu\",\"thread\":\"%llu\",\"parent\":\"%llu\",\"depth\":%u,\"start\":\"%lld\",\"end\":%s,\"stack\":\"%llu\",\"callsite\":\"%llu\",\"name\":%s,\"function\":%s,\"file\":%s,\"line\":%u,\"color\":%u,\"text\":%s}", (unsigned long long)z.id, (unsigned long long)z.thread, (unsigned long long)z.parent, z.depth, (long long)z.start, z.end ? Q( std::to_string( *z.end ) ).c_str() : "null", (unsigned long long)z.stack, (unsigned long long)z.callsite, Q( z.name ).c_str(), Q( z.function ).c_str(), Q( z.file ).c_str(), z.line, z.color, Q( z.text ).c_str() );
        }
        std::puts( "]}" );
        return 0;
    }
    catch( const std::exception& e )
    {
        std::fprintf( stderr, "%s\n", e.what() );
        return 1;
    }
}
#ifdef _WIN32
int wmain( int argc, wchar_t** argv )
{
    auto args = CaptureUtf8Arguments( argc, argv );
    std::vector<char*> pointers;
    for( auto& a : args ) pointers.push_back( a.data() );
    return StoreMain( argc, pointers.data() );
}
#else
int main( int argc, char** argv ) { return StoreMain( argc, argv ); }
#endif
