#include "TracyGpuAnalysisTraceSource.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionDerived.hpp"
#include "TracyTraceSessionInventory.hpp"
#include "TracyTraceSessionStore.hpp"

#include "TracyJnGpuCatalog.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"
#include "TracyStreamJournal.hpp"
#include "tracy_lz4.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr uint64_t LogicalTwoHoursNs = 2ull * 60 * 60 * 1000 * 1000 * 1000;
constexpr uint64_t ShardBoundaryNs = 30ull * 1000 * 1000 * 1000;
constexpr uint64_t DurationBoundaryCount = LogicalTwoHoursNs / ShardBoundaryNs;
constexpr uint64_t LargeFrameIdBase = uint64_t( 1 ) << 32;
constexpr uint64_t LongJobId = 9001;
constexpr uint64_t LongIoRequestId = 7001;
constexpr uint64_t LongGpuResourceId = 8001;
constexpr uint64_t LongGpuAllocationId = 8101;
constexpr uint64_t CatalogGeneration = 1;
constexpr uint32_t LongThreadId = 42;

struct Arguments
{
    std::string command;
    std::string mode;
    std::filesystem::path output;
    std::filesystem::path source;
    std::filesystem::path session;
    uint32_t factor = 1;
    uint64_t records = 0;
};

bool ParseUnsigned( std::string_view text, uint64_t& value )
{
    const auto result = std::from_chars( text.data(), text.data() + text.size(), value );
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool ParseArguments( int argc, char** argv, Arguments& args, std::string& error )
{
    if( argc < 2 )
    {
        error = "usage: tracy-trace-session-scale-corpus <generate|inspect> --mode <duration|density> ...";
        return false;
    }
    args.command = argv[1];
    for( int index = 2; index < argc; ++index )
    {
        const std::string_view option = argv[index];
        if( index + 1 >= argc )
        {
            error = "missing value for " + std::string( option );
            return false;
        }
        const std::string_view value = argv[++index];
        if( option == "--mode" ) args.mode = value;
        else if( option == "--output" ) args.output = std::filesystem::path( value );
        else if( option == "--source" ) args.source = std::filesystem::path( value );
        else if( option == "--session" ) args.session = std::filesystem::path( value );
        else if( option == "--factor" )
        {
            uint64_t parsed = 0;
            if( !ParseUnsigned( value, parsed ) || parsed == 0 || parsed > 4 )
            {
                error = "factor must be one of 1, 2, or 4";
                return false;
            }
            args.factor = uint32_t( parsed );
        }
        else if( option == "--records" )
        {
            if( !ParseUnsigned( value, args.records ) || args.records == 0 )
            {
                error = "records must be a positive integer";
                return false;
            }
        }
        else
        {
            error = "unknown option: " + std::string( option );
            return false;
        }
    }
    if( args.command != "generate" && args.command != "inspect" )
    {
        error = "command must be generate or inspect";
        return false;
    }
    if( args.mode != "duration" && args.mode != "density" &&
        !( args.command == "inspect" && args.mode == "audit" ) )
    {
        error = "mode must be duration or density";
        return false;
    }
    if( args.command == "generate" && args.output.empty() )
    {
        error = "generate requires --output";
        return false;
    }
    if( args.command == "inspect" && ( args.source.empty() || args.session.empty() ) )
    {
        error = "inspect requires --source and --session";
        return false;
    }
    if( args.mode == "density" && args.factor != 1 && args.factor != 2 && args.factor != 4 )
    {
        error = "density factor must be 1, 2, or 4";
        return false;
    }
    return true;
}

tracy::stream::FileHeader DeterministicHeader( uint32_t factor, bool duration )
{
    tracy::stream::FileHeader header;
    header.protocolVersion = 90;
    header.createdUnixNs = duration ? 1720000000000000001ull :
        1720000000000000100ull + factor;
    for( size_t index = 0; index < header.sessionId.size(); ++index )
        header.sessionId[index] = uint8_t( index + 1 + ( duration ? 0 : factor * 17 ) );
    return header;
}

void AppendQueueItem( std::vector<uint8_t>& frame, const tracy::QueueItem& item )
{
    const auto bytes = tracy::QueueDataSize[size_t( item.hdr.type )];
    const auto offset = frame.size();
    frame.resize( offset + bytes );
    std::memcpy( frame.data() + offset, &item, bytes );
}

void AppendThreadContext( std::vector<uint8_t>& frame, uint32_t thread )
{
    tracy::QueueItem item {};
    item.hdr.type = tracy::QueueType::ThreadContext;
    item.threadCtx.thread = thread;
    AppendQueueItem( frame, item );
}

void AppendString( std::vector<uint8_t>& frame, tracy::QueueType type,
    uint64_t pointer, std::string_view text )
{
    tracy::QueueItem item {};
    item.hdr.type = type;
    item.stringTransfer.ptr = pointer;
    const auto fixed = tracy::QueueDataSize[size_t( type )];
    const auto length = uint16_t( text.size() );
    const auto offset = frame.size();
    frame.resize( offset + fixed + sizeof( length ) + text.size() );
    std::memcpy( frame.data() + offset, &item, fixed );
    std::memcpy( frame.data() + offset + fixed, &length, sizeof( length ) );
    std::memcpy( frame.data() + offset + fixed + sizeof( length ), text.data(), text.size() );
}

void AppendLargePayload( std::vector<uint8_t>& frame, tracy::QueueType type,
    uint64_t payloadId, std::span<const uint8_t> payload )
{
    tracy::QueueItem item {};
    item.hdr.type = type;
    item.stringTransfer.ptr = payloadId;
    const auto fixed = tracy::QueueDataSize[size_t( type )];
    const auto bytes = uint32_t( payload.size() );
    const auto offset = frame.size();
    frame.resize( offset + fixed + sizeof( bytes ) + payload.size() );
    std::memcpy( frame.data() + offset, &item, fixed );
    std::memcpy( frame.data() + offset + fixed, &bytes, sizeof( bytes ) );
    if( !payload.empty() )
        std::memcpy( frame.data() + offset + fixed + sizeof( bytes ), payload.data(), payload.size() );
}

template<typename Record>
std::vector<uint8_t> MakeCatalogPayload( const Record& record )
{
    tracy::JnGpuCatalogBatchEnvelopeV1 envelope {};
    envelope.magic = tracy::JnGpuCatalogBatchMagic;
    envelope.catalogSchema = tracy::JnGpuCatalogSchemaVersion;
    envelope.evidenceSchema = tracy::JnGpuDetailedEvidenceSchemaVersion;
    envelope.recordBytes = sizeof( Record );
    envelope.recordCount = 1;
    envelope.payloadBytes = sizeof( Record );
    envelope.checksum = tracy::JnGpuCatalogChecksum64( &record, sizeof( record ) );
    std::vector<uint8_t> payload( sizeof( envelope ) + sizeof( record ) );
    std::memcpy( payload.data(), &envelope, sizeof( envelope ) );
    std::memcpy( payload.data() + sizeof( envelope ), &record, sizeof( record ) );
    return payload;
}

bool AppendCompressedFrame( tracy::stream::JournalWriter& writer,
    tracy::LZ4_stream_t* compressor, const std::vector<uint8_t>& frame,
    uint64_t monotonicNs, std::string& error )
{
    if( frame.empty() || frame.size() > size_t( tracy::LZ4Size ) )
    {
        error = "synthetic protocol frame has invalid size";
        return false;
    }
    std::vector<char> compressed( tracy::LZ4Size );
    // The caller reuses or destroys its frame buffer after this call. An
    // independent LZ4 block must not reference that mutable previous input.
    tracy::LZ4_resetStream_fast( compressor );
    const auto compressedBytes = tracy::LZ4_compress_fast_continue(
        compressor, reinterpret_cast<const char*>( frame.data() ), compressed.data(),
        int( frame.size() ), int( compressed.size() ), 1 );
    if( compressedBytes <= 0 )
    {
        error = "synthetic protocol frame compression failed";
        return false;
    }
    std::vector<uint8_t> record( sizeof( tracy::lz4sz_t ) + size_t( compressedBytes ) );
    const auto storedBytes = tracy::lz4sz_t( compressedBytes );
    std::memcpy( record.data(), &storedBytes, sizeof( storedBytes ) );
    std::memcpy( record.data() + sizeof( storedBytes ), compressed.data(), size_t( compressedBytes ) );
    return writer.Append( tracy::stream::RecordType::ClientToServer,
        tracy::stream::RecordFlagCompressedFrame, record, monotonicNs, error );
}

bool AppendHandshake( tracy::stream::JournalWriter& writer, std::string& error )
{
    if( !writer.Append( tracy::stream::RecordType::SessionBegin,
        tracy::stream::RecordFlagHandshake, "n30a-scale", 0, error ) ) return false;
    tracy::WelcomeMessage welcome {};
    welcome.timerMul = 1.0;
    welcome.initBegin = 0;
    welcome.resolution = 1;
    welcome.epoch = 1720000000000000000ull;
    welcome.exectime = 1719999999000000000ull;
    welcome.pid = 30001;
    welcome.samplingPeriod = 500000;
    welcome.flags = tracy::WelcomeFlag::OnDemand;
    welcome.cpuArch = tracy::CpuArchX64;
    std::memcpy( welcome.cpuManufacturer, "N30A Synthetic", 14 );
    std::memcpy( welcome.programName, "N30A Scale Corpus", 17 );
    std::memcpy( welcome.hostInfo, "N30A", 5 );
    if( !writer.Append( tracy::stream::RecordType::ClientToServer,
        tracy::stream::RecordFlagHandshake,
        std::span<const uint8_t>( reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) ),
        0, error ) ) return false;
    tracy::OnDemandPayloadMessage onDemand {};
    onDemand.frames = 0;
    onDemand.currentTime = 0;
    return writer.Append( tracy::stream::RecordType::ClientToServer,
        tracy::stream::RecordFlagHandshake,
        std::span<const uint8_t>( reinterpret_cast<const uint8_t*>( &onDemand ), sizeof( onDemand ) ),
        0, error );
}

