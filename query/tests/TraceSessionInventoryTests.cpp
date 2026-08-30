#include "TracyTraceSessionInventory.hpp"
#include "TracyTraceSessionCanonical.hpp"
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
