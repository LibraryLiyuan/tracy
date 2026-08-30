#include "TracyTraceSessionGpuCanonical.hpp"

#include "TracyGpuAnalysis.hpp"
#include "TracyGpuAnalysisStore.hpp"
#include "TracyJnGpuCatalog.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"
#include "TracyStreamJournal.hpp"
#include "TracyJnGpuCatalogResolve.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace tracy::analysis
{
namespace
{

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
          state.transform->baseTime != welcome.initBegin ) )
    {
        error = "session_welcome_time_transform_conflict";
        return false;
    }
    state.transform->timerMultiplier = welcome.timerMul;
    state.transform->baseTime = welcome.initBegin;
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
    case JnGpuCatalogBatchKind::Resource: first = AppendCatalogRecords( data.gpuCatalogResources, records, event.recordCount, *state.transform ); break;
    case JnGpuCatalogBatchKind::Allocation: first = AppendCatalogRecords( data.gpuCatalogAllocations, records, event.recordCount, *state.transform ); break;
    case JnGpuCatalogBatchKind::View: first = AppendCatalogRecords( data.gpuCatalogViews, records, event.recordCount, *state.transform ); break;
    case JnGpuCatalogBatchKind::Logical: first = AppendCatalogRecords( data.gpuCatalogLogicals, records, event.recordCount, *state.transform ); break;
    case JnGpuCatalogBatchKind::Part: first = AppendCatalogRecords( data.gpuCatalogParts, records, event.recordCount, *state.transform ); break;
    case JnGpuCatalogBatchKind::Relation: first = AppendCatalogRecords( data.gpuCatalogRelations, records, event.recordCount, *state.transform ); break;
    case JnGpuCatalogBatchKind::VirtualGeometry: first = AppendCatalogRecords( data.gpuCatalogVg, records, event.recordCount, *state.transform ); break;
    case JnGpuCatalogBatchKind::RangeSet: first = AppendCatalogRecords( data.gpuRangeSets, records, event.recordCount, *state.transform ); break;
    case JnGpuCatalogBatchKind::DetailedEvidence: first = AppendCatalogRecords( data.gpuDetailedEvidence, records, event.recordCount, *state.transform ); break;
    case JnGpuCatalogBatchKind::String:
    {
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
        const auto& event = item.jnGpuReferenceUse;
        data.present = true; data.schemaVersion = JnTraceSchemaVersion;
        data.gpuReferenceUses.push_back( { state.transform->ToNanoseconds( event.time ),
            event.passId, event.resourceId, record.threadContext, event.usageMask, 0, event.flags, 1 } );
        return true;
    }
    case QueueType::JnGpuReferenceSetUse:
    {
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

}

int64_t TraceSessionTimeTransform::ToNanoseconds( int64_t value ) const
{
    return present ? int64_t( double( value - baseTime ) * timerMultiplier ) : value;
}

bool LoadTraceSessionGpuCanonicalData( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, JnTraceData& data,
    TraceSessionTimeTransform& timeTransform, TraceSessionGpuCanonicalStats& stats,
    std::string& error )
{
    error.clear(); data = {}; timeTransform = {}; stats = {};
    MetadataState metadata { &timeTransform };
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain != "control" ) continue;
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard, VisitMetadata, &metadata, error ) ) return false;
    }
    if( !timeTransform.present )
    {
        error = "session_welcome_time_transform_missing";
        return false;
    }
    GpuLoadState state { &data, &timeTransform, &stats };
    std::vector<const TraceSessionShard*> gpuShards;
    for( const auto& shard : manifest.shards )
        if( shard.domain == "gpu_catalog" || shard.domain == "gpu_memory" ) gpuShards.push_back( &shard );
    std::sort( gpuShards.begin(), gpuShards.end(), []( const auto* left, const auto* right ) {
        if( left->sourceRecordBegin != right->sourceRecordBegin ) return left->sourceRecordBegin < right->sourceRecordBegin;
        return left->shardId < right->shardId;
    } );
    for( const auto* shard : gpuShards )
        if( !VisitTraceSessionCanonicalShard( sessionRoot, *shard, VisitGpuCanonical, &state, error ) ) return false;
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
    JnTraceData data;
    TraceSessionTimeTransform transform;
    TraceSessionGpuCanonicalStats canonicalStats;
    if( !LoadTraceSessionGpuCanonicalData(
        sessionRoot, manifest, data, transform, canonicalStats, error ) ) return false;
    GpuAnalysisBuildControl buildControl;
    buildControl.stopToken = control.stopToken;
    buildControl.progress = control.progress;
    auto snapshot = BuildGpuAnalysisSnapshotConsuming( data, nullptr, {}, buildControl );
    if( snapshot.manifest.state != GpuAnalysisState::Complete || !snapshot.manifest.complete )
    {
        error = "session_gpu_analysis_incomplete:" + snapshot.manifest.reason;
        return false;
    }
    GpuAnalysisTraceIdentity identity;
    identity.sha256 = manifest.source.sha256;
    identity.fileSize = manifest.source.fileSize;
    stats.resourceCount = snapshot.resources.size();
    stats.allocationCount = snapshot.allocations.size();
    stats.passCount = snapshot.passes.size();
    return WriteGpuAnalysisDerivedStoreAt( TraceSessionGpuAnalysisRoot( sessionRoot, manifest ),
        identity, snapshot, control, stats.generation, stats.writtenBytes, error );
}

}