bool AppendTerminal( tracy::stream::JournalWriter& writer, uint64_t monotonicNs,
    std::string& error )
{
    std::array<uint8_t, 24> payload {};
    const uint16_t schema = 1;
    const uint16_t bytes = uint16_t( payload.size() );
    // Protocol close reason 1 is CaptureComplete. Keep the synthetic writer
    // independent from the capture-only observer implementation header.
    const uint32_t reason = 1;
    std::memcpy( payload.data(), &schema, sizeof( schema ) );
    std::memcpy( payload.data() + 2, &bytes, sizeof( bytes ) );
    std::memcpy( payload.data() + 4, &reason, sizeof( reason ) );
    return writer.Append( tracy::stream::RecordType::SessionEnd,
        tracy::stream::RecordFlagTerminal, payload, monotonicNs, error );
}

void AppendJnFramePair( std::vector<uint8_t>& frame, int64_t time, uint64_t frameId )
{
    tracy::QueueItem item {};
    item.hdr.type = tracy::QueueType::JnFrame;
    item.jnFrame = { time, frameId, frameId,
        uint8_t( tracy::JnFrameDomain::Player ), uint8_t( tracy::JnFramePhase::Begin ),
        uint8_t( tracy::JnFrameFlags::Canonical ) };
    AppendQueueItem( frame, item );
    item.jnFrame.phase = uint8_t( tracy::JnFramePhase::End );
    AppendQueueItem( frame, item );
}

