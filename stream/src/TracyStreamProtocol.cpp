#include "TracyStreamProtocol.hpp"

#include <array>
#include <limits>

namespace tracy::stream
{
namespace
{

void Put16( uint8_t* output, size_t offset, uint16_t value )
{
    output[offset] = uint8_t( value );
    output[offset + 1] = uint8_t( value >> 8 );
}

void Put32( uint8_t* output, size_t offset, uint32_t value )
{
    for( size_t i = 0; i < 4; i++ ) output[offset + i] = uint8_t( value >> ( i * 8 ) );
}

void Put64( uint8_t* output, size_t offset, uint64_t value )
{
    for( size_t i = 0; i < 8; i++ ) output[offset + i] = uint8_t( value >> ( i * 8 ) );
}

uint32_t JournalFlags( ProtocolChunk chunk )
{
    switch( chunk )
    {
    case ProtocolChunk::Handshake: return RecordFlagHandshake;
    case ProtocolChunk::CompressedFrame: return RecordFlagCompressedFrame;
    case ProtocolChunk::ServerQuery: return RecordFlagServerQuery;
    default: return RecordFlagNone;
    }
}

}

StreamProtocolObserver::StreamProtocolObserver( std::unique_ptr<JournalWriter> writer, const ProtocolJournalOptions& options )
    : m_writer( std::move( writer ) )
    , m_options( options )
    , m_started( std::chrono::steady_clock::now() )
{
}

StreamProtocolObserver::~StreamProtocolObserver()
{
    OnProtocolClose( ProtocolCloseReason::ObserverDestroyed );
    if( m_writerThread.joinable() ) m_writerThread.join();
}

std::unique_ptr<StreamProtocolObserver> StreamProtocolObserver::CreateFileJournal( const std::filesystem::path& path, std::string_view address, uint16_t port, uint32_t protocolVersion, bool overwrite, const ProtocolJournalOptions& options, std::string& error )
{
    error.clear();
    auto header = MakeFileHeader( protocolVersion );
    auto writer = JournalWriter::CreateFileJournal( path, header, overwrite, options.writer, error );
    if( !writer ) return {};
    return CreateJournal( std::move( writer ), address, port, protocolVersion, options, error );
}

std::unique_ptr<StreamProtocolObserver> StreamProtocolObserver::CreateJournal( std::unique_ptr<JournalWriter> writer, std::string_view address, uint16_t port, uint32_t protocolVersion, const ProtocolJournalOptions& options, std::string& error )
{
    error.clear();
    if( !writer )
    {
        error = "protocol journal writer is null";
        return {};
    }
    if( options.bufferBytes == 0 )
    {
        error = "protocol journal buffer size must be non-zero";
        return {};
    }

    auto observer = std::unique_ptr<StreamProtocolObserver>( new StreamProtocolObserver( std::move( writer ), options ) );
    if( !observer->AppendSessionBegin( address, port, protocolVersion, error ) )
    {
        std::lock_guard lock( observer->m_lock );
        observer->SetFailureLocked( error );
        observer->m_finalized = true;
        return {};
    }

    try
    {
        observer->m_writerThread = std::thread( [instance = observer.get()] { instance->WriterLoop(); } );
    }
    catch( const std::exception& exception )
    {
        error = std::string( "cannot start protocol journal writer thread: " ) + exception.what();
        std::lock_guard lock( observer->m_lock );
        observer->SetFailureLocked( error );
        observer->m_finalized = true;
        return {};
    }
    return observer;
}

bool StreamProtocolObserver::AppendSessionBegin( std::string_view address, uint16_t port, uint32_t protocolVersion, std::string& error )
{
    if( address.size() > std::numeric_limits<uint32_t>::max() )
    {
        error = "capture address is too long";
        return false;
    }

    std::array<uint8_t, 24> metadata = {};
    Put16( metadata.data(), 0, 1 );
    Put16( metadata.data(), 2, uint16_t( metadata.size() ) );
    Put32( metadata.data(), 4, protocolVersion );
    Put16( metadata.data(), 8, port );
    Put32( metadata.data(), 12, uint32_t( address.size() ) );

    const std::array<PayloadSpan, 2> payload = {
        PayloadSpan { metadata.data(), metadata.size() },
        PayloadSpan { reinterpret_cast<const uint8_t*>( address.data() ), address.size() }
    };
    if( !m_writer->Append( RecordType::SessionBegin, RecordFlagHandshake, payload, 0, error ) ) return false;
    if( !m_writer->Flush( FlushMode::Durable, error ) ) return false;
    m_bytesAtLastDurable = 0;
    m_lastDurableNs = 0;
    m_committedSize = m_writer->CommittedSize();
    m_durableSize = m_writer->DurableSize();
    return true;
}

uint64_t StreamProtocolObserver::TimestampNs() const
{
    return uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - m_started ).count() );
}

