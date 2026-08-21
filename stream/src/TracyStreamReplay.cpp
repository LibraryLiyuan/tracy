#include "TracyStreamReplay.hpp"

#include "../../public/common/TracyProtocol.hpp"

#include <algorithm>
#include <iterator>

namespace tracy::stream
{
namespace
{

bool IsOrderIndependentQueryPayload( const std::vector<uint8_t>& payload )
{
    if( payload.size() != tracy::ServerQueryPacketSize ) return false;
    switch( payload[0] )
    {
    case tracy::ServerQueryString:
    case tracy::ServerQueryThreadString:
    case tracy::ServerQuerySourceLocation:
    case tracy::ServerQueryPlotName:
    case tracy::ServerQueryFrameName:
    case tracy::ServerQueryFiberName:
    case tracy::ServerQueryExternalName:
    case tracy::ServerQueryCallstackFrame:
    case tracy::ServerQuerySymbol:
    case tracy::ServerQuerySymbolCode:
    case tracy::ServerQuerySourceCode:
        return true;
    default:
        return false;
    }
}

std::string SequenceText( uint64_t first, uint64_t last )
{
    if( first == last ) return std::to_string( first );
    return std::to_string( first ) + "-" + std::to_string( last );
}

std::string QueryBytesText( const std::array<uint8_t, tracy::ServerQueryPacketSize>& query )
{
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve( query.size() * 2 );
    for( const auto value : query )
    {
        result.push_back( Hex[value >> 4] );
        result.push_back( Hex[value & 0x0F] );
    }
    return result;
}

bool VerifyOrderedPacket( const ReplayServerPacket& recorded, const ReplayServerPacket& replayed, std::string& error )
{
    if( recorded.payload.size() != replayed.payload.size() )
    {
        error = "sequence " + std::to_string( recorded.sequence ) +
            ": server payload size differs (recorded=" + std::to_string( recorded.payload.size() ) +
            ", replay=" + std::to_string( replayed.payload.size() ) + ")";
        return false;
    }
    const auto mismatch = std::mismatch( recorded.payload.begin(), recorded.payload.end(), replayed.payload.begin() );
    if( mismatch.first != recorded.payload.end() )
    {
        const auto offset = size_t( mismatch.first - recorded.payload.begin() );
        error = "sequence " + std::to_string( recorded.sequence ) +
            ": ordered server stream diverged at payload byte " + std::to_string( offset ) +
            " (recorded=" + std::to_string( unsigned( recorded.payload[offset] ) ) +
            ", replay=" + std::to_string( unsigned( replayed.payload[offset] ) ) + ")";
        return false;
    }
    return true;
}

}

bool IsOrderIndependentServerQuery( const ReplayServerPacket& packet )
{
    return ( packet.flags & RecordFlagServerQuery ) != 0 && IsOrderIndependentQueryPayload( packet.payload );
}

bool ReplayServerTranscriptVerifier::Append(
    const ReplayServerPacket& recorded,
    const ReplayServerPacket& replayed,
    std::string& error )
{
    if( ( recorded.flags & RecordFlagServerQuery ) != 0 &&
        recorded.payload.size() > tracy::ServerQueryPacketSize &&
        recorded.payload.size() % tracy::ServerQueryPacketSize == 0 )
    {
        if( recorded.payload.size() != replayed.payload.size() )
        {
            error = "sequence " + std::to_string( recorded.sequence ) +
                ": batched server-query payload size differs (recorded=" + std::to_string( recorded.payload.size() ) +
                ", replay=" + std::to_string( replayed.payload.size() ) + ")";
            return false;
        }
        for( size_t offset=0; offset<recorded.payload.size(); offset+=tracy::ServerQueryPacketSize )
        {
            ReplayServerPacket recordedQuery { recorded.sequence, recorded.flags,
                std::vector<uint8_t>( recorded.payload.begin() + offset,
                    recorded.payload.begin() + offset + tracy::ServerQueryPacketSize ) };
            ReplayServerPacket replayedQuery { replayed.sequence, replayed.flags,
                std::vector<uint8_t>( replayed.payload.begin() + offset,
                    replayed.payload.begin() + offset + tracy::ServerQueryPacketSize ) };
            if( !Append( recordedQuery, replayedQuery, error ) ) return false;
        }
        return true;
    }
    if( IsOrderIndependentServerQuery( recorded ) )
    {
        if( replayed.payload.size() != tracy::ServerQueryPacketSize )
        {
            error = "sequence " + std::to_string( recorded.sequence ) +
                ": replayed definition query has size " + std::to_string( replayed.payload.size() ) +
                " instead of " + std::to_string( tracy::ServerQueryPacketSize );
            return false;
        }
        if( !m_hasBatch )
        {
            m_firstSequence = recorded.sequence;
            m_hasBatch = true;
        }

        QueryBytes expected {};
        QueryBytes actual {};
        std::copy( recorded.payload.begin(), recorded.payload.end(), expected.begin() );
        std::copy( replayed.payload.begin(), replayed.payload.end(), actual.begin() );
        m_recordedBatch.emplace_back( expected );
        m_replayedBatch.emplace_back( actual );
        m_lastSequence = recorded.sequence;
        return true;
    }

    if( !FlushBatch( error ) ) return false;
    return VerifyOrderedPacket( recorded, replayed, error );
}

bool ReplayServerTranscriptVerifier::Finish( std::string& error )
{
    return FlushBatch( error );
}

bool ReplayServerTranscriptVerifier::FlushBatch( std::string& error )
{
    if( !m_hasBatch ) return true;
    std::sort( m_recordedBatch.begin(), m_recordedBatch.end() );
    std::sort( m_replayedBatch.begin(), m_replayedBatch.end() );
    bool matches = m_recordedBatch == m_replayedBatch;
    if( !matches )
    {
        const auto mismatch = std::mismatch(
            m_recordedBatch.begin(), m_recordedBatch.end(),
            m_replayedBatch.begin(), m_replayedBatch.end() );
        const auto index = size_t( mismatch.first - m_recordedBatch.begin() );
        std::vector<QueryBytes> recordedOnly;
        std::vector<QueryBytes> replayOnly;
        std::set_difference(
            m_recordedBatch.begin(), m_recordedBatch.end(),
            m_replayedBatch.begin(), m_replayedBatch.end(),
            std::back_inserter( recordedOnly ) );
        std::set_difference(
            m_replayedBatch.begin(), m_replayedBatch.end(),
            m_recordedBatch.begin(), m_recordedBatch.end(),
            std::back_inserter( replayOnly ) );
        const auto allType = []( const std::vector<QueryBytes>& values, uint8_t type ) {
            return !values.empty() && std::all_of( values.begin(), values.end(),
                [type]( const QueryBytes& value ) { return value[0] == type; } );
        };
        // String and ThreadString definition requests are scheduled from two
        // independent Worker queues. With the exact same client byte stream a
        // short batch can legitimately move wholly from one queue to the other
        // during replay. Accept only that narrow, balanced classification
        // drift. Pointer/tag changes within either class and every other query
        // or control packet remain byte-exact failures.
        const bool balancedStringClassificationDrift = recordedOnly.size() == replayOnly.size() &&
            ( ( allType( recordedOnly, uint8_t( tracy::ServerQueryThreadString ) ) &&
                allType( replayOnly, uint8_t( tracy::ServerQueryString ) ) ) ||
              ( allType( recordedOnly, uint8_t( tracy::ServerQueryString ) ) &&
                allType( replayOnly, uint8_t( tracy::ServerQueryThreadString ) ) ) );
        if( balancedStringClassificationDrift ) matches = true;
        const auto listText = []( const std::vector<QueryBytes>& queries ) {
            std::string text;
            const auto limit = std::min<size_t>( queries.size(), 8 );
            for( size_t i = 0; i < limit; i++ )
            {
                if( i != 0 ) text += ',';
                text += QueryBytesText( queries[i] );
            }
            if( queries.size() > limit ) text += ",...";
            return text;
        };
        if( !matches ) error = "order-independent server-query set at sequences " +
            SequenceText( m_firstSequence, m_lastSequence ) +
            " differs at sorted index " + std::to_string( index ) +
            " (recorded=" + QueryBytesText( *mismatch.first ) +
            ", replay=" + QueryBytesText( *mismatch.second ) +
            ", count=" + std::to_string( m_recordedBatch.size() ) +
            ", recorded-only=" + std::to_string( recordedOnly.size() ) + "[" + listText( recordedOnly ) + "]" +
            ", replay-only=" + std::to_string( replayOnly.size() ) + "[" + listText( replayOnly ) + "])";
    }
    m_recordedBatch.clear();
    m_replayedBatch.clear();
    m_hasBatch = false;
    return matches;
}

bool VerifyServerTranscript(
    const std::vector<ReplayServerPacket>& recorded,
    const std::vector<ReplayServerPacket>& replayed,
    std::string& error )
{
    error.clear();
    if( recorded.size() != replayed.size() )
    {
        error = "server packet count differs (recorded=" + std::to_string( recorded.size() ) +
            ", replay=" + std::to_string( replayed.size() ) + ")";
        return false;
    }

    ReplayServerTranscriptVerifier verifier;
    for( size_t index = 0; index < recorded.size(); index++ )
    {
        if( !verifier.Append( recorded[index], replayed[index], error ) ) return false;
    }
    return verifier.Finish( error );
}

}
