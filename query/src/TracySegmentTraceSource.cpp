#include "TracySegmentTraceSource.hpp"
#include "TracyQueryIndex.hpp"

#include "../../public/common/TracyAlloc.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracySocket.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../stream/src/TracyStreamJournal.hpp"
#include "../../stream/src/TracyStreamReplay.hpp"
#include "../../stream/src/TracyStreamSnapshotMap.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace tracy::query
{
namespace
{

class PayloadReader
{
public:
    explicit PayloadReader( const std::filesystem::path& path )
        : m_file( path, std::ios::binary )
    {}

    bool Read( const stream::RecordInfo& record, std::vector<uint8_t>& payload, std::string& error )
    {
        if( record.payloadSize > uint64_t( std::numeric_limits<size_t>::max() ) )
        {
            error = "journal payload is too large";
            return false;
        }
        payload.resize( size_t( record.payloadSize ) );
        m_file.clear();
        m_file.seekg( std::streamoff( record.offset + stream::RecordHeaderSize ), std::ios::beg );
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
    void operator()( Socket* socket ) const
    {
        if( !socket ) return;
        socket->~Socket();
        tracy_free( socket );
    }
};

class ReplayError
{
public:
    void Set( std::string message )
    {
        std::lock_guard lock( m_lock );
        if( m_message.empty() ) m_message = std::move( message );
        m_failed.store( true, std::memory_order_release );
    }

    bool Failed() const { return m_failed.load( std::memory_order_acquire ); }
    std::string Message() const
    {
        std::lock_guard lock( m_lock );
        return m_message;
    }

private:
    mutable std::mutex m_lock;
    std::atomic<bool> m_failed { false };
    std::string m_message;
};

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

std::filesystem::path MakeSnapshotPath( uint64_t revision )
{
    static std::atomic<uint64_t> counter { 0 };
    std::error_code error;
    auto directory = std::filesystem::temp_directory_path( error ) / "tracy-query-segments";
    if( error || !std::filesystem::create_directories( directory, error ) && error )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::OpenFailed, "cannot create the live-query cache directory" );
    }
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return directory / ( "revision-" + std::to_string( revision ) + "-" + std::to_string( stamp ) + "-" +
        std::to_string( counter.fetch_add( 1, std::memory_order_relaxed ) ) + ".tracy" );
}

std::string StreamFingerprint( const stream::JournalReadView& view );

std::filesystem::path MakePersistentSnapshotPath( const stream::JournalReadView& view )
{
    std::ostringstream suffix;
    suffix << ".revision-" << view.revision << '-' << std::hex << std::setw( 8 ) << std::setfill( '0' ) << view.prefixCrc32c;
    const auto fingerprint = StreamFingerprint( view );
    suffix << '-' << fingerprint.substr( 0, std::min<size_t>( 16, fingerprint.size() ) ) << ".tracy";
    auto path = view.path;
    path += suffix.str();
    return path;
}

std::string StreamFingerprint( const stream::JournalReadView& view )
{
    std::ostringstream out;
    out << std::hex << std::setfill( '0' );
    for( const auto byte : view.header.sessionId ) out << std::setw( 2 ) << unsigned( byte );
    out << std::setw( 16 ) << view.header.createdUnixNs;
    out << std::setw( 8 ) << view.header.protocolVersion;
    out << std::setw( 8 ) << view.header.flags;
    return out.str();
}

int64_t StreamWriteTime( const std::filesystem::path& path )
{
    return std::filesystem::last_write_time( path ).time_since_epoch().count();
}

std::filesystem::path StreamIndexCachePath( const std::filesystem::path& path )
{
    auto result = path;
    result += ".jnidx-stream";
    return result;
}

struct CachedStreamRevision
{
    std::shared_ptr<stream::JournalReadView> view;
    std::filesystem::path snapshotPath;
    std::string fingerprint;
};

std::optional<CachedStreamRevision> ReadStreamIndexCache( const std::filesystem::path& streamPath )
{
    try
    {
        const auto cachePath = StreamIndexCachePath( streamPath );
        if( !std::filesystem::is_regular_file( cachePath ) || std::filesystem::file_size( cachePath ) > 64 * 1024 ) return std::nullopt;
        std::ifstream input( cachePath, std::ios::binary );
        const auto value = nlohmann::json::parse( input );
        if( value.value( "schema", 0u ) != 1 || !value.value( "complete", false ) ) return std::nullopt;
        if( std::filesystem::file_size( streamPath ) != std::stoull( value.at( "stream_bytes" ).get<std::string>() ) ||
            StreamWriteTime( streamPath ) != std::stoll( value.at( "stream_write_time" ).get<std::string>() ) ) return std::nullopt;
        const auto file = std::filesystem::path( value.at( "snapshot" ).get<std::string>() );
        if( file.empty() || file != file.filename() ) return std::nullopt;
        auto snapshotPath = streamPath.parent_path() / file;
        const auto validation = QueryIndex::Validate( snapshotPath );
        if( !validation.manifest ) return std::nullopt;
        auto view = std::make_shared<stream::JournalReadView>();
        view->path = streamPath; view->revision = std::stoull( value.at( "revision" ).get<std::string>() );
        view->validSize = std::stoull( value.at( "valid_size" ).get<std::string>() );
        view->recordCount = std::stoull( value.at( "record_count" ).get<std::string>() );
        view->watermarkNs = std::stoull( value.at( "watermark_ns" ).get<std::string>() );
        view->prefixCrc32c = value.at( "prefix_crc32c" ).get<uint32_t>();
        view->observedFileSize = std::filesystem::file_size( streamPath ); view->observedCode = stream::ScanCode::Ok; view->complete = true;
        return CachedStreamRevision { std::move( view ), std::move( snapshotPath ), value.at( "fingerprint" ).get<std::string>() };
    }
    catch( ... )
    {
        return std::nullopt;
    }
}

void WriteStreamIndexCache( const stream::JournalReadView& view, const std::filesystem::path& snapshotPath )
{
    if( !view.complete ) return;
    const auto cachePath = StreamIndexCachePath( view.path );
    auto temporary = cachePath; temporary += ".tmp";
    const nlohmann::json value = {
        { "schema", 1 }, { "complete", true }, { "stream_bytes", std::to_string( std::filesystem::file_size( view.path ) ) },
        { "stream_write_time", std::to_string( StreamWriteTime( view.path ) ) }, { "revision", std::to_string( view.revision ) },
        { "valid_size", std::to_string( view.validSize ) }, { "record_count", std::to_string( view.recordCount ) },
        { "watermark_ns", std::to_string( view.watermarkNs ) }, { "prefix_crc32c", view.prefixCrc32c },
        { "fingerprint", StreamFingerprint( view ) }, { "snapshot", snapshotPath.filename().string() }
    };
    {
        std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
        if( !output ) return;
        output << value.dump();
        if( !output ) return;
    }
    std::error_code error;
    std::filesystem::remove( cachePath, error ); error.clear();
    std::filesystem::rename( temporary, cachePath, error );
    if( error ) { std::error_code ignored; std::filesystem::remove( temporary, ignored ); }
}

std::filesystem::path ReplayRevision( const stream::JournalReadView& view )
{
    stream::ScanOptions scanOptions;
    scanOptions.maxCollectedRecords = 2'000'000;
    const auto scan = stream::ScanJournalPrefix( view.path, view.validSize, scanOptions );
    if( !scan.HasRecoverablePrefix() || scan.records.size() != scan.recordCount )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "committed stream revision cannot be enumerated" );
    }
    if( scan.lastSequence != view.revision || scan.validSize != view.validSize || scan.prefixCrc32c != view.prefixCrc32c )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "committed stream revision changed while it was being opened" );
    }
    if( scan.header.protocolVersion != ProtocolVersion )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::UnsupportedVersion, "stream protocol version does not match this query build" );
    }

    std::vector<stream::RecordInfo> clientRecords;
    std::vector<stream::RecordInfo> serverRecords;
    bool hasLocalDisconnect = false;
    bool hasSessionBegin = false;
    bool deferSymbolExpansion = false;
    stream::RecordInfo sessionBeginRecord;
    uint64_t drainControlSequence = 0;
    stream::RecordInfo drainControlRecord;
    uint8_t drainControlVersion = 0;
    uint32_t serverQuerySpaceOverride = 0;
    for( const auto& record : scan.records )
    {
        if( record.type == stream::RecordType::SessionBegin && !hasSessionBegin )
        {
            hasSessionBegin = true;
            sessionBeginRecord = record;
        }
        else if( record.type == stream::RecordType::ClientToServer ) clientRecords.emplace_back( record );
        else if( record.type == stream::RecordType::ServerToClient ) serverRecords.emplace_back( record );
        else if( record.type == stream::RecordType::Diagnostic &&
            ( record.flags & stream::RecordFlagLocalControl ) != 0 )
        {
            hasLocalDisconnect = true;
            if( ( record.flags & stream::RecordFlagServerQuery ) == 0 )
            {
                drainControlSequence = record.sequence;
                drainControlRecord = record;
            }
        }
    }
    if( clientRecords.empty() || serverRecords.empty() )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "stream revision does not yet contain a complete Tracy handshake" );
    }
    std::vector<bool> orderIndependentServerRecords;
    orderIndependentServerRecords.reserve( serverRecords.size() );
    {
        PayloadReader serverReader( view.path );
        std::vector<uint8_t> payload;
        std::string error;
        for( const auto& record : serverRecords )
        {
            if( !serverReader.Read( record, payload, error ) )
            {
                throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt,
                    "cannot read server dependency record: " + error );
            }
            const stream::ReplayServerPacket packet { record.sequence, record.flags, payload };
            orderIndependentServerRecords.push_back( stream::IsOrderIndependentServerQuery( packet ) );
        }
    }
    bool recordedEndsWithTerminate = false;
    {
        PayloadReader tailReader( view.path );
        std::vector<uint8_t> payload;
        std::string error;
        if( !tailReader.Read( serverRecords.back(), payload, error ) )
        {
            throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "cannot read final recorded server packet: " + error );
        }
        recordedEndsWithTerminate = payload.size() == ServerQueryPacketSize && payload[0] == ServerQueryTerminate;
    }
    if( hasSessionBegin )
    {
        PayloadReader sessionReader( view.path );
        std::vector<uint8_t> payload;
        std::string error;
        if( !sessionReader.Read( sessionBeginRecord, payload, error ) )
        {
            throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "cannot read session metadata: " + error );
        }
        if( payload.size() >= 2 )
        {
            const auto version = uint16_t( payload[0] ) | ( uint16_t( payload[1] ) << 8 );
            if( version == 2 )
            {
                if( payload.size() < 24 )
                {
                    throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "invalid version 2 session metadata" );
                }
                const auto headerSize = uint16_t( payload[2] ) | ( uint16_t( payload[3] ) << 8 );
                const auto flags =
                    uint32_t( payload[16] ) |
                    ( uint32_t( payload[17] ) << 8 ) |
                    ( uint32_t( payload[18] ) << 16 ) |
                    ( uint32_t( payload[19] ) << 24 );
                if( headerSize != 24 || ( flags & ~stream::SessionBeginSupportedFlags ) != 0 )
                {
                    throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::UnsupportedVersion, "unsupported version 2 session metadata" );
                }
                deferSymbolExpansion = ( flags & stream::SessionBeginFlagDeferredSymbolExpansion ) != 0;
            }
            else if( version != 1 )
            {
                throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::UnsupportedVersion, "unsupported session metadata version" );
            }
        }
    }
    if( drainControlSequence != 0 )
    {
        PayloadReader drainReader( view.path );
        std::vector<uint8_t> payload;
        std::string error;
        if( !drainReader.Read( drainControlRecord, payload, error ) )
        {
            throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "cannot read protocol drain control record: " + error );
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
                throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "invalid recorded server-query window" );
            }
        }
        else
        {
            throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::UnsupportedVersion, "unsupported protocol drain control payload" );
        }
    }

    EnableLocalReplayOnly();
    static std::atomic<uint32_t> nextPort { 19086 };
    std::unique_ptr<ListenSocket> listener;
    uint16_t port = 0;
    for( uint32_t attempt = 0; attempt < 1024; attempt++ )
    {
        const auto candidate = uint16_t( 19000 + ( nextPort.fetch_add( 1, std::memory_order_relaxed ) % 20000 ) );
        auto socket = std::make_unique<ListenSocket>();
        if( socket->Listen( candidate, 1 ) )
        {
            port = candidate;
            listener = std::move( socket );
            break;
        }
    }
    if( !listener )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::ResourceLimit, "no local port is available for stream revision replay" );
    }

    const bool replayProtocolOnly = deferSymbolExpansion || drainControlVersion >= 3;
    Worker worker( "127.0.0.1", port, -1, nullptr, Worker::Mode::Full,
        Worker::DefaultRecorderDefinitionLimit, Worker::DefaultRecorderQueryQueueLimit,
        replayProtocolOnly, serverQuerySpaceOverride, replayProtocolOnly, true, true );
    if( hasLocalDisconnect && drainControlSequence == 0 ) worker.MarkProtocolDisconnect();
    std::unique_ptr<Socket, SocketDeleter> peer;
    const auto acceptDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( !peer && std::chrono::steady_clock::now() < acceptDeadline ) peer.reset( listener->Accept() );
    if( !peer )
    {
        worker.Shutdown();
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Internal, "Tracy Worker did not connect to the revision replay socket" );
    }
    if( !peer->SetSendTimeout( 10000 ) )
    {
        worker.Shutdown();
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Internal, "cannot configure the local replay send timeout" );
    }

    ReplayError replayError;
    std::atomic<uint64_t> replayedServerSequence { 0 };
    std::thread verifier( [&] {
        const auto failReplay = [&]( std::string message ) {
            replayError.Set( std::move( message ) );
            peer->Close();
        };
        PayloadReader reader( view.path );
        stream::ReplayServerTranscriptVerifier transcriptVerifier;
        std::string error;
        for( const auto& record : serverRecords )
        {
            if( replayError.Failed() ) return;
            stream::ReplayServerPacket recorded { record.sequence, record.flags, {} };
            if( !reader.Read( record, recorded.payload, error ) )
            {
                failReplay( "server record " + std::to_string( record.sequence ) + ": " + error );
                return;
            }
            stream::ReplayServerPacket replayed { record.sequence, record.flags, {} };
            replayed.payload.resize( recorded.payload.size() );
            // Full replay may need to expand a large first batch of sampling
            // callstacks before it can reproduce the recorder's first query.
            // Treat that CPU work separately from a closed server stream.
            const auto recordDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
            if( !replayed.payload.empty() && !peer->Read( replayed.payload.data(), int( replayed.payload.size() ), 100, [&] {
                return replayError.Failed() || std::chrono::steady_clock::now() >= recordDeadline;
            } ) )
            {
                const auto timedOut = std::chrono::steady_clock::now() >= recordDeadline;
                failReplay( timedOut ?
                    "timed out waiting for Worker server record " + std::to_string( record.sequence ) :
                    "Worker server stream ended before record " + std::to_string( record.sequence ) );
                return;
            }
            if( !transcriptVerifier.Append( recorded, replayed, error ) )
            {
                failReplay( error );
                return;
            }
            // Preserve the journal's cross-direction causal order. A client
            // response must not be replayed before Full Worker has reproduced
            // the earlier server query it answers.
            replayedServerSequence.store( record.sequence, std::memory_order_release );
        }
        if( !transcriptVerifier.Finish( error ) )
        {
            failReplay( error );
        }
    } );

    PayloadReader clientReader( view.path );
    std::vector<uint8_t> payload;
    std::string readError;
    size_t serverDependencyCursor = 0;
    auto replayClientRecord = [&]( const stream::RecordInfo& record ) {
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
            const auto dependencyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 120 );
            while( replayedServerSequence.load( std::memory_order_acquire ) < requiredServerSequence &&
                !replayError.Failed() && std::chrono::steady_clock::now() < dependencyDeadline )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
            if( replayedServerSequence.load( std::memory_order_acquire ) < requiredServerSequence )
            {
                replayError.Set( "client record " + std::to_string( record.sequence ) +
                    ": timed out waiting for preceding server record " +
                    std::to_string( requiredServerSequence ) );
                return false;
            }
        }
        if( !clientReader.Read( record, payload, readError ) )
        {
            replayError.Set( "client record " + std::to_string( record.sequence ) + ": " + readError );
            return false;
        }
        if( !payload.empty() && peer->Send( payload.data(), int( payload.size() ) ) != int( payload.size() ) )
        {
            replayError.Set( "cannot replay client record " + std::to_string( record.sequence ) );
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
            if( ( record.flags & stream::RecordFlagCompressedFrame ) != 0 ) replayedFrames++;
        }
        if( !replayError.Failed() && scan.complete && recordedEndsWithTerminate )
        {
            const auto replayDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
            while( worker.GetProtocolFramesProcessed() < replayedFrames && !replayError.Failed() &&
                std::chrono::steady_clock::now() < replayDeadline )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
            if( worker.GetProtocolFramesProcessed() < replayedFrames )
            {
                replayError.Set( "Worker did not process the complete Full-capture stream revision" );
            }
            else if( worker.IsConnected() )
            {
                worker.RequestProtocolReplayTerminate();
            }
        }
    }
    else
    {
        const auto preDrainFrameCount = std::count_if( clientRecords.begin(), clientRecords.end(), [&]( const auto& record ) {
            return record.sequence < drainControlSequence &&
                ( record.flags & stream::RecordFlagCompressedFrame ) != 0;
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
            const bool compressed = ( record.flags & stream::RecordFlagCompressedFrame ) != 0;
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

    if( replayError.Failed() && peer->IsValid() ) peer->Close();
    verifier.join();
    if( !replayError.Failed() )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        uint8_t extraByte = 0;
        if( peer->HasData() && peer->ReadRaw( &extraByte, 1, 1 ) )
        {
            replayError.Set( "Worker emitted server bytes beyond the committed revision; first byte=" +
                std::to_string( unsigned( extraByte ) ) );
        }
    }
    if( peer->IsValid() ) peer->Close();

    const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 10 );
    while( !worker.HasData() && worker.GetHandshakeStatus() == HandshakePending && std::chrono::steady_clock::now() < workerDeadline )
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
        replayError.Set( "Worker did not finish the committed revision" );
    }
    if( !worker.HasData() ) replayError.Set( "Worker rejected the committed revision metadata" );
    if( replayError.Failed() )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, replayError.Message() );
    }

    const auto snapshotPath = MakeSnapshotPath( view.revision );
    auto output = std::unique_ptr<FileWrite>( FileWrite::Open( snapshotPath.string().c_str(), FileCompression::Zstd, 3, 4 ) );
    if( !output )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::OpenFailed, "cannot create the live revision snapshot" );
    }
    worker.Write( *output, false );
    output->Finish();
    return snapshotPath;
}

}

