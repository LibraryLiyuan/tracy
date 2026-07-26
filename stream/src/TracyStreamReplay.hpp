#pragma once

#include "TracyStreamJournal.hpp"

#include "../../public/common/TracyProtocol.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tracy::stream
{

struct ReplayServerPacket
{
    uint64_t sequence = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> payload;
};

bool IsOrderIndependentServerQuery( const ReplayServerPacket& packet );

class ReplayServerTranscriptVerifier
{
public:
    bool Append( const ReplayServerPacket& recorded, const ReplayServerPacket& replayed, std::string& error );
    bool Finish( std::string& error );

private:
    using QueryBytes = std::array<uint8_t, tracy::ServerQueryPacketSize>;

    bool FlushBatch( std::string& error );

    std::vector<QueryBytes> m_recordedBatch;
    std::vector<QueryBytes> m_replayedBatch;
    uint64_t m_firstSequence = 0;
    uint64_t m_lastSequence = 0;
    bool m_hasBatch = false;
};

// ProtocolOnly recording and Full replay intentionally use different event
// dispatchers. They must emit the same server-query set, but independent
// definition queries may be scheduled in a different order. This comparison
// preserves exact ordering for handshakes, disconnect/terminate controls and
// source-transfer fragments, while comparing each contiguous independent
// definition-query batch as a byte-exact multiset.
bool VerifyServerTranscript(
    const std::vector<ReplayServerPacket>& recorded,
    const std::vector<ReplayServerPacket>& replayed,
    std::string& error );

}
