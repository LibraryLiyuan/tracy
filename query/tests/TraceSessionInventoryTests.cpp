#include "TracyTraceSessionInventory.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyTraceSessionDerived.hpp"
#include "TracyGpuAnalysisStore.hpp"
#include "TracyGpuAnalysisTraceSource.hpp"
#include "TracyTraceSessionMemory.hpp"
#include "TracyTraceSessionSampling.hpp"
#include "TracyTraceSessionScheduling.hpp"
#include "TracyTraceSessionRelations.hpp"
#include "TracyTraceSessionRuntime.hpp"
#include "TracyTraceSessionIoGfx.hpp"
#include "TracyTraceSessionGpuZones.hpp"
#include "TracyTraceSessionFrameImages.hpp"
#include "TracyTraceSessionJobs.hpp"
#include "TracyTraceSessionCpuZones.hpp"
#include "TracyQueryService.hpp"
#include "TracyTraceSessionProtocolInventory.hpp"
#include "TracyJnGpuCatalogResolve.hpp"

#include "TracyStreamJournal.hpp"
#include "TracyProtocolObserver.hpp"
#include "TracyForceInline.hpp"
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

void TestSharedGpuPointerIdentityOutlivesOwnership( TestContext& test )
{
    constexpr uint64_t Generation = 7;
    constexpr uint64_t Pointer = 0xCAFE;
    tracy::JnTraceData data;

    tracy::JnGpuCatalogResourceRecordV1 first {};
    first.time = 10;
    first.resourceId = 100;
    first.pointerToken = Pointer;
    first.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    first.exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    data.gpuCatalogResources.push_back( first );

    auto destroyed = first;
    destroyed.time = 20;
    destroyed.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Destroy );
    data.gpuCatalogResources.push_back( destroyed );

    auto replacement = first;
    replacement.time = 30;
    replacement.resourceId = 101;
    replacement.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Snapshot );
    replacement.exactness = uint8_t( tracy::JnGpuCatalogExactness::OpenBoundary );
    data.gpuCatalogResources.push_back( replacement );

    data.gpuCatalogBatches.push_back( { Generation, 0, 0, 0, 1, 3,
        uint32_t( 3 * sizeof( tracy::JnGpuCatalogResourceRecordV1 ) ),
        uint8_t( tracy::JnGpuCatalogBatchKind::Resource ), 1, 0, 1 } );

    tracy::JnGpuReferencePassData beforeReplacement {};
    beforeReplacement.time = 25;
    beforeReplacement.passId = 1000;
    data.gpuReferencePasses.push_back( beforeReplacement );
    auto afterReplacement = beforeReplacement;
    afterReplacement.time = 35;
    afterReplacement.passId = 1001;
    data.gpuReferencePasses.push_back( afterReplacement );

    tracy::JnGpuRangeSetRecordV1 oldUse {};
    oldUse.passInstanceId = 1000;
    oldUse.pointerToken = Pointer;
    oldUse.lengthBytes = 64;
    oldUse.rangeKind = uint8_t( tracy::JnGpuRangeKind::Buffer );
    oldUse.exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    data.gpuRangeSets.push_back( oldUse );
    auto newUse = oldUse;
    newUse.passInstanceId = 1001;
    data.gpuRangeSets.push_back( newUse );
    data.gpuCatalogBatches.push_back( { Generation, 0, 0, 0, 2, 2,
        uint32_t( 2 * sizeof( tracy::JnGpuRangeSetRecordV1 ) ),
        uint8_t( tracy::JnGpuCatalogBatchKind::RangeSet ), 1, 0, 1 } );

    const auto result = tracy::ResolveJnGpuCatalogGenerationData(
        data, Generation, []( uint64_t token ) { return token; } );
    test.Check( result.totalUnresolved == 0 && result.coreUnresolved == 0,
        "shared GPU resolver keeps exact pointer identity across ownership destroy" );
    test.Check( data.gpuRangeSets[0].resourceId == 100,
        "post-Destroy command-list use resolves to the preceding pointer generation" );
    test.Check( data.gpuRangeSets[1].resourceId == 101,
        "a later pointer definition switches subsequent uses to the replacement generation" );
}

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

struct CanonicalProgressState
{
    uint64_t calls = 0;
    uint64_t sourceBytes = 0;
    uint64_t totalSourceBytes = 0;
    uint64_t sourceRecords = 0;
    uint64_t committedShards = 0;
    bool monotonic = true;
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

void RecordCanonicalProgress( uint64_t sourceBytes, uint64_t totalSourceBytes,
    uint64_t sourceRecords, uint64_t committedShards, void* userData )
{
    auto& state = *static_cast<CanonicalProgressState*>( userData );
    if( sourceBytes < state.sourceBytes || sourceRecords < state.sourceRecords ||
        committedShards < state.committedShards || sourceBytes > totalSourceBytes )
        state.monotonic = false;
    state.calls++;
    state.sourceBytes = sourceBytes;
    state.totalSourceBytes = totalSourceBytes;
    state.sourceRecords = sourceRecords;
    state.committedShards = committedShards;
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

void TestCanonicalPackedFrameStorage( TestContext& test, const std::filesystem::path& directory )
{
    std::vector<uint8_t> frame;
    constexpr uint32_t EventCount = 1024;
    for( uint32_t i = 0; i < EventCount; ++i )
        AppendFixedEvent( frame, tracy::QueueType::FrameVsync );

    auto* compressor = tracy::LZ4_createStream();
    test.Check( compressor != nullptr, "create packed canonical compressor" );
    if( !compressor ) return;
    const auto compressed = CompressContinuedFrame( compressor, frame, test );
    tracy::LZ4_freeStream( compressor );
    if( compressed.empty() ) return;

    const auto source = directory / "canonical-packed.tracy-stream";
    tracy::stream::WriterOptions writerOptions;
    writerOptions.durableHeader = false;
    std::string error;
    auto writer = tracy::stream::JournalWriter::CreateFileJournal(
        source, DeterministicHeader(), false, writerOptions, error );
    test.Check( writer != nullptr, "create packed canonical journal: " + error );
    if( !writer ) return;
    test.Check( writer->Append( tracy::stream::RecordType::ClientToServer,
        tracy::stream::RecordFlagCompressedFrame, compressed, 1, error ),
        "append dense packed canonical frame" );
    writer.reset();

    tracy::analysis::TraceSessionInventory inventory;
    tracy::analysis::TraceSessionInventoryOptions inventoryOptions;
    inventoryOptions.runDirectory = directory / "canonical-packed-runs";
    test.Check( tracy::analysis::BuildTraceSessionInventory(
        source, inventoryOptions, inventory, error ),
        "inventory dense packed canonical frame: " + error );

    tracy::analysis::TraceSessionCanonicalOptions canonicalOptions;
    canonicalOptions.targetShardBytes = 2 * 1024 * 1024;
    canonicalOptions.softShardBytes = 3 * 1024 * 1024;
    canonicalOptions.hardShardBytes = 4 * 1024 * 1024;
    canonicalOptions.minimumShardSpanNs = 0;
    canonicalOptions.maximumShardSpanNs = 1000000000;
    tracy::analysis::TraceSessionManifest manifest;
    const auto sessionRoot = directory / "canonical-packed-session";
    test.Check( tracy::analysis::BuildTraceSessionCanonical( source, sessionRoot,
        "generation-packed", inventory, canonicalOptions, manifest, error ) ==
        tracy::analysis::TraceSessionCanonicalBuildResult::Complete,
        "build dense packed canonical frame: " + error );

    uint64_t canonicalPayloadBytes = 0;
    uint64_t canonicalLogicalRecords = 0;
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        canonicalPayloadBytes += shard.uncompressedBytes;
        canonicalLogicalRecords += shard.recordCount;
    }
    test.Check( canonicalLogicalRecords == uint64_t( EventCount ) + 1,
        "packed canonical manifest preserves logical event and frame counts" );
    test.Check( canonicalPayloadBytes <= frame.size() + 4096,
        "packed canonical physical bytes scale with decoded frame bytes, not per-event headers" );

    CanonicalReadState read;
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        test.Check( tracy::analysis::VisitTraceSessionCanonicalShard( sessionRoot, shard,
            CountCanonicalRecord, &read, error ), "read dense packed canonical frame: " + error );
    }
    test.Check( read.records == canonicalLogicalRecords &&
        read.domains[size_t( tracy::analysis::TraceSessionProtocolDomain::Frame )] == EventCount,
        "packed canonical reader lazily restores every logical event" );

    tracy::analysis::TraceSessionCanonicalAudit audit;
    test.Check( tracy::analysis::AuditTraceSessionCanonical(
        sessionRoot, manifest, inventory, audit, error ),
        "audit dense packed canonical frame: " + error );
    test.Check( audit.protocolEvents == EventCount && audit.protocolFrames == 1 &&
        audit.protocolEncodedBytes == frame.size(),
        "packed canonical audit remains byte-for-byte equivalent to inventory" );
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
    CanonicalProgressState canonicalProgress;
    canonicalOptions.progress = RecordCanonicalProgress;
    canonicalOptions.progressUserData = &canonicalProgress;
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
    test.Check( tracy::analysis::CanResumeTraceSessionCanonical(
        sessionRoot, manifest, error ),
        "packed checkpoint is selected as a compatible resumable generation: " + error );
    const auto cancelledShards = manifest.shards;

    const auto legacyRoot = directory / "canonical-legacy-encoding";
    std::vector<uint8_t> legacyCheckpoint = {
        'J', 'N', 'C', 'H', 'K', 'P', 'T', '1',
        1, 0, 0, 0, 0, 0, 0, 0
    };
    tracy::analysis::TraceSessionShard legacyCheckpointShard;
    legacyCheckpointShard.shardId = 1;
    legacyCheckpointShard.domain = "checkpoint";
    legacyCheckpointShard.recordCount = 1;
    test.Check( tracy::analysis::WriteTraceSessionShard( legacyRoot, "legacy",
        legacyCheckpointShard, legacyCheckpoint.data(), legacyCheckpoint.size(), error ),
        "write legacy Canonical checkpoint fixture: " + error );
    tracy::analysis::TraceSessionManifest legacyManifest;
    legacyManifest.generation = "legacy";
    legacyManifest.shards.push_back( legacyCheckpointShard );
    test.Check( !tracy::analysis::CanResumeTraceSessionCanonical(
        legacyRoot, legacyManifest, error ) &&
        error == "canonical_packed_encoding_unsupported",
        "resume selection rejects the preserved pre-packed experimental generation quickly" );

    canonicalOptions.shouldCancel = nullptr;
    canonicalOptions.cancelUserData = nullptr;
    test.Check( tracy::analysis::BuildTraceSessionCanonical( path, sessionRoot, "generation-1",
        inventory, canonicalOptions, manifest, error ) ==
        tracy::analysis::TraceSessionCanonicalBuildResult::Complete,
        "resume canonical session from checkpoint: " + error );
    test.Check( manifest.state == tracy::analysis::TraceSessionState::CanonicalBuilding,
        "completed canonical facts remain in canonical-building state until derived work" );
    manifest.state = tracy::analysis::TraceSessionState::DerivedFailed;
    manifest.reason = "synthetic_derived_failure";
    test.Check( tracy::analysis::CanReuseCompletedTraceSessionCanonical(
        sessionRoot, manifest, inventory, error ),
        "a Derived failure reuses the fully committed Canonical generation instead of resuming Canonical: " + error );
    manifest.state = tracy::analysis::TraceSessionState::CanonicalBuilding;
    manifest.reason.clear();
    test.Check( canonicalProgress.calls > 0 && canonicalProgress.monotonic &&
        canonicalProgress.sourceBytes == canonicalProgress.totalSourceBytes &&
        canonicalProgress.sourceRecords == inventory.recordCount &&
        canonicalProgress.committedShards == manifest.shards.size(),
        "canonical progress reports committed monotonic source and shard coverage" );
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
    bool sawControlDomain = false;
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) checkpointRecords += shard.recordCount;
        else
        {
            canonicalRecords += shard.recordCount;
            sawControlDomain |= shard.domain == "control";
            test.Check( shard.domain == "control",
                "packed canonical keeps each protocol frame in one physical source-order shard" );
        }
    }
    test.Check( canonicalRecords == inventory.protocolInventory.eventCount + inventory.recordCount,
        "canonical records cover decoded events and non-compressed transport records" );
    test.Check( checkpointRecords > 0, "each committed canonical segment has a checkpoint" );
    test.Check( sawControlDomain,
        "packed canonical persists source frames without duplicating bytes across domains" );
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
        []( const auto& shard ) { return shard.domain == "control"; } );
    test.Check( omitted != incompleteManifest.shards.end(), "audit omission fixture has packed data shard" );
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
    limitedOptions.softMemoryBytes = 72;
    limitedOptions.hardMemoryBytes = 80;
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

tracy_no_inline void TestSessionPlotTraceSource( TestContext& test,
    tracy::analysis::GpuAnalysisTraceSource& source )
{
    const auto capabilities = source.GetCapabilities();
    const auto plotCapability = std::find_if( capabilities.begin(), capabilities.end(),
        []( const auto& value ) { return value.domain == "plot"; } );
    test.Check( plotCapability != capabilities.end() && plotCapability->present &&
        plotCapability->indexed && plotCapability->queryable,
        "Session advertises Plot only after its disk-backed semantic reader is ready" );
    const auto plots = source.GetPlotList();
    const auto points = source.ScanPlots( {} );
    test.Check( plots.size() == 1 && plots[0].name == "Synthetic Plot" &&
        plots[0].pointCount == 1 && plots[0].min == 1 && plots[0].max == 1 &&
        plots[0].sum == 1 && points.size() == 1 && points[0].plotRef == plots[0].ref &&
        points[0].value == 1,
        "Session Plot reader preserves exact names, aggregates, and points without a Worker" );
}

tracy_no_inline void TestSessionPlotQuery( TestContext& test, tracy::query::QueryService& query,
    const std::string& traceId, tracy::analysis::GpuAnalysisTraceSource& source )
{
    const auto plots = source.GetPlotList();
    const auto plotRef = plots.empty() ? std::string {} : plots[0].ref;
    const auto plotList = query.Execute( {
        { "protocol", "tracy-query/1" }, { "id", "session-plot-list" }, { "method", "plot.list" },
        { "params", { { "trace_id", traceId } } }
    } );
    const auto plotPoints = query.Execute( {
        { "protocol", "tracy-query/1" }, { "id", "session-plot-points" }, { "method", "plot.points" },
        { "params", { { "trace_id", traceId }, { "plot_ref", plotRef } } }
    } );
    test.Check( plotList.value( "ok", false ) && plotList["data"]["plots"].size() == 1 &&
        plotList["data"]["plots"][0]["name"] == "Synthetic Plot" &&
        plotList["data"]["plots"][0]["point_count"] == "1" &&
        plotPoints.value( "ok", false ) && plotPoints["data"]["points"].size() == 1 &&
        plotPoints["data"]["points"][0]["value"] == 1,
        "Query 1.34 reads exact Session Plot evidence without a Worker" );
}

tracy_no_inline void TestSessionMessageTraceSource( TestContext& test,
    tracy::analysis::GpuAnalysisTraceSource& source )
{
    const auto capabilities = source.GetCapabilities();
    const auto capability = std::find_if( capabilities.begin(), capabilities.end(),
        []( const auto& value ) { return value.domain == "message"; } );
    std::vector<tracy::analysis::MessageDto> messages;
    try { messages = source.ScanMessages( {} ); } catch( ... ) {}
    const auto plain = std::find_if( messages.begin(), messages.end(),
        []( const auto& value ) { return value.text == "Synthetic message"; } );
    const auto stacked = std::find_if( messages.begin(), messages.end(),
        []( const auto& value ) { return value.text == "Stacked evidence"; } );
    const auto managed = std::find_if( messages.begin(), messages.end(),
        []( const auto& value ) { return value.text == "Managed stacked evidence"; } );
    const auto literal = std::find_if( messages.begin(), messages.end(),
        []( const auto& value ) { return value.text == "Late literal evidence"; } );
    test.Check( capability != capabilities.end() && capability->present &&
        capability->indexed && capability->queryable && messages.size() == 4 &&
        plain != messages.end() && plain->color == 0xFFFFFFFF && plain->callstack == 0 &&
        stacked != messages.end() && stacked->callstack == 3 && stacked->callstackRef &&
        managed != messages.end() && managed->callstack == 4 && managed->callstackRef &&
        literal != messages.end() && literal->color == 0xFF332211 && literal->callstack == 0 &&
        !stacked->threadRef.empty(),
        "Session Message reader preserves direct/late-literal text, thread, color, and callstack semantics without a Worker" );
}