std::unique_ptr<SegmentTraceSource> SegmentTraceSource::Open( const std::filesystem::path& path, StateCallback stateCallback, bool preferIndex )
{
    if( preferIndex )
    {
        const auto cached = ReadStreamIndexCache( path );
        if( cached )
        {
            if( stateCallback ) stateCallback( analysis::TraceSourceState::Loading );
            const auto validation = QueryIndex::Validate( cached->snapshotPath );
            if( validation.manifest )
            {
                auto source = QueryIndex::Open( *validation.manifest, std::move( stateCallback ), cached->fingerprint );
                return std::unique_ptr<SegmentTraceSource>( new SegmentTraceSource( {}, cached->view, cached->snapshotPath,
                    std::move( source ), true, true ) );
            }
        }
    }
    std::string error;
    auto uniqueStore = stream::JournalStore::Open( path, error );
    if( !uniqueStore )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, error.empty() ? "cannot open stream journal" : error );
    }
    auto store = std::shared_ptr<stream::JournalStore>( std::move( uniqueStore ) );
    return OpenRevision( store, store->AcquireReadView(), std::move( stateCallback ), preferIndex );
}

std::unique_ptr<SegmentTraceSource> SegmentTraceSource::OpenRevision(
    std::shared_ptr<stream::JournalStore> store,
    std::shared_ptr<const stream::JournalReadView> view,
    StateCallback stateCallback,
    bool preferIndex )
{
    if( !store || !view )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Internal, "stream revision view is unavailable" );
    }
    if( stateCallback ) stateCallback( analysis::TraceSourceState::Loading );
    std::filesystem::path snapshotPath;
    bool persistentSnapshot = false;
    try
    {
        if( preferIndex )
        {
            stream::ConvertedSnapshotMap convertedSnapshot;
            std::string snapshotMapError;
            if( stream::ReadConvertedSnapshotMap( view->path, *view, convertedSnapshot, snapshotMapError ) )
            {
                try
                {
                    snapshotPath = convertedSnapshot.snapshotPath;
                    persistentSnapshot = true;
                    auto validation = QueryIndex::Validate( snapshotPath );
                    if( !validation.manifest )
                    {
                        auto buildCallback = stateCallback;
                        QueryIndex::Build( snapshotPath, std::move( buildCallback ) );
                        validation = QueryIndex::Validate( snapshotPath );
                    }
                    if( validation.manifest )
                    {
                        auto openCallback = stateCallback;
                        auto source = QueryIndex::Open( *validation.manifest, std::move( openCallback ), StreamFingerprint( *view ) );
                        WriteStreamIndexCache( *view, snapshotPath );
                        return std::unique_ptr<SegmentTraceSource>( new SegmentTraceSource( std::move( store ), std::move( view ),
                            std::move( snapshotPath ), std::move( source ), true, true ) );
                    }
                }
                catch( const std::exception& )
                {
                    // A sidecar is only an optimization hint. Fall back to replaying the
                    // committed journal if the converted snapshot or its index is unusable.
                }
                snapshotPath.clear();
                persistentSnapshot = false;
            }

            snapshotPath = MakePersistentSnapshotPath( *view );
            persistentSnapshot = true;
            auto validation = std::filesystem::exists( snapshotPath ) ? QueryIndex::Validate( snapshotPath ) : QueryIndexValidation {};
            if( !validation.manifest )
            {
                auto replayPath = ReplayRevision( *view );
                std::error_code error;
                std::filesystem::remove( snapshotPath, error );
                error.clear();
                std::filesystem::rename( replayPath, snapshotPath, error );
                if( error )
                {
                    error.clear();
                    std::filesystem::copy_file( replayPath, snapshotPath, std::filesystem::copy_options::overwrite_existing, error );
                    std::error_code ignored; std::filesystem::remove( replayPath, ignored );
                    if( error ) throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::OpenFailed, "cannot publish the committed stream snapshot: " + error.message() );
                }
                auto buildCallback = stateCallback;
                QueryIndex::Build( snapshotPath, std::move( buildCallback ) );
                validation = QueryIndex::Validate( snapshotPath );
            }
            if( !validation.manifest )
                throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "committed stream query index validation failed: " + validation.reason );
            auto source = QueryIndex::Open( *validation.manifest, std::move( stateCallback ), StreamFingerprint( *view ) );
            WriteStreamIndexCache( *view, snapshotPath );
            return std::unique_ptr<SegmentTraceSource>( new SegmentTraceSource( std::move( store ), std::move( view ),
                std::move( snapshotPath ), std::move( source ), true, true ) );
        }
        snapshotPath = ReplayRevision( *view );
        auto source = analysis::WorkerTraceSource::Open( snapshotPath, std::move( stateCallback ), StreamFingerprint( *view ) );
        return std::unique_ptr<SegmentTraceSource>( new SegmentTraceSource( std::move( store ), std::move( view ),
            std::move( snapshotPath ), std::move( source ), false, false ) );
    }
    catch( ... )
    {
        std::error_code ignored;
        if( !persistentSnapshot ) std::filesystem::remove( snapshotPath, ignored );
        throw;
    }
}

