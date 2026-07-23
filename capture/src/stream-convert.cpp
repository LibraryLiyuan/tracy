#include "../../public/common/TracyAlloc.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracySocket.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../stream/src/TracyStreamJournal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

struct Options
{
    std::filesystem::path input;
    std::filesystem::path output;
    uint16_t port = 18086;
    bool overwrite = false;
};

void Usage()
{
    std::fprintf( stderr, "Usage: tracy-stream-convert -i input.tracy-stream -o output.tracy [-p port] [-f]\n" );
}

bool ParsePort( const char* text, uint16_t& port )
{
    char* end = nullptr;
    const auto value = std::strtol( text, &end, 10 );
    if( !end || *end != '\0' || value < 1 || value > 65535 ) return false;
    port = uint16_t( value );
    return true;
}

bool ParseArguments( int argc, char** argv, Options& options )
{
    for( int i = 1; i < argc; i++ )
    {
        const std::string_view argument = argv[i];
        if( argument == "-i" && i + 1 < argc )
        {
            options.input = std::filesystem::u8path( argv[++i] );
        }
        else if( argument == "-o" && i + 1 < argc )
        {
            options.output = std::filesystem::u8path( argv[++i] );
        }
        else if( argument == "-p" && i + 1 < argc )
        {
            if( !ParsePort( argv[++i], options.port ) ) return false;
        }
        else if( argument == "-f" )
        {
            options.overwrite = true;
        }
        else
        {
            return false;
        }
    }
    return !options.input.empty() && !options.output.empty();
}

class PayloadReader
{
public:
    explicit PayloadReader( const std::filesystem::path& path )
        : m_file( path, std::ios::binary )
    {
    }

    bool IsOpen() const { return bool( m_file ); }

    bool Read( const tracy::stream::RecordInfo& record, std::vector<uint8_t>& payload, std::string& error )
    {
        if( record.payloadSize > uint64_t( std::numeric_limits<size_t>::max() ) )
        {
            error = "record payload does not fit in memory";
            return false;
        }
        const auto offset = record.offset + tracy::stream::RecordHeaderSize;
        if( offset > uint64_t( std::numeric_limits<std::streamoff>::max() ) )
        {
            error = "record offset exceeds stream API";
            return false;
        }
        payload.resize( size_t( record.payloadSize ) );
        m_file.clear();
        m_file.seekg( std::streamoff( offset ), std::ios::beg );
        if( !m_file )
        {
            error = "cannot seek to journal payload";
            return false;
        }
        if( !payload.empty() )
        {
            m_file.read( reinterpret_cast<char*>( payload.data() ), std::streamsize( payload.size() ) );
            if( !m_file || m_file.gcount() != std::streamsize( payload.size() ) )
            {
                error = "cannot read journal payload";
                return false;
            }
        }
        return true;
    }

private:
    std::ifstream m_file;
};

struct SocketDeleter
{
    void operator()( tracy::Socket* socket ) const
    {
        if( !socket ) return;
        socket->~Socket();
        tracy::tracy_free( socket );
    }
};

class ReplayError
{
public:
    void Set( std::string message )
    {
        std::lock_guard<std::mutex> lock( m_lock );
        if( m_message.empty() ) m_message = std::move( message );
        m_failed.store( true, std::memory_order_relaxed );
    }

    bool Failed() const { return m_failed.load( std::memory_order_relaxed ); }

    std::string Message() const
    {
        std::lock_guard<std::mutex> lock( m_lock );
        return m_message;
    }

private:
    mutable std::mutex m_lock;
    std::atomic<bool> m_failed { false };
    std::string m_message;
};

