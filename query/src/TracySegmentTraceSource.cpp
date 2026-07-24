#include "TracySegmentTraceSource.hpp"

#include "../../public/common/TracyAlloc.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracySocket.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../stream/src/TracyStreamJournal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
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
    uint64_t drainControlSequence = 0;
    for( const auto& record : scan.records )
    {
        if( record.type == stream::RecordType::ClientToServer ) clientRecords.emplace_back( record );
        else if( record.type == stream::RecordType::ServerToClient ) serverRecords.emplace_back( record );
        else if( record.type == stream::RecordType::Diagnostic &&
            ( record.flags & stream::RecordFlagLocalControl ) != 0 )
        {
            hasLocalDisconnect = true;
            if( ( record.flags & stream::RecordFlagServerQuery ) == 0 )
                drainControlSequence = record.sequence;
        }
    }
    if( clientRecords.empty() || serverRecords.empty() )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, "stream revision does not yet contain a complete Tracy handshake" );
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

    Worker worker( "127.0.0.1", port, -1 );
    if( hasLocalDisconnect && drainControlSequence == 0 ) worker.MarkProtocolDisconnect();
    std::unique_ptr<Socket, SocketDeleter> peer;
    const auto acceptDeadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while( !peer && std::chrono::steady_clock::now() < acceptDeadline ) peer.reset( listener->Accept() );
    if( !peer )
    {
        worker.Shutdown();
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Internal, "Tracy Worker did not connect to the revision replay socket" );
    }

    ReplayError replayError;
    std::thread verifier( [&] {
        PayloadReader reader( view.path );
        std::vector<uint8_t> expected;
        std::vector<uint8_t> actual;
        std::string error;
        for( const auto& record : serverRecords )
        {
            if( replayError.Failed() ) return;
            if( !reader.Read( record, expected, error ) )
            {
                replayError.Set( "server record " + std::to_string( record.sequence ) + ": " + error );
                return;
            }
            actual.resize( expected.size() );
            if( !actual.empty() && !peer->Read( actual.data(), int( actual.size() ), 1000 ) )
            {
                replayError.Set( "Worker server stream ended before record " + std::to_string( record.sequence ) );
                return;
            }
            const auto mismatch = std::mismatch( expected.begin(), expected.end(), actual.begin(), actual.end() );
            if( mismatch.first != expected.end() )
            {
                replayError.Set( "Worker query stream diverged at record " + std::to_string( record.sequence ) );
                return;
            }
        }
    } );

    PayloadReader clientReader( view.path );
    std::vector<uint8_t> payload;
    std::string readError;
    auto replayClientRecord = [&]( const stream::RecordInfo& record ) {
        if( replayError.Failed() ) return false;
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
        for( const auto& record : clientRecords )
        {
            if( !replayClientRecord( record ) ) break;
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
            worker.RequestProtocolDrain();
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
            replayError.Set( "Worker emitted server bytes beyond the committed revision" );
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

std::unique_ptr<SegmentTraceSource> SegmentTraceSource::Open( const std::filesystem::path& path, StateCallback stateCallback )
{
    std::string error;
    auto uniqueStore = stream::JournalStore::Open( path, error );
    if( !uniqueStore )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Corrupt, error.empty() ? "cannot open stream journal" : error );
    }
    auto store = std::shared_ptr<stream::JournalStore>( std::move( uniqueStore ) );
    return OpenRevision( store, store->AcquireReadView(), std::move( stateCallback ) );
}

std::unique_ptr<SegmentTraceSource> SegmentTraceSource::OpenRevision(
    std::shared_ptr<stream::JournalStore> store,
    std::shared_ptr<const stream::JournalReadView> view,
    StateCallback stateCallback )
{
    if( !store || !view )
    {
        throw analysis::TraceLoadError( analysis::TraceLoadErrorCode::Internal, "stream revision view is unavailable" );
    }
    if( stateCallback ) stateCallback( analysis::TraceSourceState::Loading );
    auto snapshotPath = ReplayRevision( *view );
    try
    {
        auto source = analysis::WorkerTraceSource::Open( snapshotPath, std::move( stateCallback ) );
        return std::unique_ptr<SegmentTraceSource>( new SegmentTraceSource( std::move( store ), std::move( view ), std::move( snapshotPath ), std::move( source ) ) );
    }
    catch( ... )
    {
        std::error_code ignored;
        std::filesystem::remove( snapshotPath, ignored );
        throw;
    }
}

SegmentTraceSource::SegmentTraceSource(
    std::shared_ptr<stream::JournalStore> store,
    std::shared_ptr<const stream::JournalReadView> view,
    std::filesystem::path snapshotPath,
    std::unique_ptr<analysis::WorkerTraceSource> source )
    : m_store( std::move( store ) )
    , m_view( std::move( view ) )
    , m_snapshotPath( std::move( snapshotPath ) )
    , m_source( std::move( source ) )
{}

SegmentTraceSource::~SegmentTraceSource()
{
    m_source.reset();
    std::error_code ignored;
    std::filesystem::remove( m_snapshotPath, ignored );
}

std::shared_ptr<const stream::JournalReadView> SegmentTraceSource::RefreshView()
{
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
TRACY_SEGMENT_FORWARD2( std::string, MakeEntityRef, std::string_view, kind, uint64_t, id )
TRACY_SEGMENT_FORWARD2( std::optional<uint64_t>, ParseEntityRef, std::string_view, ref, std::string_view, kind )
TRACY_SEGMENT_FORWARD0( analysis::GpuMemoryAttribution, GetGpuMemoryAttribution )
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

}