SegmentTraceSource::SegmentTraceSource(
    std::shared_ptr<stream::JournalStore> store,
    std::shared_ptr<const stream::JournalReadView> view,
    std::filesystem::path snapshotPath,
    std::unique_ptr<analysis::TraceSource> source,
    bool preferIndex,
    bool persistentSnapshot )
    : m_store( std::move( store ) )
    , m_view( std::move( view ) )
    , m_snapshotPath( std::move( snapshotPath ) )
    , m_source( std::move( source ) )
    , m_preferIndex( preferIndex )
    , m_persistentSnapshot( persistentSnapshot )
{}

SegmentTraceSource::~SegmentTraceSource()
{
    m_source.reset();
    std::error_code ignored;
    if( !m_persistentSnapshot ) std::filesystem::remove( m_snapshotPath, ignored );
}

std::shared_ptr<const stream::JournalReadView> SegmentTraceSource::RefreshView()
{
    if( !m_store ) return m_view;
    m_store->Refresh();
    return m_store->AcquireReadView();
}

analysis::TraceReadView SegmentTraceSource::AcquireReadView() const
{
    return { analysis::TraceSourceKind::Segment, analysis::TraceSourceState::Ready, m_view->revision, int64_t( m_view->watermarkNs ), m_view->complete };
}

