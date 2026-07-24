#ifndef __TRACYPROTOCOLOBSERVER_HPP__
#define __TRACYPROTOCOLOBSERVER_HPP__

#include <cstddef>
#include <cstdint>
#include <span>

namespace tracy
{

enum class ProtocolDirection : uint8_t
{
    ClientToServer,
    ServerToClient,
    LocalControl
};

enum class ProtocolChunk : uint8_t
{
    Handshake,
    CompressedFrame,
    ServerQuery,
    ControlState
};

enum class ProtocolCloseReason : uint32_t
{
    LocalShutdown = 1,
    PeerDisconnected = 2,
    CaptureComplete = 3,
    ProtocolMismatch = 4,
    NotAvailable = 5,
    HandshakeDropped = 6,
    MemoryLimit = 7,
    InstrumentationFailure = 8,
    RecorderFailure = 9,
    TransportError = 10,
    ObserverDestroyed = 11
};

struct ProtocolDataSpan
{
    const void* data = nullptr;
    size_t size = 0;
};

class ProtocolObserver
{
public:
    virtual ~ProtocolObserver() = default;

    virtual bool OnProtocolData( ProtocolDirection direction, ProtocolChunk chunk, std::span<const ProtocolDataSpan> data ) = 0;
    virtual bool OnProtocolClose( ProtocolCloseReason reason ) = 0;
};

}

#endif
