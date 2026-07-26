#include "TracyStreamReplay.hpp"

#include "../../public/common/TracyProtocol.hpp"

#include <algorithm>

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
    if( IsOrderIndependentServerQuery( recorded ) )
    {
        if( replayed.payload.size() != tracy::ServerQueryPacketSize )
        {
            error = "sequence " + std::to_string( recorded.sequence ) +
                ": replayed definition query has size " + std::to_string( replayed.payload.size() ) +
                " instead of " + std::to_string( tracy::ServerQueryPacketSize );
            return false;
        }
        if( m_hasBatch && recorded.sequence != m_lastSequence + 1 && !FlushBatch( error ) ) return false;
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
    const auto matches = m_recordedBatch == m_replayedBatch;
    if( !matches )
    {
        error = "order-independent server-query batch at sequences " +
            SequenceText( m_firstSequence, m_lastSequence ) +
            " differs in query bytes or multiplicity";
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
