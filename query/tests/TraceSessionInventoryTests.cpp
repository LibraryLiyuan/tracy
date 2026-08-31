#include "TracyTraceSessionInventory.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyTraceSessionDerived.hpp"
#include "TracyGpuAnalysisStore.hpp"
#include "TracyGpuAnalysisTraceSource.hpp"
#include "TracyTraceSessionMemory.hpp"
#include "TracyTraceSessionSampling.hpp"
#include "TracyTraceSessionScheduling.hpp"
#include "TracyTraceSessionGpuZones.hpp"
#include "TracyQueryService.hpp"
#include "TracyTraceSessionProtocolInventory.hpp"

#include "TracyStreamJournal.hpp"
#include "TracyProtocolObserver.hpp"
#include "TracyQueue.hpp"
#include "TracyProtocol.hpp"
#include "tracy_lz4.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace
{

struct TestContext
{
    void Check( bool condition, const std::string& message )
    {
        if( condition ) return;
        std::cerr << "FAIL: " << message << '\n';
        failed++;
    }

    int failed = 0;
};

struct ProgressState
{
    std::array<uint64_t, 2> completed {};
    std::array<uint64_t, 2> total {};
    bool monotonic = true;
};

struct CanonicalCancelState
{
    uint64_t checks = 0;
    uint64_t cancelAfterChecks = 0;
};

struct CanonicalReadState
{
    uint64_t records = 0;
    std::array<uint64_t, size_t( tracy::analysis::TraceSessionProtocolDomain::Count )> domains {};
    uint32_t frameThreadContext = 0;
    uint32_t jobThreadContext = 0;
    int64_t frameSemanticTime = 0;
    std::vector<std::tuple<uint64_t, uint64_t, uint32_t, uint8_t>> protocolOrder;
};

struct CanonicalOrderedReadState
{
    uint64_t records = 0;
    bool monotonic = true;
    bool havePrevious = false;
    std::tuple<uint64_t, uint64_t, uint32_t> previous {};
    std::vector<uint8_t> protocolTypes;
};

bool CountCanonicalRecord( const tracy::analysis::TraceSessionCanonicalRecord& record,
    void* userData, std::string& )
{
    auto& state = *static_cast<CanonicalReadState*>( userData );
    state.records++;
    if( record.kind == tracy::analysis::TraceSessionCanonicalRecordKind::ProtocolEvent )
    {
        state.domains[size_t( record.domain )]++;
        state.protocolOrder.emplace_back( record.sourceSequence,
            record.protocolFrameOrdinal, record.protocolFrameOffset, record.type );
    }
    if( record.type == uint8_t( tracy::QueueType::FrameVsync ) )
    {
        state.frameThreadContext = record.threadContext;
        if( record.hasSemanticTime ) state.frameSemanticTime = record.semanticTime;
    }
    else if( record.type == uint8_t( tracy::QueueType::JnJobSchedule ) )
    {
        state.jobThreadContext = record.threadContext;
    }
    return true;
}

bool CountCanonicalRecordOrdered( const tracy::analysis::TraceSessionCanonicalRecord& record,
    void* userData, std::string& )
{
    auto& state = *static_cast<CanonicalOrderedReadState*>( userData );
    const auto key = std::make_tuple( record.sourceSequence,
        record.protocolFrameOrdinal, record.protocolFrameOffset );
    if( state.havePrevious && key < state.previous ) state.monotonic = false;
    state.previous = key;
    state.havePrevious = true;
    state.records++;
    if( record.kind == tracy::analysis::TraceSessionCanonicalRecordKind::ProtocolEvent )
        state.protocolTypes.push_back( record.type );
    return true;
}

bool RequestCanonicalCancel( void* userData )
{
    auto& state = *static_cast<CanonicalCancelState*>( userData );
    state.checks++;
    return state.cancelAfterChecks != 0 && state.checks >= state.cancelAfterChecks;
}

struct DiskProbeState
{
    uint32_t calls = 0;
};

bool ExhaustDiskAfterFirstShard( const std::filesystem::path&, uint64_t& capacity,
    uint64_t& available, void* userData, std::string& )
{
    auto& state = *static_cast<DiskProbeState*>( userData );
    state.calls++;
    capacity = 1024ull * 1024 * 1024;
    available = state.calls == 1 ? capacity : 1;
    return true;
}

void RecordProgress( tracy::analysis::TraceSessionInventoryPhase phase,
    uint64_t completed, uint64_t total, void* userData )
{
    auto& state = *static_cast<ProgressState*>( userData );
    const auto index = size_t( phase );
    if( completed < state.completed[index] || completed > total ) state.monotonic = false;
    state.completed[index] = completed;
    state.total[index] = total;
}

std::filesystem::path UniqueTestDirectory()
{
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ( "tracy-session-inventory-tests-" + std::to_string( ticks ) );
}

tracy::stream::FileHeader DeterministicHeader()
{
    tracy::stream::FileHeader header;
    header.protocolVersion = 90;
    header.createdUnixNs = 123456789;
    for( size_t i = 0; i < header.sessionId.size(); i++ ) header.sessionId[i] = uint8_t( i + 1 );
    return header;
}

bool AppendBytes( tracy::stream::JournalWriter& writer, tracy::stream::RecordType type,
    uint32_t flags, size_t bytes, uint64_t timestamp, std::string& error )
{
    std::vector<uint8_t> payload( bytes );
    for( size_t i = 0; i < bytes; i++ ) payload[i] = uint8_t( i * 17 + timestamp );
    return writer.Append( type, flags, payload, timestamp, error );
}

bool CreateJournal( const std::filesystem::path& path, size_t clientRecords,
    bool terminal, std::string& error,
    tracy::ProtocolCloseReason closeReason = tracy::ProtocolCloseReason::CaptureComplete )
{
    tracy::stream::WriterOptions options;
    options.durableHeader = false;
    auto writer = tracy::stream::JournalWriter::CreateFileJournal(
        path, DeterministicHeader(), false, options, error );
    if( !writer ) return false;
    if( !AppendBytes( *writer, tracy::stream::RecordType::SessionBegin,
        tracy::stream::RecordFlagHandshake, 24, 0, error ) ) return false;
    for( size_t i = 0; i < clientRecords; i++ )
    {
        if( !AppendBytes( *writer, tracy::stream::RecordType::ClientToServer,
            tracy::stream::RecordFlagNone, 100 + i % 7, i + 1, error ) ) return false;
    }
    if( !AppendBytes( *writer, tracy::stream::RecordType::ServerToClient,
        tracy::stream::RecordFlagServerQuery, 40, clientRecords + 1, error ) ) return false;
    if( !AppendBytes( *writer, tracy::stream::RecordType::Checkpoint,
        tracy::stream::RecordFlagDurabilityBoundary, 16, clientRecords + 2, error ) ) return false;
    if( terminal )
    {
        std::array<uint8_t, 32> payload {};
        const uint16_t schema = 1;
        const uint16_t size = uint16_t( payload.size() );
        const uint32_t reason = uint32_t( closeReason );
        std::memcpy( payload.data(), &schema, sizeof( schema ) );
        std::memcpy( payload.data() + 2, &size, sizeof( size ) );
        std::memcpy( payload.data() + 4, &reason, sizeof( reason ) );
        if( !writer->Append( tracy::stream::RecordType::SessionEnd,
            tracy::stream::RecordFlagTerminal, payload, clientRecords + 3, error ) ) return false;
    }
    return true;
}

void TestCompleteInventory( TestContext& test, const std::filesystem::path& directory )
{
    const auto path = directory / "complete.tracy-stream";
    std::string error;
    test.Check( CreateJournal( path, 3, true, error ), "create complete journal: " + error );

    tracy::analysis::TraceSessionInventory inventory;
    tracy::analysis::TraceSessionInventoryOptions options;
    ProgressState progress;
    options.progress = RecordProgress;
    options.progressUserData = &progress;
    test.Check( tracy::analysis::BuildTraceSessionInventory( path, options, inventory, error ),
        "build complete inventory: " + error );
    test.Check( inventory.protocol == 90, "inventory preserves protocol" );
    test.Check( inventory.captureEndMetadataPresent, "inventory parses capture end metadata" );
    test.Check( inventory.captureEndReason == uint32_t( tracy::ProtocolCloseReason::CaptureComplete ),
        "inventory preserves capture close reason" );
    test.Check( inventory.complete, "terminal journal is complete" );
    test.Check( !inventory.sourceDegraded, "clean source is not degraded" );
    test.Check( inventory.recordCount == 7, "all committed journal records are counted" );
    test.Check( inventory.committedRevision == 7, "committed revision is the final sequence" );
    test.Check( inventory.validSize == inventory.sourceFileSize, "clean journal valid size equals file size" );
    test.Check( inventory.sourceSha256.size() == 64, "source receives a full SHA-256 identity" );
    test.Check( inventory.retainedRecordMetadata == 0, "inventory retains no per-record metadata in memory" );
    test.Check( progress.monotonic, "inventory progress is monotonic" );
    test.Check( progress.completed[size_t( tracy::analysis::TraceSessionInventoryPhase::JournalScan )] == inventory.sourceFileSize,
        "journal scan progress reaches source size" );
    test.Check( progress.completed[size_t( tracy::analysis::TraceSessionInventoryPhase::SourceHash )] == inventory.sourceFileSize,
        "source hash progress reaches source size" );

    const auto& client = inventory.records[tracy::analysis::TraceSessionJournalClass::ClientToServer];
    const auto& server = inventory.records[tracy::analysis::TraceSessionJournalClass::ServerToClient];
    test.Check( client.count == 3, "client record count" );
    test.Check( client.payloadBytes == 303, "client payload bytes" );
    test.Check( server.count == 1 && server.payloadBytes == 40, "server count and bytes" );
    test.Check( inventory.estimatedCanonicalBytes >= inventory.validSize,
        "canonical estimate is conservative" );
    test.Check( inventory.estimatedTotalBuildBytes >= inventory.estimatedCanonicalBytes,
        "total estimate includes canonical" );

    const auto inventoryPath = directory / "inventory" / "inventory-v1";
    test.Check( tracy::analysis::SaveTraceSessionInventory( inventoryPath, inventory, error ),
        "save inventory: " + error );
    const auto loaded = tracy::analysis::LoadTraceSessionInventory( inventoryPath, error );
    test.Check( loaded.has_value(), "load inventory: " + error );
    if( loaded )
    {
        test.Check( loaded->sourceSha256 == inventory.sourceSha256, "inventory identity round-trip" );
        test.Check( loaded->records == inventory.records, "inventory counters round-trip" );
        test.Check( loaded->captureEndMetadataPresent == inventory.captureEndMetadataPresent &&
            loaded->captureEndReason == inventory.captureEndReason,
            "capture end quality round-trip" );
    }

    CanonicalCancelState cancel { 0, 1 };
    tracy::analysis::TraceSessionInventory cancelled;
    auto cancelOptions = options;
    cancelOptions.progress = nullptr;
    cancelOptions.progressUserData = nullptr;
    cancelOptions.shouldCancel = RequestCanonicalCancel;
    cancelOptions.cancelUserData = &cancel;
    test.Check( !tracy::analysis::BuildTraceSessionInventory(
        path, cancelOptions, cancelled, error ) && error == "cancelled_safe_restart",
        "Inventory Ctrl+C stops only after a committed record and never publishes a partial inventory" );
}

void TestDegradedCaptureEnd( TestContext& test, const std::filesystem::path& directory )
{
    const auto path = directory / "protocol-mismatch.tracy-stream";
    std::string error;
    test.Check( CreateJournal( path, 1, true, error, tracy::ProtocolCloseReason::ProtocolMismatch ),
        "create degraded terminal journal: " + error );
    tracy::analysis::TraceSessionInventory inventory;
    test.Check( tracy::analysis::BuildTraceSessionInventory(
        path, tracy::analysis::TraceSessionInventoryOptions {}, inventory, error ),
        "inventory degraded terminal journal: " + error );
    test.Check( inventory.complete, "degraded source still preserves committed terminal revision" );
    test.Check( inventory.sourceDegraded, "protocol mismatch marks source degraded" );
    test.Check( inventory.qualityReason == "protocol_mismatch", "close reason maps to stable quality reason" );
}

void TestRecoverableTail( TestContext& test, const std::filesystem::path& directory )
{
    const auto path = directory / "truncated.tracy-stream";
    std::string error;
    test.Check( CreateJournal( path, 2, false, error ), "create incomplete journal: " + error );
    const auto committedSize = std::filesystem::file_size( path );
    {
        std::ofstream output( path, std::ios::binary | std::ios::app );
        output.write( "partial", 7 );
    }

    tracy::analysis::TraceSessionInventory inventory;
    test.Check( tracy::analysis::BuildTraceSessionInventory(
        path, tracy::analysis::TraceSessionInventoryOptions {}, inventory, error ),
        "recover committed prefix: " + error );
    test.Check( !inventory.complete, "source without SessionEnd is incomplete" );
    test.Check( inventory.sourceDegraded, "trailing partial bytes degrade the source" );
    test.Check( inventory.qualityReason == "truncated_tail", "truncated tail reason is explicit" );
    test.Check( inventory.validSize == committedSize, "invalid tail is excluded from committed prefix" );
    test.Check( inventory.sourceFileSize == committedSize + 7, "source identity includes trailing bytes" );
    test.Check( inventory.recordCount == 5, "only committed records are counted" );
}

void TestBoundedMetadata( TestContext& test, const std::filesystem::path& directory )
{
    const auto path = directory / "many-records.tracy-stream";
    std::string error;
    test.Check( CreateJournal( path, 10000, true, error ), "create many-record journal: " + error );
    tracy::analysis::TraceSessionInventory inventory;
    test.Check( tracy::analysis::BuildTraceSessionInventory(
        path, tracy::analysis::TraceSessionInventoryOptions {}, inventory, error ),
        "inventory many records: " + error );
    test.Check( inventory.recordCount == 10004, "large record count is exact" );
    test.Check( inventory.retainedRecordMetadata == 0, "record metadata remains bounded at zero" );
}

void TestCapacityPreflight( TestContext& test )
{
    tracy::analysis::TraceSessionInventory inventory;
    inventory.estimatedTotalBuildBytes = 100ull << 30;

    tracy::analysis::TraceSessionCapacityPolicy policy;
    tracy::analysis::TraceSessionCapacityResult result;
    test.Check( tracy::analysis::EvaluateTraceSessionCapacity(
        inventory, 2ull << 40, 400ull << 30, policy, result ), "capacity accepts adequate disk" );
    test.Check( result.requiredReserveBytes == ( ( 2ull << 40 ) + 9 ) / 10,
        "10 percent volume reserve dominates 64 GiB" );
    test.Check( !tracy::analysis::EvaluateTraceSessionCapacity(
        inventory, 2ull << 40, 250ull << 30, policy, result ), "capacity rejects unsafe free space" );
    test.Check( result.reason == "insufficient_disk", "disk rejection reason" );

    inventory.estimatedTotalBuildBytes = 300ull << 30;
    test.Check( !tracy::analysis::EvaluateTraceSessionCapacity(
        inventory, 2ull << 40, 900ull << 30, policy, result ), "capacity rejects store estimate over limit" );
    test.Check( result.reason == "session_store_limit", "store limit rejection reason" );
}

void AppendFixedEvent( std::vector<uint8_t>& frame, tracy::QueueType type )
{
    tracy::QueueItem item {};
    item.hdr.type = type;
    const auto size = tracy::QueueDataSize[size_t( type )];
    const auto previous = frame.size();
    frame.resize( previous + size );
    std::memcpy( frame.data() + previous, &item, size );
}

void AppendThreadContextEvent( std::vector<uint8_t>& frame, uint32_t thread )
{
    tracy::QueueItem item {};
    item.hdr.type = tracy::QueueType::ThreadContext;
    item.threadCtx.thread = thread;
    const auto size = tracy::QueueDataSize[size_t( item.hdr.type )];
    const auto previous = frame.size();
    frame.resize( previous + size );
    std::memcpy( frame.data() + previous, &item, size );
}

void AppendFrameVsyncEvent( std::vector<uint8_t>& frame, int64_t time, uint32_t id )
{
    tracy::QueueItem item {};
    item.hdr.type = tracy::QueueType::FrameVsync;
    item.frameVsync.time = time;
    item.frameVsync.id = id;
    const auto size = tracy::QueueDataSize[size_t( item.hdr.type )];
    const auto previous = frame.size();
    frame.resize( previous + size );
    std::memcpy( frame.data() + previous, &item, size );
}

void AppendQueueItem( std::vector<uint8_t>& frame, const tracy::QueueItem& item )
{
    const auto size = tracy::QueueDataSize[size_t( item.hdr.type )];
    const auto previous = frame.size();
    frame.resize( previous + size );
    std::memcpy( frame.data() + previous, &item, size );
}

void AppendLargePayloadEvent( std::vector<uint8_t>& frame, tracy::QueueType type,
    uint64_t payloadId, const std::vector<uint8_t>& payload )
{
    tracy::QueueItem item {};
    item.hdr.type = type;
    item.stringTransfer.ptr = payloadId;
    const auto fixed = tracy::QueueDataSize[size_t( type )];
    const auto previous = frame.size();
    frame.resize( previous + fixed + sizeof( uint32_t ) + payload.size() );
    std::memcpy( frame.data() + previous, &item, fixed );
    const auto size = uint32_t( payload.size() );
    std::memcpy( frame.data() + previous + fixed, &size, sizeof( size ) );
    if( !payload.empty() ) std::memcpy( frame.data() + previous + fixed + sizeof( size ),
        payload.data(), payload.size() );
}

void AppendStringEvent( std::vector<uint8_t>& frame, tracy::QueueType type, const std::string& value )
{
    tracy::QueueItem item {};
    item.hdr.type = type;
    const auto fixed = tracy::QueueDataSize[size_t( type )];
    const auto previous = frame.size();
    frame.resize( previous + fixed + sizeof( uint16_t ) + value.size() );
    std::memcpy( frame.data() + previous, &item, fixed );
    const auto length = uint16_t( value.size() );
    std::memcpy( frame.data() + previous + fixed, &length, sizeof( length ) );
    std::memcpy( frame.data() + previous + fixed + sizeof( length ), value.data(), value.size() );
}

void AppendStringEvent( std::vector<uint8_t>& frame, tracy::QueueType type,
    uint64_t pointer, const std::string& value )
{
    tracy::QueueItem item {};
    item.hdr.type = type;
    item.stringTransfer.ptr = pointer;
    const auto fixed = tracy::QueueDataSize[size_t( type )];
    const auto previous = frame.size();
    frame.resize( previous + fixed + sizeof( uint16_t ) + value.size() );
    std::memcpy( frame.data() + previous, &item, fixed );
    const auto length = uint16_t( value.size() );
    std::memcpy( frame.data() + previous + fixed, &length, sizeof( length ) );
    std::memcpy( frame.data() + previous + fixed + sizeof( length ), value.data(), value.size() );
}

void AppendZeroProtocolEvent( std::vector<uint8_t>& frame, tracy::QueueType type )
{
    tracy::QueueItem item {};
    item.hdr.type = type;
    const auto fixed = tracy::QueueDataSize[size_t( type )];
    const auto previous = frame.size();
    frame.resize( previous + fixed );
    std::memcpy( frame.data() + previous, &item, fixed );
    if( uint8_t( type ) >= uint8_t( tracy::QueueType::StringData ) )
    {
        const bool large = type == tracy::QueueType::FrameImageData ||
            type == tracy::QueueType::SymbolCode || type == tracy::QueueType::SourceCode ||
            type == tracy::QueueType::JnGpuReferenceSetDefinition ||
            type == tracy::QueueType::JnGpuCatalogBatchData;
        frame.resize( frame.size() + ( large ? sizeof( uint32_t ) : sizeof( uint16_t ) ), 0 );
    }
    else if( type == tracy::QueueType::SingleStringData ||
        type == tracy::QueueType::SecondStringData )
    {
        frame.resize( frame.size() + sizeof( uint16_t ), 0 );
    }
}

void TestAllProtocolQueueTypesAreCanonicalFacts( TestContext& test )
{
    std::vector<uint8_t> frame;
    for( uint16_t index = 0; index < uint16_t( tracy::QueueType::NUM_TYPES ); index++ )
        AppendZeroProtocolEvent( frame, tracy::QueueType( index ) );

    tracy::analysis::TraceSessionProtocolInventory inventory;
    std::string error;
    test.Check( tracy::analysis::CountTraceProtocolFrame( frame, inventory, error ),
        "decode one canonical fact for every QueueType: " + error );
    test.Check( inventory.eventCount == uint16_t( tracy::QueueType::NUM_TYPES ),
        "every QueueType contributes exactly one canonical source fact" );
    uint64_t classified = 0;
    for( uint16_t index = 0; index < uint16_t( tracy::QueueType::NUM_TYPES ); index++ )
    {
        test.Check( inventory.events[index].count == 1,
            "QueueType is neither omitted nor duplicated: " + std::to_string( index ) );
        const auto domain = tracy::analysis::ClassifyTraceProtocolEvent( uint8_t( index ) );
        test.Check( domain < tracy::analysis::TraceSessionProtocolDomain::Count,
            "QueueType has a stable canonical domain: " + std::to_string( index ) );
    }
    for( const auto& domain : inventory.domains ) classified += domain.count;
    test.Check( classified == inventory.eventCount,
        "stable canonical domains partition the complete protocol without loss" );
}

void TestProtocolFrameInventory( TestContext& test )
{
    std::vector<uint8_t> frame;
    AppendFixedEvent( frame, tracy::QueueType::FrameVsync );
    AppendFixedEvent( frame, tracy::QueueType::JnJobSchedule );
    AppendFixedEvent( frame, tracy::QueueType::ContextSwitch );
    AppendFixedEvent( frame, tracy::QueueType::MemAlloc );
    AppendFixedEvent( frame, tracy::QueueType::JnGpuCatalogControl );
    AppendFixedEvent( frame, tracy::QueueType::JnGpuReferencePass );
    AppendStringEvent( frame, tracy::QueueType::StringData, "hello" );

    tracy::analysis::TraceSessionProtocolInventory inventory;
    std::string error;
    test.Check( tracy::analysis::CountTraceProtocolFrame( frame, inventory, error ),
        "count valid protocol frame: " + error );
    test.Check( inventory.eventCount == 7, "protocol event count" );
    test.Check( inventory.events[size_t( tracy::QueueType::FrameVsync )].count == 1,
        "fixed event count" );
    test.Check( inventory.events[size_t( tracy::QueueType::JnJobSchedule )].count == 1,
        "JN Job event count" );
    test.Check( inventory.events[size_t( tracy::QueueType::StringData )].variablePayloadBytes == 5,
        "variable payload bytes" );
    test.Check( inventory.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Frame )].count == 1,
        "frame domain count" );
    test.Check( inventory.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Job )].count == 1,
        "job domain count" );
    test.Check( inventory.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Scheduling )].count == 1,
        "scheduling domain count" );
    test.Check( inventory.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::CpuMemory )].count == 1,
        "CPU memory domain count" );
    test.Check( inventory.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::GpuCatalog )].count == 1,
        "GPU Catalog domain count" );
    test.Check( inventory.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::GpuMemory )].count == 1,
        "GPU memory domain count" );
    test.Check( inventory.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Dictionary )].count == 1,
        "dictionary domain count" );
    test.Check( inventory.encodedBytes == frame.size(), "encoded bytes cover the frame exactly" );

    frame.pop_back();
    tracy::analysis::TraceSessionProtocolInventory corrupt;
    test.Check( !tracy::analysis::CountTraceProtocolFrame( frame, corrupt, error ),
        "truncated variable payload is rejected" );
    test.Check( error == "protocol_event_exceeds_frame", "corrupt frame reason is explicit" );
}

