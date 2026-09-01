#include "TracyTraceSessionGpuCanonical.hpp"

#include "TracyGpuAnalysis.hpp"
#include "TracyGpuAnalysisCache.hpp"
#include "TracyGpuAnalysisPath.hpp"
#include "TracyGpuAnalysisStore.hpp"
#include "TracyHash.hpp"
#include "TracyJnGpuCatalog.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"
#include "TracyStreamJournal.hpp"
#include "TracyJnGpuCatalogResolve.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#  include <Windows.h>
#  undef FindResource
#endif

namespace tracy::analysis
{
static_assert( size_t( JnGpuCatalogBatchKind::String ) + 1 ==
    TraceSessionGpuCatalogBatchKindCount );

namespace
{

constexpr uint64_t TimeTransformFileMagic = 0x31544653544e4aull; // JNSTF1
constexpr uint64_t TimeTransformManifestMagic = 0x314d5453544e4aull; // JNSTM1
constexpr uint32_t TimeTransformSchema = 2;
constexpr uint64_t SourceGapResourceIdBase = 0xE000000000000000ull;

#pragma pack( push, 1 )
struct TimeTransformFileHeader
{
    uint64_t magic = TimeTransformFileMagic;
    uint32_t schema = TimeTransformSchema;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    int64_t baseTime = 0;
    double timerMultiplier = 0;
    uint64_t processId = 0;
    uint64_t welcomeCount = 0;
    uint32_t generationBytes = 0;
};
#pragma pack( pop )

struct TimeTransformManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    uint64_t welcomeCount = 0;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_time_transform_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec; std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_time_transform_atomic_replace_failed:" + ec.message(); return false;
#endif
}

bool SaveTimeTransformManifest( const std::filesystem::path& root,
    const TimeTransformManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_time_transform_manifest_open_failed"; return false; }
    out << "magic " << TimeTransformManifestMagic << '\n'
        << "schema " << TimeTransformSchema << '\n'
        << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n'
        << "source_size " << value.sourceSize << '\n'
        << "generation " << std::quoted( value.generation ) << '\n'
        << "file_bytes " << value.fileBytes << '\n'
        << "file_sha256 " << std::quoted( value.fileSha256 ) << '\n'
        << "welcome_count " << value.welcomeCount << '\n';
    out.flush(); if( !out ) { error = "session_time_transform_manifest_write_failed"; return false; }
    out.close(); return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadTimeTransformManifest( const std::filesystem::path& root,
    TimeTransformManifest& value, std::string& error )
{
    value = {}; std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_time_transform_manifest_not_found"; return false; }
    uint64_t magic = 0; uint32_t schema = 0; std::string key;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic;
        else if( key == "schema" ) in >> schema;
        else if( key == "source_sha256" ) in >> std::quoted( value.sourceSha256 );
        else if( key == "source_size" ) in >> value.sourceSize;
        else if( key == "generation" ) in >> std::quoted( value.generation );
        else if( key == "file_bytes" ) in >> value.fileBytes;
        else if( key == "file_sha256" ) in >> std::quoted( value.fileSha256 );
        else if( key == "welcome_count" ) in >> value.welcomeCount;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_time_transform_manifest_parse_failed"; return false; }
    }
    if( magic != TimeTransformManifestMagic || schema != TimeTransformSchema ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 || value.welcomeCount == 0 )
    { error = "session_time_transform_manifest_invalid"; return false; }
    return true;
}

template<typename T>
bool AppendFixedRecords( std::vector<T>& output, const uint8_t* records,
    uint32_t count, int64_t ( *convert )( int64_t, const TraceSessionTimeTransform& ),
    const TraceSessionTimeTransform& transform )
{
    if( count > ( std::numeric_limits<size_t>::max() - output.size() ) ) return false;
    const auto first = output.size();
    output.resize( first + count );
    if( count != 0 ) std::memcpy( output.data() + first, records, sizeof( T ) * count );
    if( convert ) for( size_t i = first; i < output.size(); ++i ) output[i].time = convert( output[i].time, transform );
    return true;
}

int64_t ConvertTime( int64_t value, const TraceSessionTimeTransform& transform )
{
    return transform.ToNanoseconds( value );
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    {
        error = "session_gpu_protocol_record_invalid";
        return false;
    }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    {
        error = "session_gpu_protocol_type_mismatch";
        return false;
    }
    return true;
}

struct MetadataState
{
    TraceSessionTimeTransform* transform = nullptr;
    uint64_t welcomeCount = 0;
};

bool VisitMetadata( const TraceSessionCanonicalRecord& record, void* userData, std::string& error )
{
    auto& state = *static_cast<MetadataState*>( userData );
    if( record.kind != TraceSessionCanonicalRecordKind::TransportRecord ||
        record.type != uint8_t( tracy::stream::RecordType::ClientToServer ) ||
        ( record.flags & tracy::stream::RecordFlagHandshake ) == 0 ||
        record.payload.size() != sizeof( WelcomeMessage ) ) return true;
    WelcomeMessage welcome {};
    std::memcpy( &welcome, record.payload.data(), sizeof( welcome ) );
    if( !std::isfinite( welcome.timerMul ) || welcome.timerMul <= 0 )
    {
        error = "session_welcome_timer_multiplier_invalid";
        return false;
    }
    if( state.welcomeCount != 0 &&
        ( state.transform->timerMultiplier != welcome.timerMul ||
          state.transform->baseTime != welcome.initBegin ||
          state.transform->processId != welcome.pid ) )
    {
        error = "session_welcome_time_transform_conflict";
        return false;
    }
    state.transform->timerMultiplier = welcome.timerMul;
    state.transform->baseTime = welcome.initBegin;
    state.transform->processId = welcome.pid;
    state.transform->present = true;
    state.welcomeCount++;
    return true;
}

struct CatalogRuntime
{
    uint32_t lastSequence = 0;
    size_t generationIndex = 0;
    bool began = false;
    bool ended = false;
    bool valid = false;
};

struct GpuLoadState
{
    JnTraceData* data = nullptr;
    const TraceSessionTimeTransform* transform = nullptr;
    TraceSessionGpuCanonicalStats* stats = nullptr;
    std::unordered_map<uint64_t, std::vector<uint8_t>> catalogPayloads;
    std::unordered_map<uint64_t, CatalogRuntime> catalogRuntime;
    std::unordered_map<uint32_t, std::vector<JnGpuReferenceSetEntry>> resourceSets;
    std::unordered_map<uint64_t, int64_t> passTimes;
    bool loadReferenceEvidence = true;
    bool loadRangeEvidence = true;
    bool loadCatalogEnrichment = true;
    bool loadCatalogCore = true;
    bool loadCatalogAuxiliary = true;
    bool loadCatalogStrings = true;
};

void MarkCatalogPresent( JnTraceData& data )
{
    data.present = true;
    data.schemaVersion = JnTraceSchemaVersion;
    data.gpuCatalogPresent = true;
    data.gpuCatalogSchemaVersion = JnGpuCatalogSchemaVersion;
    data.gpuDetailedEvidenceSchemaVersion = JnGpuDetailedEvidenceSchemaVersion;
}

bool DecodeLargePayload( const TraceSessionCanonicalRecord& record, const QueueItem& item,
    uint64_t& payloadId, const uint8_t*& bytes, uint32_t& size, std::string& error )
{
    const auto fixed = QueueDataSize[record.type];
    if( record.payload.size() < fixed + sizeof( uint32_t ) )
    {
        error = "session_gpu_large_payload_header_truncated";
        return false;
    }
    std::memcpy( &size, record.payload.data() + fixed, sizeof( size ) );
    if( size != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( uint32_t ) + size )
    {
        error = "session_gpu_large_payload_size_mismatch";
        return false;
    }
    payloadId = item.stringTransfer.ptr;
    bytes = record.payload.data() + fixed + sizeof( uint32_t );
    return true;
}

bool ProcessCatalogControl( const TraceSessionCanonicalRecord& record,
    const QueueJnGpuCatalogControl& event, GpuLoadState& state, std::string& error )
{
    auto& data = *state.data;
    MarkCatalogPresent( data );
    state.stats->controlRecords++;
    if( event.generation == 0 || event.kind > uint8_t( JnGpuCatalogControlKind::GenerationEnd ) ||
        event.state > uint8_t( JnGpuCatalogGenerationState::InvalidBootstrapTimeout ) )
    {
        error = "session_gpu_catalog_control_invalid";
        return false;
    }
    const auto kind = JnGpuCatalogControlKind( event.kind );
    auto found = state.catalogRuntime.find( event.generation );
    if( kind == JnGpuCatalogControlKind::GenerationBegin )
    {
        if( found != state.catalogRuntime.end() || event.sequence != 1 )
        {
            error = "session_gpu_catalog_generation_begin_invalid";
            return false;
        }
        CatalogRuntime runtime;
        runtime.lastSequence = event.sequence;
        runtime.began = true;
        runtime.valid = event.state == uint8_t( JnGpuCatalogGenerationState::Building ) &&
            ( event.flags & uint8_t( JnGpuCatalogControlFlags::CoreInvalid ) ) == 0;
        runtime.generationIndex = data.gpuCatalogGenerations.size();
        state.catalogRuntime.emplace( event.generation, runtime );
        if( data.gpuCatalogGenerations.empty() ) data.gpuCatalogValid = true;
        data.gpuCatalogValid = data.gpuCatalogValid && runtime.valid;
        const auto time = state.transform->ToNanoseconds( event.time );
        data.gpuCatalogGenerations.push_back( JnGpuCatalogGenerationData {
            event.generation, event.value, 0, time, 0, event.sequence, 0, 0, 0, 0,
            event.state, event.flags, 1, 0, uint8_t( runtime.valid ? 1 : 0 ) } );
        data.gpuCatalogControls.push_back( JnGpuCatalogControlData { time, event.generation,
            event.value, record.threadContext, event.sequence, event.kind, event.state, event.flags } );
        return true;
    }
    if( found == state.catalogRuntime.end() || !found->second.began ||
        ( kind == JnGpuCatalogControlKind::GenerationEnd && found->second.ended ) ||
        event.sequence != found->second.lastSequence + 1 )
    {
        error = "session_gpu_catalog_control_sequence_gap";
        return false;
    }
    auto& runtime = found->second;
    runtime.lastSequence = event.sequence;
    const bool controlValid = ( event.flags & uint8_t( JnGpuCatalogControlFlags::CoreInvalid ) ) == 0 &&
        event.state != uint8_t( JnGpuCatalogGenerationState::InvalidCoreGap ) &&
        event.state != uint8_t( JnGpuCatalogGenerationState::InvalidCapacity ) &&
        event.state != uint8_t( JnGpuCatalogGenerationState::InvalidBootstrapTimeout );
    runtime.valid = runtime.valid && controlValid;
    if( kind == JnGpuCatalogControlKind::GenerationEnd ) runtime.ended = true;
    const auto time = state.transform->ToNanoseconds( event.time );
    data.gpuCatalogControls.push_back( JnGpuCatalogControlData { time, event.generation,
        event.value, record.threadContext, event.sequence, event.kind, event.state, event.flags } );
    auto& generation = data.gpuCatalogGenerations[runtime.generationIndex];
    generation.lastSequence = event.sequence;
    generation.state = event.state;
    generation.flags |= event.flags;
    generation.valid = runtime.valid ? 1 : 0;
    if( kind == JnGpuCatalogControlKind::GenerationEnd )
    {
        generation.endValue = event.value;
        generation.endTime = time;
        generation.ended = 1;
    }
    data.gpuCatalogValid = data.gpuCatalogValid && runtime.valid;
    return true;
}

template<typename T>
uint64_t AppendCatalogRecords( std::vector<T>& output, const uint8_t* records,
    uint32_t count, const TraceSessionTimeTransform& transform )
{
    const auto first = output.size();
    output.resize( first + count );
    std::memcpy( output.data() + first, records, sizeof( T ) * count );
    if constexpr( requires( T value ) { value.time; } )
        for( size_t i = first; i < output.size(); ++i ) output[i].time = transform.ToNanoseconds( output[i].time );
    return first;
}

bool ProcessCatalogBatch( const QueueJnGpuCatalogBatch& event,
    GpuLoadState& state, std::string& error )
{
    auto& data = *state.data;
    MarkCatalogPresent( data );
    const auto payloadIt = state.catalogPayloads.find( event.payloadId );
    const auto runtimeIt = state.catalogRuntime.find( event.generation );
    if( event.generation == 0 || event.payloadId == 0 || event.recordCount == 0 ||
        event.kind > uint8_t( JnGpuCatalogBatchKind::String ) ||
        payloadIt == state.catalogPayloads.end() || runtimeIt == state.catalogRuntime.end() ||
        !runtimeIt->second.began || event.sequence != runtimeIt->second.lastSequence + 1 )
    {
        error = "session_gpu_catalog_batch_metadata_invalid";
        return false;
    }
    auto payload = std::move( payloadIt->second );
    state.catalogPayloads.erase( payloadIt );
    if( payload.size() != event.payloadBytes || payload.size() < sizeof( JnGpuCatalogBatchEnvelopeV1 ) )
    {
        error = "session_gpu_catalog_batch_payload_size_mismatch";
        return false;
    }
    JnGpuCatalogBatchEnvelopeV1 envelope {};
    std::memcpy( &envelope, payload.data(), sizeof( envelope ) );
    const auto* records = payload.data() + sizeof( envelope );
    const auto recordBytes = payload.size() - sizeof( envelope );
    if( envelope.magic != JnGpuCatalogBatchMagic ||
        envelope.catalogSchema != JnGpuCatalogSchemaVersion ||
        envelope.evidenceSchema != JnGpuDetailedEvidenceSchemaVersion ||
        envelope.recordCount != event.recordCount || envelope.payloadBytes != recordBytes ||
        envelope.flags != event.flags || JnGpuCatalogChecksum64( records, recordBytes ) != envelope.checksum )
    {
        error = "session_gpu_catalog_batch_envelope_invalid";
        return false;
    }
    uint16_t expectedRecordBytes = 0;
    uint8_t expectedEncoding = uint8_t( JnGpuCatalogBatchEncoding::FixedV1 );
    switch( JnGpuCatalogBatchKind( event.kind ) )
    {
    case JnGpuCatalogBatchKind::Resource: expectedRecordBytes = sizeof( JnGpuCatalogResourceRecordV1 ); break;
    case JnGpuCatalogBatchKind::Allocation: expectedRecordBytes = sizeof( JnGpuCatalogAllocationRecordV1 ); break;
    case JnGpuCatalogBatchKind::View: expectedRecordBytes = sizeof( JnGpuCatalogViewRecordV1 ); break;
    case JnGpuCatalogBatchKind::Logical: expectedRecordBytes = sizeof( JnGpuCatalogLogicalRecordV1 ); break;
    case JnGpuCatalogBatchKind::Part: expectedRecordBytes = sizeof( JnGpuCatalogPartRecordV1 ); break;
    case JnGpuCatalogBatchKind::Relation: expectedRecordBytes = sizeof( JnGpuCatalogRelationRecordV1 ); break;
    case JnGpuCatalogBatchKind::VirtualGeometry: expectedRecordBytes = sizeof( JnGpuCatalogVgRecordV1 ); break;
    case JnGpuCatalogBatchKind::RangeSet:
        expectedRecordBytes = sizeof( JnGpuRangeSetRecordV1 );
        expectedEncoding = uint8_t( JnGpuCatalogBatchEncoding::RangeSetV1 );
        break;
    case JnGpuCatalogBatchKind::DetailedEvidence:
        expectedRecordBytes = sizeof( JnGpuDetailedEvidenceRecordV1 );
        expectedEncoding = uint8_t( JnGpuCatalogBatchEncoding::DetailedEvidenceV1 );
        break;
    case JnGpuCatalogBatchKind::String: expectedRecordBytes = 0; break;
    }
    if( event.encoding != expectedEncoding || envelope.recordBytes != expectedRecordBytes ||
        ( expectedRecordBytes != 0 && uint64_t( expectedRecordBytes ) * event.recordCount != recordBytes ) )
    {
        error = "session_gpu_catalog_batch_encoding_invalid";
        return false;
    }
    uint64_t first = 0;
    switch( JnGpuCatalogBatchKind( event.kind ) )
    {
    case JnGpuCatalogBatchKind::Resource:
        if( state.loadCatalogCore ) first = AppendCatalogRecords(
            data.gpuCatalogResources, records, event.recordCount, *state.transform );
        break;
    case JnGpuCatalogBatchKind::Allocation:
        if( state.loadCatalogCore ) first = AppendCatalogRecords(
            data.gpuCatalogAllocations, records, event.recordCount, *state.transform );
        break;
    case JnGpuCatalogBatchKind::View:
        if( state.loadCatalogAuxiliary ) first = AppendCatalogRecords(
            data.gpuCatalogViews, records, event.recordCount, *state.transform );
        break;
    case JnGpuCatalogBatchKind::Logical:
        if( state.loadCatalogEnrichment ) first = AppendCatalogRecords(
            data.gpuCatalogLogicals, records, event.recordCount, *state.transform );
        break;
    case JnGpuCatalogBatchKind::Part:
        if( state.loadCatalogAuxiliary ) first = AppendCatalogRecords(
            data.gpuCatalogParts, records, event.recordCount, *state.transform );
        break;
    case JnGpuCatalogBatchKind::Relation:
        if( state.loadCatalogEnrichment ) first = AppendCatalogRecords(
            data.gpuCatalogRelations, records, event.recordCount, *state.transform );
        break;
    case JnGpuCatalogBatchKind::VirtualGeometry:
        if( state.loadCatalogAuxiliary ) first = AppendCatalogRecords(
            data.gpuCatalogVg, records, event.recordCount, *state.transform );
        break;
    case JnGpuCatalogBatchKind::RangeSet:
        if( state.loadRangeEvidence ) first = AppendCatalogRecords(
            data.gpuRangeSets, records, event.recordCount, *state.transform );
        break;
    case JnGpuCatalogBatchKind::DetailedEvidence:
        if( state.loadRangeEvidence ) first = AppendCatalogRecords(
            data.gpuDetailedEvidence, records, event.recordCount, *state.transform );
        break;
    case JnGpuCatalogBatchKind::String:
    {
        if( !state.loadCatalogStrings ) break;
        first = data.gpuCatalogStrings.size();
        const auto* cursor = records;
        const auto* end = records + recordBytes;
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            if( size_t( end - cursor ) < sizeof( JnGpuCatalogStringRecordHeaderV1 ) )
            { error = "session_gpu_catalog_string_header_truncated"; return false; }
            JnGpuCatalogStringRecordHeaderV1 header {};
            std::memcpy( &header, cursor, sizeof( header ) ); cursor += sizeof( header );
            if( header.stringId == 0 || header.byteLength > 256 || size_t( end - cursor ) < header.byteLength )
            { error = "session_gpu_catalog_string_invalid"; return false; }
            data.gpuCatalogStrings.push_back( { event.generation, header,
                std::string( reinterpret_cast<const char*>( cursor ), header.byteLength ) } );
            cursor += header.byteLength;
        }
        if( cursor != end ) { error = "session_gpu_catalog_string_trailing_bytes"; return false; }
        break;
    }
    }
    auto& runtime = runtimeIt->second;
    runtime.lastSequence = event.sequence;
    auto& generation = data.gpuCatalogGenerations[runtime.generationIndex];
    generation.lastSequence = event.sequence;
    generation.batchCount++;
    generation.recordCount += event.recordCount;
    generation.payloadBytes += envelope.payloadBytes;
    data.gpuCatalogBatches.push_back( { event.generation, envelope.checksum, 0, first,
        event.sequence, event.recordCount, envelope.payloadBytes, event.kind,
        event.encoding, event.flags, 1 } );
    state.stats->catalogRecordCounts[event.kind] += event.recordCount;
    state.stats->catalogPayloadBytes[event.kind] += envelope.payloadBytes;
    state.stats->catalogBatches++;
    return true;
}

bool VisitGpuCanonical( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    auto& state = *static_cast<GpuLoadState*>( userData );
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    auto& data = *state.data;
    switch( item.hdr.type )
    {
    case QueueType::JnGpuCatalogBatchData:
    {
        uint64_t payloadId = 0; const uint8_t* bytes = nullptr; uint32_t size = 0;
        if( !DecodeLargePayload( record, item, payloadId, bytes, size, error ) ) return false;
        if( payloadId == 0 || size < sizeof( JnGpuCatalogBatchEnvelopeV1 ) ||
            size > 64u * 1024 * 1024 || !state.catalogPayloads.emplace(
                payloadId, std::vector<uint8_t>( bytes, bytes + size ) ).second )
        { error = "session_gpu_catalog_payload_invalid"; return false; }
        state.stats->catalogPayloads++;
        return true;
    }
    case QueueType::JnGpuCatalogControl:
        return ProcessCatalogControl( record, item.jnGpuCatalogControl, state, error );
    case QueueType::JnGpuCatalogBatch:
        return ProcessCatalogBatch( item.jnGpuCatalogBatch, state, error );
    case QueueType::JnGpuReferenceSetDefinition:
    {
        if( !state.loadReferenceEvidence ) return true;
        uint64_t setId = 0; const uint8_t* bytes = nullptr; uint32_t size = 0;
        if( !DecodeLargePayload( record, item, setId, bytes, size, error ) ) return false;
        if( setId == 0 || size == 0 || size % sizeof( JnGpuReferenceSetEntry ) != 0 ||
            size / sizeof( JnGpuReferenceSetEntry ) > 0xffff )
        { error = "session_gpu_resource_set_definition_invalid"; return false; }
        std::vector<JnGpuReferenceSetEntry> entries( size / sizeof( JnGpuReferenceSetEntry ) );
        std::memcpy( entries.data(), bytes, size );
        if( std::any_of( entries.begin(), entries.end(), []( const auto& entry ) { return entry.resourceId == 0; } ) ||
            !state.resourceSets.emplace( uint32_t( setId ), std::move( entries ) ).second )
        { error = "session_gpu_resource_set_definition_invalid"; return false; }
        state.stats->referenceSetDefinitions++;
        return true;
    }
    case QueueType::JnGpuReferencePass:
    {
        if( !state.loadReferenceEvidence ) return true;
        const auto& event = item.jnGpuReferencePass;
        const auto time = state.transform->ToNanoseconds( event.time );
        data.present = true; data.schemaVersion = JnTraceSchemaVersion;
        data.gpuReferencePasses.push_back( { time, event.passId, event.frameIndex,
            record.threadContext, event.taxonomyId, event.taxonomyLevel, event.flags } );
        if( event.passId != 0 ) state.passTimes[event.passId] = time;
        return true;
    }
    case QueueType::JnGpuReferenceUse:
    {
        if( !state.loadReferenceEvidence ) return true;
        const auto& event = item.jnGpuReferenceUse;
        data.present = true; data.schemaVersion = JnTraceSchemaVersion;
        data.gpuReferenceUses.push_back( { state.transform->ToNanoseconds( event.time ),
            event.passId, event.resourceId, record.threadContext, event.usageMask, 0, event.flags, 1 } );
        return true;
    }
    case QueueType::JnGpuReferenceSetUse:
    {
        if( !state.loadReferenceEvidence ) return true;
        const auto& event = item.jnGpuReferenceSetUse;
        const auto set = state.resourceSets.find( event.resourceSetId );
        const auto pass = state.passTimes.find( event.passId );
        if( event.encoding != 2 || set == state.resourceSets.end() || pass == state.passTimes.end() ||
            set->second.size() != event.entryCount )
        { error = "session_gpu_resource_set_use_invalid"; return false; }
        for( const auto& entry : set->second ) data.gpuReferenceUses.push_back( {
            pass->second, event.passId, entry.resourceId, record.threadContext,
            entry.usageMask, event.resourceSetId, event.flags, event.encoding } );
        state.stats->referenceSetUses++;
        state.stats->expandedReferenceUses += set->second.size();
        return true;
    }
    case QueueType::JnGpuReferenceEnd:
    {
        if( !state.loadReferenceEvidence ) return true;
        const auto& event = item.jnGpuReferenceEnd;
        data.gpuReferenceEnds.push_back( { state.transform->ToNanoseconds( event.time ),
            event.passId, event.commandListId, record.threadContext,
            event.totalReferenceCount, event.droppedReferenceCount, event.flags } );
        state.passTimes.erase( event.passId );
        return true;
    }
    default: return true;
    }
}