std::vector<analysis::Capability> SegmentTraceSource::GetCapabilities() const
{
    auto result = m_source->GetCapabilities();
    for( auto& capability : result )
    {
        if( capability.queryable )
        {
            capability.reason = "available at committed stream revision " + std::to_string( m_view->revision );
        }
    }
    return result;
}

#define TRACY_SEGMENT_FORWARD0( Return, Name ) Return SegmentTraceSource::Name() const { return m_source->Name(); }
#define TRACY_SEGMENT_FORWARD1( Return, Name, T1, A1 ) Return SegmentTraceSource::Name( T1 A1 ) const { return m_source->Name( A1 ); }
#define TRACY_SEGMENT_FORWARD2( Return, Name, T1, A1, T2, A2 ) Return SegmentTraceSource::Name( T1 A1, T2 A2 ) const { return m_source->Name( A1, A2 ); }
#define TRACY_SEGMENT_FORWARD3( Return, Name, T1, A1, T2, A2, T3, A3 ) Return SegmentTraceSource::Name( T1 A1, T2 A2, T3 A3 ) const { return m_source->Name( A1, A2, A3 ); }
#define TRACY_SEGMENT_FORWARD4( Return, Name, T1, A1, T2, A2, T3, A3, T4, A4 ) Return SegmentTraceSource::Name( T1 A1, T2 A2, T3 A3, T4 A4 ) const { return m_source->Name( A1, A2, A3, A4 ); }
#define TRACY_SEGMENT_FORWARD5( Return, Name, T1, A1, T2, A2, T3, A3, T4, A4, T5, A5 ) Return SegmentTraceSource::Name( T1 A1, T2 A2, T3 A3, T4 A4, T5 A5 ) const { return m_source->Name( A1, A2, A3, A4, A5 ); }