bool SamePath( const std::filesystem::path& left, const std::filesystem::path& right )
{
    std::error_code ec;
    if( std::filesystem::exists( left, ec ) && !ec && std::filesystem::exists( right, ec ) && !ec )
    {
        if( std::filesystem::equivalent( left, right, ec ) && !ec ) return true;
    }
    ec.clear();
    const auto absoluteLeft = std::filesystem::absolute( left, ec ).lexically_normal();
    if( ec ) return false;
    const auto absoluteRight = std::filesystem::absolute( right, ec ).lexically_normal();
    if( ec ) return false;
#ifdef _WIN32
    auto leftText = absoluteLeft.wstring();
    auto rightText = absoluteRight.wstring();
    std::transform( leftText.begin(), leftText.end(), leftText.begin(), []( wchar_t value ) { return wchar_t( std::towlower( value ) ); } );
    std::transform( rightText.begin(), rightText.end(), rightText.begin(), []( wchar_t value ) { return wchar_t( std::towlower( value ) ); } );
    return leftText == rightText;
#else
    return absoluteLeft == absoluteRight;
#endif
}

void EnableLocalReplayOnly()
{
#ifdef _WIN32
    _putenv_s( "TRACY_ONLY_LOCALHOST", "1" );
    _putenv_s( "TRACY_ONLY_IPV4", "1" );
#else
    setenv( "TRACY_ONLY_LOCALHOST", "1", 1 );
    setenv( "TRACY_ONLY_IPV4", "1", 1 );
#endif
}

}