#pragma pack( push, 1 )
struct ResourceSetIndexRecord
{
    uint32_t setId = 0;
    uint32_t count = 0;
    uint64_t entryOffset = 0;
};
#pragma pack( pop )

static_assert( sizeof( ResourceSetIndexRecord ) == 16 );

struct GpuPassRangeRawEntry
{
    uint64_t generation = 0;
    // Range records are produced against the explicit Gfx GPU-pass identity,
    // while ResourceSet evidence is keyed by the reference-pass identity.
    // Keep both: evidencePassId is the disk-join key and record.passInstanceId
    // remains the original typed identity exposed to Query.
    uint64_t evidencePassId = 0;
    JnGpuRangeSetRecordV1 record {};
};

struct GpuPassAliasEntry
{
    uint64_t explicitPassId = 0;
    uint64_t referencePassId = 0;
};

static_assert( std::is_trivially_copyable_v<GpuPassRangeRawEntry> );
static_assert( std::is_trivially_copyable_v<GpuPassAliasEntry> );

struct ResourceSetBuildState
{
    std::ofstream* index = nullptr;
    std::ofstream* entries = nullptr;
    uint64_t entryCount = 0;
    uint64_t setCount = 0;
    uint32_t lastSetId = 0;
};

bool VisitResourceSetDefinition( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type != uint8_t( QueueType::JnGpuReferenceSetDefinition ) ) return true;
    auto& state = *static_cast<ResourceSetBuildState*>( userData );
    QueueItem item {}; if( !DecodeItem( record, item, error ) ) return false;
    uint64_t setId = 0; const uint8_t* bytes = nullptr; uint32_t size = 0;
    if( !DecodeLargePayload( record, item, setId, bytes, size, error ) ) return false;
    if( setId == 0 || setId > std::numeric_limits<uint32_t>::max() ||
        setId <= state.lastSetId || size == 0 ||
        size % sizeof( JnGpuReferenceSetEntry ) != 0 ||
        size / sizeof( JnGpuReferenceSetEntry ) > 0xffff )
    { error = "session_gpu_resource_set_spool_definition_invalid"; return false; }
    const auto count = uint32_t( size / sizeof( JnGpuReferenceSetEntry ) );
    const auto* values = reinterpret_cast<const JnGpuReferenceSetEntry*>( bytes );
    if( std::any_of( values, values + count,
        []( const auto& value ) { return value.resourceId == 0; } ) )
    { error = "session_gpu_resource_set_spool_entry_invalid"; return false; }
    ResourceSetIndexRecord index { uint32_t( setId ), count, state.entryCount };
    state.index->write( reinterpret_cast<const char*>( &index ), sizeof( index ) );
    state.entries->write( reinterpret_cast<const char*>( values ), size );
    if( !*state.index || !*state.entries )
    { error = "session_gpu_resource_set_spool_write_failed"; return false; }
    state.entryCount += count; ++state.setCount; state.lastSetId = uint32_t( setId );
    return true;
}

class ReadOnlyMappedFile
{
public:
    ~ReadOnlyMappedFile() { Close(); }
    ReadOnlyMappedFile( const ReadOnlyMappedFile& ) = delete;
    ReadOnlyMappedFile& operator=( const ReadOnlyMappedFile& ) = delete;
    ReadOnlyMappedFile() = default;
    bool Open( const std::filesystem::path& path, std::string& error )
    {
        Close();
#ifdef _WIN32
        const auto ioPath = GpuAnalysisIoPath( path );
        m_file = CreateFileW( ioPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
        if( m_file == INVALID_HANDLE_VALUE )
        { error = "session_gpu_spool_map_open_failed:" + std::to_string( GetLastError() ); return false; }
        LARGE_INTEGER size {};
        if( !GetFileSizeEx( m_file, &size ) || size.QuadPart < 0 )
        { error = "session_gpu_spool_map_size_failed:" + std::to_string( GetLastError() ); Close(); return false; }
        m_size = uint64_t( size.QuadPart );
        if( m_size == 0 ) return true;
        m_mapping = CreateFileMappingW( m_file, nullptr, PAGE_READONLY, 0, 0, nullptr );
        if( !m_mapping )
        { error = "session_gpu_spool_map_create_failed:" + std::to_string( GetLastError() ); Close(); return false; }
        m_data = static_cast<const uint8_t*>( MapViewOfFile( m_mapping, FILE_MAP_READ, 0, 0, 0 ) );
        if( !m_data )
        { error = "session_gpu_spool_map_view_failed:" + std::to_string( GetLastError() ); Close(); return false; }
#else
        std::ifstream in( GpuAnalysisIoPath( path ), std::ios::binary );
        if( !in ) { error = "session_gpu_spool_map_open_failed"; return false; }
        in.seekg( 0, std::ios::end ); const auto size = in.tellg(); in.seekg( 0, std::ios::beg );
        if( size < 0 ) { error = "session_gpu_spool_map_size_failed"; return false; }
        m_fallback.resize( size_t( size ) );
        if( !m_fallback.empty() ) in.read( reinterpret_cast<char*>( m_fallback.data() ), size );
        if( !in && !m_fallback.empty() ) { error = "session_gpu_spool_map_read_failed"; return false; }
        m_data = m_fallback.data(); m_size = m_fallback.size();
#endif
        return true;
    }
    const uint8_t* Data() const { return m_data; }
    uint64_t Size() const { return m_size; }
private:
    void Close()
    {
#ifdef _WIN32
        if( m_data ) UnmapViewOfFile( m_data );
        if( m_mapping ) CloseHandle( m_mapping );
        if( m_file != INVALID_HANDLE_VALUE ) CloseHandle( m_file );
        m_file = INVALID_HANDLE_VALUE; m_mapping = nullptr;
#else
        m_fallback.clear();
#endif
        m_data = nullptr; m_size = 0;
    }
    const uint8_t* m_data = nullptr;
    uint64_t m_size = 0;
#ifdef _WIN32
    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_mapping = nullptr;
#else
    std::vector<uint8_t> m_fallback;
#endif
};

class ResourceSetReader
{
public:
    bool Open( const std::filesystem::path& indexPath,
        const std::filesystem::path& entriesPath, std::string& error )
    {
        if( !m_index.Open( indexPath, error ) || !m_entries.Open( entriesPath, error ) ) return false;
        if( m_index.Size() % sizeof( ResourceSetIndexRecord ) != 0 ||
            m_entries.Size() % sizeof( JnGpuReferenceSetEntry ) != 0 )
        { error = "session_gpu_resource_set_spool_size_invalid"; return false; }
        m_indexCount = m_index.Size() / sizeof( ResourceSetIndexRecord );
        m_entryCount = m_entries.Size() / sizeof( JnGpuReferenceSetEntry );
        return true;
    }
    std::span<const JnGpuReferenceSetEntry> Find( uint32_t setId ) const
    {
        const auto* begin = reinterpret_cast<const ResourceSetIndexRecord*>( m_index.Data() );
        const auto* end = begin + m_indexCount;
        const auto* found = std::lower_bound( begin, end, setId,
            []( const auto& value, uint32_t id ) { return value.setId < id; } );
        if( found == end || found->setId != setId ||
            found->entryOffset > m_entryCount || found->count > m_entryCount - found->entryOffset ) return {};
        const auto* entries = reinterpret_cast<const JnGpuReferenceSetEntry*>( m_entries.Data() );
        return { entries + found->entryOffset, found->count };
    }
private:
    ReadOnlyMappedFile m_index, m_entries;
    uint64_t m_indexCount = 0, m_entryCount = 0;
};

class RangeByPassReader
{
public:
    bool Open( const std::filesystem::path& path, std::string& error )
    {
        std::error_code ec;
        const auto bytes = std::filesystem::file_size( GpuAnalysisIoPath( path ), ec );
        if( ec ) { error = "session_gpu_range_pass_alias_size_failed:" + ec.message(); return false; }
        if( bytes == 0 ) { m_count = 0; return true; }
        if( !m_data.Open( path, error ) ) return false;
        if( m_data.Size() % sizeof( GpuPassRangeRawEntry ) != 0 )
        { error = "session_gpu_range_by_pass_size_invalid"; return false; }
        m_count = m_data.Size() / sizeof( GpuPassRangeRawEntry );
        return true;
    }
    std::span<const GpuPassRangeRawEntry> Find( uint64_t passId ) const
    {
        if( m_count == 0 ) return {};
        const auto* begin = reinterpret_cast<const GpuPassRangeRawEntry*>( m_data.Data() );
        const auto* end = begin + m_count;
        const auto* first = std::lower_bound( begin, end, passId,
            []( const auto& value, uint64_t id ) { return value.evidencePassId < id; } );
        const auto* last = std::upper_bound( first, end, passId,
            []( uint64_t id, const auto& value ) { return id < value.evidencePassId; } );
        return { first, size_t( last - first ) };
    }
    uint64_t Count() const { return m_count; }
private:
    ReadOnlyMappedFile m_data;
    uint64_t m_count = 0;
};

struct GpuPassParentEntry
{
    uint64_t passId = 0;
    uint64_t parentPassId = 0;
};

struct GpuPassResourceEntry
{
    uint64_t passId = 0;
    uint64_t resourceId = 0;
};

static_assert( std::is_trivially_copyable_v<GpuPassParentEntry> );
static_assert( std::is_trivially_copyable_v<GpuPassResourceEntry> );

class PassParentReader
{
public:
    bool Open( const std::filesystem::path& path, std::string& error )
    {
        if( !m_data.Open( path, error ) ) return false;
        if( m_data.Size() % sizeof( GpuPassParentEntry ) != 0 )
        { error = "session_gpu_pass_parent_file_size_invalid"; return false; }
        m_count = m_data.Size() / sizeof( GpuPassParentEntry );
        return true;
    }
    const GpuPassParentEntry* Find( uint64_t passId ) const
    {
        const auto* begin = reinterpret_cast<const GpuPassParentEntry*>( m_data.Data() );
        const auto* end = begin + m_count;
        const auto* found = std::lower_bound( begin, end, passId,
            []( const auto& value, uint64_t id ) { return value.passId < id; } );
        return found != end && found->passId == passId ? found : nullptr;
    }
    uint64_t Count() const { return m_count; }
private:
    ReadOnlyMappedFile m_data;
    uint64_t m_count = 0;
};

class PassResourceReader
{
public:
    bool Open( const std::filesystem::path& path, std::string& error )
    {
        if( !m_data.Open( path, error ) ) return false;
        if( m_data.Size() % sizeof( GpuPassResourceEntry ) != 0 )
        { error = "session_gpu_pass_resource_file_size_invalid"; return false; }
        m_count = m_data.Size() / sizeof( GpuPassResourceEntry );
        return true;
    }
    std::span<const GpuPassResourceEntry> Find( uint64_t passId ) const
    {
        if( m_count == 0 ) return {};
        const auto* begin = reinterpret_cast<const GpuPassResourceEntry*>( m_data.Data() );
        const auto* end = begin + m_count;
        const auto* first = std::lower_bound( begin, end, passId,
            []( const auto& value, uint64_t id ) { return value.passId < id; } );
        const auto* last = std::upper_bound( first, end, passId,
            []( uint64_t id, const auto& value ) { return id < value.passId; } );
        return { first, size_t( last - first ) };
    }
    uint64_t Count() const { return m_count; }
private:
    ReadOnlyMappedFile m_data;
    uint64_t m_count = 0;
};

struct ResourceLifetimeEntry
{
    uint64_t key = 0;
    int64_t time = 0;
    uint64_t resourceId = 0;
    uint8_t operation = 0;
};

bool IsCatalogDefinition( JnGpuCatalogRecordOperation operation )
{
    return operation == JnGpuCatalogRecordOperation::Create ||
        operation == JnGpuCatalogRecordOperation::Update ||
        operation == JnGpuCatalogRecordOperation::Bind ||
        operation == JnGpuCatalogRecordOperation::Open ||
        operation == JnGpuCatalogRecordOperation::Snapshot;
}

class ResourceLifetimeResolver
{
public:
    struct SourceGap
    {
        uint64_t resourceId = 0;
        int64_t endTime = 0;
        bool created = false;
    };
    explicit ResourceLifetimeResolver( const JnTraceData& data )
    {
        m_pointer.reserve( data.gpuCatalogResources.size() );
        m_resourceIds.reserve( data.gpuCatalogResources.size() );
        for( const auto& value : data.gpuCatalogResources )
        {
            if( value.resourceId != 0 ) m_resourceIds.insert( value.resourceId );
            if( value.pointerToken != 0 )
                m_pointer.push_back( { value.pointerToken, value.time, value.resourceId, value.operation } );
        }
        m_logical.reserve( data.gpuCatalogLogicals.size() );
        for( const auto& value : data.gpuCatalogLogicals ) if( value.logicalResourceId != 0 )
            m_logical.push_back( { value.logicalResourceId, value.time, value.resourceId, value.operation } );
        const auto order = []( const auto& lhs, const auto& rhs ) {
            if( lhs.key != rhs.key ) return lhs.key < rhs.key;
            if( lhs.time != rhs.time ) return lhs.time < rhs.time;
            return lhs.resourceId < rhs.resourceId;
        };
        std::stable_sort( m_pointer.begin(), m_pointer.end(), order );
        std::stable_sort( m_logical.begin(), m_logical.end(), order );
    }
    ResourceLifetimeResolver() = default;
    bool AddPointerFile( const std::filesystem::path& path, std::string& error )
    {
        if( !m_pointerFile.Open( path, error ) ) return false;
        if( m_pointerFile.Size() % sizeof( ResourceLifetimeEntry ) != 0 )
        { error = "session_gpu_pointer_lifetime_file_size_invalid"; return false; }
        m_pointerFileCount = m_pointerFile.Size() / sizeof( ResourceLifetimeEntry );
        if( m_pointerFileCount > uint64_t( std::numeric_limits<size_t>::max() ) )
        { error = "session_gpu_pointer_lifetime_file_count_overflow"; return false; }
        return true;
    }
    std::optional<SourceGap> ResolveSourceGapBeforeFirstDefinition(
        uint64_t token, int64_t time )
    {
        const auto pointer = PointerValues();
        const auto first = std::lower_bound( pointer.begin(), pointer.end(), token,
            []( const auto& value, uint64_t candidate ) { return value.key < candidate; } );
        const bool absent = first == pointer.end() || first->key != token;
        if( !absent && ( time >= first->time ||
            JnGpuCatalogRecordOperation( first->operation ) != JnGpuCatalogRecordOperation::Create ) )
            return std::nullopt;
        const auto endTime = absent ? int64_t( -1 ) : first->time;
        if( const auto found = m_sourceGaps.find( token ); found != m_sourceGaps.end() )
            return SourceGap { found->second, endTime, false };
        while( m_nextSourceGapId == 0 || m_resourceIds.contains( m_nextSourceGapId ) )
        {
            if( ++m_nextSourceGapId < SourceGapResourceIdBase ) return std::nullopt;
        }
        const auto resourceId = m_nextSourceGapId++;
        m_resourceIds.insert( resourceId );
        m_sourceGaps.emplace( token, resourceId );
        m_sourceGapIds.insert( resourceId );
        return SourceGap { resourceId, endTime, true };
    }
    bool IsSourceGapResource( uint64_t resourceId ) const
    {
        return resourceId >= SourceGapResourceIdBase && m_sourceGapIds.contains( resourceId );
    }
    bool AddLogicalFile( const std::filesystem::path& path, std::string& error )
    {
        if( !m_logicalFile.Open( path, error ) ) return false;
        if( m_logicalFile.Size() % sizeof( ResourceLifetimeEntry ) != 0 )
        { error = "session_gpu_logical_lifetime_file_size_invalid"; return false; }
        m_logicalFileCount = m_logicalFile.Size() / sizeof( ResourceLifetimeEntry );
        if( m_logicalFileCount > uint64_t( std::numeric_limits<size_t>::max() ) )
        { error = "session_gpu_logical_lifetime_file_count_overflow"; return false; }
        return true;
    }
    uint64_t Resolve( uint64_t token, int64_t time ) const
    {
        auto value = ResolveIn( PointerValues(), token, time, true );
        if( value != 0 ) return value;
        value = ResolveIn( m_logical, token, time, false );
        if( value != 0 || m_logicalFileCount == 0 ) return value;
        return ResolveIn( std::span<const ResourceLifetimeEntry>(
            reinterpret_cast<const ResourceLifetimeEntry*>( m_logicalFile.Data() ),
            size_t( m_logicalFileCount ) ), token, time, false );
    }
    uint64_t ResolveAtOrUniqueInterval( uint64_t token, int64_t time,
        int64_t intervalBegin, int64_t intervalEnd ) const
    {
        const auto pointer = PointerValues();
        auto exact = ResolveUniqueIntervalIn( pointer, token, time, time, true );
        if( exact.keyPresent )
        {
            if( exact.resourceId != 0 || exact.ambiguous ) return exact.resourceId;
            return ResolveUniqueIntervalIn( pointer, token, intervalBegin,
                intervalEnd, true ).resourceId;
        }
        exact = ResolveUniqueIntervalIn( m_logical, token, time, time, false );
        if( exact.keyPresent )
        {
            if( exact.resourceId != 0 || exact.ambiguous ) return exact.resourceId;
            return ResolveUniqueIntervalIn( m_logical, token, intervalBegin,
                intervalEnd, false ).resourceId;
        }
        const auto logicalFile = std::span<const ResourceLifetimeEntry>(
            reinterpret_cast<const ResourceLifetimeEntry*>( m_logicalFile.Data() ),
            size_t( m_logicalFileCount ) );
        exact = ResolveUniqueIntervalIn( logicalFile, token, time, time, false );
        if( exact.resourceId != 0 || exact.ambiguous ) return exact.resourceId;
        return ResolveUniqueIntervalIn( logicalFile, token, intervalBegin,
            intervalEnd, false ).resourceId;
    }
    std::string Describe( uint64_t token, int64_t time ) const
    {
        return "pointer{" + DescribeIn( PointerValues(), token, time ) + "};logical-memory{" +
            DescribeIn( m_logical, token, time ) + "};logical-file{" +
            DescribeIn( std::span<const ResourceLifetimeEntry>(
                reinterpret_cast<const ResourceLifetimeEntry*>( m_logicalFile.Data() ),
                size_t( m_logicalFileCount ) ), token, time ) + "}";
    }
private:
    std::span<const ResourceLifetimeEntry> PointerValues() const
    {
        if( m_pointerFileCount != 0 ) return {
            reinterpret_cast<const ResourceLifetimeEntry*>( m_pointerFile.Data() ),
            size_t( m_pointerFileCount ) };
        return m_pointer;
    }
    struct IntervalResolution
    {
        bool keyPresent = false;
        bool ambiguous = false;
        uint64_t resourceId = 0;
    };
    static IntervalResolution ResolveUniqueIntervalIn(
        std::span<const ResourceLifetimeEntry> values, uint64_t key,
        int64_t intervalBegin, int64_t intervalEnd, bool allowImplicitOpenBoundary )
    {
        IntervalResolution result;
        const auto first = std::lower_bound( values.begin(), values.end(), key,
            []( const auto& value, uint64_t candidate ) { return value.key < candidate; } );
        const auto last = std::upper_bound( first, values.end(), key,
            []( uint64_t candidate, const auto& value ) { return candidate < value.key; } );
        result.keyPresent = first != last;
        if( !result.keyPresent ) return result;
        if( intervalEnd < intervalBegin ) std::swap( intervalBegin, intervalEnd );
        uint64_t active = 0;
        int64_t activeBegin = std::numeric_limits<int64_t>::max();
        bool ambiguous = false;
        const auto consider = [&]( uint64_t resourceId, int64_t begin, int64_t end ) {
            // Resource lifetimes are [begin,end).  An exact event-time lookup
            // is attempted first; this interval fallback is only for producer
            // timestamps that precede a late definition or outlive an early
            // destroy while still intersecting exactly one lifetime.
            if( resourceId == 0 || end <= intervalBegin || begin > intervalEnd ) return;
            if( result.resourceId == 0 ) result.resourceId = resourceId;
            else if( result.resourceId != resourceId ) ambiguous = true;
        };
        for( auto it = first; it != last; ++it )
        {
            const auto operation = JnGpuCatalogRecordOperation( it->operation );
            const bool close = operation == JnGpuCatalogRecordOperation::Destroy ||
                operation == JnGpuCatalogRecordOperation::Close ||
                operation == JnGpuCatalogRecordOperation::Unbind;
            if( close )
            {
                if( allowImplicitOpenBoundary )
                {
                    // Destroy/Close ends CPU ownership, but command-list
                    // reference packets may be flushed afterwards. Pointer
                    // generation identity therefore remains valid until the
                    // next Create/Open/Snapshot/Update for this address. An
                    // unmatched first Destroy still proves the capture began
                    // inside that resource generation.
                    if( active == 0 && it->resourceId != 0 )
                    {
                        active = it->resourceId;
                        activeBegin = it == first ?
                            std::numeric_limits<int64_t>::min() : it->time;
                    }
                    continue;
                }
                if( active != 0 )
                {
                    if( it->resourceId == 0 || it->resourceId == active )
                    {
                        consider( active, activeBegin, it->time );
                        active = 0;
                    }
                    else
                    {
                        // A close for another identity cannot terminate the
                        // currently active pointer generation. Preserve both
                        // pieces of source evidence; overlapping queries will
                        // correctly become ambiguous instead of silently
                        // switching identities.
                        consider( it->resourceId, std::numeric_limits<int64_t>::min(), it->time );
                    }
                }
                else if( it->resourceId != 0 )
                {
                    // Capture can begin after resource creation and observe
                    // only its Destroy. The close itself proves an open-left
                    // lifetime up to this timestamp.
                    consider( it->resourceId, std::numeric_limits<int64_t>::min(), it->time );
                }
                continue;
            }
            const bool startsLifetime = operation == JnGpuCatalogRecordOperation::Create ||
                operation == JnGpuCatalogRecordOperation::Open ||
                operation == JnGpuCatalogRecordOperation::Snapshot ||
                ( !allowImplicitOpenBoundary && operation == JnGpuCatalogRecordOperation::Bind );
            if( !startsLifetime )
            {
                // Update proves that this exact resource identity is live at
                // the observation timestamp. It enriches an active matching
                // lifetime, while an orphan Update opens only from the
                // observation time (or the capture boundary when it is the
                // first pointer fact). It must never reach backwards through
                // a previously closed pointer generation.
                if( operation == JnGpuCatalogRecordOperation::Update && it->resourceId != 0 )
                {
                    if( active != 0 && active != it->resourceId )
                        consider( active, activeBegin, it->time );
                    if( active == 0 || active != it->resourceId )
                    {
                        active = it->resourceId;
                        activeBegin = allowImplicitOpenBoundary && it == first ?
                            std::numeric_limits<int64_t>::min() : it->time;
                    }
                }
                continue;
            }
            if( it->resourceId == 0 ) continue;
            if( active != 0 && active != it->resourceId )
                consider( active, activeBegin, it->time );
            if( active == it->resourceId ) continue;
            active = it->resourceId;
            if( operation == JnGpuCatalogRecordOperation::Open ||
                operation == JnGpuCatalogRecordOperation::Snapshot )
            {
                // Open/Snapshot means the creation edge is outside the
                // observed data, not that it predates every earlier pointer
                // generation in the capture. Only the first pointer fact is
                // truly open-left; a later observation starts a new identity
                // boundary at its own timestamp.
                activeBegin = it == first ? std::numeric_limits<int64_t>::min() : it->time;
            }
            else
            {
                activeBegin = it->time;
            }
        }
        if( active != 0 ) consider( active, activeBegin, std::numeric_limits<int64_t>::max() );
        if( ambiguous ) { result.ambiguous = true; result.resourceId = 0; }
        return result;
    }
    static std::string DescribeIn( std::span<const ResourceLifetimeEntry> values,
        uint64_t key, int64_t time )
    {
        const auto first = std::lower_bound( values.begin(), values.end(), key,
            []( const auto& value, uint64_t candidate ) { return value.key < candidate; } );
        const auto last = std::upper_bound( first, values.end(), key,
            []( uint64_t candidate, const auto& value ) { return candidate < value.key; } );
        if( first == last ) return "count=0";
        const auto next = std::upper_bound( first, last, time,
            []( int64_t candidate, const auto& value ) { return candidate < value.time; } );
        std::string result = "count=" + std::to_string( last - first );
        if( next != first )
        {
            const auto& value = *( next - 1 );
            result += ",before_time=" + std::to_string( value.time ) +
                ",before_resource=" + std::to_string( value.resourceId ) +
                ",before_operation=" + std::to_string( value.operation );
        }
        if( next != last )
        {
            result += ",after_time=" + std::to_string( next->time ) +
                ",after_resource=" + std::to_string( next->resourceId ) +
                ",after_operation=" + std::to_string( next->operation );
        }
        return result;
    }
    static uint64_t ResolveIn( std::span<const ResourceLifetimeEntry> values,
        uint64_t key, int64_t time, bool allowImplicitOpenBoundary )
    {
        const auto first = std::lower_bound( values.begin(), values.end(), key,
            []( const auto& value, uint64_t candidate ) { return value.key < candidate; } );
        const auto last = std::upper_bound( first, values.end(), key,
            []( uint64_t candidate, const auto& value ) { return candidate < value.key; } );
        auto cursor = std::upper_bound( first, last, time,
            []( int64_t candidate, const auto& value ) { return candidate < value.time; } );
        // Bootstrap Open/Snapshot records describe resources that were already
        // alive at capture start.  Their transport timestamps are observation
        // times, not creation times, so the lifetime is open on the left.  The
        // legacy Worker resolver uses INT64_MIN for the same records.  Pointer
        // records also preserve its fallback for a first non-Create definition
        // (for example an Update observed without the original Create).
        if( cursor == first && first != last )
        {
            const auto operation = JnGpuCatalogRecordOperation( first->operation );
            const bool explicitOpen = operation == JnGpuCatalogRecordOperation::Open ||
                operation == JnGpuCatalogRecordOperation::Snapshot;
            const bool implicitOpen = allowImplicitOpenBoundary &&
                operation != JnGpuCatalogRecordOperation::Create &&
                operation != JnGpuCatalogRecordOperation::Destroy &&
                operation != JnGpuCatalogRecordOperation::Close;
            if( ( explicitOpen || implicitOpen ) && first->resourceId != 0 )
                return first->resourceId;
        }
        while( cursor != first )
        {
            const auto& value = *--cursor;
            const auto operation = JnGpuCatalogRecordOperation( value.operation );
            if( operation == JnGpuCatalogRecordOperation::Destroy ||
                operation == JnGpuCatalogRecordOperation::Close ) return 0;
            if( IsCatalogDefinition( operation ) ) return value.resourceId;
        }
        return 0;
    }
    std::vector<ResourceLifetimeEntry> m_pointer, m_logical;
    std::unordered_map<uint64_t, uint64_t> m_sourceGaps;
    std::unordered_set<uint64_t> m_sourceGapIds;
    std::unordered_set<uint64_t> m_resourceIds;
    uint64_t m_nextSourceGapId = SourceGapResourceIdBase;
    ReadOnlyMappedFile m_logicalFile;
    uint64_t m_logicalFileCount = 0;
    ReadOnlyMappedFile m_pointerFile;
    uint64_t m_pointerFileCount = 0;
};

struct PassEvidenceEntry
{
    uint64_t passId = 0;
    GpuDetailedEvidenceAnalysisRecord evidence;
};

struct GpuLogicalRawEntry
{
    uint64_t logicalId = 0;
    uint64_t generation = 0;
    JnGpuCatalogLogicalRecordV1 record {};
};

static_assert( std::is_trivially_copyable_v<GpuLogicalRawEntry> );
static_assert( std::is_trivially_copyable_v<GpuAnalysisLogicalStoreEntry> );
static_assert( std::is_trivially_copyable_v<GpuAnalysisCatalogRelationStoreEntry> );
static_assert( std::is_trivially_copyable_v<GpuViewAnalysisRecord> );
static_assert( std::is_trivially_copyable_v<GpuPartAnalysisRecord> );
static_assert( std::is_trivially_copyable_v<GpuVgAnalysisRecord> );

template<typename T, typename Compare>
bool SaveGpuSpoolRun( std::vector<T>& values, const std::filesystem::path& root,
    const char* prefix, std::vector<std::filesystem::path>& runs, Compare compare,
    std::string& error )
{
    if( values.empty() ) return true;
    std::sort( values.begin(), values.end(), compare );
    std::error_code ec; std::filesystem::create_directories( GpuAnalysisIoPath( root ), ec );
    if( ec ) { error = std::string( "session_gpu_enrichment_run_directory_failed:" ) + ec.message(); return false; }
    std::ostringstream name; name << prefix << '-' << std::setw( 6 ) << std::setfill( '0' )
        << runs.size() << ".bin";
    const auto path = root / name.str();
    std::ofstream out( GpuAnalysisIoPath( path ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_gpu_enrichment_run_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( values.data() ),
        std::streamsize( values.size() * sizeof( T ) ) );
    out.flush(); if( !out ) { error = "session_gpu_enrichment_run_write_failed"; return false; }
    runs.push_back( path ); values.clear(); return true;
}

struct CatalogResourceRawEntry
{
    uint64_t generation = 0;
    uint64_t ordinal = 0;
    JnGpuCatalogResourceRecordV1 record {};
};

struct CatalogAllocationRawEntry
{
    uint64_t generation = 0;
    uint64_t ordinal = 0;
    JnGpuCatalogAllocationRecordV1 record {};
};

struct CatalogAllocationDelta
{
    int64_t time = 0;
    uint64_t ordinal = 0;
    int64_t bytes = 0;
};

struct CatalogResourceLookupEntry
{
    uint64_t resourceId = 0;
    uint64_t allocationId = 0;
    uint64_t capacityBytes = 0;
    uint8_t aliveAtEnd = 0;
    uint8_t reserved[7] {};
};

struct CatalogAllocationLookupEntry
{
    uint64_t allocationId = 0;
    uint64_t sizeBytes = 0;
    uint64_t resourceCount = 0;
    uint8_t aliveAtEnd = 0;
    uint8_t reserved[7] {};
};

struct CatalogAllocationResourceEntry
{
    uint64_t allocationId = 0;
    uint64_t resourceId = 0;
};

struct CatalogResourceNameEntry
{
    uint64_t nameHash = 0;
    uint64_t resourceId = 0;
    uint64_t allocationId = 0;
    uint64_t capacityBytes = 0;
};

struct CatalogStringRawEntry
{
    uint64_t generation = 0;
    uint64_t ordinal = 0;
    uint32_t stringId = 0;
    uint32_t byteLength = 0;
    char bytes[256] {};
};

static_assert( std::is_trivially_copyable_v<CatalogResourceRawEntry> );
static_assert( std::is_trivially_copyable_v<CatalogAllocationRawEntry> );
static_assert( std::is_trivially_copyable_v<CatalogAllocationDelta> );
static_assert( std::is_trivially_copyable_v<CatalogResourceLookupEntry> );
static_assert( std::is_trivially_copyable_v<CatalogAllocationLookupEntry> );
static_assert( std::is_trivially_copyable_v<CatalogAllocationResourceEntry> );
static_assert( std::is_trivially_copyable_v<CatalogResourceNameEntry> );
static_assert( std::is_trivially_copyable_v<CatalogStringRawEntry> );

class BoundedCatalogLookup
{
public:
    bool Open( const GpuAnalysisCatalogSpool& spool, std::string& error )
    {
        if( !m_resources.Open( spool.resourceLookupPath, error ) ||
            !m_allocations.Open( spool.allocationLookupPath, error ) ) return false;
        if( m_resources.Size() % sizeof( CatalogResourceLookupEntry ) != 0 ||
            m_allocations.Size() % sizeof( CatalogAllocationLookupEntry ) != 0 )
        { error = "session_gpu_catalog_lookup_size_invalid"; return false; }
        m_resourceCount = m_resources.Size() / sizeof( CatalogResourceLookupEntry );
        m_allocationCount = m_allocations.Size() / sizeof( CatalogAllocationLookupEntry );
        return true;
    }
    bool HasResource( uint64_t id ) const { return FindResource( id ) != nullptr ||
        std::any_of( m_appended.begin(), m_appended.end(), [=]( const auto& value ) { return value.resourceId == id; } ); }
    const CatalogResourceLookupEntry* FindResource( uint64_t id ) const
    {
        const auto values = std::span<const CatalogResourceLookupEntry>(
            reinterpret_cast<const CatalogResourceLookupEntry*>( m_resources.Data() ), size_t( m_resourceCount ) );
        const auto found = std::lower_bound( values.begin(), values.end(), id,
            []( const auto& value, uint64_t key ) { return value.resourceId < key; } );
        return found == values.end() || found->resourceId != id ? nullptr : &*found;
    }
    const CatalogAllocationLookupEntry* FindAllocation( uint64_t id ) const
    {
        const auto values = std::span<const CatalogAllocationLookupEntry>(
            reinterpret_cast<const CatalogAllocationLookupEntry*>( m_allocations.Data() ), size_t( m_allocationCount ) );
        const auto found = std::lower_bound( values.begin(), values.end(), id,
            []( const auto& value, uint64_t key ) { return value.allocationId < key; } );
        return found == values.end() || found->allocationId != id ? nullptr : &*found;
    }
    uint64_t PhysicalBytes( const std::vector<uint64_t>& resources ) const
    {
        std::unordered_set<uint64_t> allocations;
        uint64_t bytes = 0;
        for( const auto id : resources )
        {
            const auto* resource = FindResource( id );
            if( !resource || resource->allocationId == 0 ||
                !allocations.emplace( resource->allocationId ).second ) continue;
            const auto* allocation = FindAllocation( resource->allocationId );
            if( allocation && allocation->aliveAtEnd ) bytes += allocation->sizeBytes;
        }
        return bytes;
    }
    void AddResource( const GpuResourceAnalysisRecord& value ) { m_appended.push_back( value ); }
private:
    ReadOnlyMappedFile m_resources, m_allocations;
    uint64_t m_resourceCount = 0, m_allocationCount = 0;
    std::vector<GpuResourceAnalysisRecord> m_appended;
};

class BoundedCatalogStringLookup
{
public:
    bool Open( const GpuAnalysisCatalogSpool& spool, std::string& error )
    {
        if( !m_index.Open( spool.stringIndexPath, error ) ||
            !m_data.Open( spool.stringDataPath, error ) ) return false;
        if( m_index.Size() % sizeof( GpuAnalysisCatalogStringIndexEntry ) != 0 )
        { error = "session_gpu_catalog_string_index_size_invalid"; return false; }
        m_count = m_index.Size() / sizeof( GpuAnalysisCatalogStringIndexEntry ); return true;
    }
    std::string Find( uint64_t generation, uint32_t stringId ) const
    {
        if( stringId == 0 ) return {};
        const auto values = std::span<const GpuAnalysisCatalogStringIndexEntry>(
            reinterpret_cast<const GpuAnalysisCatalogStringIndexEntry*>( m_index.Data() ), size_t( m_count ) );
        const auto found = std::lower_bound( values.begin(), values.end(),
            std::pair<uint64_t, uint32_t> { generation, stringId }, []( const auto& value, const auto& key ) {
                return value.generation != key.first ? value.generation < key.first : value.stringId < key.second;
            } );
        if( found == values.end() || found->generation != generation || found->stringId != stringId ||
            found->dataOffset > m_data.Size() || found->byteLength > m_data.Size() - found->dataOffset ) return {};
        return std::string( reinterpret_cast<const char*>( m_data.Data() + found->dataOffset ), found->byteLength );
    }
private:
    ReadOnlyMappedFile m_index, m_data;
    uint64_t m_count = 0;
};

template<typename T, typename Compare>
class GpuSpoolRunCursor
{
public:
    explicit GpuSpoolRunCursor( Compare compare ) : m_compare( compare ) {}
    bool Open( const std::vector<std::filesystem::path>& paths, std::string& error )
    {
        m_cursors.resize( paths.size() );
        for( size_t i = 0; i < paths.size(); ++i )
        {
            auto& cursor = m_cursors[i];
            cursor.in.open( GpuAnalysisIoPath( paths[i] ), std::ios::binary );
            if( !cursor.in ) { error = "session_gpu_catalog_run_open_failed"; return false; }
            if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( T ) ) )
                m_heap.push( Node { cursor.value, i } );
            else if( !cursor.in.eof() )
            { error = "session_gpu_catalog_run_read_failed"; return false; }
        }
        return true;
    }
    bool Empty() const { return m_heap.empty(); }
    const T& Front() const { return m_heap.top().value; }
    bool Pop( T& value, std::string& error )
    {
        if( m_heap.empty() ) { error = "session_gpu_catalog_run_empty"; return false; }
        const auto node = m_heap.top(); m_heap.pop(); value = node.value;
        auto& cursor = m_cursors[node.run];
        if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( T ) ) )
            m_heap.push( Node { cursor.value, node.run } );
        else if( !cursor.in.eof() )
        { error = "session_gpu_catalog_run_read_failed"; return false; }
        return true;
    }