void AppendCatalogBatch( std::vector<uint8_t>& frame, uint64_t payloadId,
    uint32_t sequence, tracy::JnGpuCatalogBatchKind kind,
    const std::vector<uint8_t>& payload )
{
    AppendLargePayload( frame, tracy::QueueType::JnGpuCatalogBatchData, payloadId, payload );
    tracy::QueueItem item {};
    item.hdr.type = tracy::QueueType::JnGpuCatalogBatch;
    item.jnGpuCatalogBatch = { CatalogGeneration, payloadId, sequence, 1,
        uint32_t( payload.size() ), uint8_t( kind ),
        uint8_t( tracy::JnGpuCatalogBatchEncoding::FixedV1 ), 0 };
    AppendQueueItem( frame, item );
}

void AppendDurationStart( std::vector<uint8_t>& frame )
{
    AppendThreadContext( frame, LongThreadId );
    tracy::QueueItem item {};
    item.hdr.type = tracy::QueueType::ZoneValidation;
    item.zoneValidation.id = 100;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneBegin;
    item.zoneBegin = { 1, 0x1000 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::SourceLocation;
    item.srcloc = { 0x1100, 0x1200, 0x1300, 77, 0x22, 0x44, 0x66 };
    AppendQueueItem( frame, item );
    AppendString( frame, tracy::QueueType::StringData, 0x1100, "N30A.LongZone" );
    AppendString( frame, tracy::QueueType::StringData, 0x1200, "LongZoneFunction" );
    AppendString( frame, tracy::QueueType::StringData, 0x1300, "TraceSessionScaleCorpus.cpp" );

    item = {};
    item.hdr.type = tracy::QueueType::JnJobType;
    item.jnJobType = { 0x2000, 31, 0, 0 };
    AppendQueueItem( frame, item );
    AppendString( frame, tracy::QueueType::StringData, 0x2000, "N30A.LongJob" );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobSchedule;
    item.jnJobSchedule = { 1, LongJobId, 0x100000001ull, 0, 0, uint8_t( 1 << 6 ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobConfig;
    item.jnJobConfig = { LongJobId, 31, 1, 1, 0, 0, 0, uint8_t( 1 << 6 ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobStage;
    item.jnJobStage = { 2, LongJobId, 1, 0, 0, uint8_t( tracy::JnJobStage::Ready ), 0 };
    AppendQueueItem( frame, item );
    item.jnJobStage = { 3, LongJobId, 2, 0, 1,
        uint8_t( tracy::JnJobStage::WorkerSliceBegin ), 0 };
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::MemAlloc;
    item.memAlloc.time = 1;
    item.memAlloc.thread = LongThreadId;
    item.memAlloc.ptr = 0xDEADBEEF;
    const uint64_t allocationBytes = 4096;
    std::memcpy( item.memAlloc.size, &allocationBytes, sizeof( item.memAlloc.size ) );
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::JnIoRequest;
    item.jnIoRequest = { 1, LongIoRequestId, LongGpuResourceId,
        uint8_t( tracy::JnIoOperation::Read ), uint8_t( tracy::JnIoSource::JnfsNative ),
        1, 1, uint8_t( tracy::JnIoFlags::Async ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnIoConfig;
    item.jnIoConfig = { LongIoRequestId, 0, 4096, 0,
        uint8_t( tracy::JnIoParentKind::None ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnIoStage;
    item.jnIoStage = { 2, LongIoRequestId, 0, 0,
        uint8_t( tracy::JnIoStage::Start ), uint8_t( tracy::JnIoStatus::Unknown ), 0 };
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::JnGpuCatalogControl;
    item.jnGpuCatalogControl = { 1, CatalogGeneration, 0, 1,
        uint8_t( tracy::JnGpuCatalogControlKind::GenerationBegin ),
        uint8_t( tracy::JnGpuCatalogGenerationState::Building ), 0 };
    AppendQueueItem( frame, item );
    tracy::JnGpuCatalogAllocationRecordV1 allocation {};
    allocation.time = 1;
    allocation.allocationId = LongGpuAllocationId;
    allocation.pointerToken = 0xD00D;
    allocation.sizeBytes = 1024 * 1024;
    allocation.residentBytes = allocation.sizeBytes;
    allocation.primaryKind = uint16_t( tracy::JnGpuCatalogPrimaryKind::StructuredBuffer );
    allocation.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    allocation.memoryDomain = uint8_t( tracy::JnGpuCatalogMemoryDomain::Local );
    allocation.allocationKind = uint8_t( tracy::JnGpuCatalogAllocationKind::Committed );
    allocation.exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    AppendCatalogBatch( frame, 0x3000, 2, tracy::JnGpuCatalogBatchKind::Allocation,
        MakeCatalogPayload( allocation ) );
    tracy::JnGpuCatalogResourceRecordV1 resource {};
    resource.time = 1;
    resource.resourceId = LongGpuResourceId;
    resource.pointerToken = 0xCAFE;
    resource.familyId = LongGpuResourceId;
    resource.allocationId = LongGpuAllocationId;
    resource.capacityBytes = allocation.sizeBytes;
    resource.width = allocation.sizeBytes;
    resource.height = 1;
    resource.depthOrArraySize = 1;
    resource.mipLevels = 1;
    resource.primaryKind = uint16_t( tracy::JnGpuCatalogPrimaryKind::StructuredBuffer );
    resource.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    resource.resourceClass = uint8_t( tracy::JnGpuCatalogResourceClass::Buffer );
    resource.dimension = uint8_t( tracy::JnGpuCatalogDimension::Buffer );
    resource.memoryDomain = uint8_t( tracy::JnGpuCatalogMemoryDomain::Local );
    resource.allocationKind = uint8_t( tracy::JnGpuCatalogAllocationKind::Committed );
    resource.exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    AppendCatalogBatch( frame, 0x3001, 3, tracy::JnGpuCatalogBatchKind::Resource,
        MakeCatalogPayload( resource ) );
    AppendJnFramePair( frame, 0, LargeFrameIdBase );
}

void AppendDurationEnd( std::vector<uint8_t>& frame )
{
    tracy::QueueItem item {};
    item.hdr.type = tracy::QueueType::ZoneValidation;
    item.zoneValidation.id = 100;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneEnd;
    item.zoneEnd.time = int64_t( LogicalTwoHoursNs - 1 );
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::JnJobStage;
    item.jnJobStage = { int64_t( LogicalTwoHoursNs - 1 ), LongJobId, 2, 0, 1,
        uint8_t( tracy::JnJobStage::WorkerSliceEnd ), 0 };
    AppendQueueItem( frame, item );
    item.jnJobStage = { int64_t( LogicalTwoHoursNs ), LongJobId, 3, 0, 0,
        uint8_t( tracy::JnJobStage::Completed ), 0 };
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::MemFree;
    item.memFree = { int64_t( LogicalTwoHoursNs - 1 ), LongThreadId, 0xDEADBEEF };
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::JnIoStage;
    item.jnIoStage = { int64_t( LogicalTwoHoursNs ), LongIoRequestId, 4096, 0,
        uint8_t( tracy::JnIoStage::Complete ), uint8_t( tracy::JnIoStatus::Success ), 0 };
    AppendQueueItem( frame, item );

    tracy::JnGpuCatalogResourceRecordV1 resource {};
    resource.time = int64_t( LogicalTwoHoursNs );
    resource.resourceId = LongGpuResourceId;
    resource.pointerToken = 0xCAFE;
    resource.familyId = LongGpuResourceId;
    resource.allocationId = LongGpuAllocationId;
    resource.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Destroy );
    resource.resourceClass = uint8_t( tracy::JnGpuCatalogResourceClass::Buffer );
    resource.dimension = uint8_t( tracy::JnGpuCatalogDimension::Buffer );
    resource.memoryDomain = uint8_t( tracy::JnGpuCatalogMemoryDomain::Local );
    resource.allocationKind = uint8_t( tracy::JnGpuCatalogAllocationKind::Committed );
    resource.exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    AppendCatalogBatch( frame, 0x3002, 4, tracy::JnGpuCatalogBatchKind::Resource,
        MakeCatalogPayload( resource ) );
    tracy::JnGpuCatalogAllocationRecordV1 allocation {};
    allocation.time = int64_t( LogicalTwoHoursNs );
    allocation.allocationId = LongGpuAllocationId;
    allocation.pointerToken = 0xD00D;
    allocation.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Destroy );
    allocation.memoryDomain = uint8_t( tracy::JnGpuCatalogMemoryDomain::Local );
    allocation.allocationKind = uint8_t( tracy::JnGpuCatalogAllocationKind::Committed );
    allocation.exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    AppendCatalogBatch( frame, 0x3003, 5, tracy::JnGpuCatalogBatchKind::Allocation,
        MakeCatalogPayload( allocation ) );
    item = {};
    item.hdr.type = tracy::QueueType::JnGpuCatalogControl;
    item.jnGpuCatalogControl = { int64_t( LogicalTwoHoursNs ), CatalogGeneration, 0, 6,
        uint8_t( tracy::JnGpuCatalogControlKind::GenerationEnd ),
        uint8_t( tracy::JnGpuCatalogGenerationState::Complete ), 0 };
    AppendQueueItem( frame, item );
}

bool GenerateDuration( const Arguments& args, std::string& error )
{
    tracy::stream::WriterOptions options;
    options.durableHeader = false;
    auto writer = tracy::stream::JournalWriter::CreateFileJournal(
        args.output, DeterministicHeader( 1, true ), false, options, error );
    if( !writer || !AppendHandshake( *writer, error ) ) return false;
    std::unique_ptr<tracy::LZ4_stream_t, decltype( &tracy::LZ4_freeStream )>
        compressor( tracy::LZ4_createStream(), tracy::LZ4_freeStream );
    if( !compressor ) { error = "cannot create LZ4 stream"; return false; }
    std::vector<uint8_t> frame;
    AppendDurationStart( frame );
    if( !AppendCompressedFrame( *writer, compressor.get(), frame, 0, error ) ) return false;
    uint64_t maximumFrameId = LargeFrameIdBase;
    for( uint64_t boundary = 1; boundary <= DurationBoundaryCount; ++boundary )
    {
        const auto time = boundary * ShardBoundaryNs;
        frame.clear();
        maximumFrameId = LargeFrameIdBase + boundary * 2 - 1;
        AppendJnFramePair( frame, int64_t( time ), maximumFrameId );
        if( !AppendCompressedFrame( *writer, compressor.get(), frame, time, error ) ) return false;
        frame.clear();
        maximumFrameId = LargeFrameIdBase + boundary * 2;
        AppendJnFramePair( frame, int64_t( time ), maximumFrameId );
        if( boundary == DurationBoundaryCount ) AppendDurationEnd( frame );
        if( !AppendCompressedFrame( *writer, compressor.get(), frame, time, error ) ) return false;
    }
    if( !AppendTerminal( *writer, LogicalTwoHoursNs, error ) ) return false;
    std::cout << "{\"schema\":\"1\",\"mode\":\"duration\","
        "\"logical_duration_ns\":\"" << LogicalTwoHoursNs << "\","
        "\"maximum_frame_id\":\"" << maximumFrameId << "\","
        "\"payload_records\":\"" << ( DurationBoundaryCount * 2 + 1 ) << "\"}" << std::endl;
    return true;
}

bool GenerateDensity( const Arguments& args, std::string& error )
{
    constexpr uint64_t DefaultBaseRecords = 100000;
    const auto payloadRecords = args.records != 0 ? args.records : DefaultBaseRecords * args.factor;
    tracy::stream::WriterOptions options;
    options.durableHeader = false;
    auto writer = tracy::stream::JournalWriter::CreateFileJournal(
        args.output, DeterministicHeader( args.factor, false ), false, options, error );
    if( !writer || !AppendHandshake( *writer, error ) ) return false;
    std::unique_ptr<tracy::LZ4_stream_t, decltype( &tracy::LZ4_freeStream )>
        compressor( tracy::LZ4_createStream(), tracy::LZ4_freeStream );
    if( !compressor ) { error = "cannot create LZ4 stream"; return false; }
    constexpr uint64_t PairsPerProtocolFrame = 256;
    uint64_t produced = 0;
    while( produced < payloadRecords )
    {
        const auto count = std::min<uint64_t>( PairsPerProtocolFrame, payloadRecords - produced );
        std::vector<uint8_t> frame;
        frame.reserve( size_t( count ) * 64 );
        for( uint64_t index = 0; index < count; ++index )
        {
            const auto ordinal = produced + index;
            AppendJnFramePair( frame, int64_t( ( ordinal + 1 ) * 100000 ), LargeFrameIdBase + ordinal );
        }
        produced += count;
        const auto time = produced * 100000;
        if( !AppendCompressedFrame( *writer, compressor.get(), frame, time, error ) ) return false;
    }
    if( !AppendTerminal( *writer, payloadRecords * 100000, error ) ) return false;
    std::cout << "{\"schema\":\"1\",\"mode\":\"density\","
        "\"factor\":\"" << args.factor << "\","
        "\"payload_records\":\"" << payloadRecords << "\"}" << std::endl;
    return true;
}

struct TypeLocation
{
    uint64_t firstShard = std::numeric_limits<uint64_t>::max();
    uint64_t lastShard = 0;
    uint64_t count = 0;
};

struct TypeScan
{
    uint64_t shardId = 0;
    std::array<TypeLocation, size_t( tracy::QueueType::NUM_TYPES )> locations {};
};

bool RecordTypeLocation( const tracy::analysis::TraceSessionCanonicalRecord& record,
    void* userData, std::string& )
{
    if( record.kind != tracy::analysis::TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<TypeScan*>( userData );
    auto& value = state.locations[record.type];
    value.firstShard = std::min( value.firstShard, state.shardId );
    value.lastShard = std::max( value.lastShard, state.shardId );
    value.count++;
    return true;
}

bool CrossShard( const TypeScan& scan, tracy::QueueType begin, tracy::QueueType end )
{
    const auto& first = scan.locations[size_t( begin )];
    const auto& last = scan.locations[size_t( end )];
    return first.count != 0 && last.count != 0 && first.firstShard != last.lastShard;
}

uint64_t DirectoryBytes( const std::filesystem::path& path )
{
    uint64_t total = 0;
    std::error_code ec;
    for( std::filesystem::recursive_directory_iterator iterator(
        path, std::filesystem::directory_options::skip_permission_denied, ec ), end;
        iterator != end && !ec; iterator.increment( ec ) )
    {
        if( !iterator->is_regular_file( ec ) || ec ) { ec.clear(); continue; }
        const auto bytes = iterator->file_size( ec );
        if( ec ) { ec.clear(); continue; }
        total += bytes;
    }
    return total;
}

bool Inspect( const Arguments& args, std::string& error )
{
    const auto manifestValue = tracy::analysis::LoadTraceSessionManifest( args.session, error );
    if( !manifestValue ) return false;
    const auto& manifest = *manifestValue;
    tracy::analysis::TraceSessionInventory inventory;
    tracy::analysis::TraceSessionInventoryOptions inventoryOptions;
    std::cerr << "[Audit.Inventory] start" << std::endl;
    if( !tracy::analysis::BuildTraceSessionInventory(
        args.source, inventoryOptions, inventory, error ) ) return false;
    tracy::analysis::TraceSessionCanonicalAudit canonicalAudit;
    if( args.mode != "audit" && !tracy::analysis::AuditTraceSessionCanonical(
        args.session, manifest, inventory, canonicalAudit, error ) ) return false;
    tracy::analysis::TraceSessionDerivedStats derivedStats;
    std::cerr << "[Audit.CanonicalAndDerived] start" << std::endl;
    const bool finalAudit = tracy::analysis::AuditTraceSessionFinal(
        args.session, manifest, inventory, derivedStats, error );
    if( !finalAudit ) return false;
    if( args.mode == "audit" ) canonicalAudit.protocolEvents = derivedStats.indexedProtocolEvents;
    std::cerr << "[Audit.CanonicalAndDerived] complete" << std::endl;
    uint64_t dataShards = 0;
    int64_t beginNs = std::numeric_limits<int64_t>::max();
    int64_t endNs = std::numeric_limits<int64_t>::min();
    TypeScan typeScan;
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        dataShards++;
        beginNs = std::min( beginNs, shard.timeBeginNs );
        endNs = std::max( endNs, shard.timeEndNs );
        typeScan.shardId = shard.shardId;
        if( args.mode != "audit" && !tracy::analysis::VisitTraceSessionCanonicalShard(
            args.session, shard, RecordTypeLocation, &typeScan, error ) ) return false;
    }
    const auto logicalDuration = beginNs <= endNs ? uint64_t( endNs - beginNs ) : 0;
    bool zoneCrossShard = false;
    bool jobCrossShard = false;
    bool allocationCrossShard = false;
    bool resourceCrossShard = false;
    bool ioCrossShard = false;
    uint64_t maximumFrameId = 0;
    if( args.mode == "duration" )
    {
        std::string openError;
        auto source = tracy::analysis::GpuAnalysisTraceSource::OpenSessionIfReady(
            args.session, {}, &openError );
        if( !source ) { error = "cannot open Session TraceSource: " + openError; return false; }
        const auto frames = source->GetCorrelatedFrameEvents();
        for( const auto& frame : frames ) maximumFrameId = std::max( maximumFrameId, frame.frameId );
        const auto zones = source->ScanCpuZones( {} );
        const auto zoneComplete = std::any_of( zones.begin(), zones.end(), []( const auto& zone ) {
            return zone.complete && zone.endNs.has_value() &&
                *zone.endNs - zone.startNs >= int64_t( LogicalTwoHoursNs - 2 );
        } );
        const auto job = source->GetJob( LongJobId );
        const auto jobComplete = job && !job->incomplete && job->completedNs.has_value();
        const auto memory = source->ScanMemoryEvents( {} );
        const auto memoryComplete = std::any_of( memory.begin(), memory.end(), []( const auto& value ) {
            return value.complete && value.freeNs.has_value() && value.size == 4096;
        } );
        const auto io = source->GetIoRequest( LongIoRequestId );
        const auto ioComplete = io && !io->truncated && io->endNs.has_value() &&
            io->transferredBytes == 4096;
        const auto resource = source->StoreReader()->FindResource( LongGpuResourceId, error );
        if( !resource )
        {
            if( error.empty() ) error = "duration GPU resource is missing";
            return false;
        }
        const auto resourceComplete = !resource->aliveAtEnd && resource->destroyTime > resource->createTime;
        zoneCrossShard = zoneComplete && CrossShard(
            typeScan, tracy::QueueType::ZoneBegin, tracy::QueueType::ZoneEnd );
        jobCrossShard = jobComplete && CrossShard(
            typeScan, tracy::QueueType::JnJobSchedule, tracy::QueueType::JnJobStage );
        allocationCrossShard = memoryComplete && CrossShard(
            typeScan, tracy::QueueType::MemAlloc, tracy::QueueType::MemFree );
        resourceCrossShard = resourceComplete &&
            typeScan.locations[size_t( tracy::QueueType::JnGpuCatalogBatch )].count >= 4 &&
            typeScan.locations[size_t( tracy::QueueType::JnGpuCatalogBatch )].firstShard !=
                typeScan.locations[size_t( tracy::QueueType::JnGpuCatalogBatch )].lastShard;
        ioCrossShard = ioComplete && CrossShard(
            typeScan, tracy::QueueType::JnIoRequest, tracy::QueueType::JnIoStage );
    }
    const auto coreGaps = derivedStats.gpuSourceGapResources + derivedStats.gpuSourceGapReferences;
    std::cout << "{\"schema\":\"1\",\"mode\":\"" << args.mode << "\","
        "\"factor\":\"" << args.factor << "\","
        "\"logical_duration_ns\":\"" << logicalDuration << "\","
        "\"maximum_frame_id\":\"" << maximumFrameId << "\","
        "\"data_shards\":\"" << dataShards << "\","
        "\"source_protocol_events\":\"" << inventory.protocolInventory.eventCount << "\","
        "\"canonical_protocol_events\":\"" << canonicalAudit.protocolEvents << "\","
        "\"payload_records\":\"0\","
        "\"session_bytes\":\"" << DirectoryBytes( args.session ) << "\","
        "\"core_build_elapsed_ns\":\"0\","
        "\"peak_private_bytes\":\"" << derivedStats.peakPrivateBytes << "\","
        "\"core_gap_count\":\"" << coreGaps << "\","
        "\"final_audit_complete\":" << ( manifest.auditComplete && finalAudit ? "true" : "false" ) << ','
        << "\"zone_cross_shard\":" << ( zoneCrossShard ? "true" : "false" ) << ','
        << "\"job_cross_shard\":" << ( jobCrossShard ? "true" : "false" ) << ','
        << "\"allocation_cross_shard\":" << ( allocationCrossShard ? "true" : "false" ) << ','
        << "\"resource_cross_shard\":" << ( resourceCrossShard ? "true" : "false" ) << ','
        << "\"io_cross_shard\":" << ( ioCrossShard ? "true" : "false" ) << "}\n";
    return true;
}

}

int main( int argc, char** argv )
{
    Arguments arguments;
    std::string error;
    if( !ParseArguments( argc, argv, arguments, error ) )
    {
        std::cerr << error << '\n';
        return 2;
    }
    bool ok = false;
    if( arguments.command == "generate" )
        ok = arguments.mode == "duration" ? GenerateDuration( arguments, error ) :
            GenerateDensity( arguments, error );
    else ok = Inspect( arguments, error );
    if( !ok )
    {
        std::cerr << "N30A scale corpus " << arguments.command << " failed: " << error << '\n';
        return 1;
    }
    return 0;
}