void TestCompressedProtocolInventory( TestContext& test )
{
    std::vector<uint8_t> frame;
    AppendFixedEvent( frame, tracy::QueueType::FrameVsync );
    AppendFixedEvent( frame, tracy::QueueType::JnGpuReferenceSetUse );
    std::vector<char> compressed( tracy::LZ4Size );
    const auto compressedBytes = tracy::LZ4_compress_default(
        reinterpret_cast<const char*>( frame.data() ), compressed.data(), int( frame.size() ), int( compressed.size() ) );
    test.Check( compressedBytes > 0, "compress protocol frame" );
    if( compressedBytes <= 0 ) return;

    std::vector<uint8_t> record( sizeof( tracy::lz4sz_t ) + size_t( compressedBytes ) );
    const auto storedSize = tracy::lz4sz_t( compressedBytes );
    std::memcpy( record.data(), &storedSize, sizeof( storedSize ) );
    std::memcpy( record.data() + sizeof( storedSize ), compressed.data(), size_t( compressedBytes ) );

    tracy::analysis::TraceSessionProtocolInventory inventory;
    tracy::analysis::TraceSessionProtocolDecoder decoder;
    std::string error;
    test.Check( decoder.ConsumeCompressedRecord( record, inventory, error ),
        "decode compressed protocol frame: " + error );
    test.Check( inventory.frameCount == 1 && inventory.eventCount == 2,
        "compressed frame and event counts" );
    test.Check( inventory.compressedBytes == record.size(), "compressed record bytes" );

    record[0]++;
    test.Check( !decoder.ConsumeCompressedRecord( record, inventory, error ),
        "compressed size mismatch is rejected" );
    test.Check( error == "compressed_record_size_mismatch", "compressed mismatch reason" );
}