private:
    struct Cursor { std::ifstream in; T value {}; };
    struct Node { T value {}; size_t run = 0; };
    struct Later
    {
        Compare compare;
        bool operator()( const Node& lhs, const Node& rhs ) const
        { return compare( rhs.value, lhs.value ); }
    };
    Compare m_compare;
    std::vector<Cursor> m_cursors;
    std::priority_queue<Node, std::vector<Node>, Later> m_heap { Later { m_compare } };
};

struct CatalogCoreSpoolBuildState
{
    const TraceSessionTimeTransform* transform = nullptr;
    const GpuAnalysisSidecarControl* control = nullptr;
    GpuAnalysisCatalogSpool* spool = nullptr;
    std::unordered_map<uint64_t, std::vector<uint8_t>> payloads;
    std::vector<CatalogResourceRawEntry> resources;
    std::vector<CatalogAllocationRawEntry> allocations;
    std::vector<CatalogStringRawEntry> strings;
    std::vector<std::filesystem::path> resourceRuns;
    std::vector<std::filesystem::path> allocationRuns;
    std::vector<std::filesystem::path> stringRuns;
    uint64_t ordinal = 0;
};

uint64_t CatalogRunTargetBytes( const GpuAnalysisSidecarControl& control )
{
    return std::max<uint64_t>( 1, std::min<uint64_t>(
        64ull * 1024 * 1024, control.targetDerivedPageBytes ) );
}

bool FlushCatalogResourceRun( CatalogCoreSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.resources, state.spool->root / "resource-runs",
        "resource", state.resourceRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.record.resourceId != rhs.record.resourceId )
                return lhs.record.resourceId < rhs.record.resourceId;
            if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
            return lhs.ordinal < rhs.ordinal;
        }, error );
}

bool FlushCatalogAllocationRun( CatalogCoreSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.allocations, state.spool->root / "allocation-runs",
        "allocation", state.allocationRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.record.allocationId != rhs.record.allocationId )
                return lhs.record.allocationId < rhs.record.allocationId;
            if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
            return lhs.ordinal < rhs.ordinal;
        }, error );
}

bool FlushCatalogStringRun( CatalogCoreSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.strings, state.spool->root / "string-runs", "string",
        state.stringRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.generation != rhs.generation ) return lhs.generation < rhs.generation;
            if( lhs.stringId != rhs.stringId ) return lhs.stringId < rhs.stringId;
            return lhs.ordinal < rhs.ordinal;
        }, error );
}

bool ProcessCatalogCoreBatch( const QueueJnGpuCatalogBatch& event,
    CatalogCoreSpoolBuildState& state, std::string& error )
{
    const auto found = state.payloads.find( event.payloadId );
    if( found == state.payloads.end() )
    { error = "session_gpu_catalog_core_payload_missing"; return false; }
    auto payload = std::move( found->second ); state.payloads.erase( found );
    if( payload.size() != event.payloadBytes || payload.size() < sizeof( JnGpuCatalogBatchEnvelopeV1 ) )
    { error = "session_gpu_catalog_core_payload_size_mismatch"; return false; }
    JnGpuCatalogBatchEnvelopeV1 envelope {};
    std::memcpy( &envelope, payload.data(), sizeof( envelope ) );
    const auto* records = payload.data() + sizeof( envelope );
    const auto recordBytes = payload.size() - sizeof( envelope );
    if( envelope.magic != JnGpuCatalogBatchMagic ||
        envelope.catalogSchema != JnGpuCatalogSchemaVersion ||
        envelope.evidenceSchema != JnGpuDetailedEvidenceSchemaVersion ||
        envelope.recordCount != event.recordCount || envelope.payloadBytes != recordBytes ||
        JnGpuCatalogChecksum64( records, recordBytes ) != envelope.checksum )
    { error = "session_gpu_catalog_core_envelope_invalid"; return false; }
    const auto kind = JnGpuCatalogBatchKind( event.kind );
    if( kind == JnGpuCatalogBatchKind::Resource )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ) ||
            envelope.recordBytes != sizeof( JnGpuCatalogResourceRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuCatalogResourceRecordV1 ) != recordBytes )
        { error = "session_gpu_catalog_resource_batch_invalid"; return false; }
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            JnGpuCatalogResourceRecordV1 value {};
            std::memcpy( &value, records + size_t( i ) * sizeof( value ), sizeof( value ) );
            value.time = state.transform->ToNanoseconds( value.time );
            state.resources.push_back( { event.generation, state.ordinal++, value } );
            state.spool->peakRecordsInMemory = std::max<uint64_t>(
                state.spool->peakRecordsInMemory, state.resources.size() );
            if( state.resources.size() * sizeof( CatalogResourceRawEntry ) >=
                CatalogRunTargetBytes( *state.control ) && !FlushCatalogResourceRun( state, error ) ) return false;
        }
    }
    else if( kind == JnGpuCatalogBatchKind::Allocation )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ) ||
            envelope.recordBytes != sizeof( JnGpuCatalogAllocationRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuCatalogAllocationRecordV1 ) != recordBytes )
        { error = "session_gpu_catalog_allocation_batch_invalid"; return false; }
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            JnGpuCatalogAllocationRecordV1 value {};
            std::memcpy( &value, records + size_t( i ) * sizeof( value ), sizeof( value ) );
            value.time = state.transform->ToNanoseconds( value.time );
            state.allocations.push_back( { event.generation, state.ordinal++, value } );
            state.spool->peakRecordsInMemory = std::max<uint64_t>(
                state.spool->peakRecordsInMemory, state.allocations.size() );
            if( state.allocations.size() * sizeof( CatalogAllocationRawEntry ) >=
                CatalogRunTargetBytes( *state.control ) && !FlushCatalogAllocationRun( state, error ) ) return false;
        }
    }
    else if( kind == JnGpuCatalogBatchKind::String )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ) || envelope.recordBytes != 0 )
        { error = "session_gpu_catalog_string_batch_invalid"; return false; }
        const auto* cursor = records; const auto* end = records + recordBytes;
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            if( size_t( end - cursor ) < sizeof( JnGpuCatalogStringRecordHeaderV1 ) )
            { error = "session_gpu_catalog_string_header_truncated"; return false; }
            JnGpuCatalogStringRecordHeaderV1 header {};
            std::memcpy( &header, cursor, sizeof( header ) ); cursor += sizeof( header );
            if( header.stringId == 0 || header.byteLength > 256 || size_t( end - cursor ) < header.byteLength )
            { error = "session_gpu_catalog_string_invalid"; return false; }
            CatalogStringRawEntry value; value.generation = event.generation;
            value.ordinal = state.ordinal++; value.stringId = header.stringId;
            value.byteLength = header.byteLength;
            if( header.byteLength != 0 ) std::memcpy( value.bytes, cursor, header.byteLength );
            cursor += header.byteLength; state.strings.push_back( value );
            state.spool->peakRecordsInMemory = std::max<uint64_t>(
                state.spool->peakRecordsInMemory, state.strings.size() );
            if( state.strings.size() * sizeof( CatalogStringRawEntry ) >=
                CatalogRunTargetBytes( *state.control ) && !FlushCatalogStringRun( state, error ) ) return false;
        }
        if( cursor != end ) { error = "session_gpu_catalog_string_trailing_bytes"; return false; }
    }
    return true;
}

bool VisitGpuCatalogCoreSpool( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<CatalogCoreSpoolBuildState*>( userData );
    if( state.control->stopToken.stop_requested() ) { error = "cancelled"; return false; }
    QueueItem item {}; if( !DecodeItem( record, item, error ) ) return false;
    if( item.hdr.type == QueueType::JnGpuCatalogBatchData )
    {
        uint64_t payloadId = 0; const uint8_t* bytes = nullptr; uint32_t size = 0;
        if( !DecodeLargePayload( record, item, payloadId, bytes, size, error ) ) return false;
        if( payloadId == 0 || size < sizeof( JnGpuCatalogBatchEnvelopeV1 ) ||
            size > 64u * 1024 * 1024 || !state.payloads.emplace(
                payloadId, std::vector<uint8_t>( bytes, bytes + size ) ).second )
        { error = "session_gpu_catalog_core_payload_invalid"; return false; }
        return true;
    }
    if( item.hdr.type == QueueType::JnGpuCatalogBatch )
        return ProcessCatalogCoreBatch( item.jnGpuCatalogBatch, state, error );
    return true;
}

struct EnrichmentSpoolBuildState
{
    const TraceSessionTimeTransform* transform = nullptr;
    ResourceLifetimeResolver* resolver = nullptr;
    const BoundedCatalogLookup* catalog = nullptr;
    ResourceSetBuildState* resourceSets = nullptr;
    GpuAnalysisPassSpool* spool = nullptr;
    const GpuAnalysisSidecarControl* control = nullptr;
    std::unordered_map<uint64_t, std::vector<uint8_t>> catalogPayloads;
    std::vector<GpuLogicalRawEntry> logicalRaw;
    std::vector<GpuPassRangeRawEntry> rangeRaw;
    std::vector<GpuPassAliasEntry> passAliases;
    std::vector<GpuAnalysisCatalogRelationStoreEntry> relations;
    std::vector<GpuViewAnalysisRecord> views;
    std::vector<GpuPartAnalysisRecord> parts;
    std::vector<GpuVgAnalysisRecord> virtualGeometry;
    std::vector<std::filesystem::path> logicalRawRuns;
    std::vector<std::filesystem::path> rangeRawRuns;
    std::vector<std::filesystem::path> passAliasRuns;
    uint64_t coreUnresolved = 0;
};

bool FlushViewRun( EnrichmentSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.views, state.spool->root / "view-runs", "view",
        state.spool->viewRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.value.resourceId != rhs.value.resourceId ) return lhs.value.resourceId < rhs.value.resourceId;
            if( lhs.value.time != rhs.value.time ) return lhs.value.time < rhs.value.time;
            return lhs.generation < rhs.generation;
        }, error );
}

bool FlushPartRun( EnrichmentSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.parts, state.spool->root / "part-runs", "part",
        state.spool->partRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.value.resourceId != rhs.value.resourceId ) return lhs.value.resourceId < rhs.value.resourceId;
            if( lhs.value.time != rhs.value.time ) return lhs.value.time < rhs.value.time;
            return lhs.generation < rhs.generation;
        }, error );
}

bool FlushVirtualGeometryRun( EnrichmentSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.virtualGeometry, state.spool->root / "vg-runs", "vg",
        state.spool->virtualGeometryRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.value.resourceId != rhs.value.resourceId ) return lhs.value.resourceId < rhs.value.resourceId;
            if( lhs.value.time != rhs.value.time ) return lhs.value.time < rhs.value.time;
            return lhs.generation < rhs.generation;
        }, error );
}

bool FlushLogicalRawRun( EnrichmentSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.logicalRaw, state.spool->root / "logical-raw-runs",
        "logical", state.logicalRawRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.logicalId != rhs.logicalId ) return lhs.logicalId < rhs.logicalId;
            if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
            if( lhs.record.resourceId != rhs.record.resourceId ) return lhs.record.resourceId < rhs.record.resourceId;
            return lhs.generation < rhs.generation;
        }, error );
}

bool FlushCatalogRelationRun( EnrichmentSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.relations, state.spool->root / "catalog-relation-runs",
        "relation", state.spool->catalogRelationRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
            if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
            if( lhs.record.sourceId != rhs.record.sourceId ) return lhs.record.sourceId < rhs.record.sourceId;
            if( lhs.record.targetId != rhs.record.targetId ) return lhs.record.targetId < rhs.record.targetId;
            return lhs.generation < rhs.generation;
        }, error );
}

