#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <inttypes.h>
#include <mutex>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyStackFrames.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyMemory.hpp"
#include "../../server/TracyPrint.hpp"
#include "../../server/TracySysUtil.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../stream/src/TracyStreamProtocol.hpp"

#ifdef _WIN32
#  include "../../getopt/getopt.h"
#endif


// This atomic is written by a signal handler (SigInt). Traditionally that would
// have had to be `volatile sig_atomic_t`, and annoyingly, `bool` was
// technically not allowed there, even though in practice it would work.
// The good thing with C++11 atomics is that we can use atomic<bool> instead
// here and be on the actually supported path.
static std::atomic<bool> s_disconnect { false };
static std::atomic<bool> s_protocolDrainActive { false };
static std::atomic<bool> s_forceStopProtocolDrain { false };

void SigInt( int )
{
    // Relaxed order is closest to a traditional `volatile` write.
    // We don't need stronger ordering since this signal handler doesn't do
    // anything else that would need to be ordered relatively to this.
    if( s_protocolDrainActive.load( std::memory_order_relaxed ) )
        s_forceStopProtocolDrain.store( true, std::memory_order_relaxed );
    else
        s_disconnect.store( true, std::memory_order_relaxed );
}

static bool s_isStdoutATerminal = false;

void InitIsStdoutATerminal() {
#ifdef _WIN32
    s_isStdoutATerminal = _isatty( fileno( stdout ) );
#else
    s_isStdoutATerminal = isatty( fileno( stdout ) );
#endif
}

bool IsStdoutATerminal() { return s_isStdoutATerminal; }

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_BLACK "\033[30m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"
#define ANSI_ERASE_LINE "\033[2K"

// Like printf, but if stdout is a terminal, prepends the output with
// the given `ansiEscape` and appends ANSI_RESET.
void AnsiPrintf( const char* ansiEscape, const char* format, ... ) {
    if( IsStdoutATerminal() )
    {
        // Prepend ansiEscape and append ANSI_RESET.
        char buf[256];
        va_list args;
        va_start( args, format );
        vsnprintf( buf, sizeof buf, format, args );
        va_end( args );
        printf( "%s%s" ANSI_RESET, ansiEscape, buf );
    }
    else
    {
        // Just a normal printf.
        va_list args;
        va_start( args, format );
        vfprintf( stdout, format, args );
        va_end( args );
    }
}

[[noreturn]] void Usage()
{
    printf( "Usage: capture [-o output.tracy] [-j output.tracy-stream] [-a address] [-p port] [-f] [-s seconds] [-m memlimit] [-d drain-idle-seconds]\n" );
    exit( 1 );
}

std::filesystem::path NormalizeOutputPath( const char* value )
{
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical( std::filesystem::path( value ), ec );
    if( ec )
    {
        ec.clear();
        result = std::filesystem::absolute( std::filesystem::path( value ), ec );
        if( ec ) result = std::filesystem::path( value );
        result = result.lexically_normal();
    }
#ifdef _WIN32
    auto native = result.native();
    std::transform( native.begin(), native.end(), native.begin(), []( wchar_t ch ) { return std::towlower( ch ); } );
    return std::filesystem::path( std::move( native ) );
#else
    return result;
#endif
}

bool SameOutputPath( const char* lhs, const char* rhs )
{
    return NormalizeOutputPath( lhs ) == NormalizeOutputPath( rhs );
}

bool ParseIntegerOption( const char* value, int& result )
{
    if( !value || value[0] == '\0' ) return false;
    auto end = value;
    while( *end != '\0' ) end++;
    const auto parsed = std::from_chars( value, end, result );
    return parsed.ec == std::errc() && parsed.ptr == end;
}

