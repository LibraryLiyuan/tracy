#include "TracyStreamProtocol.hpp"

#include <array>
#include <limits>
#include <vector>

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
}

std::unique_ptr<StreamProtocolObserver> StreamProtocolObserver::CreateFileJournal( const std::filesystem::path& path, std::string_view address, uint16_t port, uint32_t protocolVersion, bool overwrite, const ProtocolJournalOptions& options, std::string& error )
{
    error.clear();
    auto header = MakeFileHeader( protocolVersion );
    auto writer = JournalWriter::CreateFileJournal( path, header, overwrite, options.writer, error );
    if( !writer ) return {};

    auto observer = std::unique_ptr<StreamProtocolObserver>( new StreamProtocolObserver( std::move( writer ), options ) );
    if( !observer->AppendSessionBegin( address, port, protocolVersion, error ) )
    {
        observer->SetFailure( error );
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
    return true;
}

uint64_t StreamProtocolObserver::TimestampNs() const
{
    return uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - m_started ).count() );
}

void StreamProtocolObserver::SetFailure( const std::string& error )
{
    m_failed = true;
    if( m_error.empty() ) m_error = error.empty() ? "stream journal operation failed" : error;
}

bool StreamProtocolObserver::AppendCheckpoint( uint64_t timestampNs )
{
    std::array<uint8_t, 24> payload = {};
    Put16( payload.data(), 0, 1 );
    Put16( payload.data(), 2, uint16_t( payload.size() ) );
    Put64( payload.data(), 8, m_clientBytes );
    Put64( payload.data(), 16, m_serverBytes );

    std::string error;
    if( !m_writer->Append( RecordType::Checkpoint, RecordFlagDurabilityBoundary, payload, timestampNs, error ) )
    {
        SetFailure( error );
        return false;
    }
    m_bytesAtLastDurable = m_clientBytes + m_serverBytes;
    m_lastDurableNs = timestampNs;
    return true;
}

bool StreamProtocolObserver::OnProtocolData( ProtocolDirection direction, ProtocolChunk chunk, std::span<const ProtocolDataSpan> data )
{
    std::lock_guard<std::mutex> lock( m_lock );
    if( m_failed || m_finalized ) return false;

    std::vector<PayloadSpan> payload;
    payload.reserve( data.size() );
    uint64_t byteCount = 0;
    for( const auto& span : data )
    {
        if( span.size > std::numeric_limits<uint64_t>::max() - byteCount )
        {
            SetFailure( "protocol byte count overflow" );
            return false;
        }
        byteCount += span.size;
        payload.emplace_back( PayloadSpan { static_cast<const uint8_t*>( span.data ), span.size } );
    }

    auto& directionalBytes = direction == ProtocolDirection::ClientToServer ? m_clientBytes : m_serverBytes;
    const auto otherBytes = direction == ProtocolDirection::ClientToServer ? m_serverBytes : m_clientBytes;
    if( byteCount > std::numeric_limits<uint64_t>::max() - directionalBytes ||
        directionalBytes + byteCount > std::numeric_limits<uint64_t>::max() - otherBytes )
    {
        SetFailure( "protocol cumulative byte count overflow" );
        return false;
    }

    const auto timestampNs = TimestampNs();
    const auto type = direction == ProtocolDirection::ClientToServer ? RecordType::ClientToServer : RecordType::ServerToClient;
    std::string error;
    if( !m_writer->Append( type, JournalFlags( chunk ), payload, timestampNs, error ) )
    {
        SetFailure( error );
        return false;
    }

    directionalBytes += byteCount;

    const uint64_t totalBytes = m_clientBytes + m_serverBytes;
    const bool byteBoundary = m_options.durableIntervalBytes != 0 && totalBytes - m_bytesAtLastDurable >= m_options.durableIntervalBytes;
    const bool timeBoundary = m_options.durableIntervalNs != 0 && timestampNs - m_lastDurableNs >= m_options.durableIntervalNs;
    if( byteBoundary || timeBoundary ) return AppendCheckpoint( timestampNs );
    return true;
}

bool StreamProtocolObserver::OnProtocolClose( ProtocolCloseReason reason )
{
    std::lock_guard<std::mutex> lock( m_lock );
    if( m_finalized ) return !m_failed;
    m_finalized = true;
    if( m_failed ) return false;

    std::array<uint8_t, 32> payload = {};
    Put16( payload.data(), 0, 1 );
    Put16( payload.data(), 2, uint16_t( payload.size() ) );
    Put32( payload.data(), 4, uint32_t( reason ) );
    Put64( payload.data(), 8, m_clientBytes );
    Put64( payload.data(), 16, m_serverBytes );

    std::string error;
    if( !m_writer->Append( RecordType::SessionEnd, RecordFlagTerminal, payload, TimestampNs(), error ) )
    {
        SetFailure( error );
        return false;
    }
    return true;
}

bool StreamProtocolObserver::Failed() const
{
    std::lock_guard<std::mutex> lock( m_lock );
    return m_failed;
}

bool StreamProtocolObserver::Finalized() const
{
    std::lock_guard<std::mutex> lock( m_lock );
    return m_finalized;
}

std::string StreamProtocolObserver::LastError() const
{
    std::lock_guard<std::mutex> lock( m_lock );
    return m_error;
}

uint64_t StreamProtocolObserver::ClientBytes() const
{
    std::lock_guard<std::mutex> lock( m_lock );
    return m_clientBytes;
}

uint64_t StreamProtocolObserver::ServerBytes() const
{
    std::lock_guard<std::mutex> lock( m_lock );
    return m_serverBytes;
}

uint64_t StreamProtocolObserver::CommittedSize() const
{
    std::lock_guard<std::mutex> lock( m_lock );
    return m_writer->CommittedSize();
}

uint64_t StreamProtocolObserver::DurableSize() const
{
    std::lock_guard<std::mutex> lock( m_lock );
    return m_writer->DurableSize();
}

}
