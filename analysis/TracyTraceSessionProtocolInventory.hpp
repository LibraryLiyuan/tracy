#ifndef __TRACYTRACESESSIONPROTOCOLINVENTORY_HPP__
#define __TRACYTRACESESSIONPROTOCOLINVENTORY_HPP__

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace tracy::analysis
{

enum class TraceSessionProtocolDomain : uint8_t
{
    Control = 0,
    Frame,
    CpuZone,
    GpuZone,
    Job,
    CpuMemory,
    GpuMemory,
    GpuCatalog,
    Io,
    Sampling,
    Scheduling,
    MessagePlotLock,
    SourceCallstack,
    ScriptRuntime,
    Relation,
    Dictionary,
    Other,
    Count
};

struct TraceSessionProtocolEventStats
{
    uint64_t count = 0;
    uint64_t encodedBytes = 0;
    uint64_t variablePayloadBytes = 0;

    bool operator==( const TraceSessionProtocolEventStats& ) const = default;
};

struct TraceSessionProtocolInventory
{
    static constexpr size_t EventTypeCapacity = 256;

    uint64_t frameCount = 0;
    uint64_t eventCount = 0;
    uint64_t encodedBytes = 0;
    uint64_t compressedBytes = 0;
    std::array<TraceSessionProtocolEventStats, EventTypeCapacity> events {};
    std::array<TraceSessionProtocolEventStats,
        size_t( TraceSessionProtocolDomain::Count )> domains {};

    bool operator==( const TraceSessionProtocolInventory& ) const = default;
};

struct TraceSessionProtocolEventInfo
{
    uint8_t queueType = 0;
    uint32_t frameOffset = 0;
    uint32_t encodedBytes = 0;
    uint32_t variablePayloadBytes = 0;
};

using TraceSessionProtocolEventVisitor = bool ( * )(
    const TraceSessionProtocolEventInfo& event, void* userData, std::string& error );

TraceSessionProtocolDomain ClassifyTraceProtocolEvent( uint8_t queueType );
const char* TraceSessionProtocolDomainName( TraceSessionProtocolDomain domain );

// Counts one already-decompressed Tracy protocol frame. The frame must end
// exactly at an event boundary; malformed or truncated variable payloads are
// rejected instead of being approximated.
bool CountTraceProtocolFrame( std::span<const uint8_t> frame,
    TraceSessionProtocolInventory& inventory, std::string& error,
    TraceSessionProtocolEventVisitor visitor = nullptr, void* visitorUserData = nullptr );

class TraceSessionProtocolDecoder
{
public:
    TraceSessionProtocolDecoder();
    ~TraceSessionProtocolDecoder();
    TraceSessionProtocolDecoder( TraceSessionProtocolDecoder&& ) noexcept;
    TraceSessionProtocolDecoder& operator=( TraceSessionProtocolDecoder&& ) noexcept;
    TraceSessionProtocolDecoder( const TraceSessionProtocolDecoder& ) = delete;
    TraceSessionProtocolDecoder& operator=( const TraceSessionProtocolDecoder& ) = delete;

    bool ConsumeCompressedRecord( std::span<const uint8_t> record,
        TraceSessionProtocolInventory& inventory, std::string& error,
        TraceSessionProtocolEventVisitor visitor = nullptr, void* visitorUserData = nullptr );

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