int main( int argc, char** argv )
{
    Options options;
    if( !ParseArguments( argc, argv, options ) )
    {
        Usage();
        return 1;
    }
    if( SamePath( options.input, options.output ) )
    {
        std::fprintf( stderr, "Input journal and output snapshot must use different paths.\n" );
        return 1;
    }

    tracy::stream::ScanOptions scanOptions;
    scanOptions.maxCollectedRecords = 2'000'000;
    const auto scan = tracy::stream::ScanJournal( options.input, scanOptions );
    if( !scan.HasRecoverablePrefix() )
    {
        std::fprintf( stderr, "Journal cannot be replayed: %s (%s)\n", scan.message.c_str(), tracy::stream::ScanCodeName( scan.code ) );
        return 2;
    }
    if( scan.records.size() != scan.recordCount )
    {
        std::fprintf( stderr, "Journal has too many records for this converter (%llu > %zu).\n", static_cast<unsigned long long>( scan.recordCount ), scan.records.size() );
        return 2;
    }
    if( scan.header.protocolVersion != tracy::ProtocolVersion )
    {
        std::fprintf( stderr, "Journal protocol version %u does not match this converter (%u).\n", scan.header.protocolVersion, tracy::ProtocolVersion );
        return 2;
    }
    if( scan.code != tracy::stream::ScanCode::Ok )
    {
        std::fprintf( stderr, "Warning: replaying valid prefix ending at byte %llu; ignored tail status is %s.\n",
            static_cast<unsigned long long>( scan.validSize ), tracy::stream::ScanCodeName( scan.code ) );
    }

    std::error_code filesystemError;
    if( std::filesystem::exists( options.output, filesystemError ) && !filesystemError && !options.overwrite )
    {
        std::fprintf( stderr, "Output snapshot already exists; use -f to overwrite it.\n" );
        return 3;
    }

    std::vector<tracy::stream::RecordInfo> clientRecords;
    std::vector<tracy::stream::RecordInfo> serverRecords;
    clientRecords.reserve( scan.records.size() );
    serverRecords.reserve( 1024 );
    for( const auto& record : scan.records )
    {
        if( record.type == tracy::stream::RecordType::ClientToServer )
            clientRecords.push_back( record );
        else if( record.type == tracy::stream::RecordType::ServerToClient )
            serverRecords.push_back( record );
    }
    if( clientRecords.empty() || serverRecords.empty() )
    {
        std::fprintf( stderr, "Journal does not contain a bidirectional Tracy handshake.\n" );
        return 2;
    }

    EnableLocalReplayOnly();
    tracy::ListenSocket listener;
    if( !listener.Listen( options.port, 1 ) )
    {
        std::fprintf( stderr, "Cannot bind local replay port %u; choose another port with -p.\n", options.port );
        return 4;
    }

    std::printf( "Replaying %zu client records and validating %zu server records on 127.0.0.1:%u...\n", clientRecords.size(), serverRecords.size(), options.port );
    tracy::Worker worker( "127.0.0.1", options.port, -1 );

    std::unique_ptr<tracy::Socket, SocketDeleter> peer;
    const auto acceptDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( !peer && std::chrono::steady_clock::now() < acceptDeadline )
    {
        peer.reset( listener.Accept() );
    }
    if( !peer )
    {
        worker.Shutdown();
        std::fprintf( stderr, "Worker did not connect to the local replay socket.\n" );
        return 4;
    }

    ReplayError replayError;
    std::thread verifier( [&] {
        PayloadReader reader( options.input );
        if( !reader.IsOpen() )
        {
            replayError.Set( "cannot open journal for server-stream verification" );
            return;
        }
        std::vector<uint8_t> expected;
        std::vector<uint8_t> actual;
        std::string error;
        for( const auto& record : serverRecords )
        {
            if( replayError.Failed() ) return;
            if( !reader.Read( record, expected, error ) )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) + ": " + error );
                return;
            }
            actual.resize( expected.size() );
            if( !actual.empty() && !peer->Read( actual.data(), int( actual.size() ), 1000 ) )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) + ": Worker server stream ended early" );
                return;
            }
            const auto mismatch = std::mismatch( expected.begin(), expected.end(), actual.begin(), actual.end() );
            if( mismatch.first != expected.end() )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) + ": Worker server-query stream diverged at payload byte " + std::to_string( mismatch.first - expected.begin() ) );
                return;
            }
        }
    } );

    PayloadReader clientReader( options.input );
    if( !clientReader.IsOpen() )
    {
        replayError.Set( "cannot open journal for client-stream replay" );
    }
    else
    {
        std::vector<uint8_t> payload;
        std::string error;
        for( const auto& record : clientRecords )
        {
            if( replayError.Failed() ) break;
            if( !clientReader.Read( record, payload, error ) )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) + ": " + error );
                break;
            }
            if( !payload.empty() && peer->Send( payload.data(), int( payload.size() ) ) != int( payload.size() ) )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) + ": cannot send recorded client stream" );
                break;
            }
        }
    }

    if( replayError.Failed() && peer->IsValid() ) peer->Close();
    verifier.join();
    if( !replayError.Failed() )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        uint8_t extraByte = 0;
        if( peer->HasData() && peer->ReadRaw( &extraByte, 1, 1 ) )
        {
            if( scan.complete )
            {
                replayError.Set( "Worker emitted server bytes beyond the recorded complete transcript" );
            }
            else
            {
                std::fprintf( stderr, "Warning: incomplete journal ended before a trailing Worker query; the valid client prefix remains replayable.\n" );
            }
        }
    }
    if( peer->IsValid() ) peer->Close();

    const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
    while( !worker.HasData() && worker.GetHandshakeStatus() == tracy::HandshakePending && std::chrono::steady_clock::now() < workerDeadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    while( worker.IsConnected() && std::chrono::steady_clock::now() < workerDeadline )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    if( worker.IsConnected() )
    {
        worker.Disconnect();
        replayError.Set( "Worker did not finish after replay socket closed" );
    }
    if( !worker.HasData() )
    {
        replayError.Set( "Worker rejected the replay before receiving capture metadata" );
    }
    if( replayError.Failed() )
    {
        std::fprintf( stderr, "Replay failed: %s\n", replayError.Message().c_str() );
        return 5;
    }

    auto output = std::unique_ptr<tracy::FileWrite>( tracy::FileWrite::Open( options.output.string().c_str(), tracy::FileCompression::Zstd, 3, 4 ) );
    if( !output )
    {
        std::fprintf( stderr, "Cannot create output snapshot.\n" );
        return 6;
    }
    worker.Write( *output, false );
    output->Finish();
    const auto statistics = output->GetCompressionStatistics();
    std::printf( "Converted %llu valid journal bytes into %llu snapshot bytes (%.2f%%).\n",
        static_cast<unsigned long long>( scan.validSize ), static_cast<unsigned long long>( statistics.second ),
        statistics.first == 0 ? 0. : 100. * statistics.second / statistics.first );
    return 0;
}