void StreamProtocolObserver::SetFailureLocked( const std::string& error )
{
    m_failed = true;
    if( m_error.empty() ) m_error = error.empty() ? "stream journal operation failed" : error;
    m_writerReady.notify_all();
    m_queueSpace.notify_all();
}

bool StreamProtocolObserver::AppendCheckpoint( uint64_t timestampNs, uint64_t clientBytes, uint64_t serverBytes )
{
    std::array<uint8_t, 24> payload = {};
    Put16( payload.data(), 0, 1 );
    Put16( payload.data(), 2, uint16_t( payload.size() ) );
    Put64( payload.data(), 8, clientBytes );
    Put64( payload.data(), 16, serverBytes );

    std::string error;
    if( !m_writer->Append( RecordType::Checkpoint, RecordFlagDurabilityBoundary, payload, timestampNs, error ) )
    {
        std::lock_guard lock( m_lock );
        SetFailureLocked( error );
        return false;
    }
    m_bytesAtLastDurable = clientBytes + serverBytes;
    m_lastDurableNs = timestampNs;
    return true;
}

bool StreamProtocolObserver::AppendTerminal( ProtocolCloseReason reason, uint64_t clientBytes, uint64_t serverBytes )
{
    std::array<uint8_t, 32> payload = {};
    Put16( payload.data(), 0, 1 );
    Put16( payload.data(), 2, uint16_t( payload.size() ) );
    Put32( payload.data(), 4, uint32_t( reason ) );
    Put64( payload.data(), 8, clientBytes );
    Put64( payload.data(), 16, serverBytes );

    std::string error;
    if( !m_writer->Append( RecordType::SessionEnd, RecordFlagTerminal, payload, TimestampNs(), error ) )
    {
        std::lock_guard lock( m_lock );
        SetFailureLocked( error );
        return false;
    }
    return true;
}

bool StreamProtocolObserver::WriteProtocolRecord( PendingRecord& record )
{
    std::string error;
    if( !m_writer->Append( record.type, record.flags, record.payload, record.timestampNs, error ) )
    {
        std::lock_guard lock( m_lock );
        SetFailureLocked( error );
        return false;
    }

    const uint64_t totalBytes = record.clientBytesAfter + record.serverBytesAfter;
    const bool byteBoundary = m_options.durableIntervalBytes != 0 && totalBytes - m_bytesAtLastDurable >= m_options.durableIntervalBytes;
    const bool timeBoundary = m_options.durableIntervalNs != 0 && record.timestampNs - m_lastDurableNs >= m_options.durableIntervalNs;
    if( byteBoundary || timeBoundary )
    {
        return AppendCheckpoint( record.timestampNs, record.clientBytesAfter, record.serverBytesAfter );
    }
    return true;
}

void StreamProtocolObserver::WriterLoop()
{
    for( ;; )
    {
        PendingRecord record;
        ProtocolCloseReason closeReason = ProtocolCloseReason::ObserverDestroyed;
        uint64_t clientBytes = 0;
        uint64_t serverBytes = 0;
        bool shouldClose = false;
        {
            std::unique_lock lock( m_lock );
            m_writerReady.wait( lock, [this] { return m_failed || !m_queue.empty() || m_closeRequested; } );
            if( m_failed )
            {
                m_finalized = true;
                m_writerReady.notify_all();
                m_queueSpace.notify_all();
                return;
            }
            if( !m_queue.empty() )
            {
                record = std::move( m_queue.front() );
                m_queue.pop_front();
                m_queuedBytes = 0;
                m_activeBytes = record.payload.size();
                m_peakBufferedBytes = std::max( m_peakBufferedBytes, m_activeBytes );
                m_queueSpace.notify_all();
            }
            else
            {
                shouldClose = true;
                closeReason = m_closeReason;
                clientBytes = m_clientBytes;
                serverBytes = m_serverBytes;
            }
        }

        if( shouldClose )
        {
            const bool success = AppendTerminal( closeReason, clientBytes, serverBytes );
            std::lock_guard lock( m_lock );
            m_committedSize = m_writer->CommittedSize();
            m_durableSize = m_writer->DurableSize();
            m_activeBytes = 0;
            m_finalized = true;
            if( !success && !m_failed ) SetFailureLocked( "cannot append terminal protocol journal record" );
            m_writerReady.notify_all();
            m_queueSpace.notify_all();
            return;
        }

        const bool success = WriteProtocolRecord( record );
        {
            std::lock_guard lock( m_lock );
            m_committedSize = m_writer->CommittedSize();
            m_durableSize = m_writer->DurableSize();
            m_activeBytes = 0;
            m_queueSpace.notify_all();
            if( !success )
            {
                m_finalized = true;
                m_writerReady.notify_all();
                return;
            }
        }
    }
}