std::vector<uint8_t> CompressContinuedFrame( tracy::LZ4_stream_t* stream,
    const std::vector<uint8_t>& frame, TestContext& test )
{
    std::vector<char> compressed( tracy::LZ4Size );
    const auto compressedBytes = tracy::LZ4_compress_fast_continue( stream,
        reinterpret_cast<const char*>( frame.data() ), compressed.data(),
        int( frame.size() ), int( compressed.size() ), 1 );
    test.Check( compressedBytes > 0, "compress continued protocol frame" );
    if( compressedBytes <= 0 ) return {};
    std::vector<uint8_t> record( sizeof( tracy::lz4sz_t ) + size_t( compressedBytes ) );
    const auto storedSize = tracy::lz4sz_t( compressedBytes );
    std::memcpy( record.data(), &storedSize, sizeof( storedSize ) );
    std::memcpy( record.data() + sizeof( storedSize ), compressed.data(), size_t( compressedBytes ) );
    return record;
}

void TestProtocolDecoderCheckpoint( TestContext& test )
{
    std::vector<uint8_t> firstFrame;
    std::vector<uint8_t> secondFrame;
    const std::string repeated( 60000, 'Q' );
    AppendStringEvent( firstFrame, tracy::QueueType::StringData, repeated );
    AppendFixedEvent( firstFrame, tracy::QueueType::FrameVsync );
    AppendStringEvent( secondFrame, tracy::QueueType::StringData, repeated );
    AppendFixedEvent( secondFrame, tracy::QueueType::JnJobSchedule );

    auto* compressor = tracy::LZ4_createStream();
    test.Check( compressor != nullptr, "create continued compressor" );
    if( !compressor ) return;
    const auto firstRecord = CompressContinuedFrame( compressor, firstFrame, test );
    const auto secondRecord = CompressContinuedFrame( compressor, secondFrame, test );
    tracy::LZ4_freeStream( compressor );
    if( firstRecord.empty() || secondRecord.empty() ) return;

    tracy::analysis::TraceSessionProtocolDecoder decoder;
    tracy::analysis::TraceSessionProtocolInventory firstInventory;
    std::string error;
    test.Check( decoder.ConsumeCompressedRecord( firstRecord, firstInventory, error ),
        "decode first continued frame: " + error );
    const auto checkpoint = decoder.ExportDictionary();
    test.Check( !checkpoint.empty() && checkpoint.size() <= 64 * 1024,
        "decoder checkpoint exports bounded LZ4 dictionary" );

    tracy::analysis::TraceSessionProtocolDecoder resumed;
    test.Check( resumed.RestoreDictionary( checkpoint, error ),
        "restore LZ4 decoder dictionary: " + error );
    tracy::analysis::TraceSessionProtocolInventory secondInventory;
    test.Check( resumed.ConsumeCompressedRecord( secondRecord, secondInventory, error ),
        "decode dictionary-dependent frame after restore: " + error );
    test.Check( secondInventory.eventCount == 2 &&
        secondInventory.events[size_t( tracy::QueueType::JnJobSchedule )].count == 1,
        "resumed decoder preserves second frame facts" );
}

