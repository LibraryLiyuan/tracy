#include "../../public/common/TracyAlloc.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracySocket.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../stream/src/TracyStreamJournal.hpp"
#include "../../stream/src/TracyStreamReplay.hpp"

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
    bool hasLocalDisconnect = false;
    bool hasSessionBegin = false;
    bool deferSymbolExpansion = false;
    tracy::stream::RecordInfo sessionBeginRecord;
    uint64_t drainControlSequence = 0;
    tracy::stream::RecordInfo drainControlRecord;
    uint8_t drainControlVersion = 0;
    uint32_t serverQuerySpaceOverride = 0;
    clientRecords.reserve( scan.records.size() );
    serverRecords.reserve( 1024 );
    for( const auto& record : scan.records )
    {
        if( record.type == tracy::stream::RecordType::SessionBegin && !hasSessionBegin )
        {
            hasSessionBegin = true;
            sessionBeginRecord = record;
        }
        else if( record.type == tracy::stream::RecordType::ClientToServer )
            clientRecords.push_back( record );
        else if( record.type == tracy::stream::RecordType::ServerToClient )
            serverRecords.push_back( record );
        else if( record.type == tracy::stream::RecordType::Diagnostic &&
            ( record.flags & tracy::stream::RecordFlagLocalControl ) != 0 )
        {
            hasLocalDisconnect = true;
            if( ( record.flags & tracy::stream::RecordFlagServerQuery ) == 0 )
            {
                drainControlSequence = record.sequence;
                drainControlRecord = record;
            }
        }
    }
    if( clientRecords.empty() || serverRecords.empty() )
    {
        std::fprintf( stderr, "Journal does not contain a bidirectional Tracy handshake.\n" );
        return 2;
    }
    if( hasSessionBegin )
    {
        PayloadReader sessionReader( options.input );
        std::vector<uint8_t> payload;
        std::string error;
        if( !sessionReader.IsOpen() || !sessionReader.Read( sessionBeginRecord, payload, error ) )
        {
            std::fprintf( stderr, "Cannot read session metadata: %s.\n", error.c_str() );
            return 2;
        }
        if( payload.size() >= 2 )
        {
            const auto version = uint16_t( payload[0] ) | ( uint16_t( payload[1] ) << 8 );
            if( version == 2 )
            {
                if( payload.size() < 24 )
                {
                    std::fprintf( stderr, "Invalid version 2 session metadata.\n" );
                    return 2;
                }
                const auto headerSize = uint16_t( payload[2] ) | ( uint16_t( payload[3] ) << 8 );
                const auto flags =
                    uint32_t( payload[16] ) |
                    ( uint32_t( payload[17] ) << 8 ) |
                    ( uint32_t( payload[18] ) << 16 ) |
                    ( uint32_t( payload[19] ) << 24 );
                if( headerSize != 24 || ( flags & ~tracy::stream::SessionBeginSupportedFlags ) != 0 )
                {
                    std::fprintf( stderr, "Unsupported version 2 session metadata.\n" );
                    return 2;
                }
                deferSymbolExpansion = ( flags & tracy::stream::SessionBeginFlagDeferredSymbolExpansion ) != 0;
            }
            else if( version != 1 )
            {
                std::fprintf( stderr, "Unsupported session metadata version %u.\n", version );
                return 2;
            }
        }
    }
    if( drainControlSequence != 0 )
    {
        PayloadReader drainReader( options.input );
        std::vector<uint8_t> payload;
        std::string error;
        if( !drainReader.IsOpen() || !drainReader.Read( drainControlRecord, payload, error ) )
        {
            std::fprintf( stderr, "Cannot read protocol drain control record: %s.\n", error.c_str() );
            return 2;
        }
        if( payload.size() == 1 && payload[0] >= 1 && payload[0] <= 3 )
        {
            drainControlVersion = payload[0];
        }
        else if( payload.size() == 5 && payload[0] == 4 )
        {
            drainControlVersion = payload[0];
            serverQuerySpaceOverride =
                uint32_t( payload[1] ) |
                ( uint32_t( payload[2] ) << 8 ) |
                ( uint32_t( payload[3] ) << 16 ) |
                ( uint32_t( payload[4] ) << 24 );
            if( serverQuerySpaceOverride == 0 || serverQuerySpaceOverride > 8 * 1024 )
            {
                std::fprintf( stderr, "Invalid recorded server-query window.\n" );
                return 2;
            }
        }
        else
        {
            std::fprintf( stderr, "Unsupported protocol drain control payload.\n" );
            return 2;
        }
    }

    EnableLocalReplayOnly();
    tracy::ListenSocket listener;
    if( !listener.Listen( options.port, 1 ) )
    {
        std::fprintf( stderr, "Cannot bind local replay port %u; choose another port with -p.\n", options.port );
        return 4;
    }

    std::printf( "Replaying %zu client records and validating %zu server records on 127.0.0.1:%u...\n", clientRecords.size(), serverRecords.size(), options.port );
    const bool replayProtocolOnly = deferSymbolExpansion || drainControlVersion >= 3;
    tracy::Worker worker( "127.0.0.1", options.port, -1, nullptr, tracy::Worker::Mode::Full,
        tracy::Worker::DefaultRecorderDefinitionLimit, tracy::Worker::DefaultRecorderQueryQueueLimit,
        replayProtocolOnly, serverQuerySpaceOverride, replayProtocolOnly );
    if( hasLocalDisconnect && drainControlSequence == 0 ) worker.MarkProtocolDisconnect();

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
    if( !peer->SetSendTimeout( 10000 ) )
    {
        worker.Shutdown();
        std::fprintf( stderr, "Cannot configure the local replay send timeout.\n" );
        return 4;
    }

    ReplayError replayError;
    std::thread verifier( [&] {
        const auto failReplay = [&]( std::string message ) {
            replayError.Set( std::move( message ) );
            peer->Close();
        };
        PayloadReader reader( options.input );
        if( !reader.IsOpen() )
        {
            failReplay( "cannot open journal for server-stream verification" );
            return;
        }
        tracy::stream::ReplayServerTranscriptVerifier transcriptVerifier;
        std::string error;
        for( const auto& record : serverRecords )
        {
            if( replayError.Failed() ) return;
            tracy::stream::ReplayServerPacket recorded { record.sequence, record.flags, {} };
            if( !reader.Read( record, recorded.payload, error ) )
            {
                failReplay( "sequence " + std::to_string( record.sequence ) + ": " + error );
                return;
            }
            tracy::stream::ReplayServerPacket replayed { record.sequence, record.flags, {} };
            replayed.payload.resize( recorded.payload.size() );
            const auto recordDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
            if( !replayed.payload.empty() && !peer->Read( replayed.payload.data(), int( replayed.payload.size() ), 100, [&] {
                return replayError.Failed() || std::chrono::steady_clock::now() >= recordDeadline;
            } ) )
            {
                failReplay( "sequence " + std::to_string( record.sequence ) + ": Worker server stream ended early" );
                return;
            }
            if( !transcriptVerifier.Append( recorded, replayed, error ) )
            {
                failReplay( error );
                return;
            }
        }
        if( !transcriptVerifier.Finish( error ) )
        {
            failReplay( error );
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
        auto replayClientRecord = [&]( const tracy::stream::RecordInfo& record ) {
            if( replayError.Failed() ) return false;
            if( !clientReader.Read( record, payload, error ) )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) + ": " + error );
                return false;
            }
            if( !payload.empty() && peer->Send( payload.data(), int( payload.size() ) ) != int( payload.size() ) )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) + ": cannot send recorded client stream" );
                return false;
            }
            return true;
        };

        if( drainControlSequence == 0 )
        {
            for( const auto& record : clientRecords )
            {
                if( !replayClientRecord( record ) ) break;
            }
        }
        else
        {
            const auto preDrainFrameCount = std::count_if( clientRecords.begin(), clientRecords.end(), [&]( const auto& record ) {
                return record.sequence < drainControlSequence &&
                    ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0;
            } );
            if( preDrainFrameCount < 2 )
            {
                replayError.Set( "drain marker does not have two preceding client frames" );
            }

            const auto prefixFrameTarget = uint64_t( preDrainFrameCount >= 2 ? preDrainFrameCount - 2 : 0 );
            uint64_t sentFrames = 0;
            size_t splitIndex = 0;
            for( ; splitIndex < clientRecords.size() && !replayError.Failed(); splitIndex++ )
            {
                const auto& record = clientRecords[splitIndex];
                if( record.sequence > drainControlSequence ) break;
                const bool compressed = ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0;
                if( compressed && sentFrames == prefixFrameTarget ) break;
                if( !replayClientRecord( record ) ) break;
                if( compressed ) sentFrames++;
            }

            const auto prefixDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
            while( worker.GetProtocolFramesProcessed() < prefixFrameTarget &&
                !replayError.Failed() && std::chrono::steady_clock::now() < prefixDeadline )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
            if( worker.GetProtocolFramesProcessed() < prefixFrameTarget )
            {
                replayError.Set( "Worker did not process the pre-drain client revision" );
            }
            else if( !replayError.Failed() )
            {
                worker.RequestProtocolDrain( drainControlVersion >= 2 );
                for( ; splitIndex < clientRecords.size(); splitIndex++ )
                {
                    const auto& record = clientRecords[splitIndex];
                    if( record.sequence > drainControlSequence ) break;
                    if( !replayClientRecord( record ) ) break;
                }
                const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
                while( !worker.IsProtocolDrainActive() && !replayError.Failed() &&
                    std::chrono::steady_clock::now() < drainDeadline )
                {
                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                }
                if( !worker.IsProtocolDrainActive() )
                {
                    replayError.Set( "Worker did not enter protocol drain mode" );
                }
                else
                {
                    for( ; splitIndex < clientRecords.size(); splitIndex++ )
                    {
                        if( !replayClientRecord( clientRecords[splitIndex] ) ) break;
                    }
                }
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