bool StreamProtocolObserver::OnProtocolData( ProtocolDirection direction, ProtocolChunk chunk, std::span<const ProtocolDataSpan> data )
{
    uint64_t byteCount = 0;
    for( const auto& span : data )
    {
        if( span.size != 0 && span.data == nullptr ) return false;
        if( span.size > std::numeric_limits<uint64_t>::max() - byteCount ) return false;
        byteCount += span.size;
    }
    if( byteCount > m_options.bufferBytes )
    {
        std::lock_guard lock( m_lock );
        SetFailureLocked( "protocol record exceeds the configured bounded buffer size" );
        return false;
    }
    if( byteCount > size_t( std::numeric_limits<size_t>::max() ) )
    {
        std::lock_guard lock( m_lock );
        SetFailureLocked( "protocol record does not fit in memory" );
        return false;
    }

    // Preserve callback order while waiting for the single fill slot. Together
    // with the writer's active record this forms the two-buffer relay.
    std::lock_guard enqueueOrder( m_enqueueLock );
    std::unique_lock lock( m_lock );
    bool countedWait = false;
    while( !m_queue.empty() && !m_failed && !m_finalized && !m_closeRequested )
    {
        if( !countedWait )
        {
            m_backpressureWaitCount++;
            countedWait = true;
        }
        m_queueSpace.wait( lock );
    }
    if( m_failed || m_finalized || m_closeRequested ) return false;

    const bool localControl = direction == ProtocolDirection::LocalControl;
    auto& directionalBytes = direction == ProtocolDirection::ClientToServer ? m_clientBytes : m_serverBytes;
    const auto otherBytes = direction == ProtocolDirection::ClientToServer ? m_serverBytes : m_clientBytes;
    if( !localControl && ( byteCount > std::numeric_limits<uint64_t>::max() - directionalBytes ||
        directionalBytes + byteCount > std::numeric_limits<uint64_t>::max() - otherBytes ) )
    {
        SetFailureLocked( "protocol cumulative byte count overflow" );
        return false;
    }

    PendingRecord record;
    record.type = localControl ? RecordType::Diagnostic :
        ( direction == ProtocolDirection::ClientToServer ? RecordType::ClientToServer : RecordType::ServerToClient );
    record.flags = JournalFlags( chunk ) | ( localControl ? RecordFlagLocalControl : 0 );
    record.timestampNs = TimestampNs();
    record.payload.reserve( size_t( byteCount ) );
    for( const auto& span : data )
    {
        if( span.size == 0 ) continue;
        const auto* begin = static_cast<const uint8_t*>( span.data );
        record.payload.insert( record.payload.end(), begin, begin + span.size );
    }

    if( !localControl ) directionalBytes += byteCount;
    record.clientBytesAfter = m_clientBytes;
    record.serverBytesAfter = m_serverBytes;
    m_queuedBytes = byteCount;
    m_queue.emplace_back( std::move( record ) );
    m_peakBufferedBytes = std::max( m_peakBufferedBytes, m_activeBytes + m_queuedBytes );
    m_writerReady.notify_one();
    return true;
}

bool StreamProtocolObserver::OnProtocolClose( ProtocolCloseReason reason )
{
    std::unique_lock lock( m_lock );
    if( m_finalized ) return !m_failed;
    if( m_failed )
    {
        m_finalized = true;
        return false;
    }
    m_closeReason = reason;
    m_closeRequested = true;
    m_writerReady.notify_one();
    m_writerReady.wait( lock, [this] { return m_finalized; } );
    return !m_failed;
}

bool StreamProtocolObserver::Failed() const
{
    std::lock_guard lock( m_lock );
    return m_failed;
}

bool StreamProtocolObserver::Finalized() const
{
    std::lock_guard lock( m_lock );
    return m_finalized;
}

std::string StreamProtocolObserver::LastError() const
{
    std::lock_guard lock( m_lock );
    return m_error;
}

uint64_t StreamProtocolObserver::ClientBytes() const
{
    std::lock_guard lock( m_lock );
    return m_clientBytes;
}

uint64_t StreamProtocolObserver::ServerBytes() const
{
    std::lock_guard lock( m_lock );
    return m_serverBytes;
}

uint64_t StreamProtocolObserver::CommittedSize() const
{
    std::lock_guard lock( m_lock );
    return m_committedSize;
}

uint64_t StreamProtocolObserver::DurableSize() const
{
    std::lock_guard lock( m_lock );
    return m_durableSize;
}

uint64_t StreamProtocolObserver::BackpressureWaitCount() const
{
    std::lock_guard lock( m_lock );
    return m_backpressureWaitCount;
}

uint64_t StreamProtocolObserver::PeakBufferedBytes() const
{
    std::lock_guard lock( m_lock );
    return m_peakBufferedBytes;
}

}