void TestProtocolJournalInventory( TestContext& test, const std::filesystem::path& directory )
{
    std::vector<uint8_t> firstFrame;
    std::vector<uint8_t> secondFrame;
    const std::string repeated( 4096, 'R' );
    AppendThreadContextEvent( firstFrame, 77 );
    AppendFrameVsyncEvent( firstFrame, 123456, 9 );
    AppendStringEvent( firstFrame, tracy::QueueType::StringData, repeated );
    AppendFixedEvent( secondFrame, tracy::QueueType::JnJobSchedule );
    AppendStringEvent( secondFrame, tracy::QueueType::StringData, repeated );
    auto* compressor = tracy::LZ4_createStream();
    test.Check( compressor != nullptr, "create protocol journal compressor" );
    if( !compressor ) return;
    const auto firstRecord = CompressContinuedFrame( compressor, firstFrame, test );
    const auto secondRecord = CompressContinuedFrame( compressor, secondFrame, test );
    tracy::LZ4_freeStream( compressor );
    if( firstRecord.empty() || secondRecord.empty() ) return;

    const auto path = directory / "protocol.tracy-stream";
    tracy::stream::WriterOptions writerOptions;
    writerOptions.durableHeader = false;
    std::string error;
    auto writer = tracy::stream::JournalWriter::CreateFileJournal(
        path, DeterministicHeader(), false, writerOptions, error );
    test.Check( writer != nullptr, "create protocol journal: " + error );
    if( !writer ) return;
    test.Check( writer->Append( tracy::stream::RecordType::SessionBegin,
        tracy::stream::RecordFlagHandshake, "begin", 0, error ), "protocol journal begin" );
    test.Check( writer->Append( tracy::stream::RecordType::ClientToServer,
        tracy::stream::RecordFlagCompressedFrame, firstRecord, 1, error ), "protocol journal first frame" );
    test.Check( writer->Append( tracy::stream::RecordType::ClientToServer,
        tracy::stream::RecordFlagCompressedFrame, secondRecord, 2, error ), "protocol journal second frame" );
    test.Check( writer->Append( tracy::stream::RecordType::SessionEnd,
        tracy::stream::RecordFlagTerminal, "end", 3, error ), "protocol journal end" );
    writer.reset();

    tracy::analysis::TraceSessionInventory inventory;
    tracy::analysis::TraceSessionInventoryOptions options;
    options.runDirectory = directory / "protocol-runs";
    options.runTargetBytes = 128;
    test.Check( tracy::analysis::BuildTraceSessionInventory(
        path, options, inventory, error ),
        "build protocol-aware inventory: " + error );
    test.Check( inventory.protocolInventoryComplete, "protocol inventory completes" );
    test.Check( inventory.protocolInventory.frameCount == 2 && inventory.protocolInventory.eventCount == 5,
        "journal inventory contains decoded frame/event counts" );
    test.Check( inventory.protocolInventory.events[size_t( tracy::QueueType::JnJobSchedule )].count == 1,
        "journal inventory contains QueueType counts" );
    test.Check( !inventory.runs.empty(), "inventory emits immutable disk runs" );
    uint64_t journalRunRecords = 0;
    uint64_t dependencyRunRecords = 0;
    for( const auto& run : inventory.runs )
    {
        if( run.kind == tracy::analysis::TraceSessionInventoryRunKind::JournalRecord )
            journalRunRecords += run.recordCount;
        else if( run.kind == tracy::analysis::TraceSessionInventoryRunKind::ProtocolDependency )
            dependencyRunRecords += run.recordCount;
    }
    test.Check( journalRunRecords == inventory.recordCount, "journal run covers every committed record" );
    test.Check( dependencyRunRecords > 0, "dependency run indexes protocol definitions" );
    test.Check( tracy::analysis::VerifyTraceSessionInventoryRuns( options.runDirectory, inventory, error ),
        "inventory run checksums verify: " + error );

    tracy::analysis::TraceSessionCanonicalOptions canonicalOptions;
    canonicalOptions.targetShardBytes = 128;
    canonicalOptions.softShardBytes = 256;
    canonicalOptions.hardShardBytes = 512;
    canonicalOptions.minimumShardSpanNs = 0;
    canonicalOptions.maximumShardSpanNs = 100;
    tracy::analysis::TraceSessionManifest manifest;
    const auto sessionRoot = directory / "canonical-session";
    CanonicalCancelState cancelState { 0, 2 };
    canonicalOptions.shouldCancel = RequestCanonicalCancel;
    canonicalOptions.cancelUserData = &cancelState;
    test.Check( tracy::analysis::BuildTraceSessionCanonical( path, sessionRoot, "generation-1",
        inventory, canonicalOptions, manifest, error ) ==
        tracy::analysis::TraceSessionCanonicalBuildResult::CancelledResumable,
        "cancel canonical session at a complete journal record: " + error );
    test.Check( manifest.state == tracy::analysis::TraceSessionState::CancelledResumable,
        "cancelled canonical session is resumable" );
    test.Check( !manifest.shards.empty(), "cancel commits a bounded canonical shard and checkpoint" );
    const auto cancelledShards = manifest.shards;

    canonicalOptions.shouldCancel = nullptr;
    canonicalOptions.cancelUserData = nullptr;
    test.Check( tracy::analysis::BuildTraceSessionCanonical( path, sessionRoot, "generation-1",
        inventory, canonicalOptions, manifest, error ) ==
        tracy::analysis::TraceSessionCanonicalBuildResult::Complete,
        "resume canonical session from checkpoint: " + error );
    test.Check( manifest.state == tracy::analysis::TraceSessionState::CanonicalBuilding,
        "completed canonical facts remain in canonical-building state until derived work" );
    test.Check( manifest.shards.size() > cancelledShards.size(),
        "resume appends shards instead of replacing the committed prefix" );
    for( size_t i = 0; i < cancelledShards.size() && i < manifest.shards.size(); i++ )
    {
        test.Check( manifest.shards[i].shardId == cancelledShards[i].shardId &&
            manifest.shards[i].sha256 == cancelledShards[i].sha256,
            "resume preserves committed shard identity" );
    }
    uint64_t canonicalRecords = 0;
    uint64_t checkpointRecords = 0;
    bool sawFrameDomain = false;
    bool sawJobDomain = false;
    bool sawDictionaryDomain = false;
    bool sawControlDomain = false;
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) checkpointRecords += shard.recordCount;
        else
        {
            canonicalRecords += shard.recordCount;
            sawFrameDomain |= shard.domain == "frame";
            sawJobDomain |= shard.domain == "job";
            sawDictionaryDomain |= shard.domain == "dictionary";
            sawControlDomain |= shard.domain == "control";
            test.Check( shard.domain != "protocol", "canonical events are partitioned by stable data domain" );
        }
    }
    test.Check( canonicalRecords == inventory.protocolInventory.eventCount + inventory.recordCount,
        "canonical records cover decoded events and non-compressed transport records" );
    test.Check( checkpointRecords > 0, "each committed canonical segment has a checkpoint" );
    test.Check( sawFrameDomain && sawJobDomain && sawDictionaryDomain && sawControlDomain,
        "canonical segment preserves all source-present domains" );
    CanonicalReadState canonicalRead;
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        test.Check( tracy::analysis::VisitTraceSessionCanonicalShard( sessionRoot, shard,
            CountCanonicalRecord, &canonicalRead, error ),
            "read canonical domain shard: " + error );
    }
    test.Check( canonicalRead.records == canonicalRecords,
        "canonical reader visits every persisted record exactly once" );
    test.Check( canonicalRead.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Frame )] == 1 &&
        canonicalRead.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Job )] == 1 &&
        canonicalRead.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Dictionary )] == 2 &&
        canonicalRead.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Control )] == 0 &&
        canonicalRead.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Scheduling )] == 1,
        "canonical reader reproduces exact per-domain counts" );
    test.Check( canonicalRead.frameThreadContext == 77 &&
        canonicalRead.jobThreadContext == 77 &&
        canonicalRead.frameSemanticTime == 123456,
        "canonical records carry checkpointed thread context and raw semantic time across shards" );
    std::sort( canonicalRead.protocolOrder.begin(), canonicalRead.protocolOrder.end() );
    std::vector<uint8_t> restoredTypes;
    restoredTypes.reserve( canonicalRead.protocolOrder.size() );
    for( const auto& value : canonicalRead.protocolOrder ) restoredTypes.push_back( std::get<3>( value ) );
    const std::vector<uint8_t> expectedTypes = {
        uint8_t( tracy::QueueType::ThreadContext ), uint8_t( tracy::QueueType::FrameVsync ),
        uint8_t( tracy::QueueType::StringData ), uint8_t( tracy::QueueType::JnJobSchedule ),
        uint8_t( tracy::QueueType::StringData )
    };
    test.Check( restoredTypes == expectedTypes,
        "domain shards restore exact cross-domain protocol order using frame offsets" );
    CanonicalOrderedReadState orderedRead;
    test.Check( tracy::analysis::VisitTraceSessionCanonicalOrdered( sessionRoot, manifest,
        CountCanonicalRecordOrdered, &orderedRead, error ),
        "ordered canonical reader merges every checkpoint-bounded domain group: " + error );
    test.Check( orderedRead.records == canonicalRecords && orderedRead.monotonic,
        "ordered canonical reader visits every fact once in exact source order" );
    test.Check( orderedRead.protocolTypes == expectedTypes,
        "ordered canonical reader restores protocol event order without caller-side sorting" );
    test.Check( tracy::analysis::VerifyTraceSession( sessionRoot, manifest, error ),
        "canonical session shards verify: " + error );
    tracy::analysis::TraceSessionCanonicalAudit audit;
    test.Check( tracy::analysis::AuditTraceSessionCanonical( sessionRoot, manifest,
        inventory, audit, error ), "audit all canonical domains: " + error );
    test.Check( audit.protocolEvents == inventory.protocolInventory.eventCount &&
        audit.protocolFrames == inventory.protocolInventory.frameCount &&
        audit.protocolEncodedBytes == inventory.protocolInventory.encodedBytes &&
        audit.transportRecords == inventory.recordCount - inventory.protocolInventory.frameCount,
        "canonical audit conserves protocol frames/events/bytes and transport records" );
    test.Check( audit.semanticTimeEvents >= 2,
        "canonical audit reports exact semantic-time coverage without inventing timestamps" );
    auto incompleteManifest = manifest;
    const auto omitted = std::find_if( incompleteManifest.shards.begin(), incompleteManifest.shards.end(),
        []( const auto& shard ) { return shard.domain == "frame"; } );
    test.Check( omitted != incompleteManifest.shards.end(), "audit omission fixture has frame shard" );
    if( omitted != incompleteManifest.shards.end() )
    {
        incompleteManifest.shards.erase( omitted );
        test.Check( !tracy::analysis::AuditTraceSessionCanonical( sessionRoot,
            incompleteManifest, inventory, audit, error ),
            "canonical audit rejects a missing committed domain shard" );
        test.Check( error == "canonical_audit_global_count_mismatch" ||
            error == "canonical_audit_event_count_mismatch" ||
            error == "canonical_audit_domain_count_mismatch",
            "missing shard has an explicit audit mismatch: " + error );
    }

    const auto changedSource = directory / "protocol-changed.tracy-stream";
    std::filesystem::copy_file( path, changedSource,
        std::filesystem::copy_options::overwrite_existing );
    {
        std::fstream changed( changedSource, std::ios::binary | std::ios::in | std::ios::out );
        char byte = 0;
        changed.read( &byte, 1 );
        byte ^= 0x5a;
        changed.seekp( 0 );
        changed.write( &byte, 1 );
    }
    test.Check( tracy::analysis::BuildTraceSessionCanonical( changedSource, sessionRoot, "generation-1",
        inventory, canonicalOptions, manifest, error ) ==
        tracy::analysis::TraceSessionCanonicalBuildResult::Failed,
        "resume rejects a changed source identity" );
    test.Check( error == "source_sha256_mismatch",
        "changed source has an explicit strong-identity error: " + error );

    const auto damagedSessionRoot = directory / "canonical-session-damaged-checkpoint";
    cancelState = { 0, 2 };
    canonicalOptions.shouldCancel = RequestCanonicalCancel;
    canonicalOptions.cancelUserData = &cancelState;
    test.Check( tracy::analysis::BuildTraceSessionCanonical( path, damagedSessionRoot, "generation-damaged",
        inventory, canonicalOptions, manifest, error ) ==
        tracy::analysis::TraceSessionCanonicalBuildResult::CancelledResumable,
        "create resumable fixture for checkpoint corruption: " + error );
    const auto checkpointIt = std::find_if( manifest.shards.rbegin(), manifest.shards.rend(),
        []( const auto& shard ) { return shard.domain == "checkpoint"; } );
    test.Check( checkpointIt != manifest.shards.rend(), "resumable fixture contains checkpoint" );
    if( checkpointIt != manifest.shards.rend() )
    {
        std::ofstream damaged( damagedSessionRoot / checkpointIt->relativePath,
            std::ios::binary | std::ios::app );
        damaged.put( '\x7f' );
        damaged.close();
        canonicalOptions.shouldCancel = nullptr;
        canonicalOptions.cancelUserData = nullptr;
        test.Check( tracy::analysis::BuildTraceSessionCanonical( path, damagedSessionRoot,
            "generation-damaged", inventory, canonicalOptions, manifest, error ) ==
            tracy::analysis::TraceSessionCanonicalBuildResult::Failed,
            "resume rejects a damaged committed checkpoint" );
        test.Check( error == "session_shard_size_mismatch",
            "damaged checkpoint has an explicit integrity error: " + error );
    }
    canonicalOptions.shouldCancel = nullptr;
    canonicalOptions.cancelUserData = nullptr;

    auto limitedOptions = canonicalOptions;
    limitedOptions.resume = false;
    limitedOptions.targetShardBytes = 32;
    limitedOptions.softShardBytes = 40;
    limitedOptions.hardShardBytes = 48;
    limitedOptions.softMemoryBytes = 56;
    limitedOptions.hardMemoryBytes = 64;
    const auto limitedSessionRoot = directory / "canonical-session-memory-limit";
    test.Check( tracy::analysis::BuildTraceSessionCanonical( path, limitedSessionRoot,
        "generation-limited", inventory, limitedOptions, manifest, error ) ==
        tracy::analysis::TraceSessionCanonicalBuildResult::Failed,
        "canonical hard memory limit stops without discarding a committed prefix" );
    test.Check( error == "canonical_memory_hard_limit" &&
        manifest.state == tracy::analysis::TraceSessionState::InvalidCapacity &&
        manifest.reason == "resource_limit",
        "memory limit failure is explicit and resumable from committed files: " + error );
    test.Check( std::any_of( manifest.shards.begin(), manifest.shards.end(),
        []( const auto& shard ) { return shard.domain == "checkpoint"; } ),
        "memory limit preserves the checkpoint committed before the oversized segment" );

    auto diskLimitedOptions = canonicalOptions;
    diskLimitedOptions.resume = false;
    diskLimitedOptions.targetShardBytes = 32;
    diskLimitedOptions.softShardBytes = 40;
    diskLimitedOptions.hardShardBytes = 48;
    diskLimitedOptions.minimumFreeReserveBytes = 128;
    diskLimitedOptions.minimumFreeReservePercent = 0;
    DiskProbeState diskProbe;
    diskLimitedOptions.diskSpaceProbe = ExhaustDiskAfterFirstShard;
    diskLimitedOptions.diskSpaceUserData = &diskProbe;
    const auto diskLimitedSessionRoot = directory / "canonical-session-disk-limit";
    test.Check( tracy::analysis::BuildTraceSessionCanonical( path, diskLimitedSessionRoot,
        "generation-disk-limited", inventory, diskLimitedOptions, manifest, error ) ==
        tracy::analysis::TraceSessionCanonicalBuildResult::Failed,
        "canonical disk reserve stops before an unsafe shard write" );
    test.Check( error == "canonical_disk_pressure" &&
        manifest.state == tracy::analysis::TraceSessionState::InsufficientDisk &&
        manifest.reason == "insufficient_disk",
        "disk pressure failure is explicit and preserves committed state: " + error );
    test.Check( std::any_of( manifest.shards.begin(), manifest.shards.end(),
        []( const auto& shard ) { return shard.domain == "checkpoint"; } ),
        "disk pressure preserves the checkpoint committed before reserve exhaustion" );

    const auto inventoryPath = directory / "protocol-inventory";
    test.Check( tracy::analysis::SaveTraceSessionInventory( inventoryPath, inventory, error ),
        "save protocol inventory: " + error );
    const auto loaded = tracy::analysis::LoadTraceSessionInventory( inventoryPath, error );
    test.Check( loaded.has_value(), "load protocol inventory: " + error );
    if( loaded )
    {
        test.Check( loaded->protocolInventory == inventory.protocolInventory,
            "protocol inventory round-trips exactly" );
        test.Check( loaded->runs.size() == inventory.runs.size(), "inventory run manifest round-trips" );
    }

    auto corruptInventory = inventory;
    corruptInventory.protocolInventory.eventCount++;
    const auto corruptPath = directory / "protocol-inventory-bad-total";
    test.Check( tracy::analysis::SaveTraceSessionInventory( corruptPath, corruptInventory, error ),
        "save inconsistent protocol inventory: " + error );
    const auto incomplete = tracy::analysis::LoadTraceSessionInventory( corruptPath, error );
    test.Check( !incomplete.has_value(), "inconsistent protocol totals invalidate persisted inventory" );
    test.Check( error == "incomplete protocol event inventory",
        "inconsistent protocol totals have explicit integrity reason: " + error );

    if( !inventory.runs.empty() )
    {
        std::ofstream damaged( options.runDirectory / inventory.runs.front().relativePath,
            std::ios::binary | std::ios::app );
        damaged.put( '\x7f' );
        damaged.close();
        test.Check( !tracy::analysis::VerifyTraceSessionInventoryRuns(
            options.runDirectory, inventory, error ), "damaged inventory run is rejected" );
        test.Check( error == "inventory run size mismatch", "damaged run has explicit integrity reason" );
    }
}

