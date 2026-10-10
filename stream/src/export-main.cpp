#include "../../capture/src/CaptureStatus.hpp"
#include "TracyEventStore.hpp"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
namespace
{
std::atomic<bool> cancelled{ false };
BOOL WINAPI Interrupt( DWORD event )
{
    if( event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT ) return FALSE;
    cancelled.store( true );
    return TRUE;
}
void Usage()
{
    std::cout << "Usage: tracy-stream-export input.tracy-stream output.tracy [options]\n"
                 "       tracy-stream-export input.tracy-stream --store-only [--follow STOPFILE] [options]\n"
                 "  --store-only         Build or verify the event store and print its quality summary\n"
                 "                       as JSON instead of exporting a snapshot\n"
                 "  --follow STOPFILE    With --store-only: build while the journal is being recorded;\n"
                 "                       finish when it is complete, or when STOPFILE exists and the\n"
                 "                       journal stopped growing\n"
                 "  --summary FILE       With --store-only: also write the quality summary to FILE\n"
                 "  --force              Replace an existing output after validation\n"
                 "  --skeleton           Write a skeleton snapshot without zones, Job details,\n"
                 "                       graphics records and relations, for analysis tools\n"
                 "  --rebuild            Rebuild the input event store\n"
                 "  --streams N          Compression workers (1-16, default 4)\n"
                 "  --cache-mib N        SQLite page cache budget, 4-256 (default 64): export staging\n"
                 "                       uses N, building the event store N/2, store reads up to 8\n"
                 "  --temp-mib N         Export mapping disk budget (default 65536)\n"
                 "  --max-output-mib N   Compressed output budget (default 65536)\n"
                 "Ctrl+C cancels indexing or export and preserves the destination.\n";
}
std::string Escape( std::string_view text )
{
    std::string out;
    for( const char c : text )
    {
        if( c == '"' || c == '\\' ) out += '\\';
        if( uint8_t( c ) < 0x20 )
        {
            char hex[8];
            snprintf( hex, sizeof( hex ), "\\u%04x", uint8_t( c ) );
            out += hex;
            continue;
        }
        out += c;
    }
    return out;
}
// Quality summary of a built store in the shape of a validation result: the
// store is valid without errors; warnings describe what the journal lacked.
std::string QualityJson( const tracy::stream::StoreInfo& i )
{
    std::ostringstream findings;
    bool valid = true, first = true;
    const auto finding = [&]( const char* severity, const char* code, const char* message, uint64_t count ) {
        if( !count ) return;
        if( std::string_view( severity ) == "error" ) valid = false;
        findings << ( first ? "" : "," ) << "{\"severity\":\"" << severity << "\",\"code\":\"" << code << "\",\"message\":\"" << message << "\",\"count\":" << count << "}";
        first = false;
    };
    finding( "warning", "INCOMPLETE_CAPTURE", "the journal ends without a clean session end", i.cleanEnd ? 0 : 1 );
    finding( "warning", "INCOMPLETE_CPU_ZONES", "CPU zones have no end event", i.openZones );
    finding( "warning", "INCOMPLETE_GPU_ZONES", "GPU zones lack a timestamp or CPU end", i.incompleteGpuZones );
    finding( "warning", "MISPLACED_ZONE_EVENTS", "zone texts, names or colors written for a zone that was not innermost were dropped", i.misplacedZoneEvents );
    finding( "warning", "UNRESOLVED_DEFINITIONS", "events reference definitions absent from the journal", i.unresolvedDefinitions );
    finding( "warning", "TRUNCATED_STREAM_TAIL", "the journal has bytes after its last complete record", i.sourceBytes > i.validBytes ? 1 : 0 );
    std::ostringstream out;
    out << "{\"valid\":" << ( valid ? "true" : "false" ) << ",\"schema\":" << i.schema << ",\"session\":\"" << Escape( i.session ) << "\",\"program\":\"" << Escape( i.program ) << "\""
        << ",\"source_sha256\":\"" << i.sourceSha256 << "\",\"data_sha256\":\"" << i.dataSha256 << "\",\"source_bytes\":" << i.sourceBytes << ",\"valid_bytes\":" << i.validBytes
        << ",\"revision\":" << i.revision << ",\"first_ns\":" << i.firstNs << ",\"last_ns\":" << i.lastNs << ",\"clean_end\":" << ( i.cleanEnd ? "true" : "false" )
        << ",\"transcript_verified\":" << ( i.transcriptVerified ? "true" : "false" ) << ",\"counts\":{\"events\":" << i.events << ",\"cpu_zones\":" << i.cpuZones
        << ",\"open_zones\":" << i.openZones << ",\"misplaced_zone_events\":" << i.misplacedZoneEvents << ",\"gpu_zones\":" << i.gpuZones << ",\"incomplete_gpu_zones\":" << i.incompleteGpuZones << ",\"frames\":" << i.frames
        << ",\"messages\":" << i.messages << ",\"plot_points\":" << i.plotPoints << ",\"lock_events\":" << i.lockEvents << ",\"jobs\":" << i.jobs << ",\"io_requests\":" << i.ioRequests
        << ",\"allocations\":" << i.allocations << ",\"samples\":" << i.samples << ",\"context_regions\":" << i.contextRegions << ",\"auxiliary_events\":" << i.auxiliaryEvents
        << ",\"frame_images\":" << i.frameImages << ",\"unresolved_definitions\":" << i.unresolvedDefinitions << ",\"call_tree_nodes\":" << i.zoneTreeNodes << "}"
        << ",\"findings\":[" << findings.str() << "]}";
    return out.str();
}
uint64_t Number( const char* value )
{
    uint64_t number = 0;
    const std::string_view text = value;
    const auto parsed = std::from_chars( text.data(), text.data() + text.size(), number );
    if( parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || !number ) throw std::runtime_error( "Expected a positive integer option value" );
    return number;
}
}
int ExportMain( int argc, char** argv )
{
    if( argc == 2 && std::string_view( argv[1] ) == "--help" )
    {
        Usage();
        return 0;
    }
    if( argc < 3 )
    {
        Usage();
        return 2;
    }
    tracy::stream::StoreOptions read;
    // Page cache budget, kept within the bounded conversion's 128 MiB Private
    // Commit. Export staging gets all of it: its string dictionary index is
    // accessed randomly and thrashes with a few MiB. Building uses half
    // (ingest is not cache bound and index sorting stays within the build
    // cache). Store reads are ordered scans, and the cache of the store
    // connection also bounds its sorts, so it stays small.
    uint64_t cacheBytes = 64ull << 20;
    tracy::stream::StoreExportOptions options;
    const bool storeOnly = std::string_view( argv[2] ) == "--store-only";
    std::optional<std::filesystem::path> stopFile, summaryFile;
    try
    {
        for( int i = 3; i < argc; ++i )
        {
            const std::string_view key = argv[i];
            if( key == "--force" ) options.overwrite = true;
            else if( key == "--skeleton" ) options.skeleton = true;
            else if( key == "--rebuild" ) read.rebuild = true;
            else if( key == "--follow" || key == "--summary" )
            {
                if( !storeOnly ) throw std::runtime_error( "--follow and --summary require --store-only" );
                if( ++i == argc ) throw std::runtime_error( "Missing export option value" );
                ( key == "--follow" ? stopFile : summaryFile ) = std::filesystem::u8path( argv[i] );
            }
            else
            {
                if( key != "--streams" && key != "--cache-mib" && key != "--temp-mib" && key != "--max-output-mib" ) throw std::runtime_error( "Unknown export option" );
                if( ++i == argc ) throw std::runtime_error( "Missing export option value" );
                const auto number = Number( argv[i] );
                if( key == "--streams" )
                {
                    if( number > 16 ) throw std::runtime_error( "Compression streams must be 1-16" );
                    options.compressionStreams = uint8_t( number );
                }
                else
                {
                    if( number > ( uint64_t( INT64_MAX ) >> 20 ) ) throw std::runtime_error( "Byte budget overflow" );
                    const auto bytes = number << 20;
                    if( key == "--cache-mib" )
                    {
                        if( number < 4 || number > 256 ) throw std::runtime_error( "Cache budget must be 4-256 MiB" );
                        cacheBytes = bytes;
                    }
                    else if( key == "--temp-mib" ) options.temporaryBytes = bytes;
                    else options.fileBytes = bytes;
                }
            }
        }
    }
    catch( const std::exception& e )
    {
        std::cerr << e.what() << '\n';
        return 2;
    }
    SetConsoleCtrlHandler( Interrupt, TRUE );
    struct Handler
    {
        ~Handler()
        {
            SetConsoleCtrlHandler( Interrupt, FALSE );
        }
    } handler;
    read.cancelled = options.cancelled = [] {
        return cancelled.load();
    };
    try
    {
        const auto started = std::chrono::steady_clock::now();
        const auto elapsed = [&] { return std::chrono::duration<double>( std::chrono::steady_clock::now() - started ).count(); };
        // Index progress is written once a second by its own thread: a write
        // to a redirected stderr can stall for milliseconds, and the indexing
        // thread only records where it is. The lock keeps a stage line after
        // the progress it ends. One write per line: std::cerr is unbuffered.
        std::mutex output;
        std::atomic<uint64_t> progressBytes{ 0 }, progressTotal{ 0 };
        uint64_t printedBytes = 0;
        std::atomic<bool> progressDone{ false };
        std::thread progressWriter( [&] {
            while( !progressDone.load() )
            {
                for( int n = 0; n < 10 && !progressDone.load(); ++n ) std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
                std::lock_guard lock( output );
                const auto bytes = progressBytes.load();
                if( bytes == printedBytes ) continue;
                std::cerr << ( "Index " + std::to_string( bytes ) + '/' + std::to_string( progressTotal.load() ) + '\n' );
                printedBytes = bytes;
            }
        } );
        struct ProgressStop
        {
            std::atomic<bool>& done;
            std::thread& writer;
            ~ProgressStop()
            {
                done = true;
                writer.join();
            }
        } progressStop{ progressDone, progressWriter };
        read.stage = [&]( const char* name ) {
            std::lock_guard lock( output );
            printedBytes = progressBytes.load();
            std::cerr << ( std::string( name ) + " elapsed=" + std::to_string( elapsed() ) + "s\n" );
        };
        // This process uses its store handle only from the calling thread.
        read.threadConfined = true;
        read.progress = [&]( uint64_t bytes, uint64_t total ) {
            progressTotal.store( total, std::memory_order_relaxed );
            progressBytes.store( bytes, std::memory_order_relaxed );
        };
        auto build = read;
        build.cacheBytes = std::max<uint64_t>( cacheBytes / 2, 4ull << 20 );
        read.cacheBytes = std::min<uint64_t>( cacheBytes, 8ull << 20 );
        options.cacheBytes = cacheBytes;
        std::cerr << "Open/build event store\n";
        const auto input = std::filesystem::u8path( argv[1] );
        // The summary file appears complete or not at all.
        const auto summarize = [&]( const tracy::stream::StoreInfo& info ) {
            const auto json = QualityJson( info );
            // Flushed here, so the summary reaches a pipe or file before exit.
            std::cout << json << std::endl;
            if( !summaryFile ) return;
            auto temporary = *summaryFile;
            temporary += ".tmp";
            {
                std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
                out << json;
                if( !out ) throw std::runtime_error( "Cannot write store summary" );
            }
            std::filesystem::rename( temporary, *summaryFile );
        };
        if( stopFile )
        {
            summarize( tracy::stream::EventStore::BuildFollowing( input, [&] { return std::filesystem::exists( *stopFile ); }, build ) );
            return 0;
        }
        if( std::filesystem::exists( tracy::stream::EventStore::Directory( input ) ) && tracy::stream::EventStore::StoredSchema( input ) != tracy::stream::EventStoreSchema )
        {
            // Build keeps the older store beside the new one.
            std::cerr << "Rebuilding event store of schema " << tracy::stream::EventStore::StoredSchema( input ) << " as schema " << tracy::stream::EventStoreSchema << "\n";
            build.rebuild = true;
        }
        if( build.rebuild || !std::filesystem::exists( tracy::stream::EventStore::Directory( input ) ) ) tracy::stream::EventStore::Build( input, build );
        auto store = tracy::stream::EventStore::Open( input, read );
        if( storeOnly )
        {
            summarize( store->Info() );
            return 0;
        }
        options.progress = [&]( const char* stage, uint64_t bytes ) {
            std::cerr << stage << " " << bytes << " elapsed=" << elapsed() << "s\n";
        };
        const auto result = store->ExportSnapshot( std::filesystem::u8path( argv[2] ), options );
        std::cout << "Exported " << result.decodedBytes << " decoded bytes into " << result.fileBytes << " bytes; payload SHA256=" << result.sha256 << "; revision=" << result.revision << std::endl;
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << e.what() << '\n';
        return cancelled.load() ? 130 : 1;
    }
}
int wmain( int argc, wchar_t** argv )
{
    auto args = CaptureUtf8Arguments( argc, argv );
    std::vector<char*> pointers;
    for( auto& arg : args ) pointers.push_back( arg.data() );
    return ExportMain( argc, pointers.data() );
}