bool FlushRangeRawRun( EnrichmentSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.rangeRaw, state.spool->root / "range-raw-runs",
        "range", state.rangeRawRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.record.passInstanceId != rhs.record.passInstanceId )
                return lhs.record.passInstanceId < rhs.record.passInstanceId;
            if( lhs.record.resourceId != rhs.record.resourceId )
                return lhs.record.resourceId < rhs.record.resourceId;
            if( lhs.record.pointerToken != rhs.record.pointerToken )
                return lhs.record.pointerToken < rhs.record.pointerToken;
            if( lhs.record.offsetBytes != rhs.record.offsetBytes )
                return lhs.record.offsetBytes < rhs.record.offsetBytes;
            if( lhs.record.firstSubresource != rhs.record.firstSubresource )
                return lhs.record.firstSubresource < rhs.record.firstSubresource;
            return lhs.generation < rhs.generation;
        }, error );
}

bool FlushPassAliasRun( EnrichmentSpoolBuildState& state, std::string& error )
{
    return SaveGpuSpoolRun( state.passAliases, state.spool->root / "pass-alias-runs",
        "alias", state.passAliasRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.explicitPassId != rhs.explicitPassId )
                return lhs.explicitPassId < rhs.explicitPassId;
            return lhs.referencePassId < rhs.referencePassId;
        }, error );
}

bool ProcessEnrichmentCatalogBatch( const QueueJnGpuCatalogBatch& event,
    EnrichmentSpoolBuildState& state, std::string& error )
{
    const auto payloadIt = state.catalogPayloads.find( event.payloadId );
    if( payloadIt == state.catalogPayloads.end() )
    { error = "session_gpu_enrichment_payload_missing"; return false; }
    auto payload = std::move( payloadIt->second ); state.catalogPayloads.erase( payloadIt );
    if( payload.size() != event.payloadBytes || payload.size() < sizeof( JnGpuCatalogBatchEnvelopeV1 ) )
    { error = "session_gpu_enrichment_payload_size_mismatch"; return false; }
    JnGpuCatalogBatchEnvelopeV1 envelope {};
    std::memcpy( &envelope, payload.data(), sizeof( envelope ) );
    const auto* records = payload.data() + sizeof( envelope );
    const auto recordBytes = payload.size() - sizeof( envelope );
    if( envelope.magic != JnGpuCatalogBatchMagic ||
        envelope.catalogSchema != JnGpuCatalogSchemaVersion ||
        envelope.evidenceSchema != JnGpuDetailedEvidenceSchemaVersion ||
        envelope.recordCount != event.recordCount || envelope.payloadBytes != recordBytes ||
        JnGpuCatalogChecksum64( records, recordBytes ) != envelope.checksum )
    { error = "session_gpu_enrichment_envelope_invalid"; return false; }
    const auto kind = JnGpuCatalogBatchKind( event.kind );
    if( kind == JnGpuCatalogBatchKind::View )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ) ||
            envelope.recordBytes != sizeof( JnGpuCatalogViewRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuCatalogViewRecordV1 ) != recordBytes )
        { error = "session_gpu_view_batch_invalid"; return false; }
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            JnGpuCatalogViewRecordV1 value {};
            std::memcpy( &value, records + size_t( i ) * sizeof( value ), sizeof( value ) );
            value.time = state.transform->ToNanoseconds( value.time );
            if( value.resourceId == 0 && value.pointerToken != 0 )
                value.resourceId = state.resolver->Resolve( value.pointerToken, value.time );
            if( value.pointerToken != 0 && value.resourceId == 0 )
                value.exactness = uint8_t( JnGpuCatalogExactness::Partial );
            if( value.resourceId != 0 ) value.pointerToken = 0;
            if( value.resourceId != 0 && state.catalog->HasResource( value.resourceId ) )
            { state.views.push_back( { event.generation, value } ); ++state.spool->viewCount; }
            if( state.views.size() * sizeof( GpuViewAnalysisRecord ) >= 64ull * 1024 * 1024 &&
                !FlushViewRun( state, error ) ) return false;
        }
    }
    else if( kind == JnGpuCatalogBatchKind::Part )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ) ||
            envelope.recordBytes != sizeof( JnGpuCatalogPartRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuCatalogPartRecordV1 ) != recordBytes )
        { error = "session_gpu_part_batch_invalid"; return false; }
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            JnGpuCatalogPartRecordV1 value {};
            std::memcpy( &value, records + size_t( i ) * sizeof( value ), sizeof( value ) );
            value.time = state.transform->ToNanoseconds( value.time );
            if( value.resourceId != 0 && state.catalog->HasResource( value.resourceId ) )
            { state.parts.push_back( { event.generation, value } ); ++state.spool->partCount; }
            if( state.parts.size() * sizeof( GpuPartAnalysisRecord ) >= 64ull * 1024 * 1024 &&
                !FlushPartRun( state, error ) ) return false;
        }
    }
    else if( kind == JnGpuCatalogBatchKind::VirtualGeometry )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ) ||
            envelope.recordBytes != sizeof( JnGpuCatalogVgRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuCatalogVgRecordV1 ) != recordBytes )
        { error = "session_gpu_vg_batch_invalid"; return false; }
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            JnGpuCatalogVgRecordV1 value {};
            std::memcpy( &value, records + size_t( i ) * sizeof( value ), sizeof( value ) );
            value.time = state.transform->ToNanoseconds( value.time );
            if( value.resourceId == 0 && value.pointerToken != 0 )
                value.resourceId = state.resolver->Resolve( value.pointerToken, value.time );
            if( value.pointerToken != 0 && value.resourceId == 0 )
                value.exactness = uint8_t( JnGpuCatalogExactness::Partial );
            if( value.resourceId != 0 ) value.pointerToken = 0;
            if( value.resourceId != 0 && state.catalog->HasResource( value.resourceId ) )
            { state.virtualGeometry.push_back( { event.generation, value } ); ++state.spool->virtualGeometryCount; }
            if( state.virtualGeometry.size() * sizeof( GpuVgAnalysisRecord ) >= 64ull * 1024 * 1024 &&
                !FlushVirtualGeometryRun( state, error ) ) return false;
        }
    }
    else if( kind == JnGpuCatalogBatchKind::Logical )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ) ||
            envelope.recordBytes != sizeof( JnGpuCatalogLogicalRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuCatalogLogicalRecordV1 ) != recordBytes )
        { error = "session_gpu_logical_batch_invalid"; return false; }
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            if( ( i & 0xfff ) == 0 && state.control->stopToken.stop_requested() )
            { error = "cancelled"; return false; }
            JnGpuCatalogLogicalRecordV1 value {};
            std::memcpy( &value, records + size_t( i ) * sizeof( value ), sizeof( value ) );
            value.time = state.transform->ToNanoseconds( value.time );
            if( value.resourceId == 0 && value.pointerToken != 0 )
                value.resourceId = state.resolver->Resolve( value.pointerToken, value.time );
            if( value.pointerToken != 0 && value.resourceId == 0 )
                value.exactness = uint8_t( JnGpuCatalogExactness::Partial );
            if( value.resourceId != 0 ) value.pointerToken = 0;
            state.logicalRaw.push_back( { value.logicalResourceId, event.generation, value } );
            if( state.logicalRaw.size() * sizeof( GpuLogicalRawEntry ) >= 64ull * 1024 * 1024 &&
                !FlushLogicalRawRun( state, error ) ) return false;
        }
    }
    else if( kind == JnGpuCatalogBatchKind::Relation )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::FixedV1 ) ||
            envelope.recordBytes != sizeof( JnGpuCatalogRelationRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuCatalogRelationRecordV1 ) != recordBytes )
        { error = "session_gpu_catalog_relation_batch_invalid"; return false; }
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            if( ( i & 0xfff ) == 0 && state.control->stopToken.stop_requested() )
            { error = "cancelled"; return false; }
            JnGpuCatalogRelationRecordV1 value {};
            std::memcpy( &value, records + size_t( i ) * sizeof( value ), sizeof( value ) );
            value.time = state.transform->ToNanoseconds( value.time );
            if( ( value.flags & 1 ) != 0 )
            {
                const auto resolved = state.resolver->Resolve( value.sourceId, value.time );
                if( resolved == 0 )
                {
                    value.exactness = uint8_t( JnGpuCatalogExactness::Partial );
                    if( value.relation == uint8_t( JnGpuCatalogRelationKind::BackedBy ) ||
                        value.relation == uint8_t( JnGpuCatalogRelationKind::PrimaryOwner ) )
                        ++state.coreUnresolved;
                }
                else { value.sourceId = resolved; value.flags &= ~uint8_t( 1 ); }
            }
            if( ( value.flags & 2 ) != 0 )
            {
                const auto resolved = state.resolver->Resolve( value.targetId, value.time );
                if( resolved == 0 )
                {
                    value.exactness = uint8_t( JnGpuCatalogExactness::Partial );
                    if( value.relation == uint8_t( JnGpuCatalogRelationKind::BackedBy ) ||
                        value.relation == uint8_t( JnGpuCatalogRelationKind::PrimaryOwner ) )
                        ++state.coreUnresolved;
                }
                else { value.targetId = resolved; value.flags &= ~uint8_t( 2 ); }
            }
            if( state.catalog->HasResource( value.sourceId ) )
            { state.relations.push_back( { value.sourceId, event.generation, value } ); ++state.spool->catalogRelationCount; }
            if( value.targetId != value.sourceId && state.catalog->HasResource( value.targetId ) )
            { state.relations.push_back( { value.targetId, event.generation, value } ); ++state.spool->catalogRelationCount; }
            if( state.relations.size() * sizeof( GpuAnalysisCatalogRelationStoreEntry ) >= 64ull * 1024 * 1024 &&
                !FlushCatalogRelationRun( state, error ) ) return false;
        }
    }
    else if( kind == JnGpuCatalogBatchKind::RangeSet )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::RangeSetV1 ) ||
            envelope.recordBytes != sizeof( JnGpuRangeSetRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuRangeSetRecordV1 ) != recordBytes )
        { error = "session_gpu_range_spool_batch_invalid"; return false; }
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            if( ( i & 0xfff ) == 0 && state.control->stopToken.stop_requested() )
            { error = "cancelled"; return false; }
            JnGpuRangeSetRecordV1 value {};
            std::memcpy( &value, records + size_t( i ) * sizeof( value ), sizeof( value ) );
            if( value.passInstanceId == 0 )
            { error = "session_gpu_range_spool_pass_invalid"; return false; }
            state.rangeRaw.push_back( { event.generation, value.passInstanceId, value } );
            if( state.rangeRaw.size() * sizeof( GpuPassRangeRawEntry ) >= 64ull * 1024 * 1024 &&
                !FlushRangeRawRun( state, error ) ) return false;
        }
    }
    return true;
}

bool VisitGpuEnrichmentSpool( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<EnrichmentSpoolBuildState*>( userData );
    if( state.control->stopToken.stop_requested() ) { error = "cancelled"; return false; }
    QueueItem item {}; if( !DecodeItem( record, item, error ) ) return false;
    switch( item.hdr.type )
    {
    case QueueType::JnGfxLink:
        if( item.jnGfxLink.relation == uint8_t( JnGfxRelation::ReferencesResources ) )
        {
            if( item.jnGfxLink.sourceId == 0 || item.jnGfxLink.targetId == 0 )
            { error = "session_gpu_range_pass_alias_invalid"; return false; }
            state.passAliases.push_back( { item.jnGfxLink.sourceId, item.jnGfxLink.targetId } );
            if( state.passAliases.size() * sizeof( GpuPassAliasEntry ) >= 64ull * 1024 * 1024 &&
                !FlushPassAliasRun( state, error ) ) return false;
        }
        return true;
    case QueueType::JnGpuReferenceSetDefinition:
        return VisitResourceSetDefinition( record, state.resourceSets, error );
    case QueueType::JnGpuCatalogBatchData:
    {
        uint64_t payloadId = 0; const uint8_t* bytes = nullptr; uint32_t size = 0;
        if( !DecodeLargePayload( record, item, payloadId, bytes, size, error ) ) return false;
        if( payloadId == 0 || size < sizeof( JnGpuCatalogBatchEnvelopeV1 ) ||
            size > 64u * 1024 * 1024 || !state.catalogPayloads.emplace(
                payloadId, std::vector<uint8_t>( bytes, bytes + size ) ).second )
        { error = "session_gpu_enrichment_payload_invalid"; return false; }
        return true;
    }
    case QueueType::JnGpuCatalogBatch:
        return ProcessEnrichmentCatalogBatch( item.jnGpuCatalogBatch, state, error );
    default: return true;
    }
}

bool NormalizePassAliasRuns( EnrichmentSpoolBuildState& state,
    std::filesystem::path& outputPath, std::string& error )
{
    if( !FlushPassAliasRun( state, error ) ) return false;
    outputPath = state.spool->root / "pass-aliases.bin";
    std::ofstream out( GpuAnalysisIoPath( outputPath ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_gpu_range_pass_alias_open_failed"; return false; }
    struct Cursor { std::ifstream in; GpuPassAliasEntry value {}; };
    struct Node { GpuPassAliasEntry value {}; size_t run = 0; };
    const auto compare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.explicitPassId != rhs.explicitPassId ) return lhs.explicitPassId < rhs.explicitPassId;
        return lhs.referencePassId < rhs.referencePassId;
    };
    struct Later { decltype( compare ) compare; bool operator()( const Node& lhs,
        const Node& rhs ) const { return compare( rhs.value, lhs.value ); } };
    std::vector<Cursor> cursors( state.passAliasRuns.size() );
    std::priority_queue<Node, std::vector<Node>, Later> heap( Later { compare } );
    for( size_t i = 0; i < state.passAliasRuns.size(); ++i )
    {
        cursors[i].in.open( GpuAnalysisIoPath( state.passAliasRuns[i] ), std::ios::binary );
        if( !cursors[i].in ) { error = "session_gpu_range_pass_alias_run_open_failed"; return false; }
        if( cursors[i].in.read( reinterpret_cast<char*>( &cursors[i].value ), sizeof( GpuPassAliasEntry ) ) )
            heap.push( { cursors[i].value, i } );
    }
    GpuPassAliasEntry previous {};
    bool hasPrevious = false;
    uint64_t written = 0;
    while( !heap.empty() )
    {
        const auto node = heap.top(); heap.pop();
        if( ( written & 0xffff ) == 0 && state.control->stopToken.stop_requested() )
        { error = "cancelled"; return false; }
        if( hasPrevious && node.value.explicitPassId == previous.explicitPassId &&
            node.value.referencePassId != previous.referencePassId )
        { error = "session_gpu_range_pass_alias_conflict:" +
            std::to_string( node.value.explicitPassId ); return false; }
        if( !hasPrevious || node.value.explicitPassId != previous.explicitPassId )
        {
            out.write( reinterpret_cast<const char*>( &node.value ), sizeof( node.value ) );
            if( !out ) { error = "session_gpu_range_pass_alias_write_failed"; return false; }
            previous = node.value; hasPrevious = true; ++written;
        }
        auto& cursor = cursors[node.run];
        if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( GpuPassAliasEntry ) ) )
            heap.push( { cursor.value, node.run } );
        else if( !cursor.in.eof() )
        { error = "session_gpu_range_pass_alias_run_read_failed"; return false; }
    }
    out.flush(); out.close();
    if( !out ) { error = "session_gpu_range_pass_alias_flush_failed"; return false; }
    for( const auto& path : state.passAliasRuns )
    { std::error_code ec; std::filesystem::remove( GpuAnalysisIoPath( path ), ec ); }
    std::error_code ec;
    std::filesystem::remove( GpuAnalysisIoPath( state.spool->root / "pass-alias-runs" ), ec );
    return true;
}

class PassAliasReader
{
public:
    bool Open( const std::filesystem::path& path, std::string& error )
    {
        if( !m_data.Open( path, error ) ) return false;
        if( m_data.Size() % sizeof( GpuPassAliasEntry ) != 0 )
        { error = "session_gpu_range_pass_alias_size_invalid"; return false; }
        m_count = m_data.Size() / sizeof( GpuPassAliasEntry ); return true;
    }
    uint64_t Resolve( uint64_t explicitPassId ) const
    {
        if( m_count == 0 ) return 0;
        const auto* begin = reinterpret_cast<const GpuPassAliasEntry*>( m_data.Data() );
        const auto* end = begin + m_count;
        const auto* found = std::lower_bound( begin, end, explicitPassId,
            []( const auto& value, uint64_t id ) { return value.explicitPassId < id; } );
        return found != end && found->explicitPassId == explicitPassId ? found->referencePassId : 0;
    }
private:
    ReadOnlyMappedFile m_data;
    uint64_t m_count = 0;
};

bool NormalizeLogicalRuns( EnrichmentSpoolBuildState& state, std::string& error )
{
    if( !FlushLogicalRawRun( state, error ) || !FlushCatalogRelationRun( state, error ) ) return false;
    const auto lifetimePath = state.spool->root / "logical-lifetimes.bin";
    std::ofstream lifetimeOut( GpuAnalysisIoPath( lifetimePath ),
        std::ios::binary | std::ios::trunc );
    if( !lifetimeOut ) { error = "session_gpu_logical_lifetime_open_failed"; return false; }
    struct Cursor { std::ifstream in; GpuLogicalRawEntry value {}; };
    struct Node { GpuLogicalRawEntry value {}; size_t run = 0; };
    const auto compare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.logicalId != rhs.logicalId ) return lhs.logicalId < rhs.logicalId;
        if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
        if( lhs.record.resourceId != rhs.record.resourceId ) return lhs.record.resourceId < rhs.record.resourceId;
        return lhs.generation < rhs.generation;
    };
    struct Later { decltype( compare ) compare; bool operator()( const Node& lhs, const Node& rhs ) const
        { return compare( rhs.value, lhs.value ); } };
    std::vector<Cursor> cursors( state.logicalRawRuns.size() );
    std::priority_queue<Node, std::vector<Node>, Later> heap( Later { compare } );
    for( size_t i = 0; i < state.logicalRawRuns.size(); ++i )
    {
        cursors[i].in.open( GpuAnalysisIoPath( state.logicalRawRuns[i] ), std::ios::binary );
        if( !cursors[i].in ) { error = "session_gpu_logical_run_read_open_failed"; return false; }
        if( cursors[i].in.read( reinterpret_cast<char*>( &cursors[i].value ), sizeof( GpuLogicalRawEntry ) ) )
            heap.push( { cursors[i].value, i } );
    }
    // Most logical resources have only a handful of lifetime records, so keep
    // the common case in memory.  A malformed or very long capture may however
    // contain an unbounded number of records for one logical id.  Spill that
    // single group before it can make conversion memory depend on total event
    // count.  The final definition is still observed before replaying the
    // group, preserving the existing normalization semantics.
    constexpr size_t LogicalGroupMemoryLimit = 256ull * 1024;
    const auto groupSpillPath = state.spool->root / "logical-group-spill.tmp";
    std::vector<GpuLogicalRawEntry> group;
    std::ofstream groupSpill;
    uint64_t groupEntryCount = 0;
    bool groupSpilled = false;
    std::vector<GpuAnalysisLogicalStoreEntry> output;
    std::optional<GpuLogicalRawEntry> definition;
    uint64_t groupId = 0;
    const auto flushOutput = [&]() -> bool {
        return SaveGpuSpoolRun( output, state.spool->root / "logical-runs", "logical",
            state.spool->logicalRuns, []( const auto& lhs, const auto& rhs ) {
                if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
                if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
                if( lhs.record.logicalResourceId != rhs.record.logicalResourceId )
                    return lhs.record.logicalResourceId < rhs.record.logicalResourceId;
                return lhs.generation < rhs.generation;
            }, error );
    };
    const auto emitEntry = [&]( const GpuLogicalRawEntry& entry ) -> bool {
        auto normalized = entry.record; auto nameGeneration = entry.generation;
        if( definition )
        {
            const auto& source = definition->record;
            if( normalized.stableKey == 0 ) normalized.stableKey = source.stableKey;
            if( normalized.familyId == 0 ) normalized.familyId = source.familyId;
            if( normalized.physicalOffsetBytes == 0 ) normalized.physicalOffsetBytes = source.physicalOffsetBytes;
            if( normalized.lengthBytes == 0 ) normalized.lengthBytes = source.lengthBytes;
            if( normalized.aliasGroupId == 0 ) normalized.aliasGroupId = source.aliasGroupId;
            if( normalized.nameId == 0 ) { normalized.nameId = source.nameId;
                normalized.nameProvenance = source.nameProvenance; nameGeneration = definition->generation; }
            if( normalized.primaryKind == 0 ) normalized.primaryKind = source.primaryKind;
            if( normalized.definitionRevision == 0 ) normalized.definitionRevision = source.definitionRevision;
        }
        if( normalized.resourceId != 0 && state.catalog->HasResource( normalized.resourceId ) )
        {
            output.push_back( { normalized.resourceId, entry.generation, nameGeneration, normalized } );
            ++state.spool->logicalCount;
            if( output.size() * sizeof( GpuAnalysisLogicalStoreEntry ) >= 64ull * 1024 * 1024 &&
                !flushOutput() ) return false;
        }
        return true;
    };
    const auto spillGroup = [&]() -> bool {
        if( groupSpilled ) return true;
        groupSpill.open( GpuAnalysisIoPath( groupSpillPath ), std::ios::binary | std::ios::trunc );
        if( !groupSpill ) { error = "session_gpu_logical_group_spill_open_failed"; return false; }
        groupSpill.write( reinterpret_cast<const char*>( group.data() ),
            std::streamsize( group.size() * sizeof( GpuLogicalRawEntry ) ) );
        if( !groupSpill ) { error = "session_gpu_logical_group_spill_write_failed"; return false; }
        group.clear(); groupSpilled = true; return true;
    };
    const auto appendGroup = [&]( const GpuLogicalRawEntry& entry ) -> bool {
        ++groupEntryCount;
        if( groupSpilled )
        {
            groupSpill.write( reinterpret_cast<const char*>( &entry ), sizeof( entry ) );
            if( !groupSpill ) { error = "session_gpu_logical_group_spill_write_failed"; return false; }
            return true;
        }
        group.push_back( entry );
        return group.size() * sizeof( GpuLogicalRawEntry ) < LogicalGroupMemoryLimit || spillGroup();
    };
    const auto flushGroup = [&]() -> bool {
        if( groupEntryCount == 0 ) return true;
        if( groupSpilled )
        {
            groupSpill.flush(); groupSpill.close();
            if( !groupSpill ) { error = "session_gpu_logical_group_spill_flush_failed"; return false; }
            std::ifstream in( GpuAnalysisIoPath( groupSpillPath ), std::ios::binary );
            if( !in ) { error = "session_gpu_logical_group_spill_read_open_failed"; return false; }
            GpuLogicalRawEntry entry {}; uint64_t readCount = 0;
            while( in.read( reinterpret_cast<char*>( &entry ), sizeof( entry ) ) )
            { ++readCount; if( !emitEntry( entry ) ) return false; }
            if( !in.eof() || readCount != groupEntryCount )
            { error = "session_gpu_logical_group_spill_read_failed"; return false; }
            in.close(); std::error_code ec; std::filesystem::remove( GpuAnalysisIoPath( groupSpillPath ), ec );
            if( ec ) { error = std::string( "session_gpu_logical_group_spill_cleanup_failed:" ) + ec.message(); return false; }
            groupSpilled = false;
        }
        else
        {
            for( const auto& entry : group ) if( !emitEntry( entry ) ) return false;
        }
        group.clear(); groupEntryCount = 0; definition.reset(); return true;
    };
    while( !heap.empty() )
    {
        const auto node = heap.top(); heap.pop();
        if( ( state.spool->logicalCount & 0xffff ) == 0 &&
            state.control->stopToken.stop_requested() )
        { error = "cancelled"; return false; }
        if( groupId != 0 && node.value.logicalId != groupId && !flushGroup() ) return false;
        if( group.empty() ) groupId = node.value.logicalId;
        if( node.value.logicalId != 0 )
        {
            const ResourceLifetimeEntry lifetime { node.value.logicalId,
                node.value.record.time, node.value.record.resourceId,
                node.value.record.operation };
            lifetimeOut.write( reinterpret_cast<const char*>( &lifetime ), sizeof( lifetime ) );
            if( !lifetimeOut ) { error = "session_gpu_logical_lifetime_write_failed"; return false; }
        }
        if( !appendGroup( node.value ) ) return false;
        if( IsCatalogDefinition( JnGpuCatalogRecordOperation( node.value.record.operation ) ) )
            definition = node.value;
        auto& cursor = cursors[node.run];
        if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( GpuLogicalRawEntry ) ) )
            heap.push( { cursor.value, node.run } );
        else if( !cursor.in.eof() ) { error = "session_gpu_logical_run_read_failed"; return false; }
    }
    if( !flushGroup() || !flushOutput() ) return false;
    lifetimeOut.flush(); lifetimeOut.close();
    if( !lifetimeOut ) { error = "session_gpu_logical_lifetime_flush_failed"; return false; }
    for( const auto& path : state.logicalRawRuns ) { std::error_code ec; std::filesystem::remove( GpuAnalysisIoPath( path ), ec ); }
    std::error_code ec; std::filesystem::remove( GpuAnalysisIoPath( state.spool->root / "logical-raw-runs" ), ec );
    return true;
}