TRACY_SEGMENT_FORWARD0( analysis::TraceInfoDto, GetTraceInfo )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::ThreadDto>, GetThreads )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::FrameSetDto>, GetFrameSets )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::GpuContextDto>, GetGpuContexts )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::MemoryPoolDto>, GetMemoryPools )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::PlotDto>, GetPlotList )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::LockDto>, GetLocks )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::CpuZoneDto>, ScanCpuZones, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::GpuZoneDto>, ScanGpuZones, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::FrameDto>, ScanFrames, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::MemoryEventDto>, ScanMemoryEvents, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::MessageDto>, ScanMessages, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::PlotPointDto>, ScanPlots, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<std::string>, ScanLocks, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<std::string>, ScanContextSwitches, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<std::string>, ScanSamples, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::CorrelatedFrameEventDto>, GetCorrelatedFrameEvents )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::JobDto>, GetJobs )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::JobDto>, GetEvidenceJobs, uint64_t, frameId )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::IoRequestDto>, GetIoRequests )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::GfxDispatchDto>, GetGfxDispatches )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::GfxEntityDto>, GetGfxEntities )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::GfxLinkDto>, GetGfxLinks )
TRACY_SEGMENT_FORWARD2( analysis::GfxEvidenceSlice, GetEvidenceGfx, uint64_t, frameId, const std::vector<uint64_t>&, seedIds )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::RelationDto>, GetRelations )
TRACY_SEGMENT_FORWARD0( uint64_t, GetRelationCount )
TRACY_SEGMENT_FORWARD2( std::vector<analysis::RelationDto>, ScanRelations, size_t, offset, size_t, limit )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::RuntimeDomainStateDto>, GetRuntimeDomainStates )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::ScriptFrameDto>, GetScriptFrames )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::ScriptStackEventDto>, GetScriptStackEvents )
TRACY_SEGMENT_FORWARD0( analysis::CrashDto, GetCrash )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::CpuTopologyDto>, GetCpuTopology )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::CpuUsagePointDto>, GetCpuUsage )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::ContextSwitchDto>, ScanContextSwitchEvents, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::CpuContextSwitchDto>, ScanCpuContextSwitchEvents, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::SampleDto>, ScanSampleEvents, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::GhostZoneDto>, ScanGhostZones, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::HardwareSampleDto>, GetHardwareSamples )
TRACY_SEGMENT_FORWARD4( std::vector<analysis::HardwareSampleEventDto>, GetHardwareSampleEvents, uint64_t, address, std::string_view, kind, size_t, offset, size_t, limit )
TRACY_SEGMENT_FORWARD1( std::vector<analysis::LockEventDto>, ScanLockEvents, const analysis::ScanRange&, range )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::SymbolDto>, GetSymbols )
TRACY_SEGMENT_FORWARD2( std::vector<analysis::SymbolAddressMappingDto>, GetSymbolAddressMappings, size_t, offset, size_t, limit )
TRACY_SEGMENT_FORWARD1( std::optional<analysis::SymbolAddressMappingDto>, ResolveSymbolAddress, uint64_t, address )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::SourceLocationDto>, GetSourceLocations )
TRACY_SEGMENT_FORWARD2( std::vector<analysis::CallstackFrameDto>, ResolveCallstacks, const std::vector<uint32_t>&, callstacks, size_t, maxDepth )
TRACY_SEGMENT_FORWARD2( std::vector<analysis::CallstackFrameDto>, ResolveParentCallstacks, const std::vector<uint32_t>&, callstacks, size_t, maxDepth )
TRACY_SEGMENT_FORWARD2( std::vector<analysis::SourceTextDto>, ResolveSources, const std::vector<std::string>&, sourceRefs, size_t, maxBytes )
TRACY_SEGMENT_FORWARD2( std::vector<analysis::SymbolCodeDto>, ResolveSymbols, const std::vector<std::string>&, symbolRefs, size_t, maxBytes )
TRACY_SEGMENT_FORWARD2( std::vector<analysis::FrameImageDto>, ResolveFrameImages, const std::vector<std::string>&, imageRefs, size_t, maxBytes )
TRACY_SEGMENT_FORWARD3( std::vector<analysis::FrameDto>, GetFramesForSet, size_t, frameSetIndex, size_t, offset, size_t, limit )
TRACY_SEGMENT_FORWARD1( std::vector<int64_t>, GetFrameDurations, size_t, frameSetIndex )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::SourceResourceDto>, GetSourceResources )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::SymbolResourceDto>, GetSymbolResources )
TRACY_SEGMENT_FORWARD0( std::vector<analysis::FrameImageMetadataDto>, GetFrameImageResources )
TRACY_SEGMENT_FORWARD1( std::optional<analysis::CpuZoneDto>, GetCpuZone, std::string_view, ref )
TRACY_SEGMENT_FORWARD1( std::optional<analysis::GpuZoneDto>, GetGpuZone, std::string_view, ref )
TRACY_SEGMENT_FORWARD3( std::vector<analysis::CpuZoneDto>, GetCpuZoneChildren, std::string_view, ref, size_t, offset, size_t, limit )
TRACY_SEGMENT_FORWARD3( std::vector<analysis::GpuZoneDto>, GetGpuZoneChildren, std::string_view, ref, size_t, offset, size_t, limit )
TRACY_SEGMENT_FORWARD4( analysis::MemoryFrameSnapshot, GetMemoryFrameSnapshot, size_t, frameSetIndex, size_t, frameIndex, const std::vector<std::string>&, poolRefs, bool, allGpuD3D12Pools )
TRACY_SEGMENT_FORWARD1( std::optional<analysis::MemoryEventDto>, GetMemoryEvent, const analysis::MemoryEventKey&, key )
TRACY_SEGMENT_FORWARD1( std::optional<std::string>, GetMemoryPoolRef, uint64_t, internalPoolKey )
TRACY_SEGMENT_FORWARD1( std::optional<std::string>, GetCpuZoneRef, uint64_t, internalZoneIndex )
TRACY_SEGMENT_FORWARD1( std::optional<std::string>, GetGpuZoneRef, uint64_t, internalZoneIndex )
std::optional<analysis::ZoneValidationSummaryDto> SegmentTraceSource::ValidateZoneIndex( const std::function<size_t( size_t )>& allowance ) const
{
    return m_source->ValidateZoneIndex( allowance );
}
std::optional<analysis::ZoneValidationSummaryDto> SegmentTraceSource::ValidateSystemTrace( const std::function<size_t( size_t )>& allowance ) const
{
    return m_source->ValidateSystemTrace( allowance );
}
TRACY_SEGMENT_FORWARD0( std::optional<bool>, HasGpuMemoryProtocol2 )
TRACY_SEGMENT_FORWARD2( std::string, MakeEntityRef, std::string_view, kind, uint64_t, id )
TRACY_SEGMENT_FORWARD2( std::optional<uint64_t>, ParseEntityRef, std::string_view, ref, std::string_view, kind )
TRACY_SEGMENT_FORWARD0( analysis::GpuMemoryAttribution, GetGpuMemoryAttribution )
TRACY_SEGMENT_FORWARD0( analysis::GpuMemoryAttribution, GetGpuMemorySummaryAttribution )
TRACY_SEGMENT_FORWARD5( std::optional<analysis::GpuMemoryPassPage>, ScanGpuMemoryPasses, size_t, offset, size_t, limit, std::optional<uint64_t>, requestedPassId, size_t, useOffset, size_t, useLimit )
TRACY_SEGMENT_FORWARD2( std::optional<analysis::GpuMemoryRequestScopePage>, ScanGpuMemoryRequestScopes, size_t, offset, size_t, limit )
TRACY_SEGMENT_FORWARD5( std::optional<analysis::GpuMemoryAllocationPage>, ScanGpuMemoryAllocations, size_t, offset, size_t, limit, std::optional<uint64_t>, allocationId, const std::string&, poolRef, const std::string&, relationState )
TRACY_SEGMENT_FORWARD2( analysis::GpuMemoryEvidenceSlice, GetGpuMemoryEvidence, const std::vector<uint64_t>&, passIds, size_t, maxUses )
TRACY_SEGMENT_FORWARD2( analysis::SourceTextDto, ReadEmbeddedSource, size_t, sourceId, size_t, maxBytes )
TRACY_SEGMENT_FORWARD3( analysis::BinaryResourceChunkDto, ReadEmbeddedSourceBytes, size_t, sourceId, size_t, offset, size_t, maxBytes )
TRACY_SEGMENT_FORWARD2( analysis::SymbolCodeDto, ReadSymbolCode, uint64_t, symbolId, size_t, maxBytes )
TRACY_SEGMENT_FORWARD3( analysis::BinaryResourceChunkDto, ReadSymbolCodeBytes, uint64_t, symbolId, size_t, offset, size_t, maxBytes )
TRACY_SEGMENT_FORWARD3( std::vector<analysis::DisassemblyInstructionDto>, DisassembleSymbol, std::string_view, symbolRef, size_t, maxBytes, size_t, maxInstructions )
TRACY_SEGMENT_FORWARD2( analysis::FrameImageDto, ReadFrameImage, size_t, imageId, size_t, maxBytes )
TRACY_SEGMENT_FORWARD3( analysis::BinaryResourceChunkDto, ReadFrameImageBc1, size_t, imageId, size_t, offset, size_t, maxBytes )

#undef TRACY_SEGMENT_FORWARD0
#undef TRACY_SEGMENT_FORWARD1
#undef TRACY_SEGMENT_FORWARD2
#undef TRACY_SEGMENT_FORWARD3
#undef TRACY_SEGMENT_FORWARD4
#undef TRACY_SEGMENT_FORWARD5

}
