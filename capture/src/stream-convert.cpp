#include "../../public/common/TracyAlloc.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracySocket.hpp"
#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../stream/src/TracyStreamJournal.hpp"
#include "../../stream/src/TracyStreamReplay.hpp"
#include "../../stream/src/TracyStreamSnapshotMap.hpp"

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

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace
{

struct Options
{
    enum class Mode
    {
        Offline,
        Legacy
    };

    std::filesystem::path input;
    std::filesystem::path output;
    uint16_t port = 18086;
    bool overwrite = false;
    Mode mode = Mode::Offline;
};

void Usage()
{
    std::fprintf( stderr,
        "Usage: tracy-stream-convert -i input.tracy-stream -o output.tracy [-f] "
        "[--mode offline|legacy] [-p legacy-port]\n" );
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
        else if( argument == "--mode" && i + 1 < argc )
        {
            const std::string_view mode = argv[++i];
            if( mode == "offline" ) options.mode = Options::Mode::Offline;
            else if( mode == "legacy" ) options.mode = Options::Mode::Legacy;
            else return false;
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

// A one-record bounded client mailbox plus an in-process verifier for the
// Worker's server stream.  It preserves the byte stream and causal ordering
// used by legacy socket replay, but no kernel socket, listener, port or
// verifier thread is involved.
class OfflineReplayTransport final : public tracy::WorkerOfflineTransport
{
public:
    OfflineReplayTransport(
        const std::filesystem::path& path,
        const std::vector<tracy::stream::RecordInfo>& serverRecords,
        bool complete,
        ReplayError& replayError,
        std::atomic<uint64_t>& replayedServerSequence )
        : m_reader( path )
        , m_serverRecords( serverRecords )
        , m_complete( complete )
        , m_replayError( replayError )
        , m_replayedServerSequence( replayedServerSequence )
    {
        if( !m_reader.IsOpen() ) FailLocked( "cannot open journal for in-process server verification" );
    }

    bool Connect() override
    {
        std::lock_guard<std::mutex> lock( m_lock );
        return m_valid;
    }

    bool Read( void* data, int size, int timeoutMs, const std::atomic<bool>& shutdown ) override
    {
        if( size < 0 ) return false;
        auto output = static_cast<uint8_t*>( data );
        size_t remaining = size_t( size );
        std::unique_lock<std::mutex> lock( m_lock );
        while( remaining != 0 )
        {
            while( m_clientOffset == m_clientPayload.size() && !m_clientClosed && m_valid &&
                !shutdown.load( std::memory_order_relaxed ) )
            {
                if( timeoutMs > 0 ) m_cv.wait_for( lock, std::chrono::milliseconds( timeoutMs ) );
                else m_cv.wait( lock );
            }
            if( !m_valid || shutdown.load( std::memory_order_relaxed ) ) return false;
            if( m_clientOffset == m_clientPayload.size() )
            {
                if( m_clientClosed ) return false;
                continue;
            }
            const auto available = m_clientPayload.size() - m_clientOffset;
            const auto amount = std::min( available, remaining );
            memcpy( output, m_clientPayload.data() + m_clientOffset, amount );
            output += amount;
            remaining -= amount;
            m_clientOffset += amount;
            if( m_clientOffset == m_clientPayload.size() )
            {
                m_clientPayload.clear();
                m_clientOffset = 0;
                m_cv.notify_all();
            }
        }
        return true;
    }

    int Send( const void* data, int size ) override
    {
        if( size < 0 ) return -1;
        std::lock_guard<std::mutex> lock( m_lock );
        if( !m_valid ) return -1;
        const auto bytes = static_cast<const uint8_t*>( data );
        m_serverPending.insert( m_serverPending.end(), bytes, bytes + size );
        if( !ConsumeServerLocked() ) return -1;
        return size;
    }

    int GetSendBufferSize() const override
    {
        return int( ( 8 * 1024 + 4 ) * tracy::ServerQueryPacketSize );
    }

    void Close() override
    {
        std::lock_guard<std::mutex> lock( m_lock );
        m_valid = false;
        m_clientClosed = true;
        m_cv.notify_all();
    }

    bool IsValid() const override
    {
        std::lock_guard<std::mutex> lock( m_lock );
        return m_valid;
    }

    bool PublishClient( std::vector<uint8_t>& payload )
    {
        std::unique_lock<std::mutex> lock( m_lock );
        m_cv.wait( lock, [this] { return m_clientPayload.empty() || !m_valid; } );
        if( !m_valid ) return false;
        m_clientPayload = std::move( payload );
        m_clientOffset = 0;
        m_cv.notify_all();
        return true;
    }

    void CloseClient()
    {
        std::lock_guard<std::mutex> lock( m_lock );
        m_clientClosed = true;
        m_cv.notify_all();
    }

    bool Finish( std::string& error )
    {
        std::lock_guard<std::mutex> lock( m_lock );
        if( !m_error.empty() )
        {
            error = m_error;
            return false;
        }
        if( !ConsumeServerLocked() )
        {
            error = m_error;
            return false;
        }
        if( m_serverIndex != m_serverRecords.size() )
        {
            error = "server transcript ended early (consumed=" + std::to_string( m_serverIndex ) +
                ", recorded=" + std::to_string( m_serverRecords.size() ) + ")";
            return false;
        }
        if( !m_serverPending.empty() && m_complete )
        {
            error = "Worker emitted " + std::to_string( m_serverPending.size() ) +
                " bytes beyond the complete recorded server transcript";
            return false;
        }
        if( !m_verifier.Finish( error ) ) return false;
        return true;
    }

private:
    bool ConsumeServerLocked()
    {
        std::string error;
        while( m_serverIndex < m_serverRecords.size() )
        {
            const auto& record = m_serverRecords[m_serverIndex];
            if( m_serverPending.size() < record.payloadSize ) break;
            tracy::stream::ReplayServerPacket recorded { record.sequence, record.flags, {} };
            if( !m_reader.Read( record, recorded.payload, error ) )
            {
                FailLocked( "sequence " + std::to_string( record.sequence ) + ": " + error );
                return false;
            }
            tracy::stream::ReplayServerPacket replayed { record.sequence, record.flags, {} };
            replayed.payload.assign( m_serverPending.begin(), m_serverPending.begin() + size_t( record.payloadSize ) );
            if( !m_verifier.Append( recorded, replayed, error ) )
            {
                FailLocked( error );
                return false;
            }
            m_serverPending.erase( m_serverPending.begin(), m_serverPending.begin() + size_t( record.payloadSize ) );
            m_serverIndex++;
            m_replayedServerSequence.store( record.sequence, std::memory_order_release );
            m_cv.notify_all();
        }
        if( m_serverIndex == m_serverRecords.size() && !m_serverPending.empty() && m_complete )
        {
            FailLocked( "Worker emitted bytes beyond the complete recorded server transcript" );
            return false;
        }
        return true;
    }

    void FailLocked( std::string message )
    {
        if( m_error.empty() ) m_error = message;
        m_replayError.Set( message );
        m_valid = false;
        m_clientClosed = true;
        m_cv.notify_all();
    }

    PayloadReader m_reader;
    const std::vector<tracy::stream::RecordInfo>& m_serverRecords;
    const bool m_complete;
    ReplayError& m_replayError;
    std::atomic<uint64_t>& m_replayedServerSequence;
    mutable std::mutex m_lock;
    std::condition_variable m_cv;
    std::vector<uint8_t> m_clientPayload;
    size_t m_clientOffset = 0;
    bool m_clientClosed = false;
    bool m_valid = true;
    std::vector<uint8_t> m_serverPending;
    size_t m_serverIndex = 0;
    tracy::stream::ReplayServerTranscriptVerifier m_verifier;
    std::string m_error;
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

double ElapsedSeconds( std::chrono::steady_clock::time_point begin )
{
    return std::chrono::duration<double>( std::chrono::steady_clock::now() - begin ).count();
}

bool PublishSnapshotAtomically(
    const std::filesystem::path& temporary,
    const std::filesystem::path& output,
    bool overwrite,
    std::string& error )
{
#ifdef _WIN32
    std::error_code filesystemError;
    const bool exists = std::filesystem::exists( output, filesystemError ) && !filesystemError;
    if( exists )
    {
        if( !overwrite )
        {
            error = "output already exists";
            return false;
        }
        if( ReplaceFileW( output.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr ) ) return true;
        error = "ReplaceFileW failed with error " + std::to_string( GetLastError() );
        return false;
    }
    if( MoveFileExW( temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "MoveFileExW failed with error " + std::to_string( GetLastError() );
    return false;
#else
    std::error_code filesystemError;
    if( std::filesystem::exists( output, filesystemError ) && !filesystemError )
    {
        if( !overwrite )
        {
            error = "output already exists";
            return false;
        }
        std::filesystem::remove( output, filesystemError );
        if( filesystemError )
        {
            error = "cannot remove previous output: " + filesystemError.message();
            return false;
        }
    }
    std::filesystem::rename( temporary, output, filesystemError );
    if( !filesystemError ) return true;
    error = "cannot publish output: " + filesystemError.message();
    return false;
#endif
}

const char* QueueTypeName( tracy::QueueType type )
{
    switch( type )
    {
    case tracy::QueueType::ZoneValidation: return "ZoneValidation";
    case tracy::QueueType::ContextSwitch: return "ContextSwitch";
    case tracy::QueueType::ThreadWakeup: return "ThreadWakeup";
    case tracy::QueueType::ZoneBegin: return "ZoneBegin";
    case tracy::QueueType::ZoneBeginCallstack: return "ZoneBeginCallstack";
    case tracy::QueueType::ZoneEnd: return "ZoneEnd";
    case tracy::QueueType::ZoneText: return "ZoneText";
    case tracy::QueueType::ZoneName: return "ZoneName";
    case tracy::QueueType::Callstack: return "Callstack";
    case tracy::QueueType::CallstackSerial: return "CallstackSerial";
    case tracy::QueueType::CallstackSample: return "CallstackSample";
    case tracy::QueueType::CallstackSampleRef: return "CallstackSampleRef";
    case tracy::QueueType::CallstackPayload: return "CallstackPayload";
    case tracy::QueueType::GpuTime: return "GpuTime";
    case tracy::QueueType::GpuZoneBeginSerial: return "GpuZoneBeginSerial";
    case tracy::QueueType::GpuZoneBeginCallstackSerial: return "GpuZoneBeginCallstackSerial";
    case tracy::QueueType::GpuZoneEndSerial: return "GpuZoneEndSerial";
    case tracy::QueueType::JnJobSchedule: return "JnJobSchedule";
    case tracy::QueueType::JnJobConfig: return "JnJobConfig";
    case tracy::QueueType::JnJobDependency: return "JnJobDependency";
    case tracy::QueueType::JnJobStage: return "JnJobStage";
    case tracy::QueueType::JnGfxEntity: return "JnGfxEntity";
    case tracy::QueueType::JnGfxLink: return "JnGfxLink";
    case tracy::QueueType::JnRelation: return "JnRelation";
    case tracy::QueueType::JnGpuReferencePass: return "JnGpuReferencePass";
    case tracy::QueueType::JnGpuReferenceSetUse: return "JnGpuReferenceSetUse";
    case tracy::QueueType::JnGpuReferenceEnd: return "JnGpuReferenceEnd";
    case tracy::QueueType::JnResourceMetadata: return "JnResourceMetadata";
    case tracy::QueueType::SingleStringData: return "SingleStringData";
    case tracy::QueueType::SecondStringData: return "SecondStringData";
    case tracy::QueueType::MemNamePayload: return "MemNamePayload";
    case tracy::QueueType::JnGpuReferenceSetDefinition: return "JnGpuReferenceSetDefinition";
    default: return "Other";
    }
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
    const auto totalStart = std::chrono::steady_clock::now();
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
    const auto scanStart = std::chrono::steady_clock::now();
    const auto scan = tracy::stream::ScanJournal( options.input, scanOptions );
    const auto scanSeconds = ElapsedSeconds( scanStart );
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

    const auto analysisStart = std::chrono::steady_clock::now();
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
    std::vector<bool> orderIndependentServerRecords;
    orderIndependentServerRecords.reserve( serverRecords.size() );
    {
        PayloadReader serverReader( options.input );
        std::vector<uint8_t> payload;
        std::string error;
        if( !serverReader.IsOpen() )
        {
            std::fprintf( stderr, "Cannot open journal for server dependency analysis.\n" );
            return 2;
        }
        for( const auto& record : serverRecords )
        {
            if( !serverReader.Read( record, payload, error ) )
            {
                std::fprintf( stderr, "Cannot read server dependency record: %s.\n", error.c_str() );
                return 2;
            }
            const tracy::stream::ReplayServerPacket packet { record.sequence, record.flags, payload };
            orderIndependentServerRecords.push_back( tracy::stream::IsOrderIndependentServerQuery( packet ) );
        }
    }
    bool recordedEndsWithTerminate = false;
    {
        PayloadReader tailReader( options.input );
        std::vector<uint8_t> payload;
        std::string error;
        if( !tailReader.IsOpen() || !tailReader.Read( serverRecords.back(), payload, error ) )
        {
            std::fprintf( stderr, "Cannot read final recorded server packet: %s.\n", error.c_str() );
            return 2;
        }
        recordedEndsWithTerminate = payload.size() == tracy::ServerQueryPacketSize && payload[0] == tracy::ServerQueryTerminate;
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

    const bool replayProtocolOnly = deferSymbolExpansion || drainControlVersion >= 3;
    ReplayError replayError;
    std::atomic<uint64_t> replayedServerSequence { 0 };
    std::unique_ptr<OfflineReplayTransport> offlineTransport;
    std::unique_ptr<tracy::Socket, SocketDeleter> peer;
    std::unique_ptr<tracy::Worker> workerStorage;
    tracy::ListenSocket listener;

    if( options.mode == Options::Mode::Offline )
    {
        std::printf( "Offline replay of %zu client records with in-process validation of %zu server records...\n",
            clientRecords.size(), serverRecords.size() );
        offlineTransport = std::make_unique<OfflineReplayTransport>(
            options.input, serverRecords, scan.complete, replayError, replayedServerSequence );
        workerStorage = std::make_unique<tracy::Worker>( "offline", 0, -1, nullptr, tracy::Worker::Mode::OfflineConvert,
            tracy::Worker::DefaultRecorderDefinitionLimit, tracy::Worker::DefaultRecorderQueryQueueLimit,
            replayProtocolOnly, serverQuerySpaceOverride, replayProtocolOnly, true, true, offlineTransport.get() );
    }
    else
    {
        EnableLocalReplayOnly();
        if( !listener.Listen( options.port, 1 ) )
        {
            std::fprintf( stderr, "Cannot bind local replay port %u; choose another port with -p.\n", options.port );
            return 4;
        }
        std::printf( "Legacy replay of %zu client records and validation of %zu server records on 127.0.0.1:%u...\n",
            clientRecords.size(), serverRecords.size(), options.port );
        workerStorage = std::make_unique<tracy::Worker>( "127.0.0.1", options.port, -1, nullptr, tracy::Worker::Mode::Full,
            tracy::Worker::DefaultRecorderDefinitionLimit, tracy::Worker::DefaultRecorderQueryQueueLimit,
            replayProtocolOnly, serverQuerySpaceOverride, replayProtocolOnly, true, true );

        const auto acceptDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
        while( !peer && std::chrono::steady_clock::now() < acceptDeadline )
        {
            peer.reset( listener.Accept() );
        }
        if( !peer )
        {
            workerStorage->Shutdown();
            std::fprintf( stderr, "Worker did not connect to the local replay socket.\n" );
            return 4;
        }
        // Full Worker replay applies natural socket backpressure while it expands
        // dense compressed frames. A fixed send timeout can fire after partially
        // sending a frame, which cannot be retried without corrupting the stream.
        if( !peer->SetSendTimeout( 0 ) )
        {
            workerStorage->Shutdown();
            std::fprintf( stderr, "Cannot configure the local replay send timeout.\n" );
            return 4;
        }
    }

    const auto analysisSeconds = ElapsedSeconds( analysisStart );
    const auto replayStart = std::chrono::steady_clock::now();

    auto& worker = *workerStorage;
    if( hasLocalDisconnect && drainControlSequence == 0 ) worker.MarkProtocolDisconnect();

    std::thread verifier;
    if( options.mode == Options::Mode::Legacy ) verifier = std::thread( [&] {
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
            // Full replay may need to expand a large first batch of sampling
            // callstacks before it can reproduce the recorder's first query.
            // Treat that CPU work separately from a closed server stream.
            auto lastEventProgress = worker.GetProtocolEventCount();
            auto lastFrameProgress = worker.GetProtocolFramesProcessed();
            auto recordDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
            if( !replayed.payload.empty() && !peer->Read( replayed.payload.data(), int( replayed.payload.size() ), 100, [&] {
                const auto eventProgress = worker.GetProtocolEventCount();
                const auto frameProgress = worker.GetProtocolFramesProcessed();
                if( eventProgress != lastEventProgress || frameProgress != lastFrameProgress )
                {
                    lastEventProgress = eventProgress;
                    lastFrameProgress = frameProgress;
                    recordDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
                }
                return replayError.Failed() || std::chrono::steady_clock::now() >= recordDeadline;
            } ) )
            {
                const auto timedOut = std::chrono::steady_clock::now() >= recordDeadline;
                failReplay( "sequence " + std::to_string( record.sequence ) +
                    ( timedOut ? ": Worker made no protocol progress for 120 seconds while waiting for server stream; events=" +
                        std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                        std::to_string( worker.GetProtocolFramesProcessed() ) : ": Worker server stream ended early" ) );
                return;
            }
            if( !transcriptVerifier.Append( recorded, replayed, error ) )
            {
                failReplay( error );
                return;
            }
            // Preserve the journal's cross-direction causal ordering. Client
            // responses must not be replayed before the Worker has emitted the
            // earlier query they answer. Definition queries may still reorder
            // within a consecutive server batch; the transcript verifier
            // validates those batches by content and multiplicity.
            replayedServerSequence.store( record.sequence, std::memory_order_release );
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
        size_t serverDependencyCursor = 0;
        auto replayClientRecord = [&]( const tracy::stream::RecordInfo& record ) {
            if( replayError.Failed() ) return false;
            uint64_t requiredServerSequence = 0;
            while( serverDependencyCursor < serverRecords.size() &&
                serverRecords[serverDependencyCursor].sequence < record.sequence )
            {
                if( !orderIndependentServerRecords[serverDependencyCursor] )
                    requiredServerSequence = serverRecords[serverDependencyCursor].sequence;
                serverDependencyCursor++;
            }
            if( requiredServerSequence != 0 )
            {
                auto lastEventProgress = worker.GetProtocolEventCount();
                auto lastFrameProgress = worker.GetProtocolFramesProcessed();
                auto dependencyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
                while( replayedServerSequence.load( std::memory_order_acquire ) < requiredServerSequence &&
                    !replayError.Failed() && std::chrono::steady_clock::now() < dependencyDeadline )
                {
                    const auto eventProgress = worker.GetProtocolEventCount();
                    const auto frameProgress = worker.GetProtocolFramesProcessed();
                    if( eventProgress != lastEventProgress || frameProgress != lastFrameProgress )
                    {
                        lastEventProgress = eventProgress;
                        lastFrameProgress = frameProgress;
                        dependencyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
                    }
                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                }
                if( replayedServerSequence.load( std::memory_order_acquire ) < requiredServerSequence )
                {
                    replayError.Set( "sequence " + std::to_string( record.sequence ) +
                        ": Worker made no protocol progress for 120 seconds while waiting for preceding server sequence " +
                        std::to_string( requiredServerSequence ) + "; events=" +
                        std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                        std::to_string( worker.GetProtocolFramesProcessed() ) );
                    return false;
                }
            }
            if( !clientReader.Read( record, payload, error ) )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) + ": " + error );
                return false;
            }
            const bool sent = options.mode == Options::Mode::Offline ?
                offlineTransport->PublishClient( payload ) :
                ( payload.empty() || peer->Send( payload.data(), int( payload.size() ) ) == int( payload.size() ) );
            if( !sent )
            {
                replayError.Set( "sequence " + std::to_string( record.sequence ) +
                    ": recorded client stream publish ended while Worker replay was active; events=" +
                    std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                    std::to_string( worker.GetProtocolFramesProcessed() ) );
                return false;
            }
            return true;
        };

        if( drainControlSequence == 0 )
        {
            uint64_t replayedFrames = 0;
            for( const auto& record : clientRecords )
            {
                if( !replayClientRecord( record ) ) break;
                if( ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0 ) replayedFrames++;
            }
            if( !replayError.Failed() && scan.complete && recordedEndsWithTerminate )
            {
                if( !waitForWorkerProgress( [&] { return worker.GetProtocolFramesProcessed() >= replayedFrames; } ) )
                {
                    replayError.Set( "Worker made no protocol progress for 120 seconds while processing the complete Full-capture client revision; events=" +
                        std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                        std::to_string( worker.GetProtocolFramesProcessed() ) + "/" + std::to_string( replayedFrames ) );
                }
                else if( worker.IsConnected() )
                {
                    // Full capture stops locally, but old/double-write journals do not contain the
                    // ProtocolOnly BeginDrain marker. Ask the Worker's protocol thread to reproduce the
                    // recorded final Terminate after every client frame and preceding server query has
                    // been processed. The transcript verifier still rejects missing or extra packets.
                    worker.RequestProtocolReplayTerminate();
                }
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

            if( !waitForWorkerProgress( [&] { return worker.GetProtocolFramesProcessed() >= prefixFrameTarget; } ) )
            {
                replayError.Set( "Worker made no protocol progress for 120 seconds while processing the pre-drain client revision; events=" +
                    std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                    std::to_string( worker.GetProtocolFramesProcessed() ) + "/" + std::to_string( prefixFrameTarget ) );
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
                if( !waitForWorkerProgress( [&] { return worker.IsProtocolDrainActive(); } ) )
                {
                    replayError.Set( "Worker made no protocol progress for 120 seconds while entering protocol drain mode; events=" +
                        std::to_string( worker.GetProtocolEventCount() ) + ", frames=" +
                        std::to_string( worker.GetProtocolFramesProcessed() ) );
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

    if( options.mode == Options::Mode::Offline )
    {
        const auto finalServerSequence = serverRecords.back().sequence;
        auto lastEventProgress = worker.GetProtocolEventCount();
        auto lastFrameProgress = worker.GetProtocolFramesProcessed();
        auto transcriptDeadline = std::chrono::steady_clock::now() + ReplayProgressTimeout;
        while( replayedServerSequence.load( std::memory_order_acquire ) < finalServerSequence &&
            !replayError.Failed() && std::chrono::steady_clock::now() < transcriptDeadline )
        {
            const auto eventProgress = worker.GetProtocolEventCount();
            const auto frameProgress = worker.GetProtocolFramesProcessed();
            if( eventProgress != lastEventProgress || frameProgress != lastFrameProgress )
            {
                lastEventProgress = eventProgress;
                lastFrameProgress = frameProgress;
                transcriptDeadline = std::chrono::steady_clock::now() + ReplayProgressTimeout;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        if( replayedServerSequence.load( std::memory_order_acquire ) < finalServerSequence && !replayError.Failed() )
        {
            replayError.Set( "Worker made no protocol progress for 120 seconds before consuming the complete in-process server transcript" );
        }
        std::string offlineError;
        if( !replayError.Failed() && !offlineTransport->Finish( offlineError ) ) replayError.Set( offlineError );
        offlineTransport->CloseClient();
    }
    else
    {
        if( replayError.Failed() && peer->IsValid() ) peer->Close();
        verifier.join();
    }

    if( !replayError.Failed() && options.mode == Options::Mode::Legacy )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        uint8_t extraByte = 0;
        if( peer->HasData() && peer->ReadRaw( &extraByte, 1, 1 ) )
        {
            if( scan.complete )
            {
                replayError.Set( "Worker emitted server bytes beyond the recorded complete transcript; first byte=" +
                    std::to_string( unsigned( extraByte ) ) );
            }
            else
            {
                std::fprintf( stderr, "Warning: incomplete journal ended before a trailing Worker query; the valid client prefix remains replayable.\n" );
            }
        }
    }
    if( options.mode == Options::Mode::Legacy && peer->IsValid() ) peer->Close();

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
        const auto failure = worker.GetFailureType();
        if( failure != tracy::Worker::Failure::None )
        {
            std::fprintf( stderr, "Replay Worker instrumentation failure: %s\n", tracy::Worker::GetFailureString( failure ) );
            const auto& failureData = worker.GetFailureData();
            if( !failureData.message.empty() )
                std::fprintf( stderr, "Replay Worker failure context: %s\n", failureData.message.c_str() );
        }
        return 5;
    }

    if( options.mode == Options::Mode::Offline )
    {
        std::vector<std::pair<double, size_t>> estimatedCosts;
        const auto& eventStats = worker.GetOfflineEventStats();
        estimatedCosts.reserve( eventStats.size() );
        for( size_t index = 0; index < eventStats.size(); index++ )
        {
            const auto& stat = eventStats[index];
            if( stat.sampledCount == 0 ) continue;
            const auto estimatedNanoseconds = double( stat.sampledNanoseconds ) *
                double( stat.count ) / double( stat.sampledCount );
            estimatedCosts.emplace_back( estimatedNanoseconds, index );
        }
        std::sort( estimatedCosts.begin(), estimatedCosts.end(), []( const auto& left, const auto& right ) {
            return left.first > right.first;
        } );
        std::printf( "Offline sampled dispatch costs (top 20):\n" );
        const auto limit = std::min<size_t>( 20, estimatedCosts.size() );
        for( size_t rank = 0; rank < limit; rank++ )
        {
            const auto index = estimatedCosts[rank].second;
            const auto& stat = eventStats[index];
            const auto averageNanoseconds = double( stat.sampledNanoseconds ) / double( stat.sampledCount );
            std::printf( "  type=%zu name=%s count=%llu sampled=%llu avg=%.1fns estimated=%.3fs\n",
                index, QueueTypeName( tracy::QueueType( index ) ), static_cast<unsigned long long>( stat.count ),
                static_cast<unsigned long long>( stat.sampledCount ), averageNanoseconds,
                estimatedCosts[rank].first / 1'000'000'000.0 );
        }
    }

    const auto replaySeconds = ElapsedSeconds( replayStart );
    const auto writeStart = std::chrono::steady_clock::now();
    auto temporaryOutput = options.output;
    temporaryOutput += ".converting";
    {
        std::error_code ignored;
        std::filesystem::remove( temporaryOutput, ignored );
    }
    auto output = std::unique_ptr<tracy::FileWrite>( tracy::FileWrite::Open( temporaryOutput.string().c_str(), tracy::FileCompression::Zstd, 1, 4 ) );
    if( !output )
    {
        std::fprintf( stderr, "Cannot create output snapshot.\n" );
        return 6;
    }
    worker.Write( *output, false );
    output->Finish();
    const auto statistics = output->GetCompressionStatistics();
    output.reset();
    const auto writeSeconds = ElapsedSeconds( writeStart );

    const auto validationStart = std::chrono::steady_clock::now();
    workerStorage.reset();
    try
    {
        auto validationFile = std::unique_ptr<tracy::FileRead>( tracy::FileRead::Open( temporaryOutput.string().c_str() ) );
        if( !validationFile )
        {
            std::fprintf( stderr, "Cannot reopen temporary snapshot for validation.\n" );
            return 6;
        }
        tracy::Worker validationWorker( *validationFile, tracy::EventType::All, true );
        if( !validationWorker.HasData() )
        {
            std::fprintf( stderr, "Temporary snapshot validation produced no data.\n" );
            return 6;
        }
    }
    catch( const std::exception& exception )
    {
        std::fprintf( stderr, "Temporary snapshot validation failed: %s\n", exception.what() );
        return 6;
    }
    catch( ... )
    {
        std::fprintf( stderr, "Temporary snapshot validation failed with an unknown error.\n" );
        return 6;
    }
    const auto validationSeconds = ElapsedSeconds( validationStart );

    std::string publishError;
    if( !PublishSnapshotAtomically( temporaryOutput, options.output, options.overwrite, publishError ) )
    {
        std::fprintf( stderr, "Snapshot validation passed but atomic publication failed: %s\n", publishError.c_str() );
        return 6;
    }

    const auto mapStart = std::chrono::steady_clock::now();
    std::string snapshotMapError;
    if( !tracy::stream::WriteConvertedSnapshotMap( options.input, scan, options.output, snapshotMapError ) )
    {
        std::fprintf( stderr, "Snapshot was converted but its stream mapping could not be published: %s\n", snapshotMapError.c_str() );
        return 7;
    }
    const auto mapSeconds = ElapsedSeconds( mapStart );
    std::printf( "Converted %llu valid journal bytes into %llu snapshot bytes (%.2f%%).\n",
        static_cast<unsigned long long>( scan.validSize ), static_cast<unsigned long long>( statistics.second ),
        statistics.first == 0 ? 0. : 100. * statistics.second / statistics.first );
    std::printf( "Stages: scan=%.3fs analysis=%.3fs replay=%.3fs write=%.3fs validate=%.3fs map=%.3fs total=%.3fs.\n",
        scanSeconds, analysisSeconds, replaySeconds, writeSeconds, validationSeconds, mapSeconds,
        ElapsedSeconds( totalStart ) );
    return 0;
}