void TestGpuCanonicalReader( TestContext& test, const std::filesystem::path& directory )
{
    constexpr uint64_t Generation = 7;
    constexpr uint64_t PayloadId = 99;
    std::array<tracy::JnGpuCatalogResourceRecordV1, 3> resources {};
    resources[0].time = 110;
    resources[0].resourceId = 10;
    resources[0].pointerToken = 0x1234;
    resources[0].allocationId = 20;
    resources[0].capacityBytes = 4096;
    resources[0].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    resources[0].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    resources[1] = resources[0];
    resources[1].time = 114;
    resources[1].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Destroy );
    resources[2] = resources[0];
    resources[2].time = 116;
    resources[2].resourceId = 11;
    resources[2].allocationId = 21;

    std::array<tracy::JnGpuCatalogViewRecordV1, 2> views {};
    views[0].time = 112;
    views[0].viewId = 100;
    views[0].pointerToken = 0x1234;
    views[0].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    views[0].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    views[1] = views[0];
    views[1].time = 117;
    views[1].viewId = 101;

    const auto makeCatalogPayload = []( const void* records, uint32_t recordCount, uint16_t recordBytes )
    {
        tracy::JnGpuCatalogBatchEnvelopeV1 envelope {};
        envelope.magic = tracy::JnGpuCatalogBatchMagic;
        envelope.catalogSchema = tracy::JnGpuCatalogSchemaVersion;
        envelope.evidenceSchema = tracy::JnGpuDetailedEvidenceSchemaVersion;
        envelope.recordBytes = recordBytes;
        envelope.recordCount = recordCount;
        envelope.payloadBytes = uint32_t( uint64_t( recordCount ) * recordBytes );
        envelope.checksum = tracy::JnGpuCatalogChecksum64( records, envelope.payloadBytes );
        std::vector<uint8_t> payload( sizeof( envelope ) + envelope.payloadBytes );
        std::memcpy( payload.data(), &envelope, sizeof( envelope ) );
        std::memcpy( payload.data() + sizeof( envelope ), records, envelope.payloadBytes );
        return payload;
    };
    const auto resourcePayload = makeCatalogPayload( resources.data(), uint32_t( resources.size() ), sizeof( resources[0] ) );
    const auto viewPayload = makeCatalogPayload( views.data(), uint32_t( views.size() ), sizeof( views[0] ) );

    std::vector<uint8_t> frame;
    AppendThreadContextEvent( frame, 42 );
    tracy::QueueItem item {};
    const std::array<uint64_t, 1> sampleDictionaryStack { 0x10101010 };
    AppendStringEvent( frame, tracy::QueueType::CallstackSampleDictionary, 1,
        std::string( reinterpret_cast<const char*>( sampleDictionaryStack.data() ),
            sizeof( sampleDictionaryStack ) ) );
    item = {};
    item.hdr.type = tracy::QueueType::CallstackSampleRef;
    item.callstackSampleRef = { { 107, 42 }, 1 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::CallstackSampleContextSwitchRef;
    item.callstackSampleRef = { { 1, 42 }, 1 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ContextSwitch;
    item.contextSwitch.time = 1;
    item.contextSwitch.oldThread = 0;
    item.contextSwitch.newThread = 42;
    item.contextSwitch.cpu = 0;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ThreadWakeup;
    item.threadWakeup.time = 2;
    item.threadWakeup.thread = 43;
    item.threadWakeup.cpu = 1;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ContextSwitch;
    item.contextSwitch.time = 3;
    item.contextSwitch.oldThread = 42;
    item.contextSwitch.newThread = 43;
    item.contextSwitch.cpu = 0;
    item.contextSwitch.oldThreadWaitReason = 5;
    item.contextSwitch.oldThreadState = 5;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ContextSwitch;
    item.contextSwitch.time = 4;
    item.contextSwitch.oldThread = 43;
    item.contextSwitch.newThread = 0;
    item.contextSwitch.cpu = 0;
    item.contextSwitch.oldThreadWaitReason = 4;
    item.contextSwitch.oldThreadState = 1;
    AppendQueueItem( frame, item );
    const std::array<uint64_t, 2> jobCallstack { 0x20202020, 0x30303030 };
    AppendStringEvent( frame, tracy::QueueType::CallstackPayload, 0x3333,
        std::string( reinterpret_cast<const char*>( jobCallstack.data() ), sizeof( jobCallstack ) ) );
    item.hdr.type = tracy::QueueType::CallstackSerial;
    item.callstackFat.ptr = 0x3333;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnCallsiteDefinition;
    item.jnCallsiteDefinition = { 0x4444, 77, 42, 3,
        uint8_t( tracy::JnStackProvenance::SiteReused ),
        uint8_t( tracy::JnCallsiteFlags::HasCallstack ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    constexpr uint64_t JobNamePointer = 0x2222;
    item.hdr.type = tracy::QueueType::JnJobType;
    item.jnJobType = { JobNamePointer, 17, 2, 0 };
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::StringData, JobNamePointer, "SyntheticJob" );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobSchedule;
    item.jnJobSchedule = { 108, 500, 0xABC, 0, 2, uint8_t( 1 << 6 ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobConfig;
    item.jnJobConfig = { 500, 17, 64, 8, 31, 9, 2, uint8_t( 1 << 6 ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobStage;
    item.jnJobStage = { 108, 500, 77, 0, 0,
        uint8_t( tracy::JnJobStage::ScheduleCallsite ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobStage;
    item.jnJobStage = { 110, 500, 1, 3, 0, uint8_t( tracy::JnJobStage::Ready ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobStage;
    item.jnJobStage = { 111, 500, 2, 0, 32, uint8_t( tracy::JnJobStage::WorkerSliceBegin ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobStage;
    item.jnJobStage = { 115, 500, 2, 0, 32, uint8_t( tracy::JnJobStage::WorkerSliceEnd ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobStage;
    item.jnJobStage = { 116, 500, 3, 0, 0, uint8_t( tracy::JnJobStage::Completed ), 0 };
    AppendQueueItem( frame, item );

    const std::array<uint64_t, 2> zoneCallstack { 0x40404040, 0x50505050 };
    AppendStringEvent( frame, tracy::QueueType::CallstackPayload, 0x5555,
        std::string( reinterpret_cast<const char*>( zoneCallstack.data() ), sizeof( zoneCallstack ) ) );
    item = {};
    item.hdr.type = tracy::QueueType::CallstackSerial;
    item.callstackFat.ptr = 0x5555;
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::ZoneValidation;
    item.zoneValidation.id = 100;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnZoneBeginCallsite;
    item.jnZoneBeginCallsite = { { 104, 0x1000 }, 78 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::PlotDataInt;
    item.plotDataInt = { { 0x6000, 2 }, 1 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneValidation;
    item.zoneValidation.id = 101;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneBegin;
    item.zoneBegin = { 2, 0x1001 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneValidation;
    item.zoneValidation.id = 101;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneEnd;
    item.zoneEnd.time = 2;
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "Parent Override" );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneName;
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "Parent text" );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneText;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneValidation;
    item.zoneValidation.id = 100;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneEnd;
    item.zoneEnd.time = 4;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnCallsiteDefinition;
    item.jnCallsiteDefinition = { 0x1000, 78, 42, 0,
        uint8_t( tracy::JnStackProvenance::SiteReused ),
        uint8_t( tracy::JnCallsiteFlags::HasCallstack ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneValidation;
    item.zoneValidation.id = 102;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneBegin;
    item.zoneBegin = { 2, 0x1001 };
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::GpuNewContext;
    item.gpuNewContext.cpuTime = 100;
    item.gpuNewContext.gpuTime = 0;
    item.gpuNewContext.thread = 0;
    item.gpuNewContext.period = 1.f;
    item.gpuNewContext.context = 1;
    item.gpuNewContext.flags = tracy::GpuContextFlags( 0 );
    item.gpuNewContext.type = tracy::GpuContextType::Direct3D12;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::GpuZoneBegin;
    item.gpuZoneBegin.cpuTime = 2;
    item.gpuZoneBegin.thread = 42;
    item.gpuZoneBegin.queryId = 7;
    item.gpuZoneBegin.context = 1;
    item.gpuZoneBegin.srcloc = 0x5030;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::GpuZoneEnd;
    item.gpuZoneEnd.cpuTime = 2;
    item.gpuZoneEnd.thread = 42;
    item.gpuZoneEnd.queryId = 8;
    item.gpuZoneEnd.context = 1;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::GpuTime;
    item.gpuTime.gpuTime = 36;
    item.gpuTime.queryId = 7;
    item.gpuTime.context = 1;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::GpuTime;
    item.gpuTime.gpuTime = 4;
    item.gpuTime.queryId = 8;
    item.gpuTime.context = 1;
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::SourceLocation;
    item.srcloc = { 0, 0x5022, 0x5023, 77, 0, 0, 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::SourceLocation;
    item.srcloc = { 0x5001, 0x5002, 0x5003, 123, 0x11, 0x22, 0x33 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::SourceLocation;
    item.srcloc = { 0, 0x5012, 0x5013, 456, 0x44, 0x55, 0x66 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::SourceLocation;
    item.srcloc = { 0x5031, 0x5032, 0x5033, 789, 0x77, 0x88, 0x99 };
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5022, "JobFunction" );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5023, "Job.cpp" );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5001, "Parent Source" );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5002, "ParentFunction" );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5003, "Parent.cpp" );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5012, "ChildFunction" );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5013, "Child.cpp" );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5031, "Synthetic GPU Zone" );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5032, "GpuFunction" );
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x5033, "Gpu.cpp" );

    item = {};
    item.hdr.type = tracy::QueueType::MemAlloc;
    item.memAlloc.time = 109;
    item.memAlloc.thread = 42;
    item.memAlloc.ptr = 0xDEAD;
    const uint64_t firstAllocationBytes = 4096;
    std::memcpy( item.memAlloc.size, &firstAllocationBytes, 6 );
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::MemFree;
    item.memFree = { 2, 42, 0xDEAD };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::MemAlloc;
    item.memAlloc.time = 1;
    item.memAlloc.thread = 42;
    item.memAlloc.ptr = 0xDEAD;
    const uint64_t reusedAllocationBytes = 2048;
    std::memcpy( item.memAlloc.size, &reusedAllocationBytes, 6 );
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::MemAlloc;
    item.memAlloc.time = 1;
    item.memAlloc.thread = 42;
    item.memAlloc.ptr = 0xBEEF;
    const uint64_t openAllocationBytes = 6144;
    std::memcpy( item.memAlloc.size, &openAllocationBytes, 6 );
    AppendQueueItem( frame, item );

    const std::array<uint64_t, 2> memorySiteCallstack { 0x60606060, 0x70707070 };
    AppendStringEvent( frame, tracy::QueueType::CallstackPayload, 0x6666,
        std::string( reinterpret_cast<const char*>( memorySiteCallstack.data() ),
            sizeof( memorySiteCallstack ) ) );
    item = {};
    item.hdr.type = tracy::QueueType::CallstackSerial;
    item.callstackFat.ptr = 0x6666;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnCallsiteDefinition;
    item.jnCallsiteDefinition = { 0x1000, 79, 42, 4,
        uint8_t( tracy::JnStackProvenance::SiteReused ),
        uint8_t( tracy::JnCallsiteFlags::HasCallstack ), 0 };
    AppendQueueItem( frame, item );
    constexpr uint64_t GpuMemoryPoolName = 0x7000;
    item = {};
    item.hdr.type = tracy::QueueType::MemNamePayload;
    item.memName.name = GpuMemoryPoolName;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnMemAllocCallsiteNamed;
    item.jnMemAllocCallsite.time = 1;
    item.jnMemAllocCallsite.thread = 42;
    item.jnMemAllocCallsite.ptr = 77;
    const uint64_t gpuAllocationBytes = 1024;
    std::memcpy( item.jnMemAllocCallsite.size, &gpuAllocationBytes, 6 );
    item.jnMemAllocCallsite.callsiteId = 79;
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::StringData,
        GpuMemoryPoolName, "GPU D3D12 Texture" );
    const std::array<uint64_t, 1> memoryFreeCallstack { 0x80808080 };
    AppendStringEvent( frame, tracy::QueueType::CallstackPayload, 0x7777,
        std::string( reinterpret_cast<const char*>( memoryFreeCallstack.data() ),
            sizeof( memoryFreeCallstack ) ) );
    item = {};
    item.hdr.type = tracy::QueueType::CallstackSerial;
    item.callstackFat.ptr = 0x7777;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::MemNamePayload;
    item.memName.name = GpuMemoryPoolName;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::MemFreeCallstackNamed;
    item.memFree = { 1, 42, 77 };
    AppendQueueItem( frame, item );

    item = {};
    item.hdr.type = tracy::QueueType::FrameVsync;
    item.frameVsync = { 106, 9 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnGpuCatalogControl;
    item.jnGpuCatalogControl = { 105, Generation, 0, 1,
        uint8_t( tracy::JnGpuCatalogControlKind::GenerationBegin ),
        uint8_t( tracy::JnGpuCatalogGenerationState::Building ), 0 };
    AppendQueueItem( frame, item );
    AppendLargePayloadEvent( frame, tracy::QueueType::JnGpuCatalogBatchData, PayloadId, resourcePayload );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuCatalogBatch;
    item.jnGpuCatalogBatch = { Generation, PayloadId, 2, uint32_t( resources.size() ), uint32_t( resourcePayload.size() ),
        uint8_t( tracy::JnGpuCatalogBatchKind::Resource ),
        uint8_t( tracy::JnGpuCatalogBatchEncoding::FixedV1 ), 0 };
    AppendQueueItem( frame, item );
    AppendLargePayloadEvent( frame, tracy::QueueType::JnGpuCatalogBatchData, PayloadId + 1, viewPayload );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuCatalogBatch;
    item.jnGpuCatalogBatch = { Generation, PayloadId + 1, 3, uint32_t( views.size() ), uint32_t( viewPayload.size() ),
        uint8_t( tracy::JnGpuCatalogBatchKind::View ),
        uint8_t( tracy::JnGpuCatalogBatchEncoding::FixedV1 ), 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferencePass;
    item.jnGpuReferencePass = { 111, 1000, 5, 77, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceUse;
    item.jnGpuReferenceUse = { 112, 1000, 0x1234, 3, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceEnd;
    item.jnGpuReferenceEnd = { 113, 1000, 2000, 1, 0, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::FrameVsync;
    item.frameVsync = { 118, 9 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuCatalogControl;
    item.jnGpuCatalogControl = { 120, Generation, 1, 4,
        uint8_t( tracy::JnGpuCatalogControlKind::GenerationEnd ),
        uint8_t( tracy::JnGpuCatalogGenerationState::Complete ), 0 };
    AppendQueueItem( frame, item );

    auto* compressor = tracy::LZ4_createStream();
    test.Check( compressor != nullptr, "create GPU canonical compressor" );
    if( !compressor ) return;
    const auto compressed = CompressContinuedFrame( compressor, frame, test );
    tracy::LZ4_freeStream( compressor );
    if( compressed.empty() ) return;

    const auto source = directory / "gpu-canonical.tracy-stream";
    tracy::stream::WriterOptions writerOptions; writerOptions.durableHeader = false;
    std::string error;
    auto writer = tracy::stream::JournalWriter::CreateFileJournal(
        source, DeterministicHeader(), false, writerOptions, error );
    test.Check( writer != nullptr, "create GPU canonical journal: " + error );
    if( !writer ) return;
    test.Check( writer->Append( tracy::stream::RecordType::SessionBegin,
        tracy::stream::RecordFlagHandshake, "begin", 0, error ), "append GPU session begin" );
    tracy::WelcomeMessage welcome {};
    welcome.timerMul = 2.0;
    welcome.initBegin = 100;
    test.Check( writer->Append( tracy::stream::RecordType::ClientToServer,
        tracy::stream::RecordFlagHandshake,
        std::span<const uint8_t>( reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) ),
        1, error ), "append GPU welcome" );
    test.Check( writer->Append( tracy::stream::RecordType::ClientToServer,
        tracy::stream::RecordFlagCompressedFrame, compressed, 2, error ), "append GPU frame" );
    test.Check( writer->Append( tracy::stream::RecordType::SessionEnd,
        tracy::stream::RecordFlagTerminal, "end", 3, error ), "append GPU session end" );
    writer.reset();

    tracy::analysis::TraceSessionInventory inventory;
    tracy::analysis::TraceSessionInventoryOptions inventoryOptions;
    inventoryOptions.runDirectory = directory / "gpu-canonical-runs";
    test.Check( tracy::analysis::BuildTraceSessionInventory(
        source, inventoryOptions, inventory, error ), "inventory GPU canonical journal: " + error );
    tracy::analysis::TraceSessionCanonicalOptions canonicalOptions;
    canonicalOptions.minimumFreeReserveBytes = 0;
    canonicalOptions.minimumFreeReservePercent = 0;
    tracy::analysis::TraceSessionManifest manifest;
    const auto sessionRoot = directory / "gpu-canonical-session";
    test.Check( tracy::analysis::BuildTraceSessionCanonical( source, sessionRoot, "gpu-generation",
        inventory, canonicalOptions, manifest, error ) ==
        tracy::analysis::TraceSessionCanonicalBuildResult::Complete,
        "build GPU canonical session: " + error );
    tracy::JnTraceData gpuData;
    tracy::analysis::TraceSessionTimeTransform transform;
    tracy::analysis::TraceSessionGpuCanonicalStats stats;
    test.Check( tracy::analysis::LoadTraceSessionGpuCanonicalData(
        sessionRoot, manifest, gpuData, transform, stats, error ),
        "load GPU facts from canonical shards: " + error );
    test.Check( transform.present && transform.timerMultiplier == 2.0 && transform.baseTime == 100,
        "GPU reader restores the exact Welcome time transform" );
    test.Check( gpuData.gpuCatalogValid && gpuData.gpuCatalogResources.size() == 3 &&
        gpuData.gpuCatalogResources.front().resourceId == 10 &&
        gpuData.gpuCatalogResources.front().time == 20,
        "GPU reader validates and restores Catalog records with Worker-equivalent time" );
    test.Check( gpuData.gpuCatalogViews.size() == 2 &&
        gpuData.gpuCatalogViews[0].resourceId == 10 && gpuData.gpuCatalogViews[0].pointerToken == 0 &&
        gpuData.gpuCatalogViews[1].resourceId == 11 && gpuData.gpuCatalogViews[1].pointerToken == 0,
        "GPU reader resolves pointer reuse against the exact resource lifetime" );
    test.Check( gpuData.gpuReferencePasses.size() == 1 &&
        gpuData.gpuReferenceUses.size() == 1 && gpuData.gpuReferenceEnds.size() == 1 &&
        gpuData.gpuReferencePasses.front().time == 22 &&
        gpuData.gpuReferenceUses.front().time == 24 &&
        gpuData.gpuReferenceEnds.front().time == 26,
        "GPU reader restores exact pass/resource/end relations" );
    test.Check( stats.catalogPayloads == 2 && stats.catalogBatches == 2 &&
        stats.unresolvedPayloads == 0,
        "GPU reader consumes each Catalog payload exactly once" );

    tracy::analysis::GpuAnalysisSidecarControl gpuControl;
    gpuControl.minimumFreeBytes = 0;
    tracy::analysis::TraceSessionGpuDerivedStats derivedStats;
    const auto builtGpuDerived = tracy::analysis::BuildTraceSessionGpuAnalysisDerived(
        sessionRoot, manifest, gpuControl, derivedStats, error );
    test.Check( builtGpuDerived,
        "build mandatory GPU analysis from Session Canonical: " + error );
    const auto gpuRoot = tracy::analysis::TraceSessionGpuAnalysisRoot( sessionRoot, manifest );
    const auto gpuStore = tracy::analysis::LoadGpuAnalysisStoreManifest(
        gpuRoot / derivedStats.generation, error );
    test.Check( gpuStore.has_value() && gpuStore->complete &&
        gpuStore->resourceCount == 2 && gpuStore->passCount == 1,
        "Session generation publishes N29 GPU analysis without a duplicate raw sidecar: " + error );

    tracy::analysis::TraceSessionDerivedControl derivedControl;
    derivedControl.minimumFreeBytes = 0;
    tracy::analysis::TraceSessionDerivedStats mandatoryStats;
    test.Check( tracy::analysis::BuildTraceSessionMandatoryDerived( sessionRoot, manifest,
        inventory, derivedControl, mandatoryStats, error ),
        "build all mandatory Session indexes: " + error );
    tracy::analysis::TraceSessionDerivedStats auditedStats;
    test.Check( tracy::analysis::AuditTraceSessionFinal(
        sessionRoot, manifest, inventory, auditedStats, error ),
        "final audit covers Canonical, mandatory indexes, and GPU derived: " + error );
    test.Check( mandatoryStats.indexedRecords == auditedStats.indexedRecords &&
        auditedStats.indexedProtocolEvents == inventory.protocolInventory.eventCount &&
        auditedStats.gpuResources == 2 && auditedStats.gpuPasses == 1 &&
        auditedStats.jobTypes == 1 && auditedStats.jobs == 1 &&
        auditedStats.jobSchedules == 1 && auditedStats.jobConfigs == 1 &&
        auditedStats.jobDependencies == 0 && auditedStats.jobStages == 5,
        "mandatory derived counts are exact and reproducible" );
    manifest.auditComplete = true;
    manifest.mandatoryDerivedComplete = true;
    manifest.state = tracy::analysis::TraceSessionState::Complete;
    manifest.reason = "complete";
    const auto publishedSession = directory / "gpu-canonical-published.jn-trace-session";
    test.Check( tracy::analysis::PublishTraceSession(
        sessionRoot, publishedSession, manifest, error ),
        "publish only after mandatory derived and Final Audit: " + error );
    test.Check( tracy::analysis::IsTraceSessionQueryable( publishedSession, error ),
        "published Session is queryable: " + error );
    auto sessionGpuReader = tracy::analysis::GpuAnalysisStoreReader::OpenAt(
        tracy::analysis::TraceSessionGpuAnalysisRoot( publishedSession, manifest ),
        manifest.source.sha256, manifest.source.fileSize, error );
    test.Check( sessionGpuReader &&
        sessionGpuReader->Manifest().resourceCount == 2 &&
        sessionGpuReader->Manifest().passCount == 1,
        "open mandatory GPU analysis directly from a published Session: " + error );
    auto sessionSource = tracy::analysis::GpuAnalysisTraceSource::OpenSessionIfReady( publishedSession );
    test.Check( sessionSource &&
        sessionSource->AcquireReadView().sourceKind == tracy::analysis::TraceSourceKind::Session &&
        sessionSource->GetTraceInfo().fingerprint == manifest.source.sha256 &&
        !sessionSource->WorkerLoaded(),
        "open a published Session without materializing a full Worker" );
    const auto sessionCapabilities = sessionSource->GetCapabilities();
    const auto frameCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "frame"; } );
    test.Check( frameCapability != sessionCapabilities.end() && frameCapability->present &&
        frameCapability->indexed && frameCapability->queryable,
        "Session advertises Frame only after its disk-backed semantic reader is ready" );
    const auto jobCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "job"; } );
    test.Check( jobCapability != sessionCapabilities.end() && jobCapability->present &&
        jobCapability->indexed && jobCapability->queryable,
        "Session advertises Job only after its disk-backed semantic reader is ready" );
    const auto memoryCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "memory"; } );
    const bool sessionMemoryReady = memoryCapability != sessionCapabilities.end() &&
        memoryCapability->present && memoryCapability->indexed && memoryCapability->queryable;
    test.Check( sessionMemoryReady,
        "Session advertises Memory only after its disk-backed semantic reader is ready" );
    const auto sampleCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "sample"; } );
    const bool sessionSamplesReady = sampleCapability != sessionCapabilities.end() &&
        sampleCapability->present && sampleCapability->indexed && sampleCapability->queryable;
    test.Check( sessionSamplesReady,
        "Session advertises Sampling only after its disk-backed semantic reader is ready" );
    const auto schedulingCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "context_switch"; } );
    const bool sessionSchedulingReady = schedulingCapability != sessionCapabilities.end() &&
        schedulingCapability->present && schedulingCapability->indexed && schedulingCapability->queryable;
    test.Check( sessionSchedulingReady,
        "Session advertises Context Switch only after its disk-backed semantic reader is ready" );
    const auto sessionFrameSets = sessionSource->GetFrameSets();
    test.Check( sessionFrameSets.size() == 1 && sessionFrameSets[0].name == "Vsync 9" &&
        sessionFrameSets[0].continuous && sessionFrameSets[0].frameCount == 2 &&
        sessionFrameSets[0].completeFrameCount == 2,
        "Session Frame index restores the exact Vsync set and completeness" );
    const auto sessionFrames = sessionSource->GetFramesForSet( 0, 0, 3 );
    const auto sessionDurations = sessionSource->GetFrameDurations( 0 );
    test.Check( sessionFrames.size() == 2 && sessionFrames[0].beginNs == 12 &&
        sessionFrames[0].endNs == 36 && sessionFrames[1].beginNs == 36 &&
        sessionFrames[1].endNs == 40 && sessionDurations == std::vector<int64_t>( { 24, 4 } ),
        "Session Frame reader applies the Welcome transform and closes the offline tail at last semantic time" );
    const auto sessionJobs = sessionSource->GetJobs();
    test.Check( sessionJobs.size() == 1 && sessionJobs[0].jobId == 500 &&
        sessionJobs[0].name == "SyntheticJob" && sessionJobs[0].scheduleNs == 16 &&
        sessionJobs[0].readyNs == 20 && sessionJobs[0].firstRunNs == 22 &&
        sessionJobs[0].completedNs == 32 && sessionJobs[0].executionNs == 8 &&
        sessionJobs[0].scheduleCallsiteId == 77 && sessionJobs[0].scheduleCallstack == 2 &&
        sessionJobs[0].scheduleStackProvenance == "SiteReused" &&
        sessionJobs[0].stages.size() == 5,
        "Session Job reader restores Schedule, Ready, Worker Slice and Complete semantics" );
    if( sessionMemoryReady )
    {
        const auto sessionMemoryPools = sessionSource->GetMemoryPools();
        const auto sessionMemoryEvents = sessionSource->ScanMemoryEvents( {} );
        test.Check( sessionMemoryPools.size() == 2 && sessionMemoryPools[0].name == "Default allocator" &&
            sessionMemoryPools[0].eventCount == 3 && sessionMemoryPools[0].activeCount == 2 &&
            sessionMemoryPools[0].activeBytes == 8192 && sessionMemoryPools[0].freeCount == 1,
            "Session Memory reader restores pool totals and open allocations" );
        test.Check( sessionMemoryPools[1].name == "GPU D3D12 Texture" && sessionMemoryPools[1].gpuD3D12 &&
            sessionMemoryPools[1].eventCount == 1 && sessionMemoryPools[1].activeCount == 0 &&
            sessionMemoryPools[1].freeCount == 1,
            "Session Memory reader resolves named GPU D3D12 pools" );
        test.Check( sessionMemoryEvents.size() == 4 && sessionMemoryEvents[0].address == "0xdead" &&
            sessionMemoryEvents[0].size == 4096 && sessionMemoryEvents[0].allocationNs == 18 &&
            sessionMemoryEvents[0].freeNs == 22 && sessionMemoryEvents[0].allocationCallstack == 0 &&
            sessionMemoryEvents[1].address == "0xdead" && sessionMemoryEvents[1].allocationNs == 24 &&
            !sessionMemoryEvents[1].freeNs && sessionMemoryEvents[1].allocationCallstack == 0 &&
            sessionMemoryEvents[2].address == "0xbeef" && sessionMemoryEvents[2].allocationNs == 26 &&
            !sessionMemoryEvents[2].freeNs && sessionMemoryEvents[2].allocationCallstack == 0 &&
            sessionMemoryEvents[3].address == "77" && sessionMemoryEvents[3].allocationNs == 28 &&
            sessionMemoryEvents[3].freeNs == 30 && sessionMemoryEvents[3].allocationCallstack == 4 &&
            sessionMemoryEvents[3].freeCallstack == 5,
            "Session Memory reader preserves serial time, address reuse, SiteReuse and exact free callstacks" );
        const auto memorySnapshot = sessionSource->GetMemoryFrameSnapshot( 0, 0, {}, false );
        test.Check( memorySnapshot.valid && memorySnapshot.begin == 12 && memorySnapshot.end == 36 &&
            memorySnapshot.total.allocatedBytes == 13312 && memorySnapshot.total.freedBytes == 5120 &&
            memorySnapshot.total.endBytes == 8192 && memorySnapshot.activeAtEnd.size() == 2,
            "Session Memory reader builds the exact first-frame allocation snapshot" );
    }
    if( sessionSamplesReady )
    {
        const auto samples = sessionSource->ScanSampleEvents( {} );
        test.Check( samples.size() == 2 && samples[0].timeNs == 14 &&
            samples[0].threadRef == sessionSource->MakeEntityRef( "thread", 42 ) &&
            samples[0].callstack == 1 && samples[0].kind == "sample" &&
            samples[1].timeNs == 16 && samples[1].callstack == 1 &&
            samples[1].kind == "context_switch",
            "Session Sampling reader restores dictionary callstacks and shared context time" );
    }
    if( sessionSchedulingReady )
    {
        const auto contexts = sessionSource->ScanContextSwitchEvents( {} );
        const auto cpuContexts = sessionSource->ScanCpuContextSwitchEvents( {} );
        test.Check( contexts.size() == 2 && contexts[0].startNs == 18 &&
            contexts[0].endNs == 28 && contexts[0].wakeupNs == 18 &&
            contexts[0].cpu == 0 && contexts[0].wakeupCpu == 0 &&
            contexts[0].reason == 5 && contexts[0].state == 5 &&
            contexts[1].startNs == 28 && contexts[1].endNs == 36 &&
            contexts[1].wakeupNs == 22 && contexts[1].wakeupCpu == 1 &&
            contexts[1].reason == 4 && contexts[1].state == 1,
            "Session Context Switch reader restores wakeup, run and wait-state semantics" );
        test.Check( cpuContexts.size() == 2 && cpuContexts[0].startNs == 18 &&
            cpuContexts[0].endNs == 28 && cpuContexts[0].threadRef ==
                sessionSource->MakeEntityRef( "thread", 42 ) &&
            cpuContexts[1].startNs == 28 && cpuContexts[1].endNs == 36 &&
            cpuContexts[1].threadRef == sessionSource->MakeEntityRef( "thread", 43 ),
            "Session CPU scheduling reader restores exact CPU occupancy intervals" );
    }
    const auto corruptShard = std::find_if( manifest.shards.begin(), manifest.shards.end(),
        []( const auto& shard ) { return shard.domain != "checkpoint"; } );
    test.Check( corruptShard != manifest.shards.end(), "lazy checksum fixture has a Canonical data shard" );
    if( corruptShard != manifest.shards.end() )
    {
        std::ofstream damaged( publishedSession / corruptShard->relativePath,
            std::ios::binary | std::ios::app );
        damaged.put( '\x7f' );
        damaged.close();
        test.Check( tracy::analysis::IsTraceSessionQueryable( publishedSession, error ),
            "Session open stays O(manifest) after publication instead of hashing every unused shard" );
        CanonicalReadState rejected;
        test.Check( !tracy::analysis::VisitTraceSessionCanonicalShard( publishedSession,
            *corruptShard, CountCanonicalRecord, &rejected, error ),
            "the first real read still rejects a corrupted immutable Canonical shard" );
        test.Check( error == "session_shard_size_mismatch" ||
            error == "session_shard_sha256_mismatch",
            "lazy payload verification reports an explicit integrity failure: " + error );
    }

    tracy::query::SessionManager sessions( { directory } );
    tracy::query::QueryService query( sessions );
    const auto open = query.Execute( {
        { "protocol", "tracy-query/1" }, { "id", "session-open" }, { "method", "trace.open" },
        { "params", { { "path", publishedSession.string() } } }
    } );
    test.Check( open.value( "ok", false ), "Query 1.34 accepts a completed Session directory" );
    if( open.value( "ok", false ) )
    {
        const auto traceId = open["data"]["trace_id"].get<std::string>();
        test.Check( sessions.WaitReady( traceId, std::chrono::seconds( 5 ) ).state ==
            tracy::analysis::TraceSourceState::Ready,
            "SessionTraceSource becomes ready without full Worker materialization" );
        const auto frameSets = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-frame-sets" }, { "method", "frame.sets" },
            { "params", { { "trace_id", traceId } } }
        } );
        test.Check( frameSets.value( "ok", false ) &&
            frameSets["data"]["frame_sets"].size() == 1 &&
            frameSets["data"]["frame_sets"][0]["name"] == "Vsync 9" &&
            frameSets["data"]["frame_sets"][0]["frame_count"] == 2,
            "Query 1.34 reads Session Frame sets from the disk semantic index" );
        const auto frames = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-frame-list" }, { "method", "frame.list" },
            { "params", { { "trace_id", traceId }, { "frame_set", 0 } } }
        } );
        test.Check( frames.value( "ok", false ) && frames["data"]["frames"].size() == 2 &&
            frames["data"]["frames"][0]["begin_ns"] == "12" &&
            frames["data"]["frames"][0]["end_ns"] == "36" &&
            frames["data"]["frames"][1]["duration_ns"] == "4",
            "Query 1.34 preserves Session Frame timing and pagination semantics" );
        const auto jobs = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-job-search" }, { "method", "job.search" },
            { "params", { { "trace_id", traceId }, { "query", "SyntheticJob" } } }
        } );
        test.Check( jobs.value( "ok", false ) && jobs["data"]["jobs"].size() == 1 &&
            jobs["data"]["jobs"][0]["job_id"] == "500" &&
            jobs["data"]["jobs"][0]["name"] == "SyntheticJob",
            "Query 1.34 searches the Session Job semantic index without a Worker" );
        {
            std::ofstream switched( publishedSession / "CURRENT", std::ios::binary | std::ios::trunc );
            switched << "newer-generation-published-after-trace-open\n";
        }
        const auto peak = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-peak" }, { "method", "gpu.memory.peak" },
            { "params", { { "trace_id", traceId } } }
        } );
        test.Check( peak.value( "ok", false ) && peak["trace"]["source_kind"] == "session" &&
            peak["data"]["present"] == true &&
            peak["data"]["analysis_backend"] == "n29_gpu_resource_analysis_sidecar",
            "Query reads exact GPU analysis from its pinned Session generation after CURRENT changes" );
        const auto capabilities = sessionSource->GetCapabilities();
        const auto cpuZones = std::find_if( capabilities.begin(), capabilities.end(), []( const auto& value ) {
            return value.domain == "zone.cpu";
        } );
    test.Check( cpuZones != capabilities.end() && cpuZones->present && cpuZones->queryable && cpuZones->indexed,
        "Session advertises CPU zones only after its disk-backed semantic reader is ready" );
    const auto gpuZones = std::find_if( capabilities.begin(), capabilities.end(), []( const auto& value ) {
        return value.domain == "zone.gpu";
    } );
    const bool sessionGpuZonesReady = gpuZones != capabilities.end() && gpuZones->present &&
        gpuZones->queryable && gpuZones->indexed;
    test.Check( sessionGpuZonesReady,
        "Session advertises GPU Zone only after its disk-backed semantic reader is ready" );
        const auto cpuSearch = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-cpu-zone" }, { "method", "zone.cpu.search" },
            { "params", { { "trace_id", traceId } } }
        } );
        test.Check( cpuSearch.value( "ok", false ) && cpuSearch["data"]["zones"].size() == 3 &&
            cpuSearch["data"]["zones"][0]["name"] == "Parent Override" &&
            cpuSearch["data"]["zones"][0]["start_ns"] == "8" &&
            cpuSearch["data"]["zones"][0]["end_ns"] == "28" &&
            cpuSearch["data"]["zones"][0]["self_time_ns"] == "16" &&
            cpuSearch["data"]["zones"][0]["child_count"] == 1 &&
            cpuSearch["data"]["zones"][0]["callsite_id"] == 78 &&
            cpuSearch["data"]["zones"][0]["callstack"] == "3" &&
            cpuSearch["data"]["zones"][0]["provenance"] == "SiteReused" &&
            cpuSearch["data"]["zones"][0]["extra_text"] == "Parent text" &&
            cpuSearch["data"]["zones"][1]["function"] == "ChildFunction" &&
            cpuSearch["data"]["zones"][1]["start_ns"] == "16" &&
            cpuSearch["data"]["zones"][1]["end_ns"] == "20" &&
            cpuSearch["data"]["zones"][2]["start_ns"] == "32" &&
            cpuSearch["data"]["zones"][2]["end_ns"].is_null() &&
            cpuSearch["data"]["zones"][2]["complete"] == false,
            "Query 1.34 preserves Session CPU-zone hierarchy, shared delta clock, source and SiteReuse semantics" );
        if( sessionGpuZonesReady ) try
        {
            const auto gpuContexts = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-gpu-context" }, { "method", "zone.gpu.contexts" },
                { "params", { { "trace_id", traceId } } }
            } );
            const auto gpuSearch = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-gpu-zone" }, { "method", "zone.gpu.search" },
                { "params", { { "trace_id", traceId } } }
            } );
            test.Check( gpuContexts.value( "ok", false ) && gpuContexts["data"]["contexts"].size() == 1 &&
                gpuContexts["data"]["contexts"][0]["type_name"] == "direct3d12" &&
                gpuContexts["data"]["contexts"][0]["zone_count"] == "1" &&
                gpuSearch.value( "ok", false ) && gpuSearch["data"]["zones"].size() == 1 &&
                gpuSearch["data"]["zones"][0]["name"] == "Synthetic GPU Zone" &&
                gpuSearch["data"]["zones"][0]["gpu_start_ns"] == "36" &&
                gpuSearch["data"]["zones"][0]["gpu_end_ns"] == "40" &&
                gpuSearch["data"]["zones"][0]["cpu_start_ns"] == "36" &&
                gpuSearch["data"]["zones"][0]["cpu_end_ns"] == "40" &&
                gpuSearch["data"]["zones"][0]["self_time_ns"] == "4" &&
                gpuSearch["data"]["zones"][0]["query_id"] == 7 &&
                gpuSearch["data"]["zones"][0]["complete"] == true,
                "Query 1.34 restores Session GPU timestamp, CPU submission, Context and Query ID semantics" );
        }
        catch( const std::exception& exception )
        {
            test.Check( false, std::string( "Query 1.34 Session GPU Zone Reader is unavailable: " ) + exception.what() );
        }
        if( sessionMemoryReady ) try
        {
            const auto memoryPools = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-memory-pools" }, { "method", "memory.pools" },
                { "params", { { "trace_id", traceId } } }
            } );
            const auto memoryEvents = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-memory-events" }, { "method", "memory.events" },
                { "params", { { "trace_id", traceId } } }
            } );
            test.Check( memoryPools.value( "ok", false ) && memoryPools["data"]["pools"].size() == 2 &&
                memoryPools["data"]["pools"][0]["active_bytes"] == "8192" &&
                memoryPools["data"]["pools"][1]["name"] == "GPU D3D12 Texture" &&
                memoryEvents.value( "ok", false ) && memoryEvents["data"]["events"].size() == 4 &&
                memoryEvents["data"]["events"][0]["allocation_callstack"].is_null() &&
                memoryEvents["data"]["events"][1]["complete"] == false &&
                memoryEvents["data"]["events"][3]["allocation_callstack"] == "4" &&
                memoryEvents["data"]["events"][3]["free_callstack"] == "5",
                "Query 1.34 reads Session Memory pools and lifecycle events without a Worker" );
        }
        catch( const std::exception& exception )
        {
            test.Check( false, std::string( "Query 1.34 Session Memory Reader is unavailable: " ) + exception.what() );
        }
        if( sessionSamplesReady ) try
        {
            const auto samples = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-samples" }, { "method", "sample.list" },
                { "params", { { "trace_id", traceId } } }
            } );
            test.Check( samples.value( "ok", false ) && samples["data"]["samples"].size() == 2 &&
                samples["data"]["samples"][0]["time_ns"] == "14" &&
                samples["data"]["samples"][0]["callstack"] == "1" &&
                samples["data"]["samples"][1]["kind"] == "context_switch",
                "Query 1.34 reads Session Sampling events without a Worker" );
        }
        catch( const std::exception& exception )
        {
            test.Check( false, std::string( "Query 1.34 Session Sampling Reader is unavailable: " ) + exception.what() );
        }
        if( sessionSchedulingReady ) try
        {
            const auto contexts = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-contexts" },
                { "method", "context_switch.range" }, { "params", { { "trace_id", traceId } } }
            } );
            test.Check( contexts.value( "ok", false ) &&
                contexts["data"]["context_switches"].size() == 2 &&
                contexts["data"]["context_switches"][1]["wakeup_ns"] == "22" &&
                contexts["data"]["context_switches"][1]["reason_name"] == "delay_execution",
                "Query 1.34 reads Session Context Switch intervals without a Worker" );
        }
        catch( const std::exception& exception )
        {
            test.Check( false, std::string( "Query 1.34 Session Context Switch Reader is unavailable: " ) + exception.what() );
        }
    }
    const auto frameFile = tracy::analysis::TraceSessionFrameIndexRoot(
        publishedSession, manifest ) / "frames.bin";
    {
        std::fstream damaged( frameFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x5a );
        damaged.write( &byte, 1 );
    }
    test.Check( !tracy::analysis::TraceSessionFrameReader::Open(
        publishedSession, manifest, error ) && error == "session_frame_file_sha256_mismatch",
        "Session Frame reader rejects a corrupted committed semantic index" );
    const auto jobFile = tracy::analysis::TraceSessionJobIndexRoot(
        publishedSession, manifest ) / "jobs.bin";
    {
        std::fstream damaged( jobFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x3c );
        damaged.write( &byte, 1 );
    }
    test.Check( !tracy::analysis::TraceSessionJobReader::Open(
        publishedSession, manifest, error ) && error == "session_job_file_sha256_mismatch",
        "Session Job reader rejects a corrupted committed semantic index" );
    const auto memoryFile = tracy::analysis::TraceSessionMemoryIndexRoot(
        publishedSession, manifest ) / "memory.bin";
    {
        std::fstream damaged( memoryFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x69 );
        damaged.write( &byte, 1 );
    }
    tracy::analysis::TraceSessionMemoryStats rejectedMemoryStats;
    test.Check( !tracy::analysis::AuditTraceSessionMemoryDerived(
        publishedSession, manifest, rejectedMemoryStats, error ) &&
        error == "session_memory_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed Memory semantic index" );
    const auto gpuZoneFile = tracy::analysis::TraceSessionGpuZoneIndexRoot(
        publishedSession, manifest ) / "gpu-zones.bin";
    {
        std::fstream damaged( gpuZoneFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x71 );
        damaged.write( &byte, 1 );
    }
    tracy::analysis::TraceSessionGpuZoneStats rejectedGpuZoneStats;
    test.Check( !tracy::analysis::AuditTraceSessionGpuZoneDerived(
        publishedSession, manifest, rejectedGpuZoneStats, error ) &&
        error == "session_gpu_zone_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed GPU Zone semantic index" );
    const auto samplingFile = tracy::analysis::TraceSessionSamplingIndexRoot(
        publishedSession, manifest ) / "samples.bin";
    {
        std::fstream damaged( samplingFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x2d );
        damaged.write( &byte, 1 );
    }
    tracy::analysis::TraceSessionSamplingStats rejectedSamplingStats;
    test.Check( !tracy::analysis::AuditTraceSessionSamplingDerived(
        publishedSession, manifest, rejectedSamplingStats, error ) &&
        error == "session_sampling_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed Sampling semantic index" );
    const auto schedulingFile = tracy::analysis::TraceSessionSchedulingIndexRoot(
        publishedSession, manifest ) / "scheduling.bin";
    {
        std::fstream damaged( schedulingFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x47 );
        damaged.write( &byte, 1 );
    }
    tracy::analysis::TraceSessionSchedulingStats rejectedSchedulingStats;
    test.Check( !tracy::analysis::AuditTraceSessionSchedulingDerived(
        publishedSession, manifest, rejectedSchedulingStats, error ) &&
        error == "session_scheduling_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed Scheduling semantic index" );
}

}

int main()
{
    TestContext test;
    const auto directory = UniqueTestDirectory();
    std::error_code filesystemError;
    std::filesystem::create_directories( directory, filesystemError );
    test.Check( !filesystemError, "create test directory" );
    if( !filesystemError )
    {
        TestCompleteInventory( test, directory );
        TestDegradedCaptureEnd( test, directory );
        TestRecoverableTail( test, directory );
        TestBoundedMetadata( test, directory );
        TestCapacityPreflight( test );
        TestAllProtocolQueueTypesAreCanonicalFacts( test );
        TestProtocolFrameInventory( test );
        TestCompressedProtocolInventory( test );
        TestProtocolDecoderCheckpoint( test );
        TestProtocolJournalInventory( test, directory );
        TestGpuCanonicalReader( test, directory );
    }
    std::filesystem::remove_all( directory, filesystemError );
    test.Check( !filesystemError, "remove test directory" );
    if( test.failed != 0 )
    {
        std::cerr << test.failed << " inventory tests failed\n";
        return 1;
    }
    std::cout << "Trace Session Inventory tests passed\n";
    return 0;
}