int main( int argc, char** argv )
{
#ifdef _WIN32
    if( !AttachConsole( ATTACH_PARENT_PROCESS ) )
    {
        AllocConsole();
        SetConsoleMode( GetStdHandle( STD_OUTPUT_HANDLE ), 0x07 );
    }
#endif

    InitIsStdoutATerminal();

    bool overwrite = false;
    const char* address = "127.0.0.1";
    const char* output = nullptr;
    const char* journalOutput = nullptr;
    int port = 8086;
    int seconds = -1;
    int drainIdleSeconds = 30;
    int64_t memoryLimit = -1;

    int c;
    while( ( c = getopt( argc, argv, "a:o:j:p:fs:m:d:" ) ) != -1 )
    {
        switch( c )
        {
        case 'a':
            address = optarg;
            break;
        case 'o':
            output = optarg;
            break;
        case 'j':
            journalOutput = optarg;
            break;
        case 'p':
            port = atoi( optarg );
            break;
        case 'f':
            overwrite = true;
            break;
        case 's':
            seconds = atoi(optarg);
            break;
        case 'm':
            memoryLimit = std::clamp( atoll( optarg ), 1ll, 999ll ) * tracy::GetPhysicalMemorySize() / 100;
            break;
        case 'd':
            if( !ParseIntegerOption( optarg, drainIdleSeconds ) )
            {
                printf( "Protocol drain idle timeout must be a decimal integer.\n" );
                return 4;
            }
            break;
        default:
            Usage();
            break;
        }
    }

    if( !journalOutput )
    {
        const auto journalFromEnvironment = getenv( "TRACY_STREAM_OUTPUT" );
        if( journalFromEnvironment && journalFromEnvironment[0] != '\0' ) journalOutput = journalFromEnvironment;
    }

    if( !address || address[0] == '\0' || ( !output && !journalOutput ) ) Usage();
    if( port < 1 || port > 65535 )
    {
        printf( "Port must be between 1 and 65535.\n" );
        return 4;
    }
    if( seconds < -1 )
    {
        printf( "Capture duration must be -1 or a non-negative number of seconds.\n" );
        return 4;
    }
    if( drainIdleSeconds < 0 || drainIdleSeconds > 3600 )
    {
        printf( "Protocol drain idle timeout must be between 0 and 3600 seconds. Use 0 to disable it.\n" );
        return 4;
    }
    if( output && journalOutput && SameOutputPath( output, journalOutput ) )
    {
        printf( "Snapshot and journal outputs must use different paths.\n" );
        return 4;
    }

    if( output )
    {
        struct stat st;
        if( stat( output, &st ) == 0 && !overwrite )
        {
            printf( "Output file %s already exists! Use -f to force overwrite.\n", output );
            return 4;
        }

        FILE* test = fopen( output, "wb" );
        if( !test )
        {
            printf( "Cannot open output file %s for writing!\n", output );
            return 5;
        }
        fclose( test );
        unlink( output );
    }

    const bool protocolOnly = journalOutput && !output;
    std::unique_ptr<tracy::stream::StreamProtocolObserver> protocolObserver;
    if( journalOutput )
    {
        tracy::stream::ProtocolJournalOptions journalOptions;
        if( protocolOnly )
            journalOptions.sessionFlags |= tracy::stream::SessionBeginFlagDeferredSymbolExpansion;
        std::string journalError;
        protocolObserver = tracy::stream::StreamProtocolObserver::CreateFileJournal( journalOutput, address, uint16_t( port ), tracy::ProtocolVersion, overwrite, journalOptions, journalError );
        if( !protocolObserver )
        {
            printf( "Cannot create stream journal %s: %s\n", journalOutput, journalError.c_str() );
            return 5;
        }
        printf( "Streaming protocol journal to %s\n", journalOutput );
    }

    printf( "Connecting to %s:%i...", address, port );
    fflush( stdout );
    tracy::Worker worker( address, port, memoryLimit, protocolObserver.get(),
        protocolOnly ? tracy::Worker::Mode::ProtocolOnly : tracy::Worker::Mode::Full );
    while( !worker.HasData() )
    {
        const auto handshake = worker.GetHandshakeStatus();
        if( handshake == tracy::HandshakeProtocolMismatch )
        {
            printf( "\nThe client you are trying to connect to uses incompatible protocol version.\nMake sure you are using the same Tracy version on both client and server.\n" );
            return 1;
        }
        if( handshake == tracy::HandshakeNotAvailable )
        {
            printf( "\nThe client you are trying to connect to is no longer able to sent profiling data,\nbecause another server was already connected to it.\nYou can do the following:\n\n  1. Restart the client application.\n  2. Rebuild the client application with on-demand mode enabled.\n" );
            return 2;
        }
        if( handshake == tracy::HandshakeDropped )
        {
            printf( "\nThe client you are trying to connect to has disconnected during the initial\nconnection handshake. Please check your network configuration.\n" );
            return 3;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }
    printf( "\nTimer resolution: %s\n", tracy::TimeToString( worker.GetResolution() ) );
    if( protocolOnly )
    {
        printf( "Recorder mode: bounded protocol-only (%s memory limit, %zu definitions, %zu queued queries, ",
            tracy::MemSizeToString( worker.GetMemoryLimit() ),
            tracy::Worker::DefaultRecorderDefinitionLimit,
            tracy::Worker::DefaultRecorderQueryQueueLimit );
        if( drainIdleSeconds == 0 )
            printf( "drain idle timeout disabled)\n" );
        else
            printf( "%d s drain idle timeout)\n", drainIdleSeconds );
    }

#ifdef _WIN32
    signal( SIGINT, SigInt );
#else
    struct sigaction sigint, oldsigint;
    memset( &sigint, 0, sizeof( sigint ) );
    sigint.sa_handler = SigInt;
    sigaction( SIGINT, &sigint, &oldsigint );
#endif

    const auto firstTime = protocolOnly ? 0 : worker.GetFirstTime();
    auto& lock = worker.GetMbpsDataLock();

    const auto t0 = std::chrono::high_resolution_clock::now();
    while( worker.IsConnected() )
    {
        // Relaxed order is sufficient here because `s_disconnect` is only ever
        // set by this thread or by the SigInt handler, and that handler does
        // nothing else than storing `s_disconnect`.
        if( s_disconnect.load( std::memory_order_relaxed ) )
        {
            if( protocolOnly )
            {
                s_protocolDrainActive.store( true, std::memory_order_relaxed );
                printf( "\nStopping event capture and resolving pending definitions. Press Ctrl+C again to force stop.\n" );
                fflush( stdout );
            }
            worker.Disconnect();
            // Relaxed order is sufficient because only this thread ever reads
            // this value.
            s_disconnect.store(false, std::memory_order_relaxed );
            break;
        }

        lock.lock();
        const auto mbps = worker.GetMbpsData().back();
        const auto compRatio = worker.GetCompRatio();
        const auto netTotal = worker.GetDataTransferred();
        lock.unlock();

        // Output progress info only if destination is a TTY to avoid bloating
        // log files (so this is not just about usage of ANSI color codes).
        if( IsStdoutATerminal() )
        {
            const char* unit = "Mbps";
            float unitsPerMbps = 1.f;
            if( mbps < 0.1f )
            {
                unit = "Kbps";
                unitsPerMbps = 1000.f;
            }
            AnsiPrintf( ANSI_ERASE_LINE ANSI_CYAN ANSI_BOLD, "\r%7.2f %s", mbps * unitsPerMbps, unit );
            printf( " /");
            AnsiPrintf( ANSI_CYAN ANSI_BOLD, "%5.1f%%", compRatio * 100.f );
            printf( " =");
            AnsiPrintf( ANSI_YELLOW ANSI_BOLD, "%7.2f Mbps", mbps / compRatio );
            printf( " | ");
            AnsiPrintf( ANSI_YELLOW, "Tx: ");
            AnsiPrintf( ANSI_GREEN, "%s", tracy::MemSizeToString( netTotal ) );
            printf( " | ");
            AnsiPrintf( ANSI_RED ANSI_BOLD, "%s", tracy::MemSizeToString( tracy::memUsage.load( std::memory_order_relaxed ) ) );
            if( worker.GetMemoryLimit() > 0 )
            {
                printf( " / " );
                AnsiPrintf( ANSI_BLUE ANSI_BOLD, "%s", tracy::MemSizeToString( worker.GetMemoryLimit() ) );
            }
            printf( " | ");
            if( protocolOnly )
            {
                AnsiPrintf( ANSI_RED, "%" PRIu64 " events", worker.GetProtocolEventCount() );
            }
            else
            {
                AnsiPrintf( ANSI_RED, "%s", tracy::TimeToString( worker.GetLastTime() - firstTime ) );
            }
            fflush( stdout );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        if( seconds != -1 )
        {
            const auto dur = std::chrono::high_resolution_clock::now() - t0;
            if( std::chrono::duration_cast<std::chrono::seconds>(dur).count() >= seconds )
            {
                // Relaxed order is sufficient because only this thread ever reads
                // this value.
                s_disconnect.store(true, std::memory_order_relaxed );
            }
        }
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    auto lastDrainProgress = std::chrono::steady_clock::now();
    uint64_t drainCommittedSize = protocolObserver ? protocolObserver->CommittedSize() : 0;
    uint64_t drainClientBytes = protocolObserver ? protocolObserver->ClientBytes() : 0;
    uint64_t drainServerBytes = protocolObserver ? protocolObserver->ServerBytes() : 0;
    uint64_t drainEventCount = protocolOnly ? worker.GetProtocolEventCount() : 0;
    size_t drainDefinitionCount = protocolOnly ? worker.GetProtocolDefinitionCount() : 0;
    while( worker.IsConnected() )
    {
        if( protocolOnly && s_forceStopProtocolDrain.load( std::memory_order_relaxed ) )
        {
            printf( "\nProtocol drain force-stopped. The committed journal prefix remains recoverable.\n" );
            fflush( stdout );
            std::_Exit( 130 );
        }
        if( protocolOnly && drainIdleSeconds != 0 )
        {
            const auto committedSize = protocolObserver->CommittedSize();
            const auto clientBytes = protocolObserver->ClientBytes();
            const auto serverBytes = protocolObserver->ServerBytes();
            const auto eventCount = worker.GetProtocolEventCount();
            const auto definitionCount = worker.GetProtocolDefinitionCount();
            const auto now = std::chrono::steady_clock::now();
            if( committedSize != drainCommittedSize || clientBytes != drainClientBytes || serverBytes != drainServerBytes ||
                eventCount != drainEventCount || definitionCount != drainDefinitionCount )
            {
                drainCommittedSize = committedSize;
                drainClientBytes = clientBytes;
                drainServerBytes = serverBytes;
                drainEventCount = eventCount;
                drainDefinitionCount = definitionCount;
                lastDrainProgress = now;
            }
            else if( std::chrono::duration_cast<std::chrono::seconds>( now - lastDrainProgress ).count() >= drainIdleSeconds )
            {
                printf( "\nProtocol drain made no progress for %d seconds and was force-stopped. "
                    "The committed journal prefix remains recoverable.\n", drainIdleSeconds );
                fflush( stdout );
                std::_Exit( 124 );
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    s_protocolDrainActive.store( false, std::memory_order_relaxed );
    const auto t2 = std::chrono::high_resolution_clock::now();

    const auto& failure = worker.GetFailureType();
    if( !protocolOnly && failure != tracy::Worker::Failure::None )
    {
        AnsiPrintf( ANSI_RED ANSI_BOLD, "\nInstrumentation failure: %s", tracy::Worker::GetFailureString( failure ) );
        auto& fd = worker.GetFailureData();
        if( !fd.message.empty() )
        {
            printf( "\nContext: %s", fd.message.c_str() );
        }
        if( fd.callstack != 0 )
        {
            AnsiPrintf( ANSI_BOLD, "\nFailure callstack:\n" );
            auto& cs = worker.GetCallstack( fd.callstack );
            int fidx = 0;
            for( auto& entry : cs )
            {
                auto frameData = worker.GetCallstackFrame( entry );
                if( !frameData )
                {
                    printf( "%3i. %p\n", fidx++, (void*)worker.GetCanonicalPointer( entry ) );
                }
                else
                {
                    const auto fsz = frameData->size;
                    for( uint8_t f=0; f<fsz; f++ )
                    {
                        const auto& frame = frameData->data[f];
                        auto txt = worker.GetString( frame.name );

                        if( fidx == 0 && f != fsz-1 )
                        {
                            auto test = tracy::s_tracyStackFrames;
                            bool match = false;
                            do
                            {
                                if( strcmp( txt, *test ) == 0 )
                                {
                                    match = true;
                                    break;
                                }
                            }
                            while( *++test );
                            if( match ) continue;
                        }

                        if( f == fsz-1 )
                        {
                            printf( "%3i. ", fidx++ );
                        }
                        else
                        {
                            AnsiPrintf( ANSI_BLACK ANSI_BOLD, "inl. " );
                        }
                        AnsiPrintf( ANSI_CYAN, "%s  ", txt );
                        txt = worker.GetString( frame.file );
                        if( frame.line == 0 )
                        {
                            AnsiPrintf( ANSI_YELLOW, "(%s)", txt );
                        }
                        else
                        {
                            AnsiPrintf( ANSI_YELLOW, "(%s:%" PRIu32 ")", txt, frame.line );
                        }
                        if( frameData->imageName.Active() )
                        {
                            AnsiPrintf( ANSI_MAGENTA, " %s\n", worker.GetString( frameData->imageName ) );
                        }
                        else
                        {
                            printf( "\n" );
                        }
                    }
                }
            }
        }
    }

    if( protocolOnly )
    {
        printf( "\nProtocol events: %" PRIu64 "\nRetained definitions/state: %zu\nCapture time: %s\nProtocol drain time: %s\n",
            worker.GetProtocolEventCount(), worker.GetProtocolDefinitionCount(),
            tracy::TimeToString( std::chrono::duration_cast<std::chrono::nanoseconds>( t1 - t0 ).count() ),
            tracy::TimeToString( std::chrono::duration_cast<std::chrono::nanoseconds>( t2 - t1 ).count() ) );
        if( worker.DidProtocolResolverFail() )
        {
            AnsiPrintf( ANSI_RED ANSI_BOLD, "Protocol resolver failed: %s\n", worker.GetProtocolResolverError().c_str() );
        }
    }
    else
    {
        printf( "\nFrames: %" PRIu64 "\nTime span: %s\nZones: %s\nElapsed time: %s\n",
            worker.GetFrameCount( *worker.GetFramesBase() ), tracy::TimeToString( worker.GetLastTime() - firstTime ), tracy::RealToString( worker.GetZoneCount() ),
            tracy::TimeToString( std::chrono::duration_cast<std::chrono::nanoseconds>( t1 - t0 ).count() ) );
    }
    if( protocolObserver )
    {
        const std::string committedText = tracy::MemSizeToString( protocolObserver->CommittedSize() );
        const std::string durableText = tracy::MemSizeToString( protocolObserver->DurableSize() );
        printf( "Stream journal: %s committed, %s durable, %" PRIu64 " client bytes, %" PRIu64 " server bytes\n",
            committedText.c_str(), durableText.c_str(),
            protocolObserver->ClientBytes(), protocolObserver->ServerBytes() );
        if( protocolObserver->Failed() )
        {
            AnsiPrintf( ANSI_RED ANSI_BOLD, "Stream journal failed: %s\n", protocolObserver->LastError().c_str() );
        }
    }

    if( output )
    {
        printf( "Saving trace..." );
        fflush( stdout );
        auto f = std::unique_ptr<tracy::FileWrite>( tracy::FileWrite::Open( output, tracy::FileCompression::Zstd, 3, 4 ) );
        if( f )
        {
            worker.Write( *f, false );
            AnsiPrintf( ANSI_GREEN ANSI_BOLD, " done!\n" );
            f->Finish();
            const auto stats = f->GetCompressionStatistics();
            printf( "Trace size %s (%.2f%% ratio)\n", tracy::MemSizeToString( stats.second ), 100.f * stats.second / stats.first );
        }
        else
        {
            AnsiPrintf( ANSI_RED ANSI_BOLD, " failed!\n");
        }
    }

    if( protocolObserver && protocolObserver->Failed() ) return 6;
    if( worker.DidProtocolResolverFail() ) return 7;
    return 0;
}