bool NormalizeRangeRuns( EnrichmentSpoolBuildState& state, std::string& error )
{
    if( !FlushRangeRawRun( state, error ) ) return false;
    std::filesystem::path aliasPath;
    if( !NormalizePassAliasRuns( state, aliasPath, error ) ) return false;
    PassAliasReader aliases;
    if( !aliases.Open( aliasPath, error ) ) return false;
    const auto outputPath = state.spool->root / "ranges-by-pass.bin";
    std::ofstream out( GpuAnalysisIoPath( outputPath ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_gpu_range_by_pass_open_failed"; return false; }
    struct Cursor { std::ifstream in; GpuPassRangeRawEntry value {}; };
    struct Node { GpuPassRangeRawEntry value {}; size_t run = 0; };
    const auto compare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.record.passInstanceId != rhs.record.passInstanceId )
            return lhs.record.passInstanceId < rhs.record.passInstanceId;
        if( lhs.record.resourceId != rhs.record.resourceId )
            return lhs.record.resourceId < rhs.record.resourceId;
        if( lhs.record.pointerToken != rhs.record.pointerToken )
            return lhs.record.pointerToken < rhs.record.pointerToken;
        if( lhs.record.offsetBytes != rhs.record.offsetBytes )
            return lhs.record.offsetBytes < rhs.record.offsetBytes;
        if( lhs.record.firstSubresource != rhs.record.firstSubresource )
            return lhs.record.firstSubresource < rhs.record.firstSubresource;
        return lhs.generation < rhs.generation;
    };
    struct Later { decltype( compare ) compare; bool operator()( const Node& lhs, const Node& rhs ) const
        { return compare( rhs.value, lhs.value ); } };
    std::vector<Cursor> cursors( state.rangeRawRuns.size() );
    std::priority_queue<Node, std::vector<Node>, Later> heap( Later { compare } );
    for( size_t i = 0; i < state.rangeRawRuns.size(); ++i )
    {
        cursors[i].in.open( GpuAnalysisIoPath( state.rangeRawRuns[i] ), std::ios::binary );
        if( !cursors[i].in ) { error = "session_gpu_range_raw_run_open_failed"; return false; }
        if( cursors[i].in.read( reinterpret_cast<char*>( &cursors[i].value ), sizeof( GpuPassRangeRawEntry ) ) )
            heap.push( { cursors[i].value, i } );
    }
    std::vector<GpuPassRangeRawEntry> mapped;
    std::vector<std::filesystem::path> mappedRuns;
    const auto mappedCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.evidencePassId != rhs.evidencePassId ) return lhs.evidencePassId < rhs.evidencePassId;
        if( lhs.record.resourceId != rhs.record.resourceId ) return lhs.record.resourceId < rhs.record.resourceId;
        if( lhs.record.pointerToken != rhs.record.pointerToken ) return lhs.record.pointerToken < rhs.record.pointerToken;
        if( lhs.record.offsetBytes != rhs.record.offsetBytes ) return lhs.record.offsetBytes < rhs.record.offsetBytes;
        if( lhs.record.firstSubresource != rhs.record.firstSubresource )
            return lhs.record.firstSubresource < rhs.record.firstSubresource;
        return lhs.generation < rhs.generation;
    };
    uint64_t observed = 0;
    while( !heap.empty() )
    {
        const auto node = heap.top(); heap.pop();
        if( ( observed & 0xffff ) == 0 && state.control->stopToken.stop_requested() )
        { error = "cancelled"; return false; }
        auto value = node.value;
        const auto mappedPass = aliases.Resolve( value.record.passInstanceId );
        if( mappedPass != 0 ) value.evidencePassId = mappedPass;
        else if( value.record.passInstanceId >= ( uint64_t( 1 ) << 63 ) )
        { error = "session_gpu_range_explicit_pass_link_missing:" +
            std::to_string( value.record.passInstanceId ); return false; }
        else value.evidencePassId = value.record.passInstanceId;
        mapped.push_back( value ); ++observed;
        if( mapped.size() * sizeof( GpuPassRangeRawEntry ) >= 64ull * 1024 * 1024 &&
            !SaveGpuSpoolRun( mapped, state.spool->root / "range-mapped-runs",
                "mapped", mappedRuns, mappedCompare, error ) ) return false;
        auto& cursor = cursors[node.run];
        if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( GpuPassRangeRawEntry ) ) )
            heap.push( { cursor.value, node.run } );
        else if( !cursor.in.eof() ) { error = "session_gpu_range_raw_run_read_failed"; return false; }
    }
    if( !SaveGpuSpoolRun( mapped, state.spool->root / "range-mapped-runs",
        "mapped", mappedRuns, mappedCompare, error ) ) return false;
    struct MappedLater { decltype( mappedCompare ) compare; bool operator()( const Node& lhs,
        const Node& rhs ) const { return compare( rhs.value, lhs.value ); } };
    std::vector<Cursor> mappedCursors( mappedRuns.size() );
    std::priority_queue<Node, std::vector<Node>, MappedLater> mappedHeap(
        MappedLater { mappedCompare } );
    for( size_t i = 0; i < mappedRuns.size(); ++i )
    {
        mappedCursors[i].in.open( GpuAnalysisIoPath( mappedRuns[i] ), std::ios::binary );
        if( !mappedCursors[i].in ) { error = "session_gpu_range_mapped_run_open_failed"; return false; }
        if( mappedCursors[i].in.read( reinterpret_cast<char*>( &mappedCursors[i].value ),
                sizeof( GpuPassRangeRawEntry ) ) ) mappedHeap.push( { mappedCursors[i].value, i } );
    }
    uint64_t written = 0;
    while( !mappedHeap.empty() )
    {
        const auto node = mappedHeap.top(); mappedHeap.pop();
        if( ( written & 0xffff ) == 0 && state.control->stopToken.stop_requested() )
        { error = "cancelled"; return false; }
        out.write( reinterpret_cast<const char*>( &node.value ), sizeof( node.value ) );
        if( !out ) { error = "session_gpu_range_by_pass_write_failed"; return false; }
        ++written;
        auto& cursor = mappedCursors[node.run];
        if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( GpuPassRangeRawEntry ) ) )
            mappedHeap.push( { cursor.value, node.run } );
        else if( !cursor.in.eof() ) { error = "session_gpu_range_mapped_run_read_failed"; return false; }
    }
    if( written != observed ) { error = "session_gpu_range_mapped_count_mismatch"; return false; }
    out.flush(); out.close();
    if( !out ) { error = "session_gpu_range_by_pass_flush_failed"; return false; }
    for( const auto& path : state.rangeRawRuns )
    { std::error_code ec; std::filesystem::remove( GpuAnalysisIoPath( path ), ec ); }
    for( const auto& path : mappedRuns )
    { std::error_code ec; std::filesystem::remove( GpuAnalysisIoPath( path ), ec ); }
    std::error_code ec;
    std::filesystem::remove( GpuAnalysisIoPath( state.spool->root / "range-raw-runs" ), ec );
    std::filesystem::remove( GpuAnalysisIoPath( state.spool->root / "range-mapped-runs" ), ec );
    std::filesystem::remove( GpuAnalysisIoPath( aliasPath ), ec );
    return true;
}

uint64_t PhysicalBytesForPass( const GpuAnalysisSnapshot& catalog,
    const std::vector<uint64_t>& resources )
{
    std::unordered_set<uint64_t> allocations;
    uint64_t bytes = 0;
    for( const auto resourceId : resources )
    {
        const auto* resource = catalog.FindResource( resourceId );
        if( !resource || resource->allocationId == 0 ||
            !allocations.emplace( resource->allocationId ).second ) continue;
        const auto* allocation = catalog.FindAllocation( resource->allocationId );
        if( allocation && allocation->aliveAtEnd ) bytes += allocation->sizeBytes;
    }
    return bytes;
}

template<typename T>
bool WriteCatalogSpoolPage( GpuAnalysisCatalogSpool& spool,
    const GpuAnalysisTraceIdentity& identity, GpuAnalysisStorePageKind kind,
    const char* directory, std::vector<T>& values, uint64_t firstIndex,
    std::string& error )
{
    if( values.empty() ) return true;
    std::error_code ec; const auto root = spool.root / directory;
    std::filesystem::create_directories( GpuAnalysisIoPath( root ), ec );
    if( ec ) { error = "session_gpu_catalog_page_directory_failed:" + ec.message(); return false; }
    uint64_t pageIndex = 0;
    for( const auto& page : spool.pages ) if( page.kind == kind ) ++pageIndex;
    std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << pageIndex << ".bin";
    const auto path = root / name.str();
    GpuAnalysisSnapshot snapshot = spool.overview;
    if constexpr( std::is_same_v<T, GpuResourceAnalysisRecord> ) snapshot.resources = std::move( values );
    else if constexpr( std::is_same_v<T, GpuAllocationAnalysisRecord> ) snapshot.allocations = std::move( values );
    else if constexpr( std::is_same_v<T, GpuChurnCandidate> ) snapshot.churnCandidates = std::move( values );
    const GpuAnalysisCacheIdentity cacheIdentity { identity.sha256, identity.fileSize,
        std::string( GpuAnalysisAlgorithmId ) + "-session-catalog-spool1" };
    if( !SaveGpuAnalysisCache( path, cacheIdentity, snapshot, error ) ) return false;
    const auto& stored = [&]() -> const auto& {
        if constexpr( std::is_same_v<T, GpuResourceAnalysisRecord> ) return snapshot.resources;
        else if constexpr( std::is_same_v<T, GpuAllocationAnalysisRecord> ) return snapshot.allocations;
        else return snapshot.churnCandidates;
    }();
    const auto key = []( const auto& value ) -> uint64_t {
        if constexpr( std::is_same_v<std::decay_t<decltype( value )>, GpuResourceAnalysisRecord> ) return value.resourceId;
        else if constexpr( std::is_same_v<std::decay_t<decltype( value )>, GpuAllocationAnalysisRecord> ) return value.allocationId;
        else return value.resourceId ? value.resourceId : value.allocationId;
    };
    const auto bytes = std::filesystem::file_size( GpuAnalysisIoPath( path ), ec );
    if( ec ) { error = "session_gpu_catalog_page_size_failed:" + ec.message(); return false; }
    spool.pages.push_back( { kind, firstIndex, uint64_t( stored.size() ),
        key( stored.front() ), key( stored.back() ), bytes, 0,
        std::filesystem::relative( path, spool.root ) } );
    ++spool.pageCount; values.clear(); return true;
}

