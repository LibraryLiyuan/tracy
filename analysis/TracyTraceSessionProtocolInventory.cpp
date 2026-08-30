#include "TracyTraceSessionProtocolInventory.hpp"

#include "TracyQueue.hpp"
#include "TracyProtocol.hpp"
#include "tracy_lz4.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace tracy::analysis
{
namespace
{

bool IsLargePayloadEvent( QueueType type )
{
    switch( type )
    {
    case QueueType::FrameImageData:
    case QueueType::SymbolCode:
    case QueueType::SourceCode:
    case QueueType::JnGpuReferenceSetDefinition:
    case QueueType::JnGpuCatalogBatchData:
        return true;
    default:
        return false;
    }
}

bool Add( uint64_t& value, uint64_t amount )
{
    if( amount > std::numeric_limits<uint64_t>::max() - value ) return false;
    value += amount;
    return true;
}

bool Merge( TraceSessionProtocolInventory& destination,
    const TraceSessionProtocolInventory& source, std::string& error )
{
    if( !Add( destination.frameCount, source.frameCount ) ||
        !Add( destination.eventCount, source.eventCount ) ||
        !Add( destination.encodedBytes, source.encodedBytes ) ||
        !Add( destination.compressedBytes, source.compressedBytes ) )
    {
        error = "protocol_inventory_counter_overflow";
        return false;
    }
    for( size_t i = 0; i < destination.events.size(); i++ )
    {
        auto& target = destination.events[i];
        const auto& input = source.events[i];
        if( !Add( target.count, input.count ) || !Add( target.encodedBytes, input.encodedBytes ) ||
            !Add( target.variablePayloadBytes, input.variablePayloadBytes ) )
        {
            error = "protocol_inventory_counter_overflow";
            return false;
        }
    }
    for( size_t i = 0; i < destination.domains.size(); i++ )
    {
        auto& target = destination.domains[i];
        const auto& input = source.domains[i];
        if( !Add( target.count, input.count ) || !Add( target.encodedBytes, input.encodedBytes ) ||
            !Add( target.variablePayloadBytes, input.variablePayloadBytes ) )
        {
            error = "protocol_inventory_counter_overflow";
            return false;
        }
    }
    return true;
}

bool ReadLength( std::span<const uint8_t> frame, size_t offset,
    size_t lengthBytes, uint64_t& length )
{
    if( offset > frame.size() || lengthBytes > frame.size() - offset ) return false;
    if( lengthBytes == sizeof( uint16_t ) )
    {
        uint16_t value = 0;
        std::memcpy( &value, frame.data() + offset, sizeof( value ) );
        length = value;
        return true;
    }
    uint32_t value = 0;
    std::memcpy( &value, frame.data() + offset, sizeof( value ) );
    length = value;
    return true;
}

}

struct TraceSessionProtocolDecoder::Impl
{
    Impl()
        : stream( tracy::LZ4_createStreamDecode() )
        , buffer( tracy::TargetFrameSize * 3 + 1 )
    {
        if( !stream ) throw std::bad_alloc();
        tracy::LZ4_setStreamDecode( stream, nullptr, 0 );
    }

    ~Impl()
    {
        if( stream ) tracy::LZ4_freeStreamDecode( stream );
    }

    tracy::LZ4_streamDecode_t* stream = nullptr;
    std::vector<char> buffer;
    size_t bufferOffset = 0;
};

TraceSessionProtocolDecoder::TraceSessionProtocolDecoder()
    : m_impl( std::make_unique<Impl>() )
{}

TraceSessionProtocolDecoder::~TraceSessionProtocolDecoder() = default;
TraceSessionProtocolDecoder::TraceSessionProtocolDecoder( TraceSessionProtocolDecoder&& ) noexcept = default;
TraceSessionProtocolDecoder& TraceSessionProtocolDecoder::operator=( TraceSessionProtocolDecoder&& ) noexcept = default;

TraceSessionProtocolDomain ClassifyTraceProtocolEvent( uint8_t queueType )
{
    if( queueType >= uint8_t( QueueType::NUM_TYPES ) ) return TraceSessionProtocolDomain::Other;
    switch( QueueType( queueType ) )
    {
    case QueueType::FrameImage:
    case QueueType::FrameMarkMsg:
    case QueueType::FrameMarkMsgStart:
    case QueueType::FrameMarkMsgEnd:
    case QueueType::FrameVsync:
    case QueueType::JnFrame:
    case QueueType::FrameImageData:
        return TraceSessionProtocolDomain::Frame;

    case QueueType::ZoneText:
    case QueueType::ZoneName:
    case QueueType::ZoneBeginAllocSrcLoc:
    case QueueType::ZoneBeginAllocSrcLocCallstack:
    case QueueType::ZoneBegin:
    case QueueType::ZoneBeginCallstack:
    case QueueType::ZoneEnd:
    case QueueType::ZoneValidation:
    case QueueType::ZoneColor:
    case QueueType::ZoneValue:
    case QueueType::JnZoneBeginCallsite:
        return TraceSessionProtocolDomain::CpuZone;

    case QueueType::GpuZoneBegin:
    case QueueType::GpuZoneBeginCallstack:
    case QueueType::GpuZoneBeginAllocSrcLoc:
    case QueueType::GpuZoneBeginAllocSrcLocCallstack:
    case QueueType::GpuZoneEnd:
    case QueueType::GpuZoneBeginSerial:
    case QueueType::GpuZoneBeginCallstackSerial:
    case QueueType::GpuZoneBeginAllocSrcLocSerial:
    case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
    case QueueType::GpuZoneEndSerial:
    case QueueType::GpuTime:
    case QueueType::GpuContextName:
    case QueueType::GpuAnnotationName:
    case QueueType::GpuCalibration:
    case QueueType::GpuTimeSync:
    case QueueType::GpuNewContext:
    case QueueType::GpuZoneAnnotation:
    case QueueType::JnGpuZoneBeginCallsite:
        return TraceSessionProtocolDomain::GpuZone;

    case QueueType::JnJobType:
    case QueueType::JnJobSchedule:
    case QueueType::JnJobConfig:
    case QueueType::JnJobDependency:
    case QueueType::JnJobStage:
    case QueueType::JnGfxDispatch:
    case QueueType::JnGfxEntity:
    case QueueType::JnGfxLink:
        return TraceSessionProtocolDomain::Job;

    case QueueType::MemAlloc:
    case QueueType::MemAllocNamed:
    case QueueType::MemFree:
    case QueueType::MemFreeNamed:
    case QueueType::MemAllocCallstack:
    case QueueType::MemAllocCallstackNamed:
    case QueueType::MemFreeCallstack:
    case QueueType::MemFreeCallstackNamed:
    case QueueType::MemDiscard:
    case QueueType::MemDiscardCallstack:
    case QueueType::MemNamePayload:
    case QueueType::JnMemAllocCallsiteNamed:
        return TraceSessionProtocolDomain::CpuMemory;

    case QueueType::JnGpuReferenceSetDefinitionChunk:
    case QueueType::JnGpuReferenceSetUseFat:
    case QueueType::JnGpuReferencePass:
    case QueueType::JnGpuReferenceUse:
    case QueueType::JnGpuReferenceSetUse:
    case QueueType::JnGpuReferenceEnd:
    case QueueType::JnGpuReferenceSetDefinition:
        return TraceSessionProtocolDomain::GpuMemory;

    case QueueType::JnGpuCatalogBatchFat:
    case QueueType::JnGpuCatalogControl:
    case QueueType::JnGpuCatalogBatch:
    case QueueType::JnGpuCatalogBatchData:
        return TraceSessionProtocolDomain::GpuCatalog;

    case QueueType::JnIoRequest:
    case QueueType::JnIoConfig:
    case QueueType::JnIoStage:
        return TraceSessionProtocolDomain::Io;

    case QueueType::CallstackSample:
    case QueueType::CallstackSampleContextSwitch:
    case QueueType::CallstackSampleRef:
    case QueueType::CallstackSampleContextSwitchRef:
    case QueueType::HwSampleCpuCycle:
    case QueueType::HwSampleInstructionRetired:
    case QueueType::HwSampleCacheReference:
    case QueueType::HwSampleCacheMiss:
    case QueueType::HwSampleBranchRetired:
    case QueueType::HwSampleBranchMiss:
    case QueueType::CallstackSampleDictionary:
        return TraceSessionProtocolDomain::Sampling;

    case QueueType::ContextSwitch:
    case QueueType::ThreadWakeup:
    case QueueType::FiberEnter:
    case QueueType::FiberLeave:
    case QueueType::ThreadContext:
    case QueueType::SysTimeReport:
    case QueueType::SysPowerReport:
    case QueueType::TidToPid:
    case QueueType::CpuTopology:
    case QueueType::ThreadGroupHint:
        return TraceSessionProtocolDomain::Scheduling;

    case QueueType::Message:
    case QueueType::MessageColor:
    case QueueType::MessageCallstack:
    case QueueType::MessageColorCallstack:
    case QueueType::MessageAppInfo:
    case QueueType::MessageLiteral:
    case QueueType::MessageLiteralColor:
    case QueueType::MessageLiteralCallstack:
    case QueueType::MessageLiteralColorCallstack:
    case QueueType::PlotDataInt:
    case QueueType::PlotDataFloat:
    case QueueType::PlotDataDouble:
    case QueueType::PlotConfig:
    case QueueType::LockWait:
    case QueueType::LockObtain:
    case QueueType::LockRelease:
    case QueueType::LockSharedWait:
    case QueueType::LockSharedObtain:
    case QueueType::LockSharedRelease:
    case QueueType::LockName:
    case QueueType::LockAnnounce:
    case QueueType::LockTerminate:
    case QueueType::LockMark:
        return TraceSessionProtocolDomain::MessagePlotLock;

    case QueueType::CallstackSerial:
    case QueueType::Callstack:
    case QueueType::CallstackAlloc:
    case QueueType::CallstackFrameSize:
    case QueueType::SymbolInformation:
    case QueueType::ExternalNameMetadata:
    case QueueType::SymbolCodeMetadata:
    case QueueType::SourceCodeMetadata:
    case QueueType::SourceLocation:
    case QueueType::CallstackFrame:
    case QueueType::SourceLocationPayload:
    case QueueType::CallstackPayload:
    case QueueType::CallstackAllocPayload:
    case QueueType::SymbolCode:
    case QueueType::SourceCode:
    case QueueType::JnCallsiteDefinition:
        return TraceSessionProtocolDomain::SourceCallstack;

    case QueueType::JnRuntimeDomainState:
    case QueueType::JnScriptFrame:
    case QueueType::JnScriptStack:
        return TraceSessionProtocolDomain::ScriptRuntime;

    case QueueType::JnRelation:
        return TraceSessionProtocolDomain::Relation;

    case QueueType::SingleStringData:
    case QueueType::SecondStringData:
    case QueueType::StringData:
    case QueueType::ThreadName:
    case QueueType::PlotName:
    case QueueType::FrameName:
    case QueueType::ExternalName:
    case QueueType::ExternalThreadName:
    case QueueType::FiberName:
        return TraceSessionProtocolDomain::Dictionary;

    case QueueType::Terminate:
    case QueueType::KeepAlive:
    case QueueType::Crash:
    case QueueType::CrashReport:
    case QueueType::ParamSetup:
    case QueueType::AckServerQueryNoop:
    case QueueType::AckSourceCodeNotAvailable:
    case QueueType::AckSymbolCodeNotAvailable:
        return TraceSessionProtocolDomain::Control;

    default:
        return TraceSessionProtocolDomain::Other;
    }
}

const char* TraceSessionProtocolDomainName( TraceSessionProtocolDomain domain )
{
    static constexpr const char* Names[] = {
        "control", "frame", "cpu_zone", "gpu_zone", "job", "cpu_memory",
        "gpu_memory", "gpu_catalog", "io", "sampling", "scheduling",
        "message_plot_lock", "source_callstack", "script_runtime", "relation",
        "dictionary", "other"
    };
    const auto index = size_t( domain );
    return index < std::size( Names ) ? Names[index] : "invalid";
}

bool TraceSessionProtocolDecoder::ConsumeCompressedRecord( std::span<const uint8_t> record,
    TraceSessionProtocolInventory& inventory, std::string& error )
{
    error.clear();
    if( record.size() < sizeof( tracy::lz4sz_t ) )
    {
        error = "compressed_record_header_truncated";
        return false;
    }
    tracy::lz4sz_t compressedSize = 0;
    std::memcpy( &compressedSize, record.data(), sizeof( compressedSize ) );
    if( compressedSize == 0 || compressedSize > tracy::LZ4Size ||
        record.size() - sizeof( compressedSize ) != compressedSize )
    {
        error = "compressed_record_size_mismatch";
        return false;
    }

    auto* output = m_impl->buffer.data() + m_impl->bufferOffset;
    const auto decodedSize = tracy::LZ4_decompress_safe_continue( m_impl->stream,
        reinterpret_cast<const char*>( record.data() + sizeof( compressedSize ) ), output,
        int( compressedSize ), tracy::TargetFrameSize );
    if( decodedSize <= 0 )
    {
        error = "compressed_frame_decode_failed";
        return false;
    }

    TraceSessionProtocolInventory frame;
    if( !CountTraceProtocolFrame( std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>( output ), size_t( decodedSize ) ), frame, error ) )
        return false;
    frame.compressedBytes = record.size();
    if( !Merge( inventory, frame, error ) ) return false;

    m_impl->bufferOffset += size_t( decodedSize );
    if( m_impl->bufferOffset > tracy::TargetFrameSize * 2 ) m_impl->bufferOffset = 0;
    return true;
}

