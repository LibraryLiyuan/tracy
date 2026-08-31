#ifndef __TRACYTRACESESSIONPROTOCOLINVENTORY_HPP__
#define __TRACYTRACESESSIONPROTOCOLINVENTORY_HPP__

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

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
    // Valid only for the duration of the visitor call. Consumers must copy
    // bytes they need before returning.
    const uint8_t* encodedData = nullptr;
    uint8_t queueType = 0;
    uint32_t frameOffset = 0;
    uint32_t encodedBytes = 0;
    uint32_t variablePayloadBytes = 0;
};

using TraceSessionProtocolEventVisitor = bool ( * )(
    const TraceSessionProtocolEventInfo& event, void* userData, std::string& error );

TraceSessionProtocolDomain ClassifyTraceProtocolEvent( uint8_t queueType );
const char* TraceSessionProtocolDomainName( TraceSessionProtocolDomain domain );
bool TryGetTraceProtocolEventTime( const TraceSessionProtocolEventInfo& event, int64_t& time );

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
        TraceSessionProtocolEventVisitor visitor = nullptr, void* visitorUserData = nullptr,
        std::span<const uint8_t>* decodedFrame = nullptr );
    std::vector<uint8_t> ExportDictionary() const;
    bool RestoreDictionary( std::span<const uint8_t> dictionary, std::string& error );

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