template<typename T, typename Compare>
bool MergeGpuSpoolRunsToFile( const std::vector<std::filesystem::path>& runs,
    const std::filesystem::path& path, Compare compare, uint64_t& count,
    std::string& error )
{
    GpuSpoolRunCursor<T, Compare> cursor( compare );
    if( !cursor.Open( runs, error ) ) return false;
    std::ofstream out( GpuAnalysisIoPath( path ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_gpu_catalog_merge_open_failed"; return false; }
    count = 0; T value {};
    while( !cursor.Empty() )
    {
        if( !cursor.Pop( value, error ) ) return false;
        out.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
        if( !out ) { error = "session_gpu_catalog_merge_write_failed"; return false; }
        ++count;
    }
    out.flush(); out.close();
    if( !out ) { error = "session_gpu_catalog_merge_flush_failed"; return false; }
    return true;
}

bool BuildBoundedGpuCatalogSpool( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionTimeTransform& transform,
    const JnTraceData& metadata, const GpuAnalysisTraceIdentity& identity,
    const GpuAnalysisSidecarControl& control, GpuAnalysisCatalogSpool& spool,
    std::string& error )
{
    error.clear(); spool = {};
    spool.root = sessionRoot / "checkpoints" / "gpu-catalog-spool";
    std::error_code ec; std::filesystem::remove_all( GpuAnalysisIoPath( spool.root ), ec ); ec.clear();
    std::filesystem::create_directories( GpuAnalysisIoPath( spool.root ), ec );
    if( ec ) { error = "session_gpu_catalog_spool_directory_failed:" + ec.message(); return false; }
    spool.overview.manifest.catalogSchema = metadata.gpuCatalogSchemaVersion;
    spool.overview.manifest.evidenceSchema = metadata.gpuDetailedEvidenceSchemaVersion;
    spool.overview.manifest.generationCount = metadata.gpuCatalogGenerations.size();
    spool.overview.manifest.transportValid = metadata.gpuCatalogValid;
    spool.overview.manifest.complete = metadata.gpuCatalogPresent && metadata.gpuCatalogValid;
    spool.overview.manifest.state = spool.overview.manifest.complete ?
        GpuAnalysisState::Complete : GpuAnalysisState::Invalid;
    spool.overview.manifest.reason = spool.overview.manifest.complete ? "complete" : "invalid_catalog_generation";
    if( !spool.overview.manifest.complete )
    { error = "session_gpu_catalog_analysis_incomplete:" + spool.overview.manifest.reason; return false; }

    CatalogCoreSpoolBuildState state;
    state.transform = &transform; state.control = &control; state.spool = &spool;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest,
        VisitGpuCatalogCoreSpool, &state, error ) ) return false;
    if( !state.payloads.empty() )
    { error = "session_gpu_catalog_core_payload_unresolved"; return false; }
    if( !FlushCatalogResourceRun( state, error ) ||
        !FlushCatalogAllocationRun( state, error ) ||
        !FlushCatalogStringRun( state, error ) ) return false;

    const auto stringCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.generation != rhs.generation ) return lhs.generation < rhs.generation;
        if( lhs.stringId != rhs.stringId ) return lhs.stringId < rhs.stringId;
        return lhs.ordinal < rhs.ordinal;
    };
    GpuSpoolRunCursor<CatalogStringRawEntry, decltype( stringCompare )> stringCursor( stringCompare );
    if( !stringCursor.Open( state.stringRuns, error ) ) return false;
    spool.stringIndexPath = spool.root / "strings.index";
    spool.stringDataPath = spool.root / "strings.data";
    std::ofstream stringIndex( GpuAnalysisIoPath( spool.stringIndexPath ), std::ios::binary | std::ios::trunc );
    std::ofstream stringData( GpuAnalysisIoPath( spool.stringDataPath ), std::ios::binary | std::ios::trunc );
    if( !stringIndex || !stringData ) { error = "session_gpu_catalog_string_store_open_failed"; return false; }
    uint64_t stringOffset = 0; CatalogStringRawEntry pendingString {}; bool hasPendingString = false;
    const auto flushString = [&]() -> bool {
        if( !hasPendingString ) return true;
        const GpuAnalysisCatalogStringIndexEntry index { pendingString.generation,
            stringOffset, pendingString.stringId, pendingString.byteLength };
        stringIndex.write( reinterpret_cast<const char*>( &index ), sizeof( index ) );
        stringData.write( pendingString.bytes, pendingString.byteLength );
        if( !stringIndex || !stringData ) { error = "session_gpu_catalog_string_store_write_failed"; return false; }
        stringOffset += pendingString.byteLength; hasPendingString = false; return true;
    };
    while( !stringCursor.Empty() )
    {
        CatalogStringRawEntry value {}; if( !stringCursor.Pop( value, error ) ) return false;
        if( hasPendingString && ( value.generation != pendingString.generation || value.stringId != pendingString.stringId ) &&
            !flushString() ) return false;
        pendingString = value; hasPendingString = true;
    }
    if( !flushString() ) return false;
    stringIndex.flush(); stringData.flush(); stringIndex.close(); stringData.close();
    if( !stringIndex || !stringData ) { error = "session_gpu_catalog_string_store_flush_failed"; return false; }
    BoundedCatalogStringLookup stringLookup;
    if( !stringLookup.Open( spool, error ) ) return false;

    const auto resourceCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.record.resourceId != rhs.record.resourceId ) return lhs.record.resourceId < rhs.record.resourceId;
        if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
        return lhs.ordinal < rhs.ordinal;
    };
    GpuSpoolRunCursor<CatalogResourceRawEntry, decltype( resourceCompare )> resources( resourceCompare );
    if( !resources.Open( state.resourceRuns, error ) ) return false;
    std::ofstream resourceLookup( GpuAnalysisIoPath( spool.root / "resource-lookup.bin" ), std::ios::binary | std::ios::trunc );
    if( !resourceLookup ) { error = "session_gpu_catalog_resource_lookup_open_failed"; return false; }
    spool.resourceLookupPath = spool.root / "resource-lookup.bin";
    std::vector<CatalogAllocationResourceEntry> allocationResources;
    std::vector<std::filesystem::path> allocationResourceRuns;
    std::vector<CatalogResourceNameEntry> resourceNames;
    std::vector<std::filesystem::path> resourceNameRuns;
    std::vector<ResourceLifetimeEntry> lifetimes;
    std::vector<std::filesystem::path> lifetimeRuns;
    std::vector<GpuResourceAnalysisRecord> resourcePage;
    std::vector<GpuChurnCandidate> churnPage;
    std::map<uint16_t, GpuAnalysisTypeSummary> typeSummaries;
    const auto resourcePageTarget = std::max<uint64_t>( 1, control.targetDerivedPageBytes );
    uint64_t resourcePageBytes = 0, resourceFirstIndex = 0;
    uint64_t churnPageBytes = 0, churnFirstIndex = 0;
    auto flushResourcePage = [&]() {
        if( !WriteCatalogSpoolPage( spool, identity, GpuAnalysisStorePageKind::Resource,
            "resources", resourcePage, resourceFirstIndex, error ) ) return false;
        resourceFirstIndex = spool.resourceCount; resourcePageBytes = 0; return true;
    };
    auto appendChurn = [&]( GpuChurnCandidate value ) -> bool {
        churnPageBytes += sizeof( value ) + value.reason.size();
        churnPage.push_back( std::move( value ) ); ++spool.churnCount;
        if( churnPageBytes < resourcePageTarget ) return true;
        if( !WriteCatalogSpoolPage( spool, identity, GpuAnalysisStorePageKind::Churn,
            "churn", churnPage, churnFirstIndex, error ) ) return false;
        churnFirstIndex = spool.churnCount; churnPageBytes = 0; return true;
    };
    CatalogResourceRawEntry resourceRaw {};
    uint64_t currentResource = 0;
    GpuResourceAnalysisRecord resource {};
    auto flushResource = [&]() -> bool {
        if( currentResource == 0 ) return true;
        const CatalogResourceLookupEntry lookup {
            resource.resourceId, resource.allocationId, resource.capacityBytes,
            uint8_t( resource.aliveAtEnd ? 1 : 0 ), {} };
        resourceLookup.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
        if( !resourceLookup ) { error = "session_gpu_catalog_resource_lookup_write_failed"; return false; }
        if( resource.allocationId != 0 )
        {
            allocationResources.push_back( { resource.allocationId, resource.resourceId } );
            if( allocationResources.size() * sizeof( CatalogAllocationResourceEntry ) >= CatalogRunTargetBytes( control ) &&
                !SaveGpuSpoolRun( allocationResources, spool.root / "allocation-resource-runs", "relation",
                    allocationResourceRuns, []( const auto& lhs, const auto& rhs ) {
                        if( lhs.allocationId != rhs.allocationId ) return lhs.allocationId < rhs.allocationId;
                        return lhs.resourceId < rhs.resourceId;
                    }, error ) ) return false;
        }
        if( resource.aliveAtEnd )
        {
            spool.overview.logicalCapacityBytes += resource.capacityBytes;
            auto& type = typeSummaries[resource.primaryKind]; type.primaryKind = resource.primaryKind;
            ++type.liveResourceCount; type.resourceCapacityBytes += resource.capacityBytes;
            if( !appendChurn( { GpuChurnCandidateKind::AliveAtCaptureEnd,
                resource.resourceId, resource.allocationId, resource.capacityBytes, 1, 0,
                "resource is alive at capture end; this is not proof of a leak" } ) ) return false;
        }
        if( resource.openBoundary && !appendChurn( { GpuChurnCandidateKind::OpenCreateBoundary,
            resource.resourceId, resource.allocationId, resource.capacityBytes, 1, 0,
            "resource existed before the capture boundary" } ) ) return false;
        if( resource.nameHash != 0 )
        {
            resourceNames.push_back( { resource.nameHash, resource.resourceId,
                resource.allocationId, resource.capacityBytes } );
            if( resourceNames.size() * sizeof( CatalogResourceNameEntry ) >=
                CatalogRunTargetBytes( control ) && !SaveGpuSpoolRun( resourceNames,
                    spool.root / "resource-name-runs", "name", resourceNameRuns,
                    []( const auto& lhs, const auto& rhs ) {
                        if( lhs.nameHash != rhs.nameHash ) return lhs.nameHash < rhs.nameHash;
                        return lhs.resourceId < rhs.resourceId;
                    }, error ) ) return false;
        }
        resourcePageBytes += sizeof( resource ) + resource.name.size() +
            resource.history.size() * sizeof( size_t );
        resourcePage.push_back( std::move( resource ) ); ++spool.resourceCount;
        if( resourcePageBytes >= resourcePageTarget && !flushResourcePage() ) return false;
        resource = {}; currentResource = 0; return true;
    };
    while( !resources.Empty() )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        if( !resources.Pop( resourceRaw, error ) ) return false;
        const auto& value = resourceRaw.record;
        if( value.resourceId == 0 ) { ++spool.overview.manifest.invalidRecordCount; continue; }
        if( currentResource != 0 && currentResource != value.resourceId && !flushResource() ) return false;
        if( currentResource == 0 ) { currentResource = value.resourceId; resource.resourceId = value.resourceId; }
        if( resourceRaw.ordinal <= uint64_t( std::numeric_limits<size_t>::max() ) )
            resource.history.push_back( size_t( resourceRaw.ordinal ) );
        if( value.pointerToken != 0 )
        {
            lifetimes.push_back( { value.pointerToken, value.time, value.resourceId, value.operation } );
            if( lifetimes.size() * sizeof( ResourceLifetimeEntry ) >= CatalogRunTargetBytes( control ) &&
                !SaveGpuSpoolRun( lifetimes, spool.root / "lifetime-runs", "lifetime", lifetimeRuns,
                    []( const auto& lhs, const auto& rhs ) {
                        if( lhs.key != rhs.key ) return lhs.key < rhs.key;
                        if( lhs.time != rhs.time ) return lhs.time < rhs.time;
                        return lhs.resourceId < rhs.resourceId;
                    }, error ) ) return false;
        }
        const auto operation = JnGpuCatalogRecordOperation( value.operation );
        if( operation == JnGpuCatalogRecordOperation::Destroy || operation == JnGpuCatalogRecordOperation::Close )
        { resource.destroyTime = std::max<uint64_t>( resource.destroyTime, value.time < 0 ? 0 : uint64_t( value.time ) ); resource.aliveAtEnd = false; continue; }
        if( !IsCatalogDefinition( operation ) ) continue;
        if( value.time >= int64_t( resource.lastUpdateTime ) )
        {
            resource.generation = resourceRaw.generation; resource.familyId = value.familyId;
            resource.allocationId = value.allocationId; resource.capacityBytes = value.capacityBytes;
            resource.allocationOffsetBytes = value.allocationOffsetBytes;
            resource.lastUpdateTime = value.time < 0 ? 0 : uint64_t( value.time );
            resource.nameHash = value.nameHash; resource.createCallsiteId = value.createCallsiteId;
            resource.definitionRevision = value.definitionRevision; resource.declaredUsageMask = value.declaredUsageMask;
            resource.observedUsageMask = value.observedUsageMask; resource.backendFlags = value.backendFlags;
            resource.sampleCount = value.sampleCount; resource.nameOriginalLength = value.nameOriginalLength;
            resource.format = value.format; resource.width = value.width; resource.height = value.height;
            resource.depthOrArraySize = value.depthOrArraySize; resource.mipLevels = value.mipLevels;
            resource.primaryKind = value.primaryKind; resource.resourceClass = value.resourceClass;
            resource.dimension = value.dimension; resource.memoryDomain = value.memoryDomain;
            resource.allocationKind = value.allocationKind;
            resource.classificationProvenance = value.classificationProvenance;
            resource.nameProvenance = value.nameProvenance; resource.stackProvenance = value.stackProvenance;
            resource.exactness = value.exactness; resource.flags = value.flags;
            resource.invalid = value.exactness == uint8_t( JnGpuCatalogExactness::Invalid );
            resource.openBoundary |= operation == JnGpuCatalogRecordOperation::Open ||
                operation == JnGpuCatalogRecordOperation::Snapshot ||
                value.exactness == uint8_t( JnGpuCatalogExactness::OpenBoundary );
            resource.name = stringLookup.Find( resourceRaw.generation, value.nameId ); resource.aliveAtEnd = true;
        }
        if( operation == JnGpuCatalogRecordOperation::Create && resource.createTime == 0 )
            resource.createTime = resource.lastUpdateTime;
    }
    if( !flushResource() || !flushResourcePage() ) return false;
    if( !SaveGpuSpoolRun( resourceNames, spool.root / "resource-name-runs", "name",
        resourceNameRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.nameHash != rhs.nameHash ) return lhs.nameHash < rhs.nameHash;
            return lhs.resourceId < rhs.resourceId;
        }, error ) ) return false;
    const auto nameCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.nameHash != rhs.nameHash ) return lhs.nameHash < rhs.nameHash;
        return lhs.resourceId < rhs.resourceId;
    };
    GpuSpoolRunCursor<CatalogResourceNameEntry, decltype( nameCompare )> names( nameCompare );
    if( !names.Open( resourceNameRuns, error ) ) return false;
    uint64_t groupHash = 0, groupBytes = 0, groupCount = 0;
    CatalogResourceNameEntry groupFirst {};
    const auto flushNameGroup = [&]() -> bool {
        if( groupCount > 1 && !appendChurn( { GpuChurnCandidateKind::RepeatedRecreate,
            groupFirst.resourceId, groupFirst.allocationId, groupBytes, groupCount,
            double( groupCount ), "same stable name hash was created more than once" } ) ) return false;
        groupHash = 0; groupBytes = 0; groupCount = 0; groupFirst = {}; return true;
    };
    while( !names.Empty() )
    {
        CatalogResourceNameEntry value {};
        if( !names.Pop( value, error ) ) return false;
        if( groupCount != 0 && value.nameHash != groupHash && !flushNameGroup() ) return false;
        if( groupCount == 0 ) { groupHash = value.nameHash; groupFirst = value; }
        groupBytes += value.capacityBytes; ++groupCount;
    }
    if( !flushNameGroup() ) return false;
    resourceLookup.flush(); resourceLookup.close();
    if( !resourceLookup ) { error = "session_gpu_catalog_resource_lookup_flush_failed"; return false; }
    if( !SaveGpuSpoolRun( allocationResources, spool.root / "allocation-resource-runs", "relation",
        allocationResourceRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.allocationId != rhs.allocationId ) return lhs.allocationId < rhs.allocationId;
            return lhs.resourceId < rhs.resourceId;
        }, error ) ) return false;
    if( !SaveGpuSpoolRun( lifetimes, spool.root / "lifetime-runs", "lifetime", lifetimeRuns,
        []( const auto& lhs, const auto& rhs ) {
            if( lhs.key != rhs.key ) return lhs.key < rhs.key;
            if( lhs.time != rhs.time ) return lhs.time < rhs.time;
            return lhs.resourceId < rhs.resourceId;
        }, error ) ) return false;
    uint64_t lifetimeCount = 0;
    spool.pointerLifetimePath = spool.root / "pointer-lifetimes.bin";
    if( !MergeGpuSpoolRunsToFile<ResourceLifetimeEntry>( lifetimeRuns,
        spool.pointerLifetimePath, []( const auto& lhs, const auto& rhs ) {
            if( lhs.key != rhs.key ) return lhs.key < rhs.key;
            if( lhs.time != rhs.time ) return lhs.time < rhs.time;
            return lhs.resourceId < rhs.resourceId;
        }, lifetimeCount, error ) ) return false;

    const auto allocationCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.record.allocationId != rhs.record.allocationId ) return lhs.record.allocationId < rhs.record.allocationId;
        if( lhs.record.time != rhs.record.time ) return lhs.record.time < rhs.record.time;
        return lhs.ordinal < rhs.ordinal;
    };
    GpuSpoolRunCursor<CatalogAllocationRawEntry, decltype( allocationCompare )> allocations( allocationCompare );
    if( !allocations.Open( state.allocationRuns, error ) ) return false;
    const auto relationCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.allocationId != rhs.allocationId ) return lhs.allocationId < rhs.allocationId;
        return lhs.resourceId < rhs.resourceId;
    };
    GpuSpoolRunCursor<CatalogAllocationResourceEntry, decltype( relationCompare )> relations( relationCompare );
    if( !relations.Open( allocationResourceRuns, error ) ) return false;
    std::ofstream allocationLookup( GpuAnalysisIoPath( spool.root / "allocation-lookup.bin" ), std::ios::binary | std::ios::trunc );
    if( !allocationLookup ) { error = "session_gpu_catalog_allocation_lookup_open_failed"; return false; }
    spool.allocationLookupPath = spool.root / "allocation-lookup.bin";
    spool.allocationResourceCountPath = spool.allocationLookupPath;
    std::vector<GpuAllocationAnalysisRecord> allocationPage;
    std::vector<CatalogAllocationDelta> deltas;
    std::vector<std::filesystem::path> deltaRuns;
    uint64_t allocationPageBytes = 0, allocationFirstIndex = 0;
    auto flushAllocationPage = [&]() {
        if( !WriteCatalogSpoolPage( spool, identity, GpuAnalysisStorePageKind::Allocation,
            "allocations", allocationPage, allocationFirstIndex, error ) ) return false;
        allocationFirstIndex = spool.allocationCount; allocationPageBytes = 0; return true;
    };
    CatalogAllocationRawEntry allocationRaw {};
    uint64_t currentAllocation = 0; GpuAllocationAnalysisRecord allocation {};
    int64_t physicalSize = 0;
    auto flushAllocation = [&]() -> bool {
        if( currentAllocation == 0 ) return true;
        while( !relations.Empty() && relations.Front().allocationId < currentAllocation )
        {
            CatalogAllocationResourceEntry ignored {};
            if( !relations.Pop( ignored, error ) ) return false;
        }
        while( !relations.Empty() && relations.Front().allocationId == currentAllocation )
        {
            CatalogAllocationResourceEntry value {};
            if( !relations.Pop( value, error ) ) return false;
            if( allocation.resources.empty() || allocation.resources.back() != value.resourceId )
                allocation.resources.push_back( value.resourceId );
        }
        const CatalogAllocationLookupEntry lookup { allocation.allocationId, allocation.sizeBytes,
            allocation.resources.size(), uint8_t( allocation.aliveAtEnd ? 1 : 0 ), {} };
        allocationLookup.write( reinterpret_cast<const char*>( &lookup ), sizeof( lookup ) );
        if( !allocationLookup ) { error = "session_gpu_catalog_allocation_lookup_write_failed"; return false; }
        if( allocation.aliveAtEnd && allocation.parentAllocationId == 0 )
        { spool.overview.engineKnownPhysicalBytes += allocation.sizeBytes; spool.overview.residentPhysicalBytes += allocation.residentBytes; }
        allocationPageBytes += sizeof( allocation ) + allocation.history.size() * sizeof( size_t ) +
            allocation.resources.size() * sizeof( uint64_t );
        allocationPage.push_back( std::move( allocation ) ); ++spool.allocationCount;
        if( allocationPageBytes >= resourcePageTarget && !flushAllocationPage() ) return false;
        allocation = {}; currentAllocation = 0; physicalSize = 0; return true;
    };
    while( !allocations.Empty() )
    {
        if( control.stopToken.stop_requested() ) { error = "cancelled"; return false; }
        if( !allocations.Pop( allocationRaw, error ) ) return false;
        const auto& value = allocationRaw.record;
        if( value.allocationId == 0 ) { ++spool.overview.manifest.invalidRecordCount; continue; }
        if( currentAllocation != 0 && currentAllocation != value.allocationId && !flushAllocation() ) return false;
        if( currentAllocation == 0 ) { currentAllocation = value.allocationId; allocation.allocationId = value.allocationId; }
        if( allocationRaw.ordinal <= uint64_t( std::numeric_limits<size_t>::max() ) )
            allocation.history.push_back( size_t( allocationRaw.ordinal ) );
        const auto operation = JnGpuCatalogRecordOperation( value.operation );
        if( operation == JnGpuCatalogRecordOperation::Create || operation == JnGpuCatalogRecordOperation::Open ||
            operation == JnGpuCatalogRecordOperation::Snapshot ) ++spool.overview.allocationCreateCount;
        else if( operation == JnGpuCatalogRecordOperation::Destroy ) ++spool.overview.allocationDestroyCount;
        if( value.parentAllocationId == 0 )
        {
            int64_t nextSize = physicalSize;
            if( operation == JnGpuCatalogRecordOperation::Destroy || operation == JnGpuCatalogRecordOperation::Close ) nextSize = 0;
            else if( IsCatalogDefinition( operation ) ) nextSize = int64_t( value.sizeBytes );
            if( nextSize != physicalSize ) deltas.push_back( { value.time, allocationRaw.ordinal, nextSize - physicalSize } );
            if( operation != JnGpuCatalogRecordOperation::Update && IsCatalogDefinition( operation ) )
                spool.overview.allocatedPhysicalBytes += value.sizeBytes;
            if( nextSize == 0 && physicalSize > 0 ) spool.overview.freedPhysicalBytes += uint64_t( physicalSize );
            physicalSize = nextSize;
            if( deltas.size() * sizeof( CatalogAllocationDelta ) >= CatalogRunTargetBytes( control ) &&
                !SaveGpuSpoolRun( deltas, spool.root / "allocation-delta-runs", "delta", deltaRuns,
                    []( const auto& lhs, const auto& rhs ) {
                        if( lhs.time != rhs.time ) return lhs.time < rhs.time;
                        return lhs.ordinal < rhs.ordinal;
                    }, error ) ) return false;
        }
        if( operation == JnGpuCatalogRecordOperation::Destroy || operation == JnGpuCatalogRecordOperation::Close )
        { allocation.destroyTime = std::max<uint64_t>( allocation.destroyTime, value.time < 0 ? 0 : uint64_t( value.time ) ); allocation.aliveAtEnd = false; continue; }
        if( !IsCatalogDefinition( operation ) ) continue;
        if( value.time >= int64_t( allocation.lastUpdateTime ) )
        {
            allocation.generation = allocationRaw.generation; allocation.heapId = value.heapId;
            allocation.parentAllocationId = value.parentAllocationId; allocation.sizeBytes = value.sizeBytes;
            allocation.offsetBytes = value.offsetBytes; allocation.residentBytes = value.residentBytes;
            allocation.alignmentBytes = value.alignmentBytes;
            allocation.lastUpdateTime = value.time < 0 ? 0 : uint64_t( value.time );
            allocation.primaryKind = value.primaryKind; allocation.memoryDomain = value.memoryDomain;
            allocation.allocationKind = value.allocationKind; allocation.residencyState = value.residencyState;
            allocation.exactness = value.exactness; allocation.flags = value.flags;
            allocation.invalid = value.exactness == uint8_t( JnGpuCatalogExactness::Invalid );
            allocation.openBoundary |= operation == JnGpuCatalogRecordOperation::Open ||
                operation == JnGpuCatalogRecordOperation::Snapshot ||
                value.exactness == uint8_t( JnGpuCatalogExactness::OpenBoundary );
            allocation.aliveAtEnd = true;
        }
        if( operation == JnGpuCatalogRecordOperation::Create && allocation.createTime == 0 )
            allocation.createTime = allocation.lastUpdateTime;
    }
    if( !flushAllocation() || !flushAllocationPage() ) return false;
    while( !relations.Empty() )
    {
        CatalogAllocationResourceEntry ignored {};
        if( !relations.Pop( ignored, error ) ) return false;
    }
    allocationLookup.flush(); allocationLookup.close();
    if( !allocationLookup ) { error = "session_gpu_catalog_allocation_lookup_flush_failed"; return false; }
    if( !SaveGpuSpoolRun( deltas, spool.root / "allocation-delta-runs", "delta", deltaRuns,
        []( const auto& lhs, const auto& rhs ) {
            if( lhs.time != rhs.time ) return lhs.time < rhs.time;
            return lhs.ordinal < rhs.ordinal;
        }, error ) ) return false;
    const auto deltaCompare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.time != rhs.time ) return lhs.time < rhs.time;
        return lhs.ordinal < rhs.ordinal;
    };
    GpuSpoolRunCursor<CatalogAllocationDelta, decltype( deltaCompare )> deltaCursor( deltaCompare );
    if( !deltaCursor.Open( deltaRuns, error ) ) return false;
    int64_t currentPhysical = 0; CatalogAllocationDelta delta {};
    while( !deltaCursor.Empty() )
    {
        if( !deltaCursor.Pop( delta, error ) ) return false;
        currentPhysical += delta.bytes;
        if( currentPhysical < 0 ) { error = "session_gpu_catalog_physical_underflow"; return false; }
        if( uint64_t( currentPhysical ) > spool.overview.engineKnownPhysicalPeakBytes )
        { spool.overview.engineKnownPhysicalPeakBytes = uint64_t( currentPhysical ); spool.overview.engineKnownPhysicalPeakTimeNs = delta.time; }
    }
    spool.typeSummaries.clear(); for( const auto& [_, value] : typeSummaries ) spool.typeSummaries.push_back( value );
    spool.overview.manifest.resourceRecordCount = spool.resourceCount;
    spool.overview.manifest.allocationRecordCount = spool.allocationCount;
    spool.overview.manifest.viewRecordCount = metadata.gpuCatalogViews.size();
    spool.overview.manifest.partRecordCount = metadata.gpuCatalogParts.size();
    spool.overview.manifest.vgRecordCount = metadata.gpuCatalogVg.size();
    if( !WriteCatalogSpoolPage( spool, identity, GpuAnalysisStorePageKind::Churn,
        "churn", churnPage, churnFirstIndex, error ) ) return false;
    return true;
}

struct PassReferenceToken
{
    uint64_t token = 0;
    int64_t time = 0;
};

struct PassFrameState
{
    uint64_t frameId = 0;
    uint64_t active = 0;
    std::vector<GpuPassWorkingSet> passes;
    // JnGpuReferenceEnd::totalReferenceCount is the number of raw resource
    // observations made by the producer.  ResourceSet/direct-use records hold
    // the per-pass unique set after producer-side deduplication.  Keep their
    // count separate from directResources because RangeSet evidence may also
    // add resources to the working set before the pass is committed.
    std::vector<uint64_t> encodedReferenceCounts;
    // The producer may publish a resource lifetime after PassBegin/Use but
    // before PassEnd (notably resource replacement on command-list setup).
    // N29 resolves reference tokens at the authoritative pass end timestamp,
    // so retain the compact tokens until End rather than resolving too early.
    std::vector<std::vector<PassReferenceToken>> referenceTokens;
    std::unordered_map<uint64_t, size_t> byId;
    std::unordered_map<uint64_t, GpuAnalysisPassTaxonomyEntry> taxonomyByPass;
};

struct PassSpoolBuildState
{
    const TraceSessionTimeTransform* transform = nullptr;
    const ResourceSetReader* resourceSets = nullptr;
    ResourceLifetimeResolver* resolver = nullptr;
    BoundedCatalogLookup* catalog = nullptr;
    const GpuAnalysisSnapshot* catalogOverview = nullptr;
    std::vector<GpuResourceAnalysisRecord>* appendedResources = nullptr;
    const RangeByPassReader* ranges = nullptr;
    const std::vector<PassEvidenceEntry>* evidence = nullptr;
    const GpuAnalysisTraceIdentity* identity = nullptr;
    const GpuAnalysisSidecarControl* control = nullptr;
    GpuAnalysisPassSpool* spool = nullptr;
    std::map<uint64_t, PassFrameState> frames;
    std::unordered_map<uint64_t, uint64_t> passFrame;
    std::unordered_map<uint64_t, uint64_t> pendingParents;
    std::unordered_map<uint64_t, std::vector<uint8_t>> catalogPayloads;
    std::vector<GpuPassWorkingSet> page;
    std::vector<GpuAnalysisRangeStoreEntry> rangeRun;
    std::vector<GpuAnalysisPassTaxonomyEntry> taxonomyRun;
    std::vector<GpuAnalysisStablePassSummary> stableSummaryRun;
    uint64_t pageBytes = 0;
    uint64_t maxFrame = 0;
    uint64_t emittedMaxPassId = 0;
    uint64_t useCount = 0;
};

bool FlushPassTaxonomyRun( PassSpoolBuildState& state, std::string& error )
{
    if( state.taxonomyRun.empty() ) return true;
    const auto before = state.taxonomyRun.size();
    if( !SaveGpuSpoolRun( state.taxonomyRun, state.spool->root / "taxonomy-runs",
        "taxonomy", state.spool->taxonomyRuns, []( const auto& lhs, const auto& rhs ) {
            return lhs.passId < rhs.passId;
        }, error ) ) return false;
    state.spool->taxonomyCount += before;
    return true;
}

bool FlushStableSummaryRun( PassSpoolBuildState& state, std::string& error )
{
    if( state.stableSummaryRun.empty() ) return true;
    const auto before = state.stableSummaryRun.size();
    if( !SaveGpuSpoolRun( state.stableSummaryRun,
        state.spool->root / "stable-summary-runs", "stable-summary",
        state.spool->stableSummaryRuns, []( const auto& lhs, const auto& rhs ) {
            if( lhs.frameId != rhs.frameId ) return lhs.frameId < rhs.frameId;
            return lhs.taxonomyId < rhs.taxonomyId;
        }, error ) ) return false;
    state.spool->stableSummaryCount += before;
    return true;
}

uint64_t ResolvePassResource( PassSpoolBuildState& state, uint64_t token,
    int64_t time, int64_t passBegin, int64_t passEnd,
    GpuPassWorkingSet& pass )
{
    auto resourceId = state.resolver->ResolveAtOrUniqueInterval(
        token, time, passBegin, passEnd );
    if( resourceId != 0 ) return resourceId;
    const auto gap = state.resolver->ResolveSourceGapBeforeFirstDefinition( token, time );
    if( !gap ) return 0;
    if( gap->created )
    {
        GpuResourceAnalysisRecord resource;
        resource.resourceId = gap->resourceId;
        resource.createTime = 0;
        resource.destroyTime = gap->endTime < 0 ? 0 : uint64_t( gap->endTime );
        resource.lastUpdateTime = resource.destroyTime;
        resource.exactness = uint8_t( JnGpuCatalogExactness::OpenBoundary );
        resource.openBoundary = true;
        resource.aliveAtEnd = gap->endTime < 0;
        resource.name = "Untracked GPU Resource";
        state.catalog->AddResource( resource );
        state.appendedResources->push_back( std::move( resource ) );
        ++state.spool->sourceGapResourceCount;
    }
    ++state.spool->sourceGapReferenceCount;
    pass.complete = false;
    pass.truncated = true;
    return gap->resourceId;
}

bool SaveRangeRun( PassSpoolBuildState& state, std::string& error )
{
    if( state.rangeRun.empty() ) return true;
    std::sort( state.rangeRun.begin(), state.rangeRun.end(), []( const auto& lhs, const auto& rhs ) {
        if( lhs.resourceId != rhs.resourceId ) return lhs.resourceId < rhs.resourceId;
        if( lhs.record.passInstanceId != rhs.record.passInstanceId )
            return lhs.record.passInstanceId < rhs.record.passInstanceId;
        if( lhs.record.offsetBytes != rhs.record.offsetBytes )
            return lhs.record.offsetBytes < rhs.record.offsetBytes;
        if( lhs.record.firstSubresource != rhs.record.firstSubresource )
            return lhs.record.firstSubresource < rhs.record.firstSubresource;
        return lhs.record.usageMask < rhs.record.usageMask;
    } );
    std::error_code ec; const auto root = state.spool->root / "range-runs";
    std::filesystem::create_directories( GpuAnalysisIoPath( root ), ec );
    if( ec ) { error = "session_gpu_range_run_directory_failed:" + ec.message(); return false; }
    std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' )
        << state.spool->rangeRuns.size() << ".bin";
    const auto path = root / name.str();
    std::ofstream out( GpuAnalysisIoPath( path ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_gpu_range_run_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( state.rangeRun.data() ),
        std::streamsize( state.rangeRun.size() * sizeof( GpuAnalysisRangeStoreEntry ) ) );
    out.flush(); if( !out ) { error = "session_gpu_range_run_write_failed"; return false; }
    state.spool->rangeCount += state.rangeRun.size();
    state.spool->rangeRuns.push_back( path ); state.rangeRun.clear();
    return true;
}

uint64_t PassBytes( const GpuPassWorkingSet& pass )
{
    return sizeof( pass ) + pass.name.size() +
        ( pass.directResources.size() + pass.inclusiveResources.size() ) * sizeof( uint64_t ) +
        pass.detailedEvidence.size() * sizeof( GpuDetailedEvidenceAnalysisRecord );
}

bool SavePassSpoolPage( PassSpoolBuildState& state, std::string& error )
{
    if( state.page.empty() ) return true;
    std::sort( state.page.begin(), state.page.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.passId < rhs.passId;
    } );
    if( state.emittedMaxPassId != 0 && state.page.front().passId <= state.emittedMaxPassId )
    { error = "session_gpu_pass_spool_global_order_invalid"; return false; }
    state.emittedMaxPassId = state.page.back().passId;
    GpuAnalysisSnapshot snapshot;
    snapshot.manifest = state.catalogOverview->manifest;
    snapshot.passes = std::move( state.page );
    std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' )
        << state.spool->pageCount << ".bin";
    const GpuAnalysisCacheIdentity spoolIdentity { state.identity->sha256,
        state.identity->fileSize, std::string( GpuAnalysisAlgorithmId ) +
            "-session-pass-spool1" };
    if( !SaveGpuAnalysisCache( state.spool->root / name.str(), spoolIdentity,
        snapshot, error ) ) return false;
    ++state.spool->pageCount; state.page.clear(); state.pageBytes = 0;
    return SaveRangeRun( state, error );
}