bool CountTraceProtocolFrame( std::span<const uint8_t> frame,
    TraceSessionProtocolInventory& inventory, std::string& error )
{
    error.clear();
    size_t offset = 0;
    while( offset < frame.size() )
    {
        const auto index = frame[offset];
        if( index >= uint8_t( QueueType::NUM_TYPES ) )
        {
            error = "protocol_queue_type_out_of_range";
            return false;
        }
        const auto type = QueueType( index );
        const auto fixedBytes = QueueDataSize[index];
        if( fixedBytes == 0 || fixedBytes > frame.size() - offset )
        {
            error = "protocol_event_exceeds_frame";
            return false;
        }

        size_t lengthBytes = 0;
        if( index >= uint8_t( QueueType::StringData ) )
            lengthBytes = IsLargePayloadEvent( type ) ? sizeof( uint32_t ) : sizeof( uint16_t );
        else if( type == QueueType::SingleStringData || type == QueueType::SecondStringData )
            lengthBytes = sizeof( uint16_t );

        uint64_t variableBytes = 0;
        if( lengthBytes != 0 && !ReadLength( frame, offset + fixedBytes, lengthBytes, variableBytes ) )
        {
            error = "protocol_event_exceeds_frame";
            return false;
        }
        if( variableBytes > std::numeric_limits<size_t>::max() - fixedBytes - lengthBytes )
        {
            error = "protocol_event_size_overflow";
            return false;
        }
        const auto eventBytes = fixedBytes + lengthBytes + size_t( variableBytes );
        if( eventBytes > frame.size() - offset )
        {
            error = "protocol_event_exceeds_frame";
            return false;
        }

        auto& stats = inventory.events[index];
        auto& domain = inventory.domains[size_t( ClassifyTraceProtocolEvent( index ) )];
        if( !Add( stats.count, 1 ) || !Add( stats.encodedBytes, eventBytes ) ||
            !Add( stats.variablePayloadBytes, variableBytes ) ||
            !Add( domain.count, 1 ) || !Add( domain.encodedBytes, eventBytes ) ||
            !Add( domain.variablePayloadBytes, variableBytes ) ||
            !Add( inventory.eventCount, 1 ) || !Add( inventory.encodedBytes, eventBytes ) )
        {
            error = "protocol_inventory_counter_overflow";
            return false;
        }
        offset += eventBytes;
    }
    if( offset != frame.size() )
    {
        error = "protocol_frame_trailing_bytes";
        return false;
    }
    if( !Add( inventory.frameCount, 1 ) )
    {
        error = "protocol_inventory_counter_overflow";
        return false;
    }
    return true;
}

}