tracy_no_inline void TestSessionMessageQuery( TestContext& test,
    tracy::query::QueryService& query, const std::string& traceId )
{
    const auto response = query.Execute( {
        { "protocol", "tracy-query/1" }, { "id", "session-message-search" },
        { "method", "message.search" }, { "params", {
            { "trace_id", traceId }, { "filter", {
                { "text", "Synthetic message" }, { "mode", "exact" } } } } }
    } );
    test.Check( response.value( "ok", false ) &&
        response["data"]["messages"].size() == 1 &&
        response["data"]["messages"][0]["text"] == "Synthetic message" &&
        response["data"]["messages"][0]["color"] == 0xFFFFFFFF,
        "Query 1.34 reads exact Session Message evidence without a Worker" );
}

tracy_no_inline void TestSessionLockTraceSource( TestContext& test,
    tracy::analysis::GpuAnalysisTraceSource& source )
{
    const auto capabilities = source.GetCapabilities();
    const auto capability = std::find_if( capabilities.begin(), capabilities.end(),
        []( const auto& value ) { return value.domain == "lock"; } );
    std::vector<tracy::analysis::LockDto> locks;
    std::vector<tracy::analysis::LockEventDto> events;
    try { locks = source.GetLocks(); events = source.ScanLockEvents( {} ); } catch( ... ) {}
    test.Check( capability != capabilities.end() && capability->present &&
        capability->indexed && capability->queryable && locks.size() == 1 &&
        locks[0].nativeId == 700 && locks[0].name == "Synthetic Lock" &&
        locks[0].customName && *locks[0].customName == "Synthetic Lock" &&
        locks[0].eventCount == 3 && locks[0].threadCount == 1 &&
        locks[0].valid && !locks[0].contended && locks[0].terminateNs &&
        !locks[0].sourceLocationRef.empty() && events.size() == 3 &&
        events[0].type == "wait" && events[1].type == "obtain" &&
        events[1].lockCount == 1 && events[1].ownerThreadRef &&
        events[2].type == "release" && events[2].lockCount == 0,
        "Session Lock reader preserves announce/name/lifecycle and lock-state semantics without a Worker" );
}

tracy_no_inline void TestSessionLockQuery( TestContext& test,
    tracy::query::QueryService& query, const std::string& traceId )
{
    const auto locks = query.Execute( {
        { "protocol", "tracy-query/1" }, { "id", "session-lock-list" },
        { "method", "lock.list" }, { "params", {
            { "trace_id", traceId }, { "text", "Synthetic Lock" },
            { "mode", "exact" } } }
    } );
    const auto timeline = query.Execute( {
        { "protocol", "tracy-query/1" }, { "id", "session-lock-timeline" },
        { "method", "lock.timeline" }, { "params", {
            { "trace_id", traceId } } }
    } );
    test.Check( locks.value( "ok", false ) && locks["data"]["locks"].size() == 1 &&
        locks["data"]["locks"][0]["native_id"] == 700 &&
        timeline.value( "ok", false ) && timeline["data"]["events"].size() == 3 &&
        timeline["data"]["events"][0]["type"] == "wait" &&
        timeline["data"]["events"][1]["type"] == "obtain" &&
        timeline["data"]["events"][2]["type"] == "release",
        "Query 1.34 reads exact Session Lock definitions and timeline without a Worker" );
}