bool BuildStableFrameSummaries( PassSpoolBuildState& state,
    const PassFrameState& frame, std::string& error )
{
    struct Node
    {
        GpuAnalysisStablePassSummary summary;
        std::vector<uint64_t> direct;
        std::vector<uint64_t> inclusive;
        std::vector<uint32_t> children;
        bool seen = false;
    };
    std::map<uint32_t, Node> nodes;
    for( const auto& pass : frame.passes )
    {
        const auto metadata = frame.taxonomyByPass.find( pass.passId );
        if( metadata == frame.taxonomyByPass.end() )
        { error = "session_gpu_pass_taxonomy_missing:pass=" + std::to_string( pass.passId ); return false; }
        const auto taxonomyId = metadata->second.taxonomyId;
        if( taxonomyId == 0 ) continue;
        auto& node = nodes[taxonomyId];
        if( !node.seen )
        {
            node.seen = true;
            node.summary.frameId = frame.frameId;
            node.summary.taxonomyId = taxonomyId;
            node.summary.taxonomyLevel = metadata->second.taxonomyLevel;
            node.summary.complete = 1;
        }
        else if( node.summary.taxonomyLevel != metadata->second.taxonomyLevel )
        { error = "session_gpu_stable_taxonomy_level_conflict"; return false; }
        node.summary.complete = uint8_t( node.summary.complete && pass.complete );
        node.summary.truncated = uint8_t( node.summary.truncated || pass.truncated );
        node.direct.insert( node.direct.end(), pass.directResources.begin(),
            pass.directResources.end() );
        if( pass.parentPassId != 0 )
        {
            const auto parentMetadata = frame.taxonomyByPass.find( pass.parentPassId );
            // Stable Frame summaries deliberately do not absorb a relation
            // whose parent belongs to another frame.
            if( parentMetadata != frame.taxonomyByPass.end() &&
                parentMetadata->second.taxonomyId != taxonomyId )
            {
                const auto parentTaxonomyId = parentMetadata->second.taxonomyId;
                if( node.summary.parentTaxonomyId != 0 &&
                    node.summary.parentTaxonomyId != parentTaxonomyId )
                { error = "session_gpu_stable_taxonomy_parent_conflict"; return false; }
                node.summary.parentTaxonomyId = parentTaxonomyId;
            }
        }
    }
    for( auto& [taxonomyId, node] : nodes )
    {
        std::sort( node.direct.begin(), node.direct.end() );
        node.direct.erase( std::unique( node.direct.begin(), node.direct.end() ),
            node.direct.end() );
        if( node.summary.parentTaxonomyId != 0 )
        {
            const auto parent = nodes.find( node.summary.parentTaxonomyId );
            if( parent == nodes.end() )
            { error = "session_gpu_stable_taxonomy_parent_missing"; return false; }
            parent->second.children.push_back( taxonomyId );
        }
    }
    std::map<uint32_t, uint8_t> visit;
    const auto buildInclusive = [&]( auto&& self, uint32_t taxonomyId ) -> bool {
        auto& mark = visit[taxonomyId];
        if( mark == 2 ) return true;
        if( mark == 1 ) { error = "session_gpu_stable_taxonomy_cycle"; return false; }
        mark = 1;
        auto& node = nodes[taxonomyId];
        node.inclusive = node.direct;
        for( const auto childId : node.children )
        {
            if( !self( self, childId ) ) return false;
            std::vector<uint64_t> merged;
            const auto& child = nodes[childId];
            merged.reserve( node.inclusive.size() + child.inclusive.size() );
            std::set_union( node.inclusive.begin(), node.inclusive.end(),
                child.inclusive.begin(), child.inclusive.end(),
                std::back_inserter( merged ) );
            node.inclusive = std::move( merged );
            node.summary.complete = uint8_t( node.summary.complete && child.summary.complete );
            node.summary.truncated = uint8_t( node.summary.truncated || child.summary.truncated );
        }
        mark = 2;
        return true;
    };
    for( const auto& [taxonomyId, _] : nodes )
        if( !buildInclusive( buildInclusive, taxonomyId ) ) return false;
    for( auto& [_, node] : nodes )
    {
        node.summary.directResourceCount = node.direct.size();
        node.summary.inclusiveResourceCount = node.inclusive.size();
        node.summary.directResourceHash = GpuAnalysisResourceSetHash( node.direct );
        node.summary.inclusiveResourceHash = GpuAnalysisResourceSetHash( node.inclusive );
        node.summary.directPhysicalBytes = state.catalog->PhysicalBytes( node.direct );
        node.summary.inclusivePhysicalBytes = state.catalog->PhysicalBytes( node.inclusive );
        state.stableSummaryRun.push_back( node.summary );
    }
    return state.stableSummaryRun.size() * sizeof( GpuAnalysisStablePassSummary ) <
        64ull * 1024 * 1024 || FlushStableSummaryRun( state, error );
}

bool FinalizePassFrame( PassSpoolBuildState& state,
    std::map<uint64_t, PassFrameState>::iterator frameIt, bool captureEnd,
    std::string& error )
{
    auto& frame = frameIt->second;
    if( frame.active != 0 && !captureEnd ) return true;
    if( captureEnd ) for( auto& pass : frame.passes ) if( pass.endNs == 0 )
    { pass.endNs = pass.startNs; pass.complete = false; pass.truncated = true; }

    for( auto& pass : frame.passes )
    {
        std::sort( pass.directResources.begin(), pass.directResources.end() );
        pass.directResources.erase( std::unique( pass.directResources.begin(),
            pass.directResources.end() ), pass.directResources.end() );
        for( const auto& raw : state.ranges->Find( pass.passId ) )
        {
            auto range = raw.record;
            if( range.resourceId == 0 && range.pointerToken != 0 )
                range.resourceId = ResolvePassResource( state, range.pointerToken,
                    pass.startNs, pass.startNs, pass.endNs, pass );
            if( range.resourceId == 0 || !state.catalog->HasResource( range.resourceId ) )
            {
                error = "session_gpu_pass_range_resource_unresolved:pass=" +
                    std::to_string( pass.passId ) + ":token=" +
                    std::to_string( range.pointerToken ) + ":pass_begin=" +
                    std::to_string( pass.startNs ) + ":pass_end=" +
                    std::to_string( pass.endNs ) + ":lifetime=" +
                    state.resolver->Describe( range.pointerToken, pass.startNs );
                return false;
            }
            range.pointerToken = 0;
            pass.directRangeBytes += range.lengthBytes;
            pass.directResources.push_back( range.resourceId );
            state.rangeRun.push_back( { range.resourceId, raw.generation, range } );
            if( state.rangeRun.size() * sizeof( GpuAnalysisRangeStoreEntry ) >= 64ull * 1024 * 1024 &&
                !SaveRangeRun( state, error ) ) return false;
        }
        std::sort( pass.directResources.begin(), pass.directResources.end() );
        pass.directResources.erase( std::unique( pass.directResources.begin(),
            pass.directResources.end() ), pass.directResources.end() );
        const auto evidenceBegin = std::lower_bound( state.evidence->begin(), state.evidence->end(), pass.passId,
            []( const auto& value, uint64_t id ) { return value.passId < id; } );
        for( auto it = evidenceBegin; it != state.evidence->end() && it->passId == pass.passId; ++it )
            pass.detailedEvidence.push_back( it->evidence );
        pass.directPhysicalBytes = state.catalog->PhysicalBytes( pass.directResources );
    }
    if( !BuildStableFrameSummaries( state, frame, error ) ) return false;
    std::sort( frame.passes.begin(), frame.passes.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.passId < rhs.passId;
    } );
    for( auto& pass : frame.passes )
    {
        const auto taxonomy = frame.taxonomyByPass.find( pass.passId );
        if( taxonomy == frame.taxonomyByPass.end() )
        { error = "session_gpu_pass_taxonomy_missing:pass=" + std::to_string( pass.passId ); return false; }
        state.taxonomyRun.push_back( taxonomy->second );
        state.spool->passCount++;
        state.spool->directRelationCount += pass.directResources.size();
        state.pageBytes += PassBytes( pass ); state.page.push_back( std::move( pass ) );
    }
    if( state.taxonomyRun.size() * sizeof( GpuAnalysisPassTaxonomyEntry ) >=
        64ull * 1024 * 1024 && !FlushPassTaxonomyRun( state, error ) ) return false;
    for( const auto& [passId, _] : frame.byId )
    { state.passFrame.erase( passId ); state.pendingParents.erase( passId ); }
    state.frames.erase( frameIt );
    const auto target = std::min<uint64_t>( state.control->targetDerivedPageBytes,
        64ull * 1024 * 1024 );
    return state.pageBytes < std::max<uint64_t>( target, 1 ) || SavePassSpoolPage( state, error );
}

bool FlushPassFrames( PassSpoolBuildState& state, bool captureEnd, std::string& error )
{
    while( !state.frames.empty() )
    {
        auto it = state.frames.begin();
        if( !captureEnd && ( it->first + 2 >= state.maxFrame || it->second.active != 0 ) ) break;
        if( !FinalizePassFrame( state, it, captureEnd, error ) ) return false;
    }
    return captureEnd ? SavePassSpoolPage( state, error ) : true;
}

bool MergePassResourceRuns( const std::vector<std::filesystem::path>& runs,
    const std::filesystem::path& outputPath, const GpuAnalysisSidecarControl& control,
    uint64_t& written, std::string& error )
{
    written = 0;
    std::ofstream out( GpuAnalysisIoPath( outputPath ), std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_gpu_pass_inclusive_open_failed"; return false; }
    struct Cursor { std::ifstream in; GpuPassResourceEntry value {}; };
    struct Node { GpuPassResourceEntry value {}; size_t run = 0; };
    const auto compare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.passId != rhs.passId ) return lhs.passId < rhs.passId;
        return lhs.resourceId < rhs.resourceId;
    };
    struct Later { decltype( compare ) compare; bool operator()( const Node& lhs,
        const Node& rhs ) const { return compare( rhs.value, lhs.value ); } };
    std::vector<Cursor> cursors( runs.size() );
    std::priority_queue<Node, std::vector<Node>, Later> heap( Later { compare } );
    for( size_t i = 0; i < runs.size(); ++i )
    {
        cursors[i].in.open( GpuAnalysisIoPath( runs[i] ), std::ios::binary );
        if( !cursors[i].in ) { error = "session_gpu_pass_inclusive_run_open_failed"; return false; }
        if( cursors[i].in.read( reinterpret_cast<char*>( &cursors[i].value ),
                sizeof( GpuPassResourceEntry ) ) ) heap.push( { cursors[i].value, i } );
    }
    GpuPassResourceEntry previous {};
    bool hasPrevious = false;
    while( !heap.empty() )
    {
        const auto node = heap.top(); heap.pop();
        if( ( written & 0xffff ) == 0 && control.stopToken.stop_requested() )
        { error = "cancelled"; return false; }
        if( !hasPrevious || node.value.passId != previous.passId ||
            node.value.resourceId != previous.resourceId )
        {
            out.write( reinterpret_cast<const char*>( &node.value ), sizeof( node.value ) );
            if( !out ) { error = "session_gpu_pass_inclusive_write_failed"; return false; }
            previous = node.value; hasPrevious = true; ++written;
        }
        auto& cursor = cursors[node.run];
        if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ),
                sizeof( GpuPassResourceEntry ) ) ) heap.push( { cursor.value, node.run } );
        else if( !cursor.in.eof() )
        { error = "session_gpu_pass_inclusive_run_read_failed"; return false; }
    }
    out.flush(); out.close();
    if( !out ) { error = "session_gpu_pass_inclusive_flush_failed"; return false; }
    return true;
}

bool BuildGlobalPassInclusiveRollup( PassSpoolBuildState& state, std::string& error )
{
    const GpuAnalysisCacheIdentity spoolIdentity { state.identity->sha256,
        state.identity->fileSize, std::string( GpuAnalysisAlgorithmId ) +
            "-session-pass-spool1" };
    const auto parentPath = state.spool->root / "pass-parents.bin";
    std::ofstream parentOut( GpuAnalysisIoPath( parentPath ), std::ios::binary | std::ios::trunc );
    if( !parentOut ) { error = "session_gpu_pass_parent_file_open_failed"; return false; }
    uint64_t previousPassId = 0;
    for( uint64_t pageIndex = 0; pageIndex < state.spool->pageCount; ++pageIndex )
    {
        std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << pageIndex << ".bin";
        auto snapshot = LoadGpuAnalysisCache( state.spool->root / name.str(), spoolIdentity, error );
        if( !snapshot ) { error = "session_gpu_pass_rollup_page_invalid:" + error; return false; }
        for( const auto& pass : snapshot->passes )
        {
            if( pass.passId == 0 || pass.passId <= previousPassId )
            { error = "session_gpu_pass_rollup_order_invalid"; return false; }
            const GpuPassParentEntry entry { pass.passId, pass.parentPassId };
            parentOut.write( reinterpret_cast<const char*>( &entry ), sizeof( entry ) );
            previousPassId = pass.passId;
        }
    }
    parentOut.flush(); parentOut.close();
    if( !parentOut ) { error = "session_gpu_pass_parent_file_write_failed"; return false; }

    PassParentReader parents;
    if( !parents.Open( parentPath, error ) ) return false;
    if( parents.Count() != state.spool->passCount )
    { error = "session_gpu_pass_parent_count_mismatch"; return false; }

    const auto runRoot = state.spool->root / "inclusive-runs";
    std::vector<GpuPassResourceEntry> values;
    std::vector<std::filesystem::path> runs;
    constexpr uint64_t RunBytes = 64ull * 1024 * 1024;
    const auto compare = []( const auto& lhs, const auto& rhs ) {
        if( lhs.passId != rhs.passId ) return lhs.passId < rhs.passId;
        return lhs.resourceId < rhs.resourceId;
    };
    for( uint64_t pageIndex = 0; pageIndex < state.spool->pageCount; ++pageIndex )
    {
        if( state.control->stopToken.stop_requested() ) { error = "cancelled"; return false; }
        std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << pageIndex << ".bin";
        auto snapshot = LoadGpuAnalysisCache( state.spool->root / name.str(), spoolIdentity, error );
        if( !snapshot ) { error = "session_gpu_pass_rollup_page_invalid:" + error; return false; }
        for( const auto& pass : snapshot->passes )
        {
            std::vector<uint64_t> ancestors;
            ancestors.reserve( 65 );
            uint64_t current = pass.passId;
            while( current != 0 )
            {
                if( ancestors.size() >= 65 )
                { error = "session_gpu_pass_parent_depth_exceeded:pass=" + std::to_string( pass.passId ); return false; }
                const auto* entry = parents.Find( current );
                if( !entry )
                { error = "session_gpu_pass_parent_missing:pass=" + std::to_string( pass.passId ) +
                    ":ancestor=" + std::to_string( current ); return false; }
                ancestors.push_back( current );
                if( entry->parentPassId != 0 && entry->parentPassId >= current )
                { error = "session_gpu_pass_parent_order_invalid:child=" + std::to_string( current ) +
                    ":parent=" + std::to_string( entry->parentPassId ); return false; }
                current = entry->parentPassId;
            }
            for( const auto resourceId : pass.directResources )
            {
                if( resourceId == 0 ) { error = "session_gpu_pass_direct_resource_zero"; return false; }
                for( const auto ancestor : ancestors ) values.push_back( { ancestor, resourceId } );
                if( values.size() * sizeof( GpuPassResourceEntry ) >= RunBytes &&
                    !SaveGpuSpoolRun( values, runRoot, "inclusive", runs, compare, error ) ) return false;
            }
        }
    }
    if( !SaveGpuSpoolRun( values, runRoot, "inclusive", runs, compare, error ) ) return false;
    const auto inclusivePath = state.spool->root / "inclusive-by-pass.bin";
    uint64_t inclusiveCount = 0;
    if( !MergePassResourceRuns( runs, inclusivePath, *state.control, inclusiveCount, error ) ) return false;

    PassResourceReader inclusive;
    if( !inclusive.Open( inclusivePath, error ) || inclusive.Count() != inclusiveCount ) return false;
    state.spool->inclusiveRelationCount = inclusiveCount;
    for( uint64_t pageIndex = 0; pageIndex < state.spool->pageCount; ++pageIndex )
    {
        if( state.control->stopToken.stop_requested() ) { error = "cancelled"; return false; }
        std::ostringstream name; name << std::setw( 6 ) << std::setfill( '0' ) << pageIndex << ".bin";
        const auto pagePath = state.spool->root / name.str();
        auto snapshot = LoadGpuAnalysisCache( pagePath, spoolIdentity, error );
        if( !snapshot ) { error = "session_gpu_pass_rollup_page_invalid:" + error; return false; }
        for( auto& pass : snapshot->passes )
        {
            const auto entries = inclusive.Find( pass.passId );
            pass.inclusiveResources.clear(); pass.inclusiveResources.reserve( entries.size() );
            for( const auto& entry : entries ) pass.inclusiveResources.push_back( entry.resourceId );
            if( !std::includes( pass.inclusiveResources.begin(), pass.inclusiveResources.end(),
                    pass.directResources.begin(), pass.directResources.end() ) )
            { error = "session_gpu_pass_inclusive_missing_direct:pass=" + std::to_string( pass.passId ); return false; }
            if( std::any_of( pass.inclusiveResources.begin(), pass.inclusiveResources.end(),
                    [&]( uint64_t resourceId ) { return state.resolver->IsSourceGapResource( resourceId ); } ) )
            {
                pass.complete = false;
                pass.truncated = true;
            }
            pass.inclusivePhysicalBytes = state.catalog->PhysicalBytes( pass.inclusiveResources );
        }
        const auto temporary = pagePath.string() + ".rollup.tmp";
        if( !SaveGpuAnalysisCache( temporary, spoolIdentity, *snapshot, error ) ||
            !AtomicReplace( temporary, pagePath, error ) ) return false;
    }
    for( const auto& path : runs ) { std::error_code ec; std::filesystem::remove( GpuAnalysisIoPath( path ), ec ); }
    std::error_code ec;
    std::filesystem::remove( GpuAnalysisIoPath( runRoot ), ec );
    std::filesystem::remove( GpuAnalysisIoPath( parentPath ), ec );
    std::filesystem::remove( GpuAnalysisIoPath( inclusivePath ), ec );
    return true;
}

bool ProcessPassCatalogBatch( const QueueJnGpuCatalogBatch& event,
    PassSpoolBuildState& state, std::string& error )
{
    const auto payloadIt = state.catalogPayloads.find( event.payloadId );
    if( payloadIt == state.catalogPayloads.end() )
    { error = "session_gpu_pass_catalog_payload_missing"; return false; }
    auto payload = std::move( payloadIt->second ); state.catalogPayloads.erase( payloadIt );
    if( payload.size() != event.payloadBytes || payload.size() < sizeof( JnGpuCatalogBatchEnvelopeV1 ) )
    { error = "session_gpu_pass_catalog_payload_size_mismatch"; return false; }
    JnGpuCatalogBatchEnvelopeV1 envelope {};
    std::memcpy( &envelope, payload.data(), sizeof( envelope ) );
    const auto* records = payload.data() + sizeof( envelope );
    const auto recordBytes = payload.size() - sizeof( envelope );
    if( envelope.magic != JnGpuCatalogBatchMagic ||
        envelope.catalogSchema != JnGpuCatalogSchemaVersion ||
        envelope.evidenceSchema != JnGpuDetailedEvidenceSchemaVersion ||
        envelope.recordCount != event.recordCount || envelope.payloadBytes != recordBytes ||
        JnGpuCatalogChecksum64( records, recordBytes ) != envelope.checksum )
    { error = "session_gpu_pass_catalog_envelope_invalid"; return false; }
    const auto kind = JnGpuCatalogBatchKind( event.kind );
    if( kind == JnGpuCatalogBatchKind::RangeSet )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::RangeSetV1 ) ||
            envelope.recordBytes != sizeof( JnGpuRangeSetRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuRangeSetRecordV1 ) != recordBytes )
        { error = "session_gpu_pass_range_batch_invalid"; return false; }
        // Range batches are intentionally decoupled from protocol arrival
        // order.  The first bounded scan writes them to ranges-by-pass.bin;
        // FinalizePassFrame joins them by pass id and resolves at PassEnd.
    }
    else if( kind == JnGpuCatalogBatchKind::DetailedEvidence )
    {
        if( event.encoding != uint8_t( JnGpuCatalogBatchEncoding::DetailedEvidenceV1 ) ||
            envelope.recordBytes != sizeof( JnGpuDetailedEvidenceRecordV1 ) ||
            uint64_t( event.recordCount ) * sizeof( JnGpuDetailedEvidenceRecordV1 ) != recordBytes )
        { error = "session_gpu_pass_evidence_batch_invalid"; return false; }
        for( uint32_t i = 0; i < event.recordCount; ++i )
        {
            JnGpuDetailedEvidenceRecordV1 value {};
            std::memcpy( &value, records + size_t( i ) * sizeof( value ), sizeof( value ) );
            value.time = state.transform->ToNanoseconds( value.time );
            auto frameId = state.passFrame.find( value.sourceId ); uint64_t passId = value.sourceId;
            if( frameId == state.passFrame.end() )
            { frameId = state.passFrame.find( value.targetId ); passId = value.targetId; }
            if( frameId == state.passFrame.end() )
            { error = "session_gpu_detailed_evidence_pass_unresolved"; return false; }
            auto& frame = state.frames[frameId->second];
            frame.passes[frame.byId[passId]].detailedEvidence.push_back(
                { event.generation, value } );
        }
    }
    return true;
}

bool VisitGpuPassSpool( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<PassSpoolBuildState*>( userData );
    if( state.control->stopToken.stop_requested() ) { error = "cancelled"; return false; }
    QueueItem item {}; if( !DecodeItem( record, item, error ) ) return false;
    switch( item.hdr.type )
    {
    case QueueType::JnGpuCatalogBatchData:
    {
        uint64_t payloadId = 0; const uint8_t* bytes = nullptr; uint32_t size = 0;
        if( !DecodeLargePayload( record, item, payloadId, bytes, size, error ) ) return false;
        if( payloadId == 0 || size < sizeof( JnGpuCatalogBatchEnvelopeV1 ) ||
            size > 64u * 1024 * 1024 || !state.catalogPayloads.emplace(
                payloadId, std::vector<uint8_t>( bytes, bytes + size ) ).second )
        { error = "session_gpu_pass_catalog_payload_invalid"; return false; }
        return true;
    }
    case QueueType::JnGpuCatalogBatch:
        return ProcessPassCatalogBatch( item.jnGpuCatalogBatch, state, error );
    case QueueType::JnGpuReferencePass:
    {
        const auto& event = item.jnGpuReferencePass;
        if( event.passId == 0 || state.passFrame.contains( event.passId ) ||
            event.passId <= state.emittedMaxPassId )
        { error = "session_gpu_pass_begin_invalid"; return false; }
        state.maxFrame = std::max( state.maxFrame, event.frameIndex );
        auto& frame = state.frames[event.frameIndex]; frame.frameId = event.frameIndex;
        GpuPassWorkingSet pass; pass.passId = event.passId; pass.frameId = event.frameIndex;
        pass.startNs = state.transform->ToNanoseconds( event.time );
        pass.name = "GPU Pass #" + std::to_string( event.taxonomyId );
        if( const auto parent = state.pendingParents.find( event.passId );
            parent != state.pendingParents.end() ) pass.parentPassId = parent->second;
        frame.byId.emplace( event.passId, frame.passes.size() );
        frame.passes.push_back( std::move( pass ) );
        frame.taxonomyByPass.emplace( event.passId, GpuAnalysisPassTaxonomyEntry {
            event.passId, event.frameIndex, event.taxonomyId,
            event.taxonomyLevel, event.flags, {} } );
        frame.encodedReferenceCounts.push_back( 0 );
        frame.referenceTokens.emplace_back();
        ++frame.active;
        state.passFrame.emplace( event.passId, event.frameIndex );
        return FlushPassFrames( state, false, error );
    }
    case QueueType::JnRelation:
    {
        const auto& relation = item.jnRelation;
        if( relation.relationNamespace != uint8_t( JnRelationNamespace::GpuReference ) ||
            relation.relation != uint8_t( JnRelationKind::LogicalParent ) ||
            relation.sourceKind != uint8_t( JnEntityKind::GpuPass ) ||
            relation.targetKind != uint8_t( JnEntityKind::GpuPass ) ) return true;
        const auto frameId = state.passFrame.find( relation.sourceId );
        if( frameId != state.passFrame.end() )
        {
            auto& frame = state.frames[frameId->second];
            frame.passes[frame.byId[relation.sourceId]].parentPassId = relation.targetId;
        }
        else
        {
            if( relation.sourceId <= state.emittedMaxPassId )
            { error = "session_gpu_pass_parent_late_after_commit"; return false; }
            state.pendingParents[relation.sourceId] = relation.targetId;
        }
        return true;
    }
    case QueueType::JnGpuReferenceUse:
    {
        const auto& event = item.jnGpuReferenceUse;
        const auto frameId = state.passFrame.find( event.passId );
        if( frameId == state.passFrame.end() )
        { error = "session_gpu_pass_use_without_begin"; return false; }
        auto& frame = state.frames[frameId->second];
        const auto passIndex = frame.byId[event.passId];
        frame.referenceTokens[passIndex].push_back( {
            event.resourceId, state.transform->ToNanoseconds( event.time ) } );
        ++frame.encodedReferenceCounts[passIndex];
        ++state.useCount; return true;
    }
    case QueueType::JnGpuReferenceSetUse:
    {
        const auto& event = item.jnGpuReferenceSetUse;
        const auto frameId = state.passFrame.find( event.passId );
        if( frameId == state.passFrame.end() )
        { error = "session_gpu_pass_set_use_without_begin"; return false; }
        const auto entries = state.resourceSets->Find( event.resourceSetId );
        if( event.encoding != 2 || entries.empty() || entries.size() != event.entryCount )
        { error = "session_gpu_pass_resource_set_invalid"; return false; }
        auto& frame = state.frames[frameId->second];
        const auto passIndex = frame.byId[event.passId];
        for( const auto& entry : entries )
        {
            frame.referenceTokens[passIndex].push_back( {
                entry.resourceId, frame.passes[passIndex].startNs } );
            ++state.useCount;
        }
        frame.encodedReferenceCounts[passIndex] += entries.size();
        return true;
    }
    case QueueType::JnGpuReferenceEnd:
    {
        const auto& event = item.jnGpuReferenceEnd;
        const auto frameId = state.passFrame.find( event.passId );
        if( frameId == state.passFrame.end() )
        { error = "session_gpu_pass_end_without_begin"; return false; }
        auto& frame = state.frames[frameId->second];
        const auto passIndex = frame.byId[event.passId];
        auto& pass = frame.passes[passIndex];
        if( pass.endNs != 0 ) { error = "session_gpu_pass_duplicate_end"; return false; }
        pass.endNs = state.transform->ToNanoseconds( event.time );
        pass.commandListId = event.commandListId;
        pass.complete = event.droppedReferenceCount == 0;
        pass.truncated = event.droppedReferenceCount != 0;
        for( const auto& reference : frame.referenceTokens[passIndex] )
        {
            const auto resourceId = ResolvePassResource( state, reference.token,
                reference.time, pass.startNs, pass.endNs, pass );
            if( resourceId == 0 )
            {
                error = "session_gpu_pass_resource_unresolved:pass=" +
                    std::to_string( event.passId ) + ":token=" +
                    std::to_string( reference.token ) + ":use_time=" +
                    std::to_string( reference.time ) + ":pass_begin=" +
                    std::to_string( pass.startNs ) + ":pass_end=" +
                    std::to_string( pass.endNs ) + ":lifetime=" +
                    state.resolver->Describe( reference.token, reference.time );
                return false;
            }
            pass.directResources.push_back( resourceId );
        }
        frame.referenceTokens[passIndex].clear();
        const auto encodedReferenceCount = frame.encodedReferenceCounts[passIndex];
        if( event.totalReferenceCount < encodedReferenceCount )
        {
            error = "session_gpu_pass_reference_count_invalid:pass=" +
                std::to_string( event.passId ) + ":observed=" +
                std::to_string( event.totalReferenceCount ) + ":encoded_unique=" +
                std::to_string( encodedReferenceCount ) + ":dropped=" +
                std::to_string( event.droppedReferenceCount );
            return false;
        }
        if( frame.active == 0 ) { error = "session_gpu_pass_active_underflow"; return false; }
        --frame.active; return true;
    }
    default: return true;
    }
}

bool BuildGpuPassSpool( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionTimeTransform& transform,
    ResourceLifetimeResolver& resolver, BoundedCatalogLookup& catalog,
    const GpuAnalysisSnapshot& catalogOverview,
    std::vector<GpuResourceAnalysisRecord>& appendedResources,
    const std::vector<PassEvidenceEntry>& evidence,
    const GpuAnalysisTraceIdentity& identity,
    const GpuAnalysisSidecarControl& control, GpuAnalysisPassSpool& spool,
    std::string& error )
{
    error.clear(); spool = {};
    // Keep transient runs close to the Session root.  The public derived path
    // is intentionally descriptive and can already approach MAX_PATH before
    // a generation, page and temporary suffix are appended.  Spool data is
    // not part of the published schema, so a short checkpoint-local path is
    // both portable and recoverable under the Session writer lease.
    spool.root = sessionRoot / "checkpoints" / "gpu-pass-spool";
    std::error_code ec;
    std::filesystem::remove_all( GpuAnalysisIoPath( spool.root ), ec ); ec.clear();
    std::filesystem::create_directories( GpuAnalysisIoPath( spool.root ), ec );
    if( ec ) { error = "session_gpu_pass_spool_directory_failed:" + ec.message(); return false; }
    const auto setIndexPath = spool.root / "resource-sets.index";
    const auto setEntriesPath = spool.root / "resource-sets.entries";
    std::ofstream index( GpuAnalysisIoPath( setIndexPath ), std::ios::binary | std::ios::trunc );
    std::ofstream entries( GpuAnalysisIoPath( setEntriesPath ), std::ios::binary | std::ios::trunc );
    if( !index || !entries ) { error = "session_gpu_resource_set_spool_open_failed"; return false; }
    ResourceSetBuildState setState { &index, &entries };
    EnrichmentSpoolBuildState enrichment;
    enrichment.transform = &transform; enrichment.resolver = &resolver;
    enrichment.catalog = &catalog; enrichment.resourceSets = &setState;
    enrichment.spool = &spool; enrichment.control = &control;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest,
        VisitGpuEnrichmentSpool, &enrichment, error ) ) return false;
    if( !enrichment.catalogPayloads.empty() )
    { error = "session_gpu_enrichment_payload_unresolved"; return false; }
    if( enrichment.coreUnresolved != 0 )
    { error = "session_gpu_catalog_core_relation_unresolved:" +
        std::to_string( enrichment.coreUnresolved ); return false; }
    if( !FlushViewRun( enrichment, error ) || !FlushPartRun( enrichment, error ) ||
        !FlushVirtualGeometryRun( enrichment, error ) ||
        !NormalizeLogicalRuns( enrichment, error ) || !NormalizeRangeRuns( enrichment, error ) ) return false;
    if( !resolver.AddLogicalFile( spool.root / "logical-lifetimes.bin", error ) ) return false;
    index.flush(); entries.flush();
    if( !index || !entries ) { error = "session_gpu_resource_set_spool_flush_failed"; return false; }
    index.close(); entries.close();
    ResourceSetReader setReader;
    if( !setReader.Open( setIndexPath, setEntriesPath, error ) ) return false;
    RangeByPassReader rangeReader;
    if( !rangeReader.Open( spool.root / "ranges-by-pass.bin", error ) ) return false;
    PassSpoolBuildState state;
    state.transform = &transform; state.resourceSets = &setReader;
    state.resolver = &resolver; state.catalog = &catalog;
    state.catalogOverview = &catalogOverview; state.appendedResources = &appendedResources;
    state.ranges = &rangeReader;
    state.evidence = &evidence; state.identity = &identity; state.control = &control;
    state.spool = &spool;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest,
        VisitGpuPassSpool, &state, error ) ) return false;
    if( !state.catalogPayloads.empty() )
    { error = "session_gpu_pass_catalog_payload_unresolved"; return false; }
    if( !FlushPassFrames( state, true, error ) ) return false;
    if( !FlushPassTaxonomyRun( state, error ) ||
        !FlushStableSummaryRun( state, error ) ) return false;
    if( !state.pendingParents.empty() )
    { error = "session_gpu_pass_parent_without_pass"; return false; }
    if( !BuildGlobalPassInclusiveRollup( state, error ) ) return false;
    if( spool.rangeCount != rangeReader.Count() )
    {
        error = "session_gpu_pass_range_count_mismatch:source=" +
            std::to_string( rangeReader.Count() ) + ":joined=" +
            std::to_string( spool.rangeCount );
        return false;
    }
    std::filesystem::remove( GpuAnalysisIoPath( setIndexPath ), ec );
    std::filesystem::remove( GpuAnalysisIoPath( setEntriesPath ), ec );
    return true;
}

}

struct GpuAnalysisCatalogStringReader::Impl
{
    BoundedCatalogStringLookup reader;
};

struct GpuAnalysisCatalogAllocationLookupReader::Impl
{
    ReadOnlyMappedFile data;
    uint64_t count = 0;
};

GpuAnalysisCatalogStringReader::GpuAnalysisCatalogStringReader() : m_impl( std::make_unique<Impl>() ) {}
GpuAnalysisCatalogStringReader::~GpuAnalysisCatalogStringReader() = default;
GpuAnalysisCatalogStringReader::GpuAnalysisCatalogStringReader( GpuAnalysisCatalogStringReader&& ) noexcept = default;
GpuAnalysisCatalogStringReader& GpuAnalysisCatalogStringReader::operator=( GpuAnalysisCatalogStringReader&& ) noexcept = default;
bool GpuAnalysisCatalogStringReader::Open( const GpuAnalysisCatalogSpool& spool, std::string& error )
{ return m_impl->reader.Open( spool, error ); }
std::string GpuAnalysisCatalogStringReader::Find( uint64_t generation, uint32_t stringId ) const
{ return m_impl->reader.Find( generation, stringId ); }

GpuAnalysisCatalogAllocationLookupReader::GpuAnalysisCatalogAllocationLookupReader()
    : m_impl( std::make_unique<Impl>() ) {}
GpuAnalysisCatalogAllocationLookupReader::~GpuAnalysisCatalogAllocationLookupReader() = default;
GpuAnalysisCatalogAllocationLookupReader::GpuAnalysisCatalogAllocationLookupReader(
    GpuAnalysisCatalogAllocationLookupReader&& ) noexcept = default;
GpuAnalysisCatalogAllocationLookupReader& GpuAnalysisCatalogAllocationLookupReader::operator=(
    GpuAnalysisCatalogAllocationLookupReader&& ) noexcept = default;
bool GpuAnalysisCatalogAllocationLookupReader::Open(
    const GpuAnalysisCatalogSpool& spool, std::string& error )
{
    if( !m_impl ) m_impl = std::make_unique<Impl>();
    if( !m_impl->data.Open( spool.allocationResourceCountPath, error ) ) return false;
    if( m_impl->data.Size() % sizeof( CatalogAllocationLookupEntry ) != 0 )
    { error = "session_gpu_catalog_allocation_lookup_size_invalid"; return false; }
    m_impl->count = m_impl->data.Size() / sizeof( CatalogAllocationLookupEntry ); return true;
}
uint64_t GpuAnalysisCatalogAllocationLookupReader::ResourceCount( uint64_t allocationId ) const
{
    if( !m_impl || allocationId == 0 ) return 0;
    const auto values = std::span<const CatalogAllocationLookupEntry>(
        reinterpret_cast<const CatalogAllocationLookupEntry*>( m_impl->data.Data() ),
        size_t( m_impl->count ) );
    const auto found = std::lower_bound( values.begin(), values.end(), allocationId,
        []( const auto& value, uint64_t key ) { return value.allocationId < key; } );
    return found == values.end() || found->allocationId != allocationId ? 0 : found->resourceCount;
}

int64_t TraceSessionTimeTransform::ToNanoseconds( int64_t value ) const
{
    return present ? int64_t( double( value - baseTime ) * timerMultiplier ) : value;
}

std::filesystem::path TraceSessionTimeTransformRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "global" /
        "time-transform" / "1" / "exact";
}

bool AuditTraceSessionTimeTransformDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionTimeTransform& timeTransform,
    std::string& error )
{
    error.clear(); timeTransform = {};
    const auto root = TraceSessionTimeTransformRoot( sessionRoot, session );
    TimeTransformManifest manifest;
    if( !LoadTimeTransformManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_time_transform_identity_mismatch"; return false; }
    const auto path = root / "time-transform.bin"; std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_time_transform_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_time_transform_file_sha256_mismatch"; return false; }
    std::ifstream in( path, std::ios::binary ); TimeTransformFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != TimeTransformFileMagic || header.schema != TimeTransformSchema ||
        header.endian != 0x01020304 || header.sourceSize != session.source.fileSize ||
        header.welcomeCount != manifest.welcomeCount || header.generationBytes != session.generation.size() ||
        !std::isfinite( header.timerMultiplier ) || header.timerMultiplier <= 0 )
    { error = "session_time_transform_file_header_invalid"; return false; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation ||
        in.peek() != std::ifstream::traits_type::eof() )
    { error = "session_time_transform_file_identity_invalid"; return false; }
    timeTransform.timerMultiplier = header.timerMultiplier;
    timeTransform.baseTime = header.baseTime;
    timeTransform.processId = header.processId;
    timeTransform.present = true;
    return true;
}

bool BuildTraceSessionTimeTransformDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionTimeTransform& timeTransform,
    std::string& error )
{
    std::string reuseError;
    if( AuditTraceSessionTimeTransformDerived( sessionRoot, manifest, timeTransform, reuseError ) )
    { error.clear(); return true; }
    error.clear(); timeTransform = {}; MetadataState metadata { &timeTransform };
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard, VisitMetadata, &metadata, error ) ) return false;
    }
    if( !timeTransform.present ) { error = "session_welcome_time_transform_missing"; return false; }
    const auto root = TraceSessionTimeTransformRoot( sessionRoot, manifest ); std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_time_transform_directory_failed:" + ec.message(); return false; }
    const auto target = root / "time-transform.bin"; auto temporary = target; temporary += ".tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_time_transform_file_open_failed"; return false; }
    TimeTransformFileHeader header; header.sourceSize = manifest.source.fileSize;
    header.baseTime = timeTransform.baseTime; header.timerMultiplier = timeTransform.timerMultiplier;
    header.processId = timeTransform.processId;
    header.welcomeCount = metadata.welcomeCount; header.generationBytes = uint32_t( manifest.generation.size() );
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( manifest.source.sha256.data(), std::streamsize( manifest.source.sha256.size() ) );
    out.write( manifest.generation.data(), std::streamsize( manifest.generation.size() ) );
    out.flush(); if( !out ) { error = "session_time_transform_file_write_failed"; return false; }
    out.close(); if( !AtomicReplace( temporary, target, error ) ) return false;
    TimeTransformManifest fileManifest; fileManifest.sourceSha256 = manifest.source.sha256;
    fileManifest.sourceSize = manifest.source.fileSize; fileManifest.generation = manifest.generation;
    fileManifest.fileBytes = std::filesystem::file_size( target, ec );
    if( ec ) { error = "session_time_transform_file_size_failed:" + ec.message(); return false; }
    fileManifest.fileSha256 = Sha256File( target ); fileManifest.welcomeCount = metadata.welcomeCount;
    return SaveTimeTransformManifest( root, fileManifest, error );
}

bool LoadTraceSessionTimeTransform( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionTimeTransform& timeTransform,
    std::string& error )
{
    error.clear();
    timeTransform = {};
    const auto root = TraceSessionTimeTransformRoot( sessionRoot, manifest );
    std::error_code ec;
    if( std::filesystem::exists( root / "manifest", ec ) )
        return AuditTraceSessionTimeTransformDerived( sessionRoot, manifest, timeTransform, error );
    if( ec ) { error = "session_time_transform_manifest_scan_failed:" + ec.message(); return false; }
    MetadataState metadata { &timeTransform };
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard,
            VisitMetadata, &metadata, error ) ) return false;
    }
    if( !timeTransform.present )
    {
        error = "session_welcome_time_transform_missing";
        return false;
    }
    return true;
}

bool LoadTraceSessionGpuCanonicalData( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, JnTraceData& data,
    TraceSessionTimeTransform& timeTransform, TraceSessionGpuCanonicalStats& stats,
    std::string& error )
{
    error.clear(); data = {}; timeTransform = {}; stats = {};
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, timeTransform, error ) ) return false;
    GpuLoadState state { &data, &timeTransform, &stats };
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard,
            VisitGpuCanonical, &state, error ) ) return false;
    }
    stats.unresolvedPayloads = state.catalogPayloads.size();
    if( stats.unresolvedPayloads != 0 )
    {
        error = "session_gpu_catalog_payload_unresolved";
        return false;
    }
    for( const auto& [_, runtime] : state.catalogRuntime )
    {
        if( !runtime.ended )
        {
            data.gpuCatalogValid = false;
            if( runtime.generationIndex < data.gpuCatalogGenerations.size() )
                data.gpuCatalogGenerations[runtime.generationIndex].valid = 0;
        }
    }
    std::unordered_map<uint64_t, uint64_t> descriptorHeaps;
    uint64_t nextDescriptorHeapId = 1;
    const auto anonymizeDescriptor = [&]( uint64_t token )
    {
        const auto found = descriptorHeaps.find( token );
        if( found != descriptorHeaps.end() ) return found->second;
        const auto id = 0xD000000000000000ull | nextDescriptorHeapId++;
        descriptorHeaps.emplace( token, id );
        return id;
    };
    for( auto& generation : data.gpuCatalogGenerations )
    {
        const auto resolved = ResolveJnGpuCatalogGenerationData(
            data, generation.generation, anonymizeDescriptor );
        generation.unresolvedCount = resolved.totalUnresolved;
        if( !generation.ended || resolved.coreUnresolved != 0 )
        {
            generation.valid = 0;
            generation.state = uint8_t( JnGpuCatalogGenerationState::InvalidCoreGap );
            data.gpuCatalogValid = false;
        }
    }
    return true;
}

bool LoadTraceSessionGpuCatalogDataImpl( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, JnTraceData& data,
    TraceSessionTimeTransform& timeTransform, TraceSessionGpuCanonicalStats& stats,
    bool metadataOnly, std::string& error )
{
    error.clear(); data = {}; timeTransform = {}; stats = {};
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, timeTransform, error ) ) return false;
    GpuLoadState state { &data, &timeTransform, &stats };
    state.loadReferenceEvidence = false;
    state.loadRangeEvidence = false;
    state.loadCatalogEnrichment = false;
    state.loadCatalogCore = !metadataOnly;
    state.loadCatalogAuxiliary = !metadataOnly;
    state.loadCatalogStrings = !metadataOnly;
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard,
            VisitGpuCanonical, &state, error ) ) return false;
    }
    stats.unresolvedPayloads = state.catalogPayloads.size();
    if( stats.unresolvedPayloads != 0 )
    {
        error = "session_gpu_catalog_payload_unresolved";
        return false;
    }
    for( const auto& [_, runtime] : state.catalogRuntime )
    {
        if( !runtime.ended )
        {
            data.gpuCatalogValid = false;
            if( runtime.generationIndex < data.gpuCatalogGenerations.size() )
                data.gpuCatalogGenerations[runtime.generationIndex].valid = 0;
        }
    }
    std::unordered_map<uint64_t, uint64_t> descriptorHeaps;
    uint64_t nextDescriptorHeapId = 1;
    const auto anonymizeDescriptor = [&]( uint64_t token )
    {
        const auto found = descriptorHeaps.find( token );
        if( found != descriptorHeaps.end() ) return found->second;
        const auto id = 0xD000000000000000ull | nextDescriptorHeapId++;
        descriptorHeaps.emplace( token, id ); return id;
    };
    for( auto& generation : data.gpuCatalogGenerations )
    {
        JnGpuCatalogResolvedCounts resolved {};
        if( !metadataOnly ) resolved = ResolveJnGpuCatalogGenerationData(
            data, generation.generation, anonymizeDescriptor );
        generation.unresolvedCount = resolved.totalUnresolved;
        if( !generation.ended || resolved.coreUnresolved != 0 )
        {
            generation.valid = 0;
            generation.state = uint8_t( JnGpuCatalogGenerationState::InvalidCoreGap );
            data.gpuCatalogValid = false;
        }
    }
    return true;
}

bool LoadTraceSessionGpuCatalogData( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, JnTraceData& data,
    TraceSessionTimeTransform& timeTransform, TraceSessionGpuCanonicalStats& stats,
    std::string& error )
{
    return LoadTraceSessionGpuCatalogDataImpl( sessionRoot, manifest, data,
        timeTransform, stats, false, error );
}

std::filesystem::path TraceSessionGpuAnalysisRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "gpu-resource-analysis" / std::to_string( GpuAnalysisDerivedSchemaVersion ) /
        GpuAnalysisAlgorithmId;
}

bool BuildTraceSessionGpuAnalysisDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const GpuAnalysisSidecarControl& control,
    TraceSessionGpuDerivedStats& stats, std::string& error )
{
    error.clear(); stats = {};
    JnTraceData catalogData;
    TraceSessionTimeTransform transform;
    TraceSessionGpuCanonicalStats canonicalStats;
    if( !LoadTraceSessionGpuCatalogDataImpl(
        sessionRoot, manifest, catalogData, transform, canonicalStats, true, error ) ) return false;
    GpuAnalysisTraceIdentity identity;
    identity.sha256 = manifest.source.sha256;
    identity.fileSize = manifest.source.fileSize;
    GpuAnalysisCatalogSpool catalogSpool;
    if( !BuildBoundedGpuCatalogSpool( sessionRoot, manifest, transform,
        catalogData, identity, control, catalogSpool, error ) ) return false;
    ResourceLifetimeResolver resolver;
    if( !resolver.AddPointerFile( catalogSpool.pointerLifetimePath, error ) ) return false;
    BoundedCatalogLookup catalogLookup;
    if( !catalogLookup.Open( catalogSpool, error ) ) return false;
    std::vector<uint64_t> evidenceGenerations( catalogData.gpuDetailedEvidence.size() );
    for( const auto& batch : catalogData.gpuCatalogBatches )
    {
        if( batch.kind != uint8_t( JnGpuCatalogBatchKind::DetailedEvidence ) ||
            batch.firstRecordIndex >= evidenceGenerations.size() ) continue;
        const auto end = std::min<uint64_t>( evidenceGenerations.size(),
            batch.firstRecordIndex + batch.recordCount );
        std::fill( evidenceGenerations.begin() + size_t( batch.firstRecordIndex ),
            evidenceGenerations.begin() + size_t( end ), batch.generation );
    }
    std::vector<PassEvidenceEntry> evidence;
    evidence.reserve( catalogData.gpuDetailedEvidence.size() );
    for( size_t i = 0; i < catalogData.gpuDetailedEvidence.size(); ++i )
        evidence.push_back( { catalogData.gpuDetailedEvidence[i].sourceId,
            { evidenceGenerations[i], catalogData.gpuDetailedEvidence[i] } } );
    std::sort( evidence.begin(), evidence.end(), []( const auto& lhs, const auto& rhs ) {
        return lhs.passId < rhs.passId;
    } );
    catalogSpool.overview.manifest.logicalRecordCount = canonicalStats.catalogRecordCounts[
        size_t( JnGpuCatalogBatchKind::Logical )];
    catalogSpool.overview.manifest.viewRecordCount = canonicalStats.catalogRecordCounts[
        size_t( JnGpuCatalogBatchKind::View )];
    catalogSpool.overview.manifest.partRecordCount = canonicalStats.catalogRecordCounts[
        size_t( JnGpuCatalogBatchKind::Part )];
    catalogSpool.overview.manifest.vgRecordCount = canonicalStats.catalogRecordCounts[
        size_t( JnGpuCatalogBatchKind::VirtualGeometry )];
    catalogSpool.overview.manifest.relationRecordCount = canonicalStats.catalogRecordCounts[
        size_t( JnGpuCatalogBatchKind::Relation )];
    catalogSpool.overview.manifest.rangeRecordCount = canonicalStats.catalogRecordCounts[
        size_t( JnGpuCatalogBatchKind::RangeSet )];
    GpuAnalysisPassSpool passSpool;
    std::vector<GpuResourceAnalysisRecord> appendedResources;
    if( !BuildGpuPassSpool( sessionRoot, manifest, transform, resolver, catalogLookup,
        catalogSpool.overview, appendedResources,
        evidence, identity, control, passSpool, error ) ) return false;
    if( passSpool.sourceGapResourceCount != 0 )
    {
        catalogSpool.overview.manifest.reason = "source_gpu_resource_identity_gap:" +
            std::to_string( passSpool.sourceGapResourceCount );
        catalogSpool.overview.manifest.unresolvedCount += passSpool.sourceGapReferenceCount;
    }
    stats.resourceCount = catalogSpool.resourceCount + appendedResources.size();
    stats.allocationCount = catalogSpool.allocationCount;
    stats.passCount = passSpool.passCount;
    stats.sourceGapResourceCount = passSpool.sourceGapResourceCount;
    stats.sourceGapReferenceCount = passSpool.sourceGapReferenceCount;
    stats.catalogPageCount = catalogSpool.pageCount;
    stats.peakCatalogRecordsInMemory = catalogSpool.peakRecordsInMemory;
    stats.usedPagedCatalog = true;
    const auto result = WriteGpuAnalysisDerivedStoreFromCatalogAndPassSpoolsAt(
        TraceSessionGpuAnalysisRoot( sessionRoot, manifest ), identity, catalogSpool,
        appendedResources,
        passSpool, catalogData.gpuCatalogStrings, control, stats.generation,
        stats.writtenBytes, error );
    if( result )
    {
        std::error_code ec;
        std::filesystem::remove_all( GpuAnalysisIoPath( passSpool.root ), ec );
        std::filesystem::remove_all( GpuAnalysisIoPath( catalogSpool.root ), ec );
    }
    return result;
}

}