void TestGpuCanonicalReader( TestContext& test, const std::filesystem::path& directory )
{
    constexpr uint64_t Generation = 7;
    constexpr uint64_t PayloadId = 99;
    std::array<tracy::JnGpuCatalogResourceRecordV1, 11> resources {};
    // A bootstrap Snapshot has an open-left lifetime even when its transport
    // timestamp follows a pass/use that was recorded while bootstrap was in
    // flight.  The Session resolver must preserve that boundary semantics.
    resources[0].time = 112;
    resources[0].resourceId = 10;
    resources[0].pointerToken = 0x1234;
    resources[0].allocationId = 20;
    resources[0].capacityBytes = 4096;
    resources[0].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Snapshot );
    resources[0].exactness = uint8_t( tracy::JnGpuCatalogExactness::OpenBoundary );
    resources[1] = resources[0];
    resources[1].time = 114;
    resources[1].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Destroy );
    resources[2] = resources[0];
    resources[2].time = 115;
    resources[2].resourceId = 11;
    resources[2].allocationId = 21;
    resources[2].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    resources[2].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    // A normal Create can also occur after Begin and before End.  N29 resolves
    // the pass set at End; Begin-time resolution would hit a false lifetime gap.
    resources[3] = resources[0];
    resources[3].time = 112;
    resources[3].resourceId = 12;
    resources[3].pointerToken = 0x5678;
    resources[3].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    resources[3].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    // This resource is valid at its Use timestamp but is destroyed exactly at
    // PassEnd. Resolving every use at End loses valid source evidence; the
    // Session must prefer Use time and only use a unique pass-interval fallback
    // for definitions published after the Use timestamp.
    resources[4] = resources[3];
    resources[4].time = 113;
    resources[4].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Destroy );
    // An Update observed after a completed pointer generation enriches the
    // Catalog but does not open a new pointer lifetime. Treating it as Create
    // would make the following real Create falsely ambiguous.
    resources[5] = resources[0];
    resources[5].time = 114;
    resources[5].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Update );
    // A later Snapshot/Open is only open-left to the previous proven pointer
    // boundary. It must not reach through an already complete earlier
    // generation and make this pass-to-resource relation ambiguous.
    resources[6] = resources[0];
    resources[6].time = 112;
    resources[6].resourceId = 13;
    resources[6].pointerToken = 0x9999;
    resources[6].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    resources[6].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    resources[7] = resources[6];
    resources[7].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Update );
    resources[7].definitionRevision = 2;
    resources[8] = resources[6];
    resources[8].time = 116;
    resources[8].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Destroy );
    resources[9] = resources[6];
    resources[9].time = 120;
    resources[9].resourceId = 14;
    resources[9].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Snapshot );
    resources[9].exactness = uint8_t( tracy::JnGpuCatalogExactness::OpenBoundary );
    // The source can contain a reference to a resource that was alive before
    // capture but absent from the connection bootstrap. A later Create on the
    // same address proves the end of that anonymous pointer generation; it
    // must not be back-filled with the future resource identity.
    resources[10] = resources[6];
    resources[10].time = 120;
    resources[10].resourceId = 15;
    resources[10].pointerToken = 0xBEEF;
    resources[10].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    resources[10].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );

    std::array<tracy::JnGpuCatalogViewRecordV1, 2> views {};
    views[0].time = 112;
    views[0].viewId = 100;
    views[0].pointerToken = 0x1234;
    views[0].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    views[0].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    views[1] = views[0];
    views[1].time = 117;
    views[1].viewId = 101;

    // Keep one logical id hot for enough records to force the Session GPU
    // normalizer through its bounded spill path.  This guards against a single
    // pathological logical lifetime making memory proportional to trace size.
    constexpr size_t LogicalStressCount = 4096;
    std::vector<tracy::JnGpuCatalogLogicalRecordV1> logicals( LogicalStressCount );
    logicals[0].time = 112;
    logicals[0].logicalResourceId = 300;
    logicals[0].resourceId = 10;
    logicals[0].lengthBytes = 4096;
    logicals[0].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    logicals[0].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    for( size_t i = 1; i < logicals.size(); ++i )
    {
        logicals[i] = logicals[0];
        logicals[i].time += int64_t( i );
        logicals[i].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Update );
    }
    std::array<tracy::JnGpuCatalogRelationRecordV1, 1> catalogRelations {};
    catalogRelations[0].time = 112;
    catalogRelations[0].sourceId = 10;
    catalogRelations[0].targetId = 20;
    catalogRelations[0].operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    catalogRelations[0].relation = uint8_t( tracy::JnGpuCatalogRelationKind::BackedBy );
    catalogRelations[0].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    constexpr uint64_t ExplicitGpuPassId = ( uint64_t( 1 ) << 63 ) + 123;
    std::array<tracy::JnGpuRangeSetRecordV1, 1> ranges {};
    ranges[0].passInstanceId = ExplicitGpuPassId;
    ranges[0].resourceId = 10;
    ranges[0].offsetBytes = 64;
    ranges[0].lengthBytes = 256;
    ranges[0].usageMask = 1;
    ranges[0].rangeKind = uint8_t( tracy::JnGpuRangeKind::Buffer );
    ranges[0].exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );

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
    const auto logicalPayload = makeCatalogPayload( logicals.data(), uint32_t( logicals.size() ), sizeof( logicals[0] ) );
    const auto relationPayload = makeCatalogPayload( catalogRelations.data(), uint32_t( catalogRelations.size() ), sizeof( catalogRelations[0] ) );
    const auto rangePayload = makeCatalogPayload( ranges.data(), uint32_t( ranges.size() ), sizeof( ranges[0] ) );

    std::vector<uint8_t> frame;
    AppendThreadContextEvent( frame, 42 );
    AppendStringEvent( frame, tracy::QueueType::ThreadName, 42, "Main Thread" );
    tracy::QueueItem item {};
    item.hdr.type = tracy::QueueType::TidToPid;
    item.tidToPid.tid = 42;
    item.tidToPid.pid = 1001;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ThreadGroupHint;
    item.threadGroupHint.thread = 42;
    item.threadGroupHint.groupHint = 7;
    AppendQueueItem( frame, item );
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
    constexpr uint64_t HardwareSampleAddress = 0xABC;
    const std::array<std::pair<tracy::QueueType, int64_t>, 6> hardwareSamples = { {
        { tracy::QueueType::HwSampleCpuCycle, 108 },
        { tracy::QueueType::HwSampleInstructionRetired, 109 },
        { tracy::QueueType::HwSampleCacheReference, 0 },
        { tracy::QueueType::HwSampleCacheMiss, 110 },
        { tracy::QueueType::HwSampleBranchRetired, 111 },
        { tracy::QueueType::HwSampleBranchMiss, 112 }
    } };
    for( const auto& [type, time] : hardwareSamples )
    {
        item = {};
        item.hdr.type = type;
        item.hwSample.ip = HardwareSampleAddress;
        item.hwSample.time = time;
        AppendQueueItem( frame, item );
    }
    for( uint32_t cpu = 0; cpu < 2; ++cpu )
    {
        item = {};
        item.hdr.type = tracy::QueueType::CpuTopology;
        item.cpuTopology.package = 0;
        item.cpuTopology.die = 0;
        item.cpuTopology.core = 0;
        item.cpuTopology.thread = cpu;
        AppendQueueItem( frame, item );
    }
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
    // Preserve a source-side scheduling gap instead of rejecting it or
    // inventing a switch-out time. CPU 2 receives a second switch-in while
    // the first observed interval is still open and oldThread is unavailable.
    item = {};
    item.hdr.type = tracy::QueueType::ContextSwitch;
    item.contextSwitch.time = 1;
    item.contextSwitch.oldThread = 0;
    item.contextSwitch.newThread = 100;
    item.contextSwitch.cpu = 2;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ContextSwitch;
    item.contextSwitch.time = 1;
    item.contextSwitch.oldThread = 0;
    item.contextSwitch.newThread = 101;
    item.contextSwitch.cpu = 2;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ContextSwitch;
    item.contextSwitch.time = 1;
    item.contextSwitch.oldThread = 101;
    item.contextSwitch.newThread = 0;
    item.contextSwitch.cpu = 2;
    item.contextSwitch.oldThreadWaitReason = 4;
    item.contextSwitch.oldThreadState = 1;
    AppendQueueItem( frame, item );
    // More scheduler identities than the MSVC stdio handle table can keep
    // open at once.  The Session builder must use a bounded number of work
    // files instead of one permanently-open file per observed thread.
    for( uint64_t index = 0; index < 1024; ++index )
    {
        item = {};
        item.hdr.type = tracy::QueueType::ThreadWakeup;
        item.threadWakeup.time = 0;
        item.threadWakeup.thread = 0x100000 + index;
        item.threadWakeup.cpu = uint8_t( index % 32 );
        AppendQueueItem( frame, item );
    }
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
    item.jnJobSchedule = { 104, 600, 0x100000ABC, 0, 2, uint8_t( 1 << 6 ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobStage;
    item.jnJobStage = { 106, 600, 0, 0, 0,
        uint8_t( tracy::JnJobStage::Completed ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobSchedule;
    item.jnJobSchedule = { 108, 500, 0xABC, 1, 2, uint8_t( 1 << 6 ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobConfig;
    item.jnJobConfig = { 500, 17, 64, 8, 31, 9, 2, uint8_t( 1 << 6 ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnJobDependency;
    item.jnJobDependency = { 500, 600, 0x100000ABC, 0 };
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
    item = {};
    item.hdr.type = tracy::QueueType::JnRelation;
    item.jnRelation = { 117, 500, 900,
        uint8_t( tracy::JnEntityKind::Job ), uint8_t( tracy::JnEntityKind::GpuPass ),
        uint8_t( tracy::JnRelationNamespace::Job ), uint8_t( tracy::JnRelationKind::ExecutesPass ), 0 };
    AppendQueueItem( frame, item );
    constexpr uint64_t ScriptFunctionPointer = 0x7001;
    constexpr uint64_t ScriptFilePointer = 0x7002;
    constexpr uint64_t ScriptMarkerPointer = 0x7003;
    item = {};
    item.hdr.type = tracy::QueueType::JnRuntimeDomainState;
    item.jnRuntimeDomainState = { 117, 3, 20,
        uint8_t( tracy::JnRuntimeDomain::ScriptStack ), uint8_t( tracy::JnRuntimeMode::Enabled ),
        uint8_t( tracy::JnRuntimeMode::Enabled ), 0, 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnScriptFrame;
    item.jnScriptFrame = { ScriptFunctionPointer, ScriptFilePointer, 71, 123, 1, 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnScriptStack;
    item.jnScriptStack = { 117, 1001, 0, 1, 1, 0, uint8_t( tracy::JnScriptRecordKind::StackHeader ) };
    AppendQueueItem( frame, item );
    item.jnScriptStack = { 117, 1001, 71, 0, 1, 0, uint8_t( tracy::JnScriptRecordKind::StackFrame ) };
    AppendQueueItem( frame, item );
    item.jnScriptStack = { 117, 17, ScriptMarkerPointer, 71, 1, 0,
        uint8_t( tracy::JnScriptRecordKind::Marker ) };
    AppendQueueItem( frame, item );
    item.jnScriptStack = { 117, 2001, 1001, 17, 1, 0,
        uint8_t( tracy::JnScriptRecordKind::ZoneBegin ) };
    AppendQueueItem( frame, item );
    item.jnScriptStack = { 119, 2001, 0, 0, 0, 0,
        uint8_t( tracy::JnScriptRecordKind::ZoneEnd ) };
    AppendQueueItem( frame, item );
    // String queries are asynchronous in the Tracy protocol.  A script
    // record may legally reference a cold string before its StringData reply
    // appears later in the same or a subsequent protocol frame.
    AppendStringEvent( frame, tracy::QueueType::StringData, ScriptFunctionPointer, "Managed.Update" );
    AppendStringEvent( frame, tracy::QueueType::StringData, ScriptFilePointer, "Managed.cs" );
    AppendStringEvent( frame, tracy::QueueType::StringData, ScriptMarkerPointer, "ManagedTick" );
    item = {};
    item.hdr.type = tracy::QueueType::JnGfxDispatch;
    item.jnGfxDispatch = { 117, 300, 9, 2, 1, 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnGfxEntity;
    item.jnGfxEntity = { 117, 301, 300, 77, 0,
        uint8_t( tracy::JnGfxEntityKind::GfxJob ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnGfxLink;
    item.jnGfxLink = { 117, 300, 301, uint8_t( tracy::JnGfxRelation::Dispatches ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnGfxEntity;
    item.jnGfxEntity = { 117, ExplicitGpuPassId, 0, 78, 0,
        uint8_t( tracy::JnGfxEntityKind::ExplicitGpuPass ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnGfxLink;
    item.jnGfxLink = { 117, ExplicitGpuPassId, 1000,
        uint8_t( tracy::JnGfxRelation::ReferencesResources ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnFrame;
    item.jnFrame = { 118, 9, 9, uint8_t( tracy::JnFrameDomain::Player ),
        uint8_t( tracy::JnFramePhase::Begin ), uint8_t( tracy::JnFrameFlags::Canonical ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnIoRequest;
    item.jnIoRequest = { 118, 400, 500, uint8_t( tracy::JnIoOperation::Read ),
        uint8_t( tracy::JnIoSource::JnfsNative ), 2, 3, uint8_t( tracy::JnIoFlags::Async ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnIoConfig;
    item.jnIoConfig = { 400, 0, 4096, 9, uint8_t( tracy::JnIoParentKind::None ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnIoStage;
    item.jnIoStage = { 119, 400, 1024, 0, uint8_t( tracy::JnIoStage::Start ),
        uint8_t( tracy::JnIoStatus::Unknown ), 0 };
    AppendQueueItem( frame, item );
    item.jnIoStage = { 120, 400, 4096, 0, uint8_t( tracy::JnIoStage::Complete ),
        uint8_t( tracy::JnIoStatus::Success ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnIoRequest;
    item.jnIoRequest = { 121, 401, 501, uint8_t( tracy::JnIoOperation::Read ),
        uint8_t( tracy::JnIoSource::JnfsNative ), 2, 3, uint8_t( tracy::JnIoFlags::Async ) };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnIoConfig;
    item.jnIoConfig = { 401, 400, 1024, 9, uint8_t( tracy::JnIoParentKind::IoRequest ), 0 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::JnIoStage;
    item.jnIoStage = { 122, 401, 256, 0, uint8_t( tracy::JnIoStage::Start ),
        uint8_t( tracy::JnIoStatus::Unknown ), 0 };
    AppendQueueItem( frame, item );
    item.jnIoStage = { 123, 401, 1024, 0, uint8_t( tracy::JnIoStage::Complete ),
        uint8_t( tracy::JnIoStatus::Success ), 0 };
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
    AppendStringEvent( frame, tracy::QueueType::PlotName, 0x6000, "Synthetic Plot" );
    AppendStringEvent( frame, tracy::QueueType::PlotName, 0x6001, "Orphan Plot Name" );
    item = {};
    item.hdr.type = tracy::QueueType::PlotDataInt;
    item.plotDataInt = { { 0x6000, 2 }, 1 };
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "Synthetic message" );
    item = {};
    item.hdr.type = tracy::QueueType::Message;
    item.message.time = 2;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::MessageLiteralColor;
    item.messageColorLiteral.time = 2;
    item.messageColorLiteral.b = 0x33;
    item.messageColorLiteral.g = 0x22;
    item.messageColorLiteral.r = 0x11;
    item.messageColorLiteral.text = 0x7F000001;
    AppendQueueItem( frame, item );
    // Literal definitions may arrive after their consumers.  The Session
    // builder must resolve them from the complete immutable dictionary rather
    // than dropping the earlier message.
    AppendStringEvent( frame, tracy::QueueType::StringData, 0x7F000001, "Late literal evidence" );
    AppendStringEvent( frame, tracy::QueueType::CallstackPayload, 0x5557,
        std::string( reinterpret_cast<const char*>( zoneCallstack.data() ), sizeof( zoneCallstack ) ) );
    std::string managedCallstack;
    managedCallstack.push_back( char( 1 ) );
    const uint32_t managedLine = 77;
    const uint16_t managedNameBytes = 15;
    const uint16_t managedFileBytes = 10;
    managedCallstack.append( reinterpret_cast<const char*>( &managedLine ), sizeof( managedLine ) );
    managedCallstack.append( reinterpret_cast<const char*>( &managedNameBytes ), sizeof( managedNameBytes ) );
    managedCallstack.append( "Managed.Message", managedNameBytes );
    managedCallstack.append( reinterpret_cast<const char*>( &managedFileBytes ), sizeof( managedFileBytes ) );
    managedCallstack.append( "Managed.cs", managedFileBytes );
    AppendStringEvent( frame, tracy::QueueType::CallstackAllocPayload, 0x5558, managedCallstack );
    item = {};
    item.hdr.type = tracy::QueueType::CallstackAlloc;
    item.callstackFat.ptr = 0x5558;
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "Managed stacked evidence" );
    item = {};
    item.hdr.type = tracy::QueueType::MessageCallstack;
    item.message.time = 2;
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::CallstackPayload, 0x5556,
        std::string( reinterpret_cast<const char*>( zoneCallstack.data() ), sizeof( zoneCallstack ) ) );
    item = {};
    item.hdr.type = tracy::QueueType::Callstack;
    item.callstackFat.ptr = 0x5556;
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "Stacked evidence" );
    item = {};
    item.hdr.type = tracy::QueueType::MessageCallstack;
    item.message.time = 2;
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
    // The source can contain a completed zone whose producer timestamps move
    // backwards after a thread-context switch (for example, an unsynchronised
    // TSC after core migration). Preserve both source facts, but require the
    // Session derived layer to mark the timing invalid instead of rejecting
    // the complete capture or fabricating a non-negative duration.
    AppendThreadContextEvent( frame, 7 );
    AppendThreadContextEvent( frame, 42 );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneValidation;
    item.zoneValidation.id = 102;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::ZoneEnd;
    item.zoneEnd.time = 15;
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

    // Persist one native address with two inline frames. The Session source
    // index must reproduce the same callstack expansion and symbol mapping as
    // the traditional Worker without materializing the complete trace.
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "SyntheticGame.dll" );
    item = {};
    item.hdr.type = tracy::QueueType::CallstackFrameSize;
    item.callstackFrameSize = { 0x20202020, 2 };
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "InlineJob" );
    AppendStringEvent( frame, tracy::QueueType::SecondStringData, "Inline.cpp" );
    item = {};
    item.hdr.type = tracy::QueueType::CallstackFrame;
    item.callstackFrame = { 41, 0x2000, 20 };
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "JobRoot" );
    AppendStringEvent( frame, tracy::QueueType::SecondStringData, "Job.cpp" );
    item = {};
    item.hdr.type = tracy::QueueType::CallstackFrame;
    item.callstackFrame = { 42, 0x2100, 32 };
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "InlineSymbol.cpp" );
    item = {};
    item.hdr.type = tracy::QueueType::SymbolInformation;
    item.symbolInformation = { 51, 0x2000 };
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "RootSymbol.cpp" );
    item = {};
    item.hdr.type = tracy::QueueType::SymbolInformation;
    item.symbolInformation = { 52, 0x2100 };
    AppendQueueItem( frame, item );
    AppendLargePayloadEvent( frame, tracy::QueueType::SymbolCode, 0x2100,
        std::vector<uint8_t> { 0x90, 0xC3 } );

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
    const std::vector<uint8_t> frameImageBc1 { 0x00, 0xf8, 0x00, 0xf8,
        0x55, 0x55, 0x55, 0x55 };
    AppendLargePayloadEvent( frame, tracy::QueueType::FrameImageData,
        0x9000, frameImageBc1 );
    item = {};
    item.hdr.type = tracy::QueueType::FrameImage;
    item.frameImage.frame = 0;
    item.frameImage.w = 4;
    item.frameImage.h = 4;
    item.frameImage.flip = 1;
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
    AppendLargePayloadEvent( frame, tracy::QueueType::JnGpuCatalogBatchData, PayloadId + 2, logicalPayload );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuCatalogBatch;
    item.jnGpuCatalogBatch = { Generation, PayloadId + 2, 4, uint32_t( logicals.size() ), uint32_t( logicalPayload.size() ),
        uint8_t( tracy::JnGpuCatalogBatchKind::Logical ),
        uint8_t( tracy::JnGpuCatalogBatchEncoding::FixedV1 ), 0 };
    AppendQueueItem( frame, item );
    AppendLargePayloadEvent( frame, tracy::QueueType::JnGpuCatalogBatchData, PayloadId + 3, relationPayload );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuCatalogBatch;
    item.jnGpuCatalogBatch = { Generation, PayloadId + 3, 5, uint32_t( catalogRelations.size() ), uint32_t( relationPayload.size() ),
        uint8_t( tracy::JnGpuCatalogBatchKind::Relation ),
        uint8_t( tracy::JnGpuCatalogBatchEncoding::FixedV1 ), 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferencePass;
    item.jnGpuReferencePass = { 111, 1000, 5, 77, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceUse;
    item.jnGpuReferenceUse = { 111, 1000, 0x1234, 3, 0 };
    AppendQueueItem( frame, item );
    item.jnGpuReferenceUse = { 111, 1000, 0x5678, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceEnd;
    // totalReferenceCount is the producer's raw observation count.  The
    // ResourceSet/direct uses are the unique set after per-pass deduplication,
    // so repeated observations must not make an otherwise exact pass invalid.
    item.jnGpuReferenceEnd = { 113, 1000, 2000, 5, 0, 0 };
    AppendQueueItem( frame, item );
    // Force frame 5 to be committed before its asynchronously batched RangeSet
    // arrives.  A bounded Session converter must join this exact late evidence
    // from disk by pass id instead of retaining every historical pass.
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferencePass;
    item.jnGpuReferencePass = { 114, 1001, 8, 78, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnRelation;
    item.jnRelation = { 114, 1001, 1000,
        uint8_t( tracy::JnEntityKind::GpuPass ), uint8_t( tracy::JnEntityKind::GpuPass ),
        uint8_t( tracy::JnRelationNamespace::GpuReference ),
        uint8_t( tracy::JnRelationKind::LogicalParent ), 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceUse;
    item.jnGpuReferenceUse = { 114, 1001, 0x1234, 1, 0 };
    AppendQueueItem( frame, item );
    item.jnGpuReferenceUse = { 114, 1001, 0x9999, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceEnd;
    item.jnGpuReferenceEnd = { 115, 1001, 2001, 2, 0, 0 };
    AppendQueueItem( frame, item );
    // Resource ownership ended at 116, but a command-list reference captured
    // for the same pointer generation may be flushed before the address is
    // reused by the Snapshot at 120. Identity resolution must retain resource
    // 13 without extending its allocation/ownership lifetime.
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferencePass;
    item.jnGpuReferencePass = { 118, 1002, 9, 79, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceUse;
    item.jnGpuReferenceUse = { 118, 1002, 0x9999, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceEnd;
    item.jnGpuReferenceEnd = { 119, 1002, 2002, 1, 0, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferencePass;
    item.jnGpuReferencePass = { 117, 1003, 10, 80, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceUse;
    item.jnGpuReferenceUse = { 117, 1003, 0xBEEF, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceEnd;
    item.jnGpuReferenceEnd = { 118, 1003, 2003, 1, 0, 0 };
    AppendQueueItem( frame, item );
    // A second source gap has no Catalog definition anywhere in the capture.
    // The derived store must preserve the Pass relation through an anonymous
    // open-both-boundaries resource instead of rejecting the whole Session.
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferencePass;
    item.jnGpuReferencePass = { 119, 1004, 11, 81, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceUse;
    item.jnGpuReferenceUse = { 119, 1004, 0xCAFE, 1, 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuReferenceEnd;
    item.jnGpuReferenceEnd = { 120, 1004, 2004, 1, 0, 0 };
    AppendQueueItem( frame, item );
    AppendLargePayloadEvent( frame, tracy::QueueType::JnGpuCatalogBatchData,
        PayloadId + 4, rangePayload );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuCatalogBatch;
    item.jnGpuCatalogBatch = { Generation, PayloadId + 4, 6,
        uint32_t( ranges.size() ), uint32_t( rangePayload.size() ),
        uint8_t( tracy::JnGpuCatalogBatchKind::RangeSet ),
        uint8_t( tracy::JnGpuCatalogBatchEncoding::RangeSetV1 ), 0 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::FrameVsync;
    item.frameVsync = { 118, 9 };
    AppendQueueItem( frame, item );
    item = {}; item.hdr.type = tracy::QueueType::JnGpuCatalogControl;
    item.jnGpuCatalogControl = { 120, Generation, 1, 7,
        uint8_t( tracy::JnGpuCatalogControlKind::GenerationEnd ),
        uint8_t( tracy::JnGpuCatalogGenerationState::Complete ), 0 };
    AppendQueueItem( frame, item );

    // Keep the Lock serial deltas after the Memory fixture so this independent
    // domain does not change the Memory oracle's exact timestamps.
    item = {};
    item.hdr.type = tracy::QueueType::LockAnnounce;
    item.lockAnnounce.id = 700;
    item.lockAnnounce.time = 116;
    item.lockAnnounce.lckloc = 0x1000;
    item.lockAnnounce.type = tracy::LockType::Lockable;
    AppendQueueItem( frame, item );
    AppendStringEvent( frame, tracy::QueueType::SingleStringData, "Synthetic Lock" );
    item = {};
    item.hdr.type = tracy::QueueType::LockName;
    item.lockName.id = 700;
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::LockWait;
    item.lockWait = { 42, 700, 1 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::LockObtain;
    item.lockObtain = { 42, 700, 1 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::LockRelease;
    item.lockRelease = { 700, 1 };
    AppendQueueItem( frame, item );
    item = {};
    item.hdr.type = tracy::QueueType::LockTerminate;
    item.lockTerminate = { 700, 119 };
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
    welcome.pid = 1001;
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
    test.Check( gpuData.gpuCatalogValid && gpuData.gpuCatalogResources.size() == 11 &&
        gpuData.gpuCatalogResources.front().resourceId == 10 &&
        gpuData.gpuCatalogResources.front().time == 24,
        "GPU reader validates and restores Catalog records with Worker-equivalent time" );
    test.Check( gpuData.gpuCatalogViews.size() == 2 &&
        gpuData.gpuCatalogViews[0].resourceId == 10 && gpuData.gpuCatalogViews[0].pointerToken == 0 &&
        gpuData.gpuCatalogViews[1].resourceId == 11 && gpuData.gpuCatalogViews[1].pointerToken == 0,
        "GPU reader resolves pointer reuse against the exact resource lifetime" );
    test.Check( gpuData.gpuReferencePasses.size() == 5 &&
        gpuData.gpuReferenceUses.size() == 7 && gpuData.gpuReferenceEnds.size() == 5 &&
        gpuData.gpuReferenceEnds.front().totalReferenceCount == 5 &&
        gpuData.gpuReferencePasses.front().time == 22 &&
        gpuData.gpuReferenceUses.front().time == 22 &&
        gpuData.gpuReferenceEnds.front().time == 26,
        "GPU reader restores exact pass/resource/end relations" );
    test.Check( stats.catalogPayloads == 5 && stats.catalogBatches == 5 &&
        stats.unresolvedPayloads == 0,
        "GPU reader consumes each Catalog payload exactly once" );

    tracy::JnTraceData catalogOnlyData;
    tracy::analysis::TraceSessionTimeTransform catalogOnlyTransform;
    tracy::analysis::TraceSessionGpuCanonicalStats catalogOnlyStats;
    test.Check( tracy::analysis::LoadTraceSessionGpuCatalogData(
        sessionRoot, manifest, catalogOnlyData, catalogOnlyTransform,
        catalogOnlyStats, error ),
        "load bounded Catalog facts without materializing Pass/ResourceSet evidence: " + error );
    test.Check( catalogOnlyData.gpuCatalogResources.size() ==
            gpuData.gpuCatalogResources.size() &&
        catalogOnlyData.gpuCatalogAllocations.size() ==
            gpuData.gpuCatalogAllocations.size(),
        "Catalog-only reader preserves exact Resource and Allocation facts" );
    test.Check( catalogOnlyData.gpuReferencePasses.empty() &&
        catalogOnlyData.gpuReferenceUses.empty() &&
        catalogOnlyData.gpuReferenceEnds.empty(),
        "Catalog-only reader retains no high-volume Pass/ResourceSet facts" );

    tracy::analysis::GpuAnalysisSidecarControl gpuControl;
    gpuControl.minimumFreeBytes = 0;
    // Force every completed frame into a separate temporary pass page.  The
    // exact LogicalParent rollup must therefore work across both frame and
    // storage-page boundaries rather than depending on retained memory.
    gpuControl.targetDerivedPageBytes = 1;
    tracy::analysis::TraceSessionGpuDerivedStats derivedStats;
    const auto builtGpuDerived = tracy::analysis::BuildTraceSessionGpuAnalysisDerived(
        sessionRoot, manifest, gpuControl, derivedStats, error );
    test.Check( builtGpuDerived,
        "build mandatory GPU analysis from Session Canonical: " + error );
    test.Check( derivedStats.sourceGapResourceCount == 2 &&
        derivedStats.sourceGapReferenceCount == 2,
        "GPU derived reports the exact source-missing identity and reference counts" );
    const auto gpuRoot = tracy::analysis::TraceSessionGpuAnalysisRoot( sessionRoot, manifest );
    const auto gpuStore = tracy::analysis::LoadGpuAnalysisStoreManifest(
        gpuRoot / derivedStats.generation, error );
    test.Check( gpuStore.has_value() && gpuStore->complete &&
        gpuStore->resourceCount == 8 && gpuStore->passCount == 5 &&
        gpuStore->rangeCount == 1 &&
        gpuStore->sourceGapResourceCount == 2 &&
        gpuStore->sourceGapReferenceCount == 2 &&
        gpuStore->logicalCount == logicals.size() && gpuStore->catalogRelationCount == 1,
        "Session generation publishes N29 GPU analysis without a duplicate raw sidecar: " + error );
    auto gpuReader = tracy::analysis::GpuAnalysisStoreReader::OpenAt( gpuRoot,
        manifest.source.sha256, manifest.source.fileSize, error );
    const auto gpuResource = gpuReader ? gpuReader->FindResource( 10, error ) : std::nullopt;
    const auto parentGpuPass = gpuReader ? gpuReader->FindPass( 1000, error ) : std::nullopt;
    const auto delayedGpuPass = gpuReader ? gpuReader->FindPass( 1002, error ) : std::nullopt;
    const auto sourceGapGpuPass = gpuReader ? gpuReader->FindPass( 1003, error ) : std::nullopt;
    const auto sourceGapGpuResource = sourceGapGpuPass && sourceGapGpuPass->directResources.size() == 1 ?
        gpuReader->FindResource( sourceGapGpuPass->directResources.front(), error ) : std::nullopt;
    const auto absentCatalogGpuPass = gpuReader ? gpuReader->FindPass( 1004, error ) : std::nullopt;
    const auto absentCatalogGpuResource = absentCatalogGpuPass &&
        absentCatalogGpuPass->directResources.size() == 1 ?
        gpuReader->FindResource( absentCatalogGpuPass->directResources.front(), error ) : std::nullopt;
    std::vector<tracy::analysis::GpuRangeAnalysisRecord> gpuRanges;
    bool gpuRangesMore = false;
    const auto loadedGpuRanges = gpuReader && gpuReader->RangesForResource(
        10, 0, 16, gpuRanges, gpuRangesMore, error );
    test.Check( gpuResource.has_value(), "Session GPU resource lookup succeeds: " + error );
    test.Check( parentGpuPass.has_value(), "Session GPU parent pass lookup succeeds: " + error );
    test.Check( parentGpuPass && parentGpuPass->inclusiveResources ==
        std::vector<uint64_t> { 10, 12, 13 },
        "later Snapshot does not contaminate the exact earlier inclusive set; actual_count=" +
        std::to_string( parentGpuPass ? parentGpuPass->inclusiveResources.size() : 0 ) );
    test.Check( delayedGpuPass && delayedGpuPass->directResources ==
        std::vector<uint64_t> { 13 },
        "post-Destroy command-list use retains the previous pointer generation identity" );
    test.Check( sourceGapGpuPass && !sourceGapGpuPass->complete &&
        sourceGapGpuPass->truncated && sourceGapGpuPass->directResources.size() == 1,
        "a source-missing pre-Create pointer generation remains explicitly incomplete" );
    test.Check( sourceGapGpuResource && sourceGapGpuResource->openBoundary &&
        sourceGapGpuResource->allocationId == 0 && sourceGapGpuResource->capacityBytes == 0 &&
        sourceGapGpuResource->exactness == uint8_t( tracy::JnGpuCatalogExactness::OpenBoundary ),
        "source gap is represented by an anonymous open-boundary resource without guessed memory" );
    test.Check( absentCatalogGpuPass && !absentCatalogGpuPass->complete &&
        absentCatalogGpuPass->truncated && absentCatalogGpuResource &&
        absentCatalogGpuResource->openBoundary && absentCatalogGpuResource->aliveAtEnd &&
        absentCatalogGpuResource->allocationId == 0 && absentCatalogGpuResource->capacityBytes == 0,
        "a pointer absent from the entire Catalog remains an anonymous open-ended source gap" );
    test.Check( gpuResource && gpuResource->logicals.size() == logicals.size(),
        "Session GPU resource retains externally merged Logical facts" );
    test.Check( gpuResource && gpuResource->relations.size() == 1,
        "Session GPU resource retains externally merged Relation facts" );
    test.Check( loadedGpuRanges && gpuRanges.size() == 1 && !gpuRangesMore &&
        gpuRanges.front().value.passInstanceId == ExplicitGpuPassId &&
        parentGpuPass && parentGpuPass->directRangeBytes == 256,
        "Session GPU resource preserves the explicit pass Range identity and joins it to reference evidence: " + error );

    tracy::analysis::TraceSessionDerivedControl derivedControl;
    derivedControl.minimumFreeBytes = 0;
    std::string lastDerivedStage;
    derivedControl.progress = [&]( float, std::string_view stage ) {
        lastDerivedStage.assign( stage );
    };
    tracy::analysis::TraceSessionDerivedStats mandatoryStats;
    const auto mandatoryBuilt = tracy::analysis::BuildTraceSessionMandatoryDerived(
        sessionRoot, manifest, inventory, derivedControl, mandatoryStats, error );
    test.Check( mandatoryBuilt,
        "build all mandatory Session indexes at " + lastDerivedStage + ": " + error );
    const auto cpuZoneRoot = tracy::analysis::TraceSessionCpuZoneIndexRoot( sessionRoot, manifest );
    test.Check( !std::filesystem::exists( cpuZoneRoot / "zones.work" ) &&
        !std::filesystem::exists( cpuZoneRoot / "extras.work" ),
        "Session CPU Zone builder removes all temporary streams before publishing the index" );
    auto cpuZoneReader = tracy::analysis::TraceSessionCpuZoneReader::Open(
        sessionRoot, manifest, error );
    const auto mainThreadZoneRef = std::string( "tracy:v1:" ) +
        manifest.source.sha256.substr( 0, 16 ) + ":thread:2a";
    tracy::analysis::ScanRange narrowCpuZoneRange;
    narrowCpuZoneRange.startNs = 15;
    narrowCpuZoneRange.endNs = 21;
    narrowCpuZoneRange.limit = 16;
    const auto narrowCpuZones = cpuZoneReader ? cpuZoneReader->Scan( narrowCpuZoneRange ) :
        std::vector<tracy::analysis::CpuZoneDto> {};
    tracy::analysis::ScanRange allCpuZones;
    allCpuZones.limit = 16;
    const auto mainThreadCpuZones = cpuZoneReader ?
        cpuZoneReader->ScanThread( mainThreadZoneRef, allCpuZones ) :
        std::vector<tracy::analysis::CpuZoneDto> {};
    const auto cpuZoneChildren = cpuZoneReader ? cpuZoneReader->Children( 0, 0, 16 ) :
        std::vector<tracy::analysis::CpuZoneDto> {};
    test.Check( cpuZoneReader && cpuZoneReader->Stats().zoneBlocks != 0 &&
        cpuZoneReader->Stats().childLinks == 1 && cpuZoneChildren.size() == 1 &&
        cpuZoneChildren.front().parentRef == cpuZoneReader->Get( 0 )->ref &&
        narrowCpuZones.size() == 3 && mainThreadCpuZones.size() == 3 &&
        std::all_of( mainThreadCpuZones.begin(), mainThreadCpuZones.end(), [&]( const auto& value ) {
            return value.threadRef == mainThreadZoneRef;
        } ), "Session CPU Zone publishes exact time/thread block indexes: narrow=" +
            std::to_string( narrowCpuZones.size() ) + ",thread=" +
            std::to_string( mainThreadCpuZones.size() ) + ",blocks=" +
            std::to_string( cpuZoneReader ? cpuZoneReader->Stats().zoneBlocks : 0 ) + ":" + error );
    const auto gpuZoneRoot = tracy::analysis::TraceSessionGpuZoneIndexRoot( sessionRoot, manifest );
    test.Check( !std::filesystem::exists( gpuZoneRoot / "zones.work" ),
        "Session GPU Zone builder removes its temporary stream before publishing the index" );
    auto gpuZoneReader = tracy::analysis::TraceSessionGpuZoneReader::Open(
        sessionRoot, manifest, error );
    const auto gpuContextZeroRef = std::string( "tracy:v1:" ) +
        manifest.source.sha256.substr( 0, 16 ) + ":gpu-context:0";
    tracy::analysis::ScanRange narrowGpuZoneRange;
    narrowGpuZoneRange.startNs = 37;
    narrowGpuZoneRange.endNs = 39;
    narrowGpuZoneRange.limit = 16;
    const auto contextGpuZones = gpuZoneReader ? gpuZoneReader->ScanContext(
        gpuContextZeroRef, narrowGpuZoneRange ) : std::vector<tracy::analysis::GpuZoneDto> {};
    test.Check( gpuZoneReader && gpuZoneReader->Stats().zoneBlocks != 0 &&
        gpuZoneReader->Stats().childLinks == 0 && gpuZoneReader->Children( 0, 0, 16 ).empty() &&
        contextGpuZones.size() == 1 && contextGpuZones.front().contextRef == gpuContextZeroRef,
        "Session GPU Zone publishes exact time/Context block indexes: " + error );
    auto memoryReader = tracy::analysis::TraceSessionMemoryReader::Open(
        sessionRoot, manifest, error );
    const auto indexedMemoryPoolRef = memoryReader && !memoryReader->Pools().empty() ?
        memoryReader->Pools().front().ref : std::string {};
    tracy::analysis::ScanRange allMemoryEvents;
    allMemoryEvents.limit = 16;
    const auto indexedPoolEvents = memoryReader ? memoryReader->ScanPool(
        indexedMemoryPoolRef, allMemoryEvents ) : std::vector<tracy::analysis::MemoryEventDto> {};
    test.Check( memoryReader && memoryReader->Stats().eventBlocks != 0 &&
        !indexedPoolEvents.empty() && std::all_of( indexedPoolEvents.begin(),
            indexedPoolEvents.end(), [&]( const auto& value ) {
                return value.poolRef == indexedMemoryPoolRef;
            } ), "Session Memory publishes exact time/Pool block indexes: " + error );
    const auto schedulingRoot = tracy::analysis::TraceSessionSchedulingIndexRoot( sessionRoot, manifest );
    bool schedulingWorkFound = false;
    std::string schedulingWorkNames;
    for( const auto& entry : std::filesystem::directory_iterator( schedulingRoot ) )
    {
        if( entry.path().extension() != ".work" ) continue;
        schedulingWorkFound = true;
        if( !schedulingWorkNames.empty() ) schedulingWorkNames += ',';
        schedulingWorkNames += entry.path().filename().string();
    }
    test.Check( !schedulingWorkFound,
        "Session Scheduling builder removes all temporary streams before publishing the index; residual=" +
            schedulingWorkNames );
    const auto samplingRoot = tracy::analysis::TraceSessionSamplingIndexRoot( sessionRoot, manifest );
    bool samplingWorkFound = false;
    std::string samplingWorkNames;
    for( const auto& entry : std::filesystem::directory_iterator( samplingRoot ) )
    {
        if( entry.path().extension() != ".work" ) continue;
        samplingWorkFound = true;
        if( !samplingWorkNames.empty() ) samplingWorkNames += ',';
        samplingWorkNames += entry.path().filename().string();
    }
    test.Check( !samplingWorkFound,
        "Session Sampling builder removes all temporary streams before publishing the index; residual=" +
            samplingWorkNames );
    auto samplingReader = tracy::analysis::TraceSessionSamplingReader::Open(
        sessionRoot, manifest, error );
    const auto mainThreadSampleRef = std::string( "tracy:v1:" ) +
        manifest.source.sha256.substr( 0, 16 ) + ":thread:2a";
    const auto mainThreadSamples = samplingReader ? samplingReader->ScanThread(
        mainThreadSampleRef, {} ) : std::vector<tracy::analysis::SampleDto> {};
    test.Check( samplingReader && samplingReader->Stats().sampleBlocks != 0 &&
        mainThreadSamples.size() == 2 && std::all_of(
            mainThreadSamples.begin(), mainThreadSamples.end(), [&]( const auto& value ) {
                return value.threadRef == mainThreadSampleRef;
            } ),
        "Session Sampling publishes exact time/thread block indexes: " + error );
    auto schedulingReader = tracy::analysis::TraceSessionSchedulingReader::Open(
        sessionRoot, manifest, error );
    const auto cpuUsage = schedulingReader ? schedulingReader->ScanCpuUsage( 0, 64 ) :
        std::vector<tracy::analysis::CpuUsagePointDto> {};
    const auto hasOwnCpuUsage = std::any_of( cpuUsage.begin(), cpuUsage.end(),
        []( const auto& value ) { return value.own != 0; } );
    const auto hasOtherCpuUsage = std::any_of( cpuUsage.begin(), cpuUsage.end(),
        []( const auto& value ) { return value.other != 0; } );
    test.Check( schedulingReader && !cpuUsage.empty() &&
        cpuUsage.front().timeNs == 0 && cpuUsage.front().own == 0 &&
        cpuUsage.front().other == 0 && hasOwnCpuUsage && hasOtherCpuUsage &&
        schedulingReader->Stats().cpuUsagePoints == cpuUsage.size(),
        "Session Scheduling persists exact paged own/other CPU usage transitions: " + error );
    test.Check( schedulingReader && schedulingReader->Stats().threadBlocks != 0 &&
        schedulingReader->Stats().cpuBlocks != 0,
        "Session Scheduling publishes bounded immutable time-block indexes" );
    tracy::analysis::ScanRange narrowSchedulingRange;
    narrowSchedulingRange.startNs = 27;
    narrowSchedulingRange.endNs = 29;
    narrowSchedulingRange.limit = 16;
    const auto narrowThreadEvents = schedulingReader ?
        schedulingReader->ScanThreads( narrowSchedulingRange ) :
        std::vector<tracy::analysis::ContextSwitchDto> {};
    const auto narrowCpuEvents = schedulingReader ?
        schedulingReader->ScanCpus( narrowSchedulingRange ) :
        std::vector<tracy::analysis::CpuContextSwitchDto> {};
    test.Check( !narrowThreadEvents.empty() && !narrowCpuEvents.empty(),
        "Session Scheduling time-block indexes preserve exact interval intersection" );
    const auto mainThreadSummary = schedulingReader ? std::find_if(
        schedulingReader->Threads().begin(), schedulingReader->Threads().end(),
        []( const auto& value ) { return value.nativeId == 42; } ) :
        std::vector<tracy::analysis::ThreadDto>::const_iterator {};
    tracy::analysis::ScanRange allScheduling;
    allScheduling.limit = 64;
    const auto mainThreadEvents = schedulingReader &&
        mainThreadSummary != schedulingReader->Threads().end() ?
        schedulingReader->ScanThread( mainThreadSummary->ref, allScheduling ) :
        std::vector<tracy::analysis::ContextSwitchDto> {};
    const auto cpuZeroEvents = schedulingReader ? schedulingReader->ScanCpu( 0, allScheduling ) :
        std::vector<tracy::analysis::CpuContextSwitchDto> {};
    test.Check( !mainThreadEvents.empty() && std::all_of(
            mainThreadEvents.begin(), mainThreadEvents.end(), [&]( const auto& value ) {
                return value.threadRef == mainThreadSummary->ref;
            } ) && !cpuZeroEvents.empty() && std::all_of(
            cpuZeroEvents.begin(), cpuZeroEvents.end(), []( const auto& value ) {
                return value.cpu == 0;
            } ),
        "Session Scheduling Bloom filters never lose exact thread or CPU events" );
    std::vector<std::string> resumedDerivedStages;
    tracy::analysis::TraceSessionDerivedControl resumeDerivedControl;
    resumeDerivedControl.minimumFreeBytes = 0;
    resumeDerivedControl.progress = [&]( float, const char* stage ) {
        if( stage ) resumedDerivedStages.emplace_back( stage );
    };
    tracy::analysis::TraceSessionDerivedStats resumedMandatoryStats;
    test.Check( tracy::analysis::BuildTraceSessionMandatoryDerived( sessionRoot, manifest,
        inventory, resumeDerivedControl, resumedMandatoryStats, error ),
        "resume mandatory Session indexes from committed domain generations: " + error );
    test.Check( std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "frames-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "session-domain-index-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "time-transform-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "frame-images-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "jobs-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "job-pages-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "cpu-zones-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "symbols-callstacks-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "gpu-zones-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "memory-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "sampling-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "scheduling-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "plots-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "relations-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "runtime-script-reused" ) != resumedDerivedStages.end() &&
        std::find( resumedDerivedStages.begin(), resumedDerivedStages.end(), "io-gfx-reused" ) != resumedDerivedStages.end(),
        "Mandatory Derived resume reuses every verified domain generation" );
    tracy::analysis::TraceSessionDerivedStats auditedStats;
    test.Check( tracy::analysis::AuditTraceSessionFinal(
        sessionRoot, manifest, inventory, auditedStats, error ),
        "final audit covers Canonical, mandatory indexes, and GPU derived: " + error );
    test.Check( mandatoryStats.indexedRecords == auditedStats.indexedRecords &&
        auditedStats.indexedProtocolEvents == inventory.protocolInventory.eventCount &&
        auditedStats.gpuResources == 8 && auditedStats.gpuPasses == 5 &&
        auditedStats.gpuSourceGapResources == 2 &&
        auditedStats.gpuSourceGapReferences == 2 &&
        auditedStats.jobTypes == 1 && auditedStats.jobs == 2 &&
        auditedStats.jobSchedules == 2 && auditedStats.jobConfigs == 1 &&
        auditedStats.jobDependencies == 1 && auditedStats.jobStages == 6 &&
        auditedStats.invalidCpuZoneTimings == 1 && auditedStats.schedulingSourceGaps == 1,
        "mandatory derived counts are exact and reproducible" );
    test.Check( auditedStats.indexBytes <= auditedStats.indexFiles * 128,
        "generic Session index stores bounded shard summaries instead of one 48-byte row per event" );
    manifest.auditComplete = true;
    manifest.mandatoryDerivedComplete = true;
    manifest.state = tracy::analysis::TraceSessionState::CompleteSourceDegraded;
    manifest.reason = "source_cpu_zone_clock_inversion:1;source_scheduling_gap:1;source_gpu_resource_identity_gap:2";
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
        sessionGpuReader->Manifest().resourceCount == 8 &&
        sessionGpuReader->Manifest().passCount == 5 &&
        sessionGpuReader->Manifest().sourceGapResourceCount == 2 &&
        sessionGpuReader->Manifest().sourceGapReferenceCount == 2 &&
        sessionGpuReader->Manifest().reason == "source_gpu_resource_identity_gap:2",
        "open mandatory GPU analysis directly from a published Session: " + error );
    auto sessionSource = tracy::analysis::GpuAnalysisTraceSource::OpenSessionIfReady( publishedSession );
    test.Check( sessionSource &&
        sessionSource->AcquireReadView().sourceKind == tracy::analysis::TraceSourceKind::Session &&
        sessionSource->GetTraceInfo().fingerprint == manifest.source.sha256 &&
        !sessionSource->WorkerLoaded(),
        "open a published Session without materializing a full Worker" );
    test.Check( sessionSource &&
        sessionSource->AnalysisManifest().summary.rangeCount == 1 &&
        sessionSource->AnalysisManifest().summary.logicalRecordCount == logicals.size() &&
        sessionSource->AnalysisManifest().summary.relationCount == 1 &&
        sessionSource->AnalysisManifest().summary.generationCount == 1 &&
        sessionSource->AnalysisManifest().summary.payloadBytes == sessionGpuReader->Manifest().totalBytes,
        "Session GPU analysis facade preserves exact Range, Logical, Relation, generation, and byte counts" );
    const auto sessionCapabilities = sessionSource->GetCapabilities();
    const auto frameCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "frame"; } );
    test.Check( frameCapability != sessionCapabilities.end() && frameCapability->present &&
        frameCapability->indexed && frameCapability->queryable,
        "Session advertises Frame only after its disk-backed semantic reader is ready" );
    TestSessionPlotTraceSource( test, *sessionSource );
    TestSessionMessageTraceSource( test, *sessionSource );
    TestSessionLockTraceSource( test, *sessionSource );
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
    const auto hardwareSampleCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "hardware_sample"; } );
    const bool sessionHardwareSamplesReady = hardwareSampleCapability != sessionCapabilities.end() &&
        hardwareSampleCapability->present && hardwareSampleCapability->indexed &&
        hardwareSampleCapability->queryable;
    test.Check( sessionHardwareSamplesReady,
        "Session advertises Hardware Sample only after its disk-backed semantic reader is ready" );
    const auto schedulingCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "context_switch"; } );
    const bool sessionSchedulingReady = schedulingCapability != sessionCapabilities.end() &&
        schedulingCapability->present && schedulingCapability->indexed && schedulingCapability->queryable;
    test.Check( sessionSchedulingReady,
        "Session advertises Context Switch only after its disk-backed semantic reader is ready" );
    const auto threadCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "thread"; } );
    const bool sessionThreadsReady = threadCapability != sessionCapabilities.end() &&
        threadCapability->present && threadCapability->indexed && threadCapability->queryable;
    test.Check( sessionThreadsReady,
        "Session advertises Thread only after its compact disk-backed summary is ready" );
    const auto relationCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "relation"; } );
    const bool sessionRelationReady = relationCapability != sessionCapabilities.end() &&
        relationCapability->present && relationCapability->indexed && relationCapability->queryable;
    test.Check( sessionRelationReady,
        "Session advertises Relation only after its disk-backed semantic reader is ready" );
    const auto sessionRelations = sessionSource->ScanRelations( 0, 4 );
    test.Check( sessionSource->GetRelationCount() == 2 && sessionRelations.size() == 2 &&
        sessionRelations[0].timeNs == 34 && sessionRelations[0].sourceId == 500 &&
        sessionRelations[0].targetId == 900 &&
        sessionRelations[0].relationNamespace == uint8_t( tracy::JnRelationNamespace::Job ) &&
        sessionRelations[0].relation == uint8_t( tracy::JnRelationKind::ExecutesPass ),
        "Session Relation reader pages exact typed relations without a Worker" );
    const auto runtimeDomainCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "runtime.domain"; } );
    const bool sessionRuntimeDomainReady = runtimeDomainCapability != sessionCapabilities.end() &&
        runtimeDomainCapability->present && runtimeDomainCapability->indexed && runtimeDomainCapability->queryable;
    test.Check( sessionRuntimeDomainReady,
        "Session advertises Runtime Domain only after its disk-backed semantic reader is ready" );
    const auto runtimeScriptCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "runtime.script"; } );
    const bool sessionRuntimeScriptReady = runtimeScriptCapability != sessionCapabilities.end() &&
        runtimeScriptCapability->present && runtimeScriptCapability->indexed && runtimeScriptCapability->queryable;
    test.Check( sessionRuntimeScriptReady,
        "Session advertises Script Runtime only after its disk-backed semantic reader is ready" );
    const auto ioCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "io"; } );
    const bool sessionIoReady = ioCapability != sessionCapabilities.end() &&
        ioCapability->present && ioCapability->indexed && ioCapability->queryable;
    test.Check( sessionIoReady,
        "Session advertises I/O only after its disk-backed semantic reader is ready" );
    const auto gfxCapability = std::find_if( sessionCapabilities.begin(), sessionCapabilities.end(),
        []( const auto& value ) { return value.domain == "job.gfx"; } );
    const bool sessionGfxReady = gfxCapability != sessionCapabilities.end() &&
        gfxCapability->present && gfxCapability->indexed && gfxCapability->queryable;
    test.Check( sessionGfxReady,
        "Session advertises Gfx evidence only after its disk-backed semantic reader is ready" );
    try
    {
        const auto ioRequests = sessionSource->GetIoRequests();
        const auto ioRequestCount = sessionSource->GetIoRequestCount();
        const auto ioRequest400 = sessionSource->GetIoRequest( 400 );
        const auto childIoRequest = sessionSource->GetIoRequest( 401 );
        const auto missingIoRequest = sessionSource->GetIoRequest( 402 );
        const auto ioChildren = sessionSource->GetIoChildren( 400, 0, 8 );
        const auto ioRequestPage = sessionSource->ScanIoRequests( 1, 1 );
        const auto ioQueuePage = sessionSource->ScanIoRequestsByQueue( 1, 1 );
        const auto ioLatency = sessionSource->GetIoLatencyStatistics();
        const auto gfxDispatches = sessionSource->GetGfxDispatches();
        const auto gfxEntities = sessionSource->GetGfxEntities();
        const auto gfxLinks = sessionSource->GetGfxLinks();
        const auto gfxDispatchPage = sessionSource->ScanGfxDispatches( 0, 1 );
        const auto gfxEntityPage = sessionSource->ScanGfxEntities( 1, 1 );
        const auto gfxLinkPage = sessionSource->ScanGfxLinks( 1, 1 );
        const auto gfxDispatch300 = sessionSource->GetGfxDispatch( 300 );
        const auto gfxEntity301 = sessionSource->GetGfxEntity( 301 );
        const auto runtimeDomainPage = sessionSource->ScanRuntimeDomainStates( 0, 1 );
        const auto scriptFramePage = sessionSource->ScanScriptFrames( 0, 1 );
        const auto scriptStackPage = sessionSource->ScanScriptStackEvents( 1, 2 );
        const auto correlatedFrames = sessionSource->GetCorrelatedFrameEvents();
        const auto frameNineDispatches = sessionSource->GetGfxDispatchesForFrame( 9, 0, 8 );
        const auto frameNineEvents = sessionSource->GetCorrelatedFrameEventsForFrame( 9, 0, 8 );
        const auto missingFrameDispatches = sessionSource->GetGfxDispatchesForFrame( 10, 0, 8 );
        const auto missingFrameEvents = sessionSource->GetCorrelatedFrameEventsForFrame( 10, 0, 8 );
        const auto frameNineGfxEvidence = sessionSource->GetEvidenceGfx( 9, { 500 } );
        const auto dispatchGfxChain = sessionSource->GetGfxChain( 300, 16 );
        test.Check( ioRequests.size() == 2 && ioRequestCount == 2 && ioRequest400 &&
            childIoRequest && !missingIoRequest && ioRequests[0].requestId == 400 &&
            ioRequest400->requestId == 400 && ioRequest400->stages.size() == 2 &&
            ioRequests[0].requestedBytes == 4096 && ioRequests[0].transferredBytes == 4096 &&
            ioRequests[0].queueNs == 36 && ioRequests[0].startNs == 38 && ioRequests[0].endNs == 40 &&
            !ioRequests[0].orphan && !ioRequests[0].truncated,
            "Session I/O reader reconstructs one complete request without a Worker" );
        test.Check( ioRequests[1].requestId == 401 && ioRequests[1].parentId == 400 &&
            ioRequests[1].parentKind == uint8_t( tracy::JnIoParentKind::IoRequest ) &&
            ioRequests[1].requestedBytes == 1024 && ioRequests[1].transferredBytes == 1024 &&
            ioChildren.size() == 1 && ioChildren[0].requestId == 401 &&
            ioRequestPage.size() == 1 && ioRequestPage[0].requestId == 401 &&
            ioQueuePage.size() == 1 && ioQueuePage[0].requestId == 401 &&
            ioLatency && ioLatency->queue.count == 2 && ioLatency->queue.total == 4 &&
            ioLatency->execution.count == 2 && ioLatency->execution.total == 4 &&
            ioLatency->total.count == 2 && ioLatency->total.total == 8,
            "Session I/O reader preserves an exact parent-child request lifecycle" );
        test.Check( gfxDispatches.size() == 1 && gfxDispatches[0].dispatchId == 300 &&
            gfxEntities.size() == 2 && gfxEntities[0].entityId == 301 &&
            gfxEntities[1].entityId == ExplicitGpuPassId &&
            gfxLinks.size() == 2 && gfxLinks[0].sourceId == 300 && gfxLinks[0].targetId == 301 &&
            gfxLinks[1].sourceId == ExplicitGpuPassId && gfxLinks[1].targetId == 1000 &&
            gfxLinks[1].relation == uint8_t( tracy::JnGfxRelation::ReferencesResources ) &&
            correlatedFrames.size() == 1 && correlatedFrames[0].frameId == 9,
            "Session Gfx/Frame evidence reader preserves fixed binary facts without a Worker" );
        test.Check( sessionSource->GetGfxDispatchCount() == 1 &&
            sessionSource->GetGfxEntityCount() == 2 && sessionSource->GetGfxLinkCount() == 2 &&
            gfxDispatchPage.size() == 1 && gfxDispatchPage[0].dispatchId == 300 &&
            gfxEntityPage.size() == 1 && gfxEntityPage[0].entityId == ExplicitGpuPassId &&
            gfxLinkPage.size() == 1 && gfxLinkPage[0].sourceId == ExplicitGpuPassId &&
            gfxDispatch300 && gfxDispatch300->dispatchId == 300 &&
            gfxEntity301 && gfxEntity301->parentId == 300,
            "Session Gfx count/page/point APIs read bounded exact records from disk" );
        test.Check( sessionSource->GetRuntimeDomainStateCount() == 1 &&
            runtimeDomainPage.size() == 1 && runtimeDomainPage[0].timeNs == 34,
            "Session Runtime Domain reader pages exact state records" );
        test.Check( sessionSource->GetScriptFrameCount() == 1 &&
            scriptFramePage.size() == 1 && scriptFramePage[0].frameId == 71 &&
            sessionSource->GetScriptStackEventCount() == 5 &&
            scriptStackPage.size() == 2 && scriptStackPage[0].primaryId == 1001,
            "Session Script reader pages exact frame and stack records" );
        test.Check( frameNineDispatches.size() == 1 && frameNineDispatches[0].dispatchId == 300 &&
            frameNineEvents.size() == 1 && frameNineEvents[0].frameId == 9 &&
            missingFrameDispatches.empty() && missingFrameEvents.empty(),
            "Session Gfx/Frame posting readers fetch one Frame directly without scanning the full domain" );
        test.Check( frameNineGfxEvidence.dispatches.size() == 1 &&
            frameNineGfxEvidence.dispatches[0].dispatchId == 300 &&
            frameNineGfxEvidence.entities.size() == 1 &&
            frameNineGfxEvidence.entities[0].entityId == 301 &&
            frameNineGfxEvidence.links.size() == 1 &&
            frameNineGfxEvidence.links[0].sourceId == 300 &&
            frameNineGfxEvidence.links[0].targetId == 301,
            "Session Gfx adjacency postings traverse only the exact Frame/seed evidence component" );
        test.Check( dispatchGfxChain.dispatches.size() == 1 &&
            dispatchGfxChain.dispatches[0].dispatchId == 300 &&
            dispatchGfxChain.entities.size() == 1 &&
            dispatchGfxChain.entities[0].entityId == 301 &&
            dispatchGfxChain.links.size() == 1 &&
            dispatchGfxChain.links[0].sourceId == 300 &&
            dispatchGfxChain.links[0].targetId == 301 &&
            dispatchGfxChain.nodeIds.size() == 2 && !dispatchGfxChain.truncated,
            "Session Gfx chain walks exact dispatch/entity/link postings without full-domain materialization" );
        bool ioGfxWorkFound = false;
        const auto ioGfxRoot = tracy::analysis::TraceSessionIoGfxIndexRoot( publishedSession, manifest );
        for( const auto& entry : std::filesystem::directory_iterator( ioGfxRoot ) )
            ioGfxWorkFound = ioGfxWorkFound || ( entry.is_regular_file() &&
                entry.path().filename().string().ends_with( ".work" ) );
        test.Check( !ioGfxWorkFound,
            "Session I/O/Gfx posting build removes every external-sort work file before publication" );
    }
    catch( const std::exception& exception )
    {
        test.Check( false, std::string( "Session I/O/Gfx evidence reader is unavailable: " ) + exception.what() );
    }
    const auto sessionFrameSets = sessionSource->GetFrameSets();
    test.Check( sessionFrameSets.size() == 1 && sessionFrameSets[0].name == "Vsync 9" &&
        sessionFrameSets[0].continuous && sessionFrameSets[0].frameCount == 2 &&
        sessionFrameSets[0].completeFrameCount == 2,
        "Session Frame index restores the exact Vsync set and completeness" );
    const auto sessionFrames = sessionSource->GetFramesForSet( 0, 0, 3 );
    const auto sessionDurations = sessionSource->GetFrameDurations( 0 );
    test.Check( sessionFrames.size() == 2 && sessionFrames[0].beginNs == 12 &&
        sessionFrames[0].endNs == 36 && sessionFrames[1].beginNs == 36 &&
        sessionFrames[1].endNs == 46 && sessionDurations == std::vector<int64_t>( { 24, 10 } ),
        "Session Frame reader applies the Welcome transform and closes the offline tail at last semantic time" );
    const auto sessionFrameImages = sessionSource->GetFrameImageResources();
    const auto sessionFrameImageRaw = sessionSource->ReadFrameImageBc1( 0, 2, 3 );
    const auto sessionFrameImageDecoded = sessionSource->ReadFrameImage( 0, 1024 );
    test.Check( sessionSource->GetTraceInfo().counts.frameImages == 1 &&
        sessionFrameImages.size() == 1 && sessionFrameImages[0].width == 4 &&
        sessionFrameImages[0].height == 4 && sessionFrameImages[0].flipped &&
        sessionFrameImages[0].rawFrameIndex == 1 && sessionFrameImages[0].rawBc1Bytes == 8 &&
        sessionFrameImageRaw.offset == 2 && sessionFrameImageRaw.totalBytes == 8 &&
        sessionFrameImageRaw.bytes.size() == 3 && !sessionFrameImageRaw.eof &&
        sessionFrameImageDecoded.width == 4 && sessionFrameImageDecoded.height == 4 &&
        sessionFrameImageDecoded.flipped && sessionFrameImageDecoded.rgba.size() == 64,
        "Session FrameImage reader preserves metadata, paged BC1 bytes and on-demand RGBA decode" );
    const auto sessionJobs = sessionSource->GetJobs();
    const auto publishedJobRoot = tracy::analysis::TraceSessionJobIndexRoot( publishedSession, manifest );
    bool jobPostingWorkFound = false;
    for( const auto& entry : std::filesystem::directory_iterator( publishedJobRoot ) )
    {
        const auto name = entry.path().filename().string();
        jobPostingWorkFound = jobPostingWorkFound || ( entry.is_regular_file() &&
            name.ends_with( ".work" ) );
    }
    test.Check( !jobPostingWorkFound,
        "Session Job paging publishes reverse/frame postings without residual work files" );
    const auto sessionJob500 = std::find_if( sessionJobs.begin(), sessionJobs.end(),
        []( const auto& value ) { return value.jobId == 500; } );
    test.Check( sessionJobs.size() == 2 && sessionJob500 != sessionJobs.end() &&
        sessionJob500->name == "SyntheticJob" && sessionJob500->scheduleNs == 16 &&
        sessionJob500->readyNs == 20 && sessionJob500->firstRunNs == 22 &&
        sessionJob500->completedNs == 32 && sessionJob500->executionNs == 8 &&
        sessionJob500->scheduleCallsiteId == 77 && sessionJob500->scheduleCallstack == 2 &&
        sessionJob500->scheduleStackProvenance == "SiteReused" &&
        sessionJob500->dependencies.size() == 1 && sessionJob500->stages.size() == 5,
        "Session Job reader restores Schedule, Ready, Worker Slice and Complete semantics" );
    const auto sessionJobPage = sessionSource->ScanJobs( 1, 1 );
    const auto sessionJobSchedulePage = sessionSource->ScanJobsBySchedule( 0, 2 );
    const auto sessionJobById = sessionSource->GetJob( 500 );
    const auto sessionJobDependents = sessionSource->GetJobDependents( 600, 0, 16 );
    const auto sessionFrameJobs = sessionSource->GetJobsForFrame( 9, 0, 16 );
    const auto sessionHandleJobs = sessionSource->GetJobsForPackedHandle( 0xABC, 0, 16 );
    const auto sessionSlotJobs = sessionSource->GetJobsForHandleSlotNear( 0xABC, 600, 16 );
    const auto sessionJobLatency = sessionSource->GetJobLatencyStatistics();
    const auto sessionEvidenceJobs = sessionSource->GetEvidenceJobs( 9 );
    test.Check( sessionSource->GetJobCount() == 2 && sessionJobPage.size() == 1 &&
        sessionJobPage[0].jobId == 600 && sessionJobSchedulePage.size() == 2 &&
        sessionJobSchedulePage[0].jobId == 600 && sessionJobSchedulePage[1].jobId == 500 &&
        sessionJobById &&
        sessionJobById->jobId == 500 && sessionJobById->stages.size() == 5 &&
        sessionJobDependents.size() == 1 && sessionJobDependents[0].jobId == 500 &&
        sessionFrameJobs.size() == 1 && sessionFrameJobs[0].jobId == 500 &&
        sessionHandleJobs.size() == 1 && sessionHandleJobs[0].jobId == 500 &&
        sessionSlotJobs.size() == 2 && sessionSlotJobs[0].jobId == 600 &&
        sessionSlotJobs[1].jobId == 500 &&
        sessionJobLatency && sessionJobLatency->scheduleToReady.count == 1 &&
        sessionJobLatency->scheduleToReady.total == 4 &&
        sessionJobLatency->execution.count == 1 && sessionJobLatency->execution.total == 8 &&
        sessionEvidenceJobs.size() == 2,
        "Session Job reader pages jobs, dependency, Frame, packed-handle and nearby-slot evidence from exact disk postings; latency=" +
            ( sessionJobLatency ? std::to_string( sessionJobLatency->scheduleToReady.count ) + "/" +
                std::to_string( sessionJobLatency->scheduleToReady.total ) + ",exec=" +
                std::to_string( sessionJobLatency->execution.count ) + "/" +
                std::to_string( sessionJobLatency->execution.total ) : std::string( "missing" ) ) );
    tracy::analysis::TraceSessionJobBuildOptions spillOptions;
    spillOptions.maximumBufferedRecords = 2;
    tracy::analysis::TraceSessionJobStats spilledJobStats;
    std::string spillError;
    test.Check( tracy::analysis::BuildTraceSessionJobDerived( publishedSession, manifest,
        spilledJobStats, spillError, spillOptions ) && spilledJobStats.jobs == 2 &&
        spilledJobStats.stages == 6 && spilledJobStats.peakBufferedRecords <= 2,
        "Session Job builder spills exact Stage records to disk instead of enforcing a total-record memory cap: " + spillError );
    const auto spilledJobRoot = tracy::analysis::TraceSessionJobIndexRoot( publishedSession, manifest );
    size_t spilledJobTemporaryRuns = 0;
    for( const auto& entry : std::filesystem::directory_iterator( spilledJobRoot ) )
    {
        const auto name = entry.path().filename().string();
        spilledJobTemporaryRuns += entry.is_regular_file() &&
            name.starts_with( "job-id-run-" ) && name.ends_with( ".work" );
    }
    test.Check( spilledJobTemporaryRuns == 0,
        "Session Job builder removes all external distinct-ID runs before publishing the index" );
    const auto sessionSources = sessionSource->GetSourceLocations();
    const auto sessionCallsites = sessionSource->GetCallsites();
    const auto sessionStack = sessionSource->ResolveCallstacks( { 2 }, 8 );
    const auto sessionSymbols = sessionSource->GetSymbols();
    const auto sessionSymbolMapping = sessionSource->ResolveSymbolAddress( 0x20202020 );
    const auto sessionSymbolCode = sessionSource->ReadSymbolCodeBytes( 0x2100, 1, 1 );
    test.Check( sessionSources.size() == 4 && sessionSources[0].function == "JobFunction" &&
        sessionSources[0].file == "Job.cpp" && sessionSources[0].line == 77 &&
        sessionCallsites.size() == 3 && sessionCallsites[0].callsiteId == 77 &&
        sessionCallsites[0].callstack == 2 && sessionCallsites[0].provenance == "SiteReused",
        "Session Source reader preserves static SourceLocation and SiteReuse Callsite identity" );
    test.Check( sessionStack.size() == 3 && sessionStack[0].address == "0x20202020" &&
        sessionStack[0].name == "InlineJob" && sessionStack[0].file == "Inline.cpp" &&
        sessionStack[0].line == 41 && sessionStack[0].inlineFrame &&
        sessionStack[1].name == "JobRoot" && !sessionStack[1].inlineFrame &&
        sessionStack[2].address == "0x30303030" && sessionStack[2].name.empty(),
        "Session Callstack reader expands inline frames and preserves unresolved addresses" );
    test.Check( sessionSymbols.size() == 2 && sessionSymbols[0].address == "0x2000" &&
        sessionSymbols[0].file == "InlineSymbol.cpp" && sessionSymbols[0].inlineFrame &&
        sessionSymbols[1].address == "0x2100" && sessionSymbols[1].hasCode &&
        sessionSymbolMapping && sessionSymbolMapping->symbolAddress == "0x2000" &&
        sessionSymbolMapping->offset == 0x20200020 && sessionSymbolMapping->inlineMapping &&
        sessionSymbolCode.offset == 1 && sessionSymbolCode.totalBytes == 2 &&
        sessionSymbolCode.bytes == std::vector<uint8_t>( { 0xC3 } ) && sessionSymbolCode.eof,
        "Session Symbol reader preserves metadata, address mapping and paged machine code" );
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
        const auto threadSamples = sessionSource->ScanSampleEventsForThread(
            sessionSource->MakeEntityRef( "thread", 42 ), {} );
        test.Check( samples.size() == 2 && samples[0].timeNs == 14 &&
            samples[0].threadRef == sessionSource->MakeEntityRef( "thread", 42 ) &&
            samples[0].callstack == 1 && samples[0].kind == "sample" &&
            samples[1].timeNs == 16 && samples[1].callstack == 1 &&
            samples[1].kind == "context_switch" && threadSamples.size() == 2,
            "Session Sampling reader restores dictionary callstacks and shared context time" );
    }
    if( sessionHardwareSamplesReady )
    {
        const auto samples = sessionSource->GetHardwareSamples();
        const auto events = sessionSource->GetHardwareSampleEvents(
            HardwareSampleAddress, "all", 0, 16 );
        test.Check( samples.size() == 1 && samples[0].address == "0xabc" &&
            samples[0].cycles == 1 && samples[0].retired == 1 &&
            samples[0].cacheReferences == 1 && samples[0].cacheMisses == 1 &&
            samples[0].branchRetired == 1 && samples[0].branchMisses == 1 &&
            events.size() == 6 && events[0].kind == "cycles" && events[0].timeNs == 16 &&
            events[2].kind == "cache_references" && events[2].timeNs == 0 &&
            events[5].kind == "branch_misses" && events[5].timeNs == 24,
            "Session Hardware Sample reader preserves all six PMU kinds and Worker time semantics" );
    }
    if( sessionSchedulingReady )
    {
        const auto contexts = sessionSource->ScanContextSwitchEvents( {} );
        const auto cpuContexts = sessionSource->ScanCpuContextSwitchEvents( {} );
        test.Check( contexts.size() == 100 && contexts[0].startNs == 18 &&
            contexts[0].endNs == 28 && contexts[0].wakeupNs == 18 &&
            contexts[0].cpu == 0 && contexts[0].wakeupCpu == 0 &&
            contexts[0].reason == 5 && contexts[0].state == 5 &&
            contexts[1].startNs == 28 && contexts[1].endNs == 36 &&
            contexts[1].wakeupNs == 22 && contexts[1].wakeupCpu == 1 &&
            contexts[1].reason == 4 && contexts[1].state == 1 &&
            contexts[2].startNs == 38 && !contexts[2].endNs && !contexts[2].complete &&
            contexts[3].startNs == 40 && contexts[3].endNs == 42 && contexts[3].complete,
            "Session Context Switch reader restores wakeup, run and wait-state semantics" );
        test.Check( cpuContexts.size() == 4 && cpuContexts[0].startNs == 18 &&
            cpuContexts[0].endNs == 28 && cpuContexts[0].threadRef ==
                sessionSource->MakeEntityRef( "thread", 42 ) &&
            cpuContexts[1].startNs == 28 && cpuContexts[1].endNs == 36 &&
            cpuContexts[1].threadRef == sessionSource->MakeEntityRef( "thread", 43 ) &&
            cpuContexts[2].startNs == 38 && !cpuContexts[2].endNs && !cpuContexts[2].complete &&
            cpuContexts[3].startNs == 40 && cpuContexts[3].endNs == 42 && cpuContexts[3].complete,
            "Session CPU scheduling reader preserves source gaps without inventing close times" );
        const auto topology = sessionSource->GetCpuTopology();
        test.Check( topology.size() == 2 && topology[0].cpu == 0 &&
            topology[0].package == 0 && topology[0].die == 0 && topology[0].core == 0 &&
            topology[0].dieAvailability.available && topology[1].cpu == 1,
            "Session Scheduling reader restores exact CPU topology without a Worker" );
    }
    if( sessionThreadsReady )
    {
        const auto threads = sessionSource->GetThreads();
        const auto mainThread = std::find_if( threads.begin(), threads.end(),
            []( const auto& value ) { return value.nativeId == 42; } );
        test.Check( mainThread != threads.end() && mainThread->name == "Main Thread" &&
            mainThread->localName == std::optional<std::string>( "Main Thread" ) &&
            mainThread->processId == 1001 && mainThread->sampleCount == 1 &&
            mainThread->contextSwitchCount != 0 && mainThread->runningTimeNs != 0 &&
            mainThread->runningRegions && *mainThread->runningRegions != 0 &&
            mainThread->groupHint == std::optional<int32_t>( 7 ) &&
            mainThread->groupHintAvailability.available,
            "Session Thread summary preserves name, PID, Sample, scheduling and group metadata" );
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
            frames["data"]["frames"][1]["duration_ns"] == "10",
            "Query 1.34 preserves Session Frame timing and pagination semantics" );
        TestSessionPlotQuery( test, query, traceId, *sessionSource );
        TestSessionMessageQuery( test, query, traceId );
        TestSessionLockQuery( test, query, traceId );
        const auto imageRef = sessionFrameImages.empty() ? std::string {} : sessionFrameImages[0].ref;
        const auto frameImageList = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-frame-image-list" },
            { "method", "frame_image.list" }, { "params", { { "trace_id", traceId } } }
        } );
        const auto frameImageRaw = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-frame-image-raw" },
            { "method", "frame_image.raw" }, { "params", { { "trace_id", traceId },
                { "ref", imageRef }, { "offset_bytes", 2 }, { "max_bytes", 3 } } }
        } );
        test.Check( frameImageList.value( "ok", false ) &&
            frameImageList["data"]["images"].size() == 1 &&
            frameImageList["data"]["images"][0]["width"] == 4 &&
            frameImageList["data"]["images"][0]["height"] == 4 &&
            frameImageList["data"]["images"][0]["flipped"] == true &&
            frameImageRaw.value( "ok", false ) && frameImageRaw["data"]["format"] == "bc1_dxt1" &&
            frameImageRaw["data"]["offset_bytes"] == "2" &&
            frameImageRaw["data"]["returned_bytes"] == "3" &&
            frameImageRaw["data"]["total_bytes"] == "8" &&
            frameImageRaw["data"]["eof"] == false,
            "Query 1.34 lists and pages Session FrameImage evidence without a Worker" );
        const auto jobs = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-job-search" }, { "method", "job.search" },
            { "params", { { "trace_id", traceId },
                { "filter", { { "text", "SyntheticJob" } } } } }
        } );
        test.Check( jobs.value( "ok", false ) && jobs["data"]["jobs"].size() == 1 &&
            jobs["data"]["jobs"][0]["job_id"] == "500" &&
            jobs["data"]["jobs"][0]["name"] == "SyntheticJob",
            "Query 1.34 searches the Session Job semantic index without a Worker" );
        const auto jobsBySchedule = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-job-schedule-order" },
            { "method", "job.search" }, { "params", { { "trace_id", traceId },
                { "limit", 2 } } }
        } );
        test.Check( jobsBySchedule.value( "ok", false ) &&
            jobsBySchedule["data"]["jobs"].size() == 2 &&
            jobsBySchedule["data"]["jobs"][0]["job_id"] == "600" &&
            jobsBySchedule["data"]["jobs"][1]["job_id"] == "500",
            "Query 1.34 pages Session Job search in exact Schedule-time order rather than Job-ID order" );
        const auto jobDependencies = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-job-dependencies" },
            { "method", "job.dependencies" }, { "params", { { "trace_id", traceId },
                { "ref", sessionSource->MakeEntityRef( "job", 600 ) }, { "limit", 16 } } }
        } );
        test.Check( jobDependencies.value( "ok", false ) &&
            jobDependencies["data"]["upstream"].empty() &&
            jobDependencies["data"]["downstream"].size() == 1 &&
            jobDependencies["data"]["downstream"][0]["job_id"] == "500",
            "Query 1.34 resolves downstream Job dependencies through the exact reverse posting" );
        const auto sessionJobRoot = tracy::analysis::TraceSessionJobIndexRoot( publishedSession, manifest );
        const auto sessionJobRaw = sessionJobRoot / "jobs.bin";
        const auto sessionJobRawHidden = sessionJobRoot / "jobs.bin.statistics-test-hidden";
        std::filesystem::rename( sessionJobRaw, sessionJobRawHidden );
        const auto jobCriticalPath = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-job-critical-path" },
            { "method", "job.critical_path" }, { "params", { { "trace_id", traceId },
                { "ref", sessionSource->MakeEntityRef( "job", 500 ) }, { "max_nodes", 16 } } }
        } );
        const auto jobStatistics = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-job-statistics" },
            { "method", "job.statistics" }, { "params", { { "trace_id", traceId } } }
        } );
        std::filesystem::rename( sessionJobRawHidden, sessionJobRaw );
        test.Check( jobCriticalPath.value( "ok", false ) &&
            jobCriticalPath["data"]["jobs"].size() == 1 &&
            jobCriticalPath["data"]["jobs"][0]["job_id"] == "500" &&
            jobCriticalPath["data"]["processed_jobs"] == 2 &&
            jobCriticalPath["data"]["scope_jobs"] == "2" &&
            jobCriticalPath["data"]["scope"] == "upstream_closure" &&
            jobCriticalPath["data"]["root_job_id"] == "500",
            "Query 1.34 computes a root-scoped Session Job critical path from exact point reads without raw full materialization" );
        test.Check( jobStatistics.value( "ok", false ) &&
            jobStatistics["data"]["counts"]["jobs"] == "2" &&
            jobStatistics["data"]["counts"]["completed"] == "2" &&
            jobStatistics["data"]["latency"]["schedule_to_ready"]["count"] == "1" &&
            jobStatistics["data"]["latency"]["schedule_to_ready"]["total_ns"] == "4" &&
            jobStatistics["data"]["latency"]["execution"]["total_ns"] == "8" &&
            jobStatistics["data"]["quality"]["missing_ready_examples"].size() == 1 &&
            jobStatistics["data"]["quality"]["missing_ready_examples"][0]["same_slot_jobs"].size() == 1 &&
            jobStatistics["data"]["quality"]["missing_ready_examples"][0]["same_slot_jobs"][0]["job_id"] == "500",
            "Query 1.34 computes exact Session Job statistics and bounded handle diagnostics from disk postings: " +
                jobStatistics.dump() );
        if( sessionRelationReady )
        {
            const auto relations = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-relation-search" },
                { "method", "relation.search" }, { "params", { { "trace_id", traceId },
                    { "namespace", "job" }, { "relation", "executes_pass" } } }
            } );
            test.Check( relations.value( "ok", false ) && relations["data"]["present"] == true &&
                relations["data"]["relation_count"] == "2" &&
                relations["data"]["relations"].size() == 1 &&
                relations["data"]["relations"][0]["time_ns"] == "34" &&
                relations["data"]["relations"][0]["source_id"] == "500" &&
                relations["data"]["relations"][0]["target_id"] == "900",
                "Query 1.34 pages exact Session typed relations without a Worker" );
        }
        if( sessionRuntimeDomainReady )
        {
            const auto runtimeStates = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-runtime-domain" },
                { "method", "runtime.domain.states" }, { "params", { { "trace_id", traceId },
                    { "domain", "script_stack" } } }
            } );
            test.Check( runtimeStates.value( "ok", false ) &&
                runtimeStates["data"]["state_count"] == "1" &&
                runtimeStates["data"]["states"].size() == 1 &&
                runtimeStates["data"]["states"][0]["time_ns"] == "34" &&
                runtimeStates["data"]["states"][0]["effective_mode"] == "enabled",
                "Query 1.34 reads exact Session runtime-domain state without a Worker" );
        }
        if( sessionRuntimeScriptReady )
        {
            const auto scriptSummary = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-script-summary" },
                { "method", "runtime.script.summary" }, { "params", { { "trace_id", traceId } } }
            } );
            test.Check( scriptSummary.value( "ok", false ) && scriptSummary["data"]["present"] == true &&
                scriptSummary["data"]["complete"] == true &&
                scriptSummary["data"]["counts"]["frames"] == "1" &&
                scriptSummary["data"]["counts"]["stacks"] == "1" &&
                scriptSummary["data"]["counts"]["markers"] == "1" &&
                scriptSummary["data"]["counts"]["zones"] == "1" &&
                scriptSummary["data"]["counts"]["complete_zones"] == "1",
                "Query 1.34 reconstructs exact Session C#/Lua Script evidence without a Worker" );
        }
        if( sessionIoReady )
        {
            const auto ioSearch = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-io-search" },
                { "method", "io.search" }, { "params", { { "trace_id", traceId },
                    { "offset", 0 }, { "limit", 1 } } }
            } );
            const auto ioGet = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-io-get" },
                { "method", "io.get" }, { "params", { { "trace_id", traceId },
                    { "ref", sessionSource->MakeEntityRef( "io-request", 400 ) } } }
            } );
            const auto ioChain = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-io-chain" },
                { "method", "io.chain" }, { "params", { { "trace_id", traceId },
                    { "ref", sessionSource->MakeEntityRef( "io-request", 401 ) },
                    { "max_nodes", 8 } } }
            } );
            const auto ioStatistics = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-io-statistics" },
                { "method", "io.statistics" }, { "params", { { "trace_id", traceId } } }
            } );
            test.Check( ioSearch.value( "ok", false ) &&
                ioSearch["data"]["request_count"] == "2" &&
                ioSearch["data"]["requests"].size() == 1 &&
                ioSearch["data"]["requests"][0]["request_id"] == "400" &&
                ioSearch["data"]["requests"][0]["requested_bytes"] == "4096" &&
                ioSearch["data"]["requests"][0]["transferred_bytes"] == "4096" &&
                ioSearch["page"]["next_cursor"].is_string(),
                "Query 1.34 pages exact queue-ordered Session I/O evidence without a Worker" );
            test.Check( ioGet.value( "ok", false ) &&
                ioGet["data"]["request_count"] == "2" &&
                ioGet["data"]["request"]["request_id"] == "400" &&
                ioGet["data"]["request"]["stages"].size() == 2,
                "Query 1.34 point-reads one Session I/O request through the exact ID posting" );
            test.Check( ioChain.value( "ok", false ) &&
                ioChain["data"]["request_count"] == "2" &&
                ioChain["data"]["nodes"].size() == 2 &&
                ioChain["data"]["edges"].size() == 1 &&
                ioChain["data"]["edges"][0]["relation"] == "parent" &&
                ioChain["data"]["truncated"] == false,
                "Query 1.34 walks the Session I/O parent chain through exact ID and child postings" );
            test.Check( ioStatistics.value( "ok", false ) &&
                ioStatistics["data"]["counts"]["requests"] == "2" &&
                ioStatistics["data"]["counts"]["completed"] == "2" &&
                ioStatistics["data"]["quality"]["unresolved_parent"] == "0" &&
                ioStatistics["data"]["latency"]["queue"]["count"] == "2" &&
                ioStatistics["data"]["latency"]["queue"]["total_ns"] == "4" &&
                ioStatistics["data"]["latency"]["total"]["total_ns"] == "8",
                "Query 1.34 computes Session I/O statistics from bounded Request ID pages and exact disk latency runs" );
        }
        if( sessionGfxReady )
        {
            const auto gfxStats = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-gfx-stats" },
                { "method", "job.gfx.statistics" }, { "params", { { "trace_id", traceId } } }
            } );
            const auto gfxChain = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-gfx-chain" },
                { "method", "job.gfx_chain" }, { "params", { { "trace_id", traceId },
                    { "ref", sessionSource->MakeEntityRef( "gfx-dispatch", 300 ) },
                    { "max_nodes", 16 } } }
            } );
            test.Check( gfxStats.value( "ok", false ) &&
                gfxStats["data"]["counts"]["dispatches"] == "1" &&
                gfxStats["data"]["counts"]["entities"] == "2" &&
                gfxStats["data"]["counts"]["links"] == "2",
                "Query 1.34 reads exact Session Gfx evidence without a Worker" );
            test.Check( gfxChain.value( "ok", false ) &&
                gfxChain["data"]["dispatches"].size() == 1 &&
                gfxChain["data"]["dispatches"][0]["dispatch_id"] == "300" &&
                gfxChain["data"]["entities"].size() == 1 &&
                gfxChain["data"]["entities"][0]["entity_id"] == "301" &&
                gfxChain["data"]["links"].size() == 1 &&
                gfxChain["data"]["visited_nodes"] == 2 &&
                gfxChain["data"]["truncated"] == false,
                "Query 1.34 walks a bounded Session Gfx chain through exact disk adjacency postings" );
        }
        const auto sourceLocations = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-source-locations" },
            { "method", "source.locations" }, { "params", { { "trace_id", traceId },
                { "filter", { { "text", "JobFunction" } } } } }
        } );
        const auto callsite = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-callsite" },
            { "method", "source.callsite" }, { "params", { { "trace_id", traceId },
                { "callsite_id", 77 } } }
        } );
        const auto callstack = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-callstack" },
            { "method", "callstack.resolve" }, { "params", { { "trace_id", traceId },
                { "callstacks", { "2" } }, { "max_depth", 8 } } }
        } );
        const auto symbols = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-symbol-search" },
            { "method", "symbol.search" }, { "params", { { "trace_id", traceId },
                { "filter", { { "text", "JobRoot" } } } } }
        } );
        const auto rootSymbolRef = sessionSymbols.size() > 1 ? sessionSymbols[1].ref : std::string {};
        const auto symbolCode = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-symbol-code" },
            { "method", "symbol.raw_code" }, { "params", { { "trace_id", traceId },
                { "ref", rootSymbolRef }, { "offset_bytes", 1 }, { "max_bytes", 1 } } }
        } );
        test.Check( sourceLocations.value( "ok", false ) &&
            sourceLocations["data"]["source_locations"].size() == 1 &&
            sourceLocations["data"]["source_locations"][0]["line"] == 77,
            "Query 1.34 resolves Session SourceLocation without a Worker" );
        test.Check( callsite.value( "ok", false ) && callsite["data"]["callsite_id"] == 77 &&
            callsite["data"]["provenance"] == "SiteReused",
            "Query 1.34 resolves Session Callsite without a Worker" );
        test.Check( callstack.value( "ok", false ) && callstack["data"]["frames"].size() == 3 &&
            callstack["data"]["frames"][0]["name"] == "InlineJob",
            "Query 1.34 resolves Session Callstack without a Worker" );
        test.Check( symbols.value( "ok", false ) && symbols["data"]["symbols"].size() == 1 &&
            symbols["data"]["symbols"][0]["name"] == "JobRoot",
            "Query 1.34 searches Session Symbol metadata without a Worker" );
        test.Check( symbolCode.value( "ok", false ) && symbolCode["data"]["offset_bytes"] == "1" &&
            symbolCode["data"]["returned_bytes"] == "1" && symbolCode["data"]["eof"] == true,
            "Query 1.34 pages Session SymbolCode without a Worker" );
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
        const auto catalogStatus = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-gpu-catalog-status" },
            { "method", "gpu.catalog.status" }, { "params", { { "trace_id", traceId } } }
        } );
        test.Check( catalogStatus.value( "ok", false ) &&
            catalogStatus["data"]["counts"]["range_records"] == "1" &&
            catalogStatus["data"]["counts"]["logical_resources"] == std::to_string( logicals.size() ) &&
            catalogStatus["data"]["counts"]["relations"] == "1",
            "Query Catalog status exposes exact Session derived counts" );
        const auto catalogValidation = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-gpu-catalog-validation" },
            { "method", "gpu.catalog.validation" }, { "params", { { "trace_id", traceId } } }
        } );
        test.Check( catalogValidation.value( "ok", false ) &&
            catalogValidation["data"]["complete"] == true &&
            catalogValidation["data"]["validation_provenance"] == "session_final_audit" &&
            catalogValidation["data"]["source_degraded"] == true,
            "Query validates a published Session from Final Audit without materializing legacy Worker vectors" );
        std::vector<tracy::analysis::GpuAnalysisResourceSummary> resourceSummaries;
        test.Check( sessionGpuReader && sessionGpuReader->ResourceSummaryPageCount() != 0 &&
            sessionGpuReader->LoadResourceSummaryPage( 0, resourceSummaries, error ) &&
            !resourceSummaries.empty(),
            "Session GPU derived store publishes an independently readable compact Resource Summary page: " + error );
        const auto resourceSearch = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-gpu-resource-search" },
            { "method", "gpu.resource.search" }, { "params", { { "trace_id", traceId },
                { "offset", 0 }, { "limit", 1 } } }
        } );
        test.Check( resourceSearch.value( "ok", false ) &&
            resourceSearch["data"]["resources"].size() == 1 &&
            resourceSearch["data"]["total"] == "8",
            "GPU resource search uses compact summaries without reading enriched resource pages" );
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
    const auto frameImages = std::find_if( capabilities.begin(), capabilities.end(),
        []( const auto& value ) { return value.domain == "frame_image"; } );
    const bool sessionFrameImagesReady = frameImages != capabilities.end() && frameImages->present &&
        frameImages->indexed && frameImages->queryable;
        test.Check( sessionFrameImagesReady,
            "Session advertises FrameImage only after its disk-backed semantic reader is ready" );
        const auto correlatedSlice = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-correlated-slice" },
            { "method", "timeline.correlated_slice" }, { "params", {
                { "trace_id", traceId },
                { "ref", sessionSource->MakeEntityRef( "frame-identity", 9 ) },
                { "max_nodes", 128 } } }
        } );
        test.Check( correlatedSlice.value( "ok", false ) &&
            correlatedSlice["data"]["identity"]["frame_id"] == "9" &&
            correlatedSlice["data"]["jobs"].size() == 1 &&
            correlatedSlice["data"]["jobs"][0]["job_id"] == "500" &&
            correlatedSlice["data"]["gfx_dispatches"].size() == 1 &&
            correlatedSlice["data"]["gfx_dispatches"][0]["dispatch_id"] == "300",
            "Query 1.34 resolves a correlated Session Frame through Frame/Job/Gfx postings" );
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
            cpuSearch["data"]["zones"][1]["end_ns"] == "20",
            "Query 1.34 preserves Session CPU-zone hierarchy, source and SiteReuse semantics" );
        test.Check( cpuSearch.value( "ok", false ) && cpuSearch["data"]["zones"].size() == 3 &&
            cpuSearch["data"]["zones"][2]["start_ns"] == "32" &&
            cpuSearch["data"]["zones"][2]["end_ns"] == "-170" &&
            cpuSearch["data"]["zones"][2]["duration_ns"].is_null() &&
            cpuSearch["data"]["zones"][2]["complete"] == true,
            "Query 1.34 preserves both source endpoints for an inverted CPU-zone clock" );
        const auto cpuTree = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-cpu-zone-tree" },
            { "method", "zone.cpu.tree" }, { "params", { { "trace_id", traceId },
                { "ref", cpuSearch["data"]["zones"][0]["ref"] }, { "limit", 16 } } }
        } );
        test.Check( cpuTree.value( "ok", false ) &&
            cpuTree["data"]["children"].size() == 1 &&
            cpuTree["data"]["children"][0]["ref"] == cpuSearch["data"]["zones"][1]["ref"],
            "Query 1.34 resolves CPU-zone children through the exact parent posting index" );
        test.Check( cpuSearch.value( "ok", false ) && cpuSearch["data"]["zones"].size() == 3 &&
            cpuSearch["data"]["zones"][2]["timing_valid"] == false &&
            cpuSearch["data"]["zones"][2]["timing_invalid_reason"] == "source_clock_inversion",
            "Query 1.34 exposes source clock inversion without fabricating timing" );
        const auto cpuThreadSearch = query.Execute( {
            { "protocol", "tracy-query/1" }, { "id", "session-cpu-zone-thread" },
            { "method", "zone.cpu.search" }, { "params", { { "trace_id", traceId },
                { "thread_ref", mainThreadZoneRef }, { "start_ns", 15 },
                { "end_ns", 21 }, { "limit", 16 } } }
        } );
        test.Check( cpuThreadSearch.value( "ok", false ) &&
            cpuThreadSearch["data"]["zones"].size() == 3 &&
            std::all_of( cpuThreadSearch["data"]["zones"].begin(),
                cpuThreadSearch["data"]["zones"].end(), [&]( const auto& value ) {
                    return value["thread_ref"] == mainThreadZoneRef;
                } ), "Query 1.34 routes CPU-zone time/thread predicates through the immutable block index; count=" +
                    std::to_string( cpuThreadSearch.value( "ok", false ) ?
                        cpuThreadSearch["data"]["zones"].size() : 0 ) );
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
            const auto gpuContextSearch = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-gpu-zone-context" },
                { "method", "zone.gpu.search" }, { "params", { { "trace_id", traceId },
                    { "context_ref", gpuContextZeroRef }, { "start_ns", 37 },
                    { "end_ns", 39 }, { "limit", 16 } } }
            } );
            test.Check( gpuContexts.value( "ok", false ) && gpuContexts["data"]["contexts"].size() == 1 &&
                gpuContexts["data"]["contexts"][0]["type_name"] == "direct3d12" &&
                gpuContexts["data"]["contexts"][0]["zone_count"] == "1" &&
                gpuSearch.value( "ok", false ) && gpuSearch["data"]["zones"].size() == 1 &&
                gpuSearch["data"]["zones"][0]["name"] == "Synthetic GPU Zone" &&
                gpuSearch["data"]["zones"][0]["gpu_start_ns"] == "36" &&
                gpuSearch["data"]["zones"][0]["gpu_end_ns"] == "40" &&
                gpuSearch["data"]["zones"][0]["cpu_start_ns"] == "-166" &&
                gpuSearch["data"]["zones"][0]["cpu_end_ns"] == "-162" &&
                gpuSearch["data"]["zones"][0]["self_time_ns"] == "4" &&
                gpuSearch["data"]["zones"][0]["query_id"] == 7 &&
                gpuSearch["data"]["zones"][0]["complete"] == true &&
                gpuContextSearch.value( "ok", false ) &&
                gpuContextSearch["data"]["zones"].size() == 1 &&
                gpuContextSearch["data"]["zones"][0]["context_ref"] == gpuContextZeroRef,
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
            const auto memoryPoolEvents = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-memory-pool-events" },
                { "method", "memory.events" }, { "params", { { "trace_id", traceId },
                    { "pool_ref", indexedMemoryPoolRef }, { "limit", 16 } } }
            } );
            const auto memoryActiveTime = memoryPoolEvents.value( "ok", false ) &&
                !memoryPoolEvents["data"]["events"].empty() ? std::stoll(
                    memoryPoolEvents["data"]["events"][0]["allocation_ns"].get<std::string>() ) : 0;
            const auto memoryActive = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-memory-active" },
                { "method", "memory.active_at_time" }, { "params", { { "trace_id", traceId },
                    { "pool_ref", indexedMemoryPoolRef }, { "time_ns", memoryActiveTime },
                    { "limit", 16 } } }
            } );
            test.Check( memoryPools.value( "ok", false ) && memoryPools["data"]["pools"].size() == 2 &&
                memoryPools["data"]["pools"][0]["active_bytes"] == "8192" &&
                memoryPools["data"]["pools"][1]["name"] == "GPU D3D12 Texture" &&
                memoryEvents.value( "ok", false ) && memoryEvents["data"]["events"].size() == 4 &&
                memoryEvents["data"]["events"][0]["allocation_callstack"].is_null() &&
                memoryEvents["data"]["events"][1]["complete"] == false &&
                memoryEvents["data"]["events"][3]["allocation_callstack"] == "4" &&
                memoryEvents["data"]["events"][3]["free_callstack"] == "5" &&
                memoryPoolEvents.value( "ok", false ) &&
                !memoryPoolEvents["data"]["events"].empty() &&
                std::all_of( memoryPoolEvents["data"]["events"].begin(),
                    memoryPoolEvents["data"]["events"].end(), [&]( const auto& value ) {
                        return value["pool_ref"] == indexedMemoryPoolRef;
                    } ) && memoryActive.value( "ok", false ) &&
                !memoryActive["data"]["events"].empty(),
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
                { "params", { { "trace_id", traceId },
                    { "thread_ref", sessionSource->MakeEntityRef( "thread", 42 ) } } }
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
        if( sessionHardwareSamplesReady ) try
        {
            const auto counts = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-hardware-samples" },
                { "method", "hardware_sample.counts" }, { "params", { { "trace_id", traceId } } }
            } );
            const auto events = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-hardware-sample-events" },
                { "method", "hardware_sample.events" }, { "params", { { "trace_id", traceId },
                    { "address", "0xabc" }, { "kind", "branch_misses" }, { "limit", 8 } } }
            } );
            test.Check( counts.value( "ok", false ) && counts["data"]["addresses"].size() == 1 &&
                counts["data"]["addresses"][0]["cycles"] == "1" &&
                counts["data"]["addresses"][0]["branch_misses"] == "1" &&
                events.value( "ok", false ) && events["data"]["events"].size() == 1 &&
                events["data"]["events"][0]["time_ns"] == "24",
                "Query 1.34 reads exact Session Hardware Sample counts and paged events without a Worker" );
        }
        catch( const std::exception& exception )
        {
            test.Check( false, std::string( "Query 1.34 Session Hardware Sample Reader is unavailable: " ) + exception.what() );
        }
        if( sessionSchedulingReady ) try
        {
            const auto contexts = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-contexts" },
                { "method", "context_switch.range" }, { "params", { { "trace_id", traceId },
                    { "limit", 4 } } }
            } );
            test.Check( contexts.value( "ok", false ) &&
                contexts["data"]["context_switches"].size() == 4 &&
                contexts["data"]["context_switches"][1]["wakeup_ns"] == "22" &&
                contexts["data"]["context_switches"][1]["reason_name"] == "delay_execution" &&
                contexts["data"]["context_switches"][2]["complete"] == false &&
                contexts["data"]["context_switches"][2]["end_ns"].is_null(),
                "Query 1.34 reads Session Context Switch intervals without a Worker" );
            const auto mainThreadRef = sessionSource->MakeEntityRef( "thread", 42 );
            const auto threadContexts = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-thread-contexts" },
                { "method", "context_switch.thread" }, { "params", { { "trace_id", traceId },
                    { "thread_ref", mainThreadRef }, { "limit", 8 } } }
            } );
            const auto cpuTimeline = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-cpu-zero" },
                { "method", "cpu.timeline" }, { "params", { { "trace_id", traceId },
                    { "cpu", 0 }, { "limit", 8 } } }
            } );
            test.Check( threadContexts.value( "ok", false ) &&
                !threadContexts["data"]["context_switches"].empty() &&
                std::all_of( threadContexts["data"]["context_switches"].begin(),
                    threadContexts["data"]["context_switches"].end(),
                    [&]( const auto& value ) { return value["thread_ref"] == mainThreadRef; } ) &&
                cpuTimeline.value( "ok", false ) &&
                !cpuTimeline["data"]["segments"].empty() &&
                std::all_of( cpuTimeline["data"]["segments"].begin(),
                    cpuTimeline["data"]["segments"].end(),
                    []( const auto& value ) { return value["cpu"] == 0; } ),
                "Query 1.34 routes thread and CPU filters through immutable Scheduling indexes" );
            const auto topology = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-cpu-topology" },
                { "method", "cpu.topology" }, { "params", { { "trace_id", traceId } } }
            } );
            test.Check( topology.value( "ok", false ) &&
                topology["data"]["logical_cpus"].size() == 2 &&
                topology["data"]["logical_cpus"][1]["cpu"] == 1,
                "Query 1.34 reads exact Session CPU topology without a Worker" );
            const auto cpuUsage = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-cpu-usage" },
                { "method", "cpu.usage" }, { "params", { { "trace_id", traceId },
                    { "limit", 2 } } }
            } );
            test.Check( cpuUsage.value( "ok", false ) &&
                cpuUsage["data"]["points"].size() == 2 &&
                cpuUsage["data"]["points"][0]["time_ns"] == "0" &&
                cpuUsage["page"]["next_cursor"].is_string(),
                "Query 1.34 pages Session CPU usage directly from disk without a Worker" );
        }
        catch( const std::exception& exception )
        {
            test.Check( false, std::string( "Query 1.34 Session Context Switch Reader is unavailable: " ) + exception.what() );
        }
        if( sessionThreadsReady ) try
        {
            const auto threads = query.Execute( {
                { "protocol", "tracy-query/1" }, { "id", "session-thread-list" },
                { "method", "thread.list" }, { "params", { { "trace_id", traceId },
                    { "filter", { { "text", "Main Thread" } } } } }
            } );
            test.Check( threads.value( "ok", false ) &&
                threads["data"]["threads"].size() == 1 &&
                threads["data"]["threads"][0]["process_id"] == "1001" &&
                threads["data"]["threads"][0]["sample_count"] == "1" &&
                threads["data"]["threads"][0]["group_hint"] == 7,
                "Query 1.34 reads exact compact Session Thread summary without a Worker" );
        }
        catch( const std::exception& exception )
        {
            test.Check( false, std::string( "Query 1.34 Session Thread Reader is unavailable: " ) + exception.what() );
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
    const auto jobRoot = tracy::analysis::TraceSessionJobIndexRoot(
        publishedSession, manifest );
    test.Check( jobRoot.parent_path().filename() ==
        std::to_string( tracy::analysis::TraceSessionJobIndexSchemaVersion ),
        "Session Job index path isolates the current schema from older committed generations" );
    const auto jobFile = jobRoot / "jobs.bin";
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
    const auto frameImageDataFile = tracy::analysis::TraceSessionFrameImageIndexRoot(
        publishedSession, manifest ) / "frame-images.bc1";
    {
        std::fstream damaged( frameImageDataFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x6b );
        damaged.write( &byte, 1 );
    }
    tracy::analysis::TraceSessionFrameImageStats rejectedFrameImageStats;
    test.Check( !tracy::analysis::AuditTraceSessionFrameImageDerived(
        publishedSession, manifest, rejectedFrameImageStats, error ) &&
        error == "session_frame_image_data_sha256_mismatch",
        "Session Final Audit rejects corrupted committed FrameImage BC1 evidence" );
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
    const auto plotFile = tracy::analysis::TraceSessionPlotIndexRoot(
        publishedSession, manifest ) / "plots.bin";
    {
        std::fstream damaged( plotFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end ); char byte = 0; damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end ); damaged.put( byte ^ '\x31' );
    }
    tracy::analysis::TraceSessionPlotStats rejectedPlotStats;
    test.Check( !tracy::analysis::AuditTraceSessionPlotDerived(
        publishedSession, manifest, rejectedPlotStats, error ) &&
        error == "session_plot_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed Plot semantic index" );
    const auto messageFile = tracy::analysis::TraceSessionMessageIndexRoot(
        publishedSession, manifest ) / "messages.bin";
    {
        std::fstream damaged( messageFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end ); char byte = 0; damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end ); damaged.put( byte ^ '\x32' );
    }
    tracy::analysis::TraceSessionMessageStats rejectedMessageStats;
    test.Check( !tracy::analysis::AuditTraceSessionMessageDerived(
        publishedSession, manifest, rejectedMessageStats, error ) &&
        error == "session_message_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed Message semantic index" );
    const auto lockFile = tracy::analysis::TraceSessionLockIndexRoot(
        publishedSession, manifest ) / "locks.bin";
    {
        std::fstream damaged( lockFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end ); char byte = 0; damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end ); damaged.put( byte ^ '\x33' );
    }
    tracy::analysis::TraceSessionLockStats rejectedLockStats;
    test.Check( !tracy::analysis::AuditTraceSessionLockDerived(
        publishedSession, manifest, rejectedLockStats, error ) &&
        error == "session_lock_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed Lock semantic index" );
    const auto relationFile = tracy::analysis::TraceSessionRelationIndexRoot(
        publishedSession, manifest ) / "relations.bin";
    {
        std::fstream damaged( relationFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x37 );
        damaged.write( &byte, 1 );
    }
    tracy::analysis::TraceSessionRelationStats rejectedRelationStats;
    test.Check( !tracy::analysis::AuditTraceSessionRelationDerived(
        publishedSession, manifest, rejectedRelationStats, error ) &&
        error == "session_relation_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed Relation semantic index" );
    const auto runtimeFile = tracy::analysis::TraceSessionRuntimeIndexRoot(
        publishedSession, manifest ) / "runtime-script.bin";
    {
        std::fstream damaged( runtimeFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x29 );
        damaged.write( &byte, 1 );
    }
    tracy::analysis::TraceSessionRuntimeStats rejectedRuntimeStats;
    test.Check( !tracy::analysis::AuditTraceSessionRuntimeDerived(
        publishedSession, manifest, rejectedRuntimeStats, error ) &&
        error == "session_runtime_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed Runtime/Script semantic index" );
    const auto ioGfxFile = tracy::analysis::TraceSessionIoGfxIndexRoot(
        publishedSession, manifest ) / "io-gfx.bin";
    {
        std::fstream damaged( ioGfxFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x4d );
        damaged.write( &byte, 1 );
    }
    tracy::analysis::TraceSessionIoGfxStats rejectedIoGfxStats;
    test.Check( !tracy::analysis::AuditTraceSessionIoGfxDerived(
        publishedSession, manifest, rejectedIoGfxStats, error ) &&
        error == "session_io_gfx_file_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed I/O/Gfx semantic index" );
    const auto symbolFile = tracy::analysis::TraceSessionSymbolIndexRoot(
        publishedSession, manifest ) / "symbols.bin";
    {
        std::fstream damaged( symbolFile, std::ios::binary | std::ios::in | std::ios::out );
        damaged.seekg( -1, std::ios::end );
        char byte = 0;
        damaged.read( &byte, 1 );
        damaged.seekp( -1, std::ios::end );
        byte ^= char( 0x53 );
        damaged.write( &byte, 1 );
    }
    tracy::analysis::TraceSessionSymbolStats rejectedSymbolStats;
    test.Check( !tracy::analysis::AuditTraceSessionSymbolDerived(
        publishedSession, manifest, rejectedSymbolStats, error ) &&
        error == "session_symbol_metadata_sha256_mismatch",
        "Session Final Audit rejects a corrupted committed Source/Callstack/Symbol semantic index" );
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
        TestCanonicalPackedFrameStorage( test, directory );
        TestProtocolJournalInventory( test, directory );
        TestSharedGpuPointerIdentityOutlivesOwnership( test );
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
