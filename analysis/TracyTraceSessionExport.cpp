#include "TracyTraceSessionExport.hpp"

#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionCpuZones.hpp"
#include "TracyTraceSessionFrames.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyTraceSessionGpuZones.hpp"
#include "TracyTraceSessionIoGfx.hpp"
#include "TracyTraceSessionJobs.hpp"
#include "TracyTraceSessionMemory.hpp"
#include "TracyProtocol.hpp"
#include "TracyStreamJournal.hpp"
#include "tracy_lz4.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace tracy::analysis
{

namespace
{

bool IsCompleteState( TraceSessionState state )
{
    return state == TraceSessionState::Complete ||
        state == TraceSessionState::CompleteSourceDegraded;
}

bool AddSaturating( uint64_t& value, uint64_t add )
{
    if( value > std::numeric_limits<uint64_t>::max() - add )
    {
        value = std::numeric_limits<uint64_t>::max();
        return false;
    }
    value += add;
    return true;
}

struct ExportShardScan
{
    const TraceSessionExportRange* range = nullptr;
    const TraceSessionTimeTransform* transform = nullptr;
    uint64_t windowSemanticRecords = 0;
    uint64_t timelessRecords = 0;
};

bool VisitExportShardRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& )
{
    auto& scan = *static_cast<ExportShardScan*>( userData );
    if( !record.hasSemanticTime )
    {
        scan.timelessRecords++;
        return true;
    }
    const auto timeNs = scan.transform->ToNanoseconds( record.semanticTime );
    if( timeNs >= scan.range->beginNs && timeNs < scan.range->endNs )
        scan.windowSemanticRecords++;
    return true;
}

bool VerifyCurrentGeneration( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, std::string& error )
{
    if( !IsCompleteState( manifest.state ) || !manifest.mandatoryDerivedComplete ||
        !manifest.auditComplete )
    {
        error = "session_export_session_not_complete";
        return false;
    }
    auto current = LoadTraceSessionManifest( sessionRoot, error );
    if( !current ) return false;
    if( current->generation != manifest.generation ||
        current->source.sha256 != manifest.source.sha256 ||
        current->source.fileSize != manifest.source.fileSize )
    {
        error = "session_export_generation_mismatch";
        return false;
    }
    if( !IsCompleteState( current->state ) || !current->mandatoryDerivedComplete ||
        !current->auditComplete )
    {
        error = "session_export_session_not_complete";
        return false;
    }
    return true;
}

struct WindowProtocolState
{
    const TraceSessionExportRange* range = nullptr;
    const TraceSessionTimeTransform* transform = nullptr;
    TraceSessionWindowProtocolSink sink = nullptr;
    void* sinkUserData = nullptr;
    TraceSessionWindowProtocolStats* stats = nullptr;
    tracy::LZ4_stream_t* compressor = nullptr;
    std::vector<uint8_t> frame;
    std::vector<uint8_t> compressed;
    std::array<char, 64 * 1024> dictionary {};
};

bool EmitWindowProtocolRecord( WindowProtocolState& state, uint16_t recordType,
    uint32_t flags, uint64_t monotonicNs, std::span<const uint8_t> payload,
    std::string& error )
{
    TraceSessionWindowProtocolRecord output;
    output.recordType = recordType;
    output.flags = flags;
    output.monotonicNs = monotonicNs;
    output.payload = payload;
    if( !state.sink( output, state.sinkUserData, error ) )
    {
        if( error.empty() ) error = "session_export_protocol_sink_failed";
        return false;
    }
    AddSaturating( state.stats->outputBytes, payload.size() );
    return true;
}

bool FlushWindowProtocolFrame( WindowProtocolState& state,
    const TraceSessionCanonicalRecord& marker, std::string& error )
{
    state.stats->inputFrames++;
    if( state.frame.empty() ) return true;
    if( state.frame.size() > tracy::TargetFrameSize )
    {
        error = "session_export_protocol_frame_exceeds_limit";
        return false;
    }
    state.compressed.resize( sizeof( tracy::lz4sz_t ) + tracy::LZ4Size );
    const auto compressedBytes = tracy::LZ4_compress_fast_continue(
        state.compressor, reinterpret_cast<const char*>( state.frame.data() ),
        reinterpret_cast<char*>( state.compressed.data() + sizeof( tracy::lz4sz_t ) ),
        int( state.frame.size() ), tracy::LZ4Size, 1 );
    if( compressedBytes <= 0 )
    {
        error = "session_export_protocol_compress_failed";
        return false;
    }
    const auto storedSize = tracy::lz4sz_t( compressedBytes );
    std::memcpy( state.compressed.data(), &storedSize, sizeof( storedSize ) );
    state.compressed.resize( sizeof( storedSize ) + size_t( compressedBytes ) );
    const auto flags = marker.flags | tracy::stream::RecordFlagCompressedFrame;
    if( !EmitWindowProtocolRecord( state,
        uint16_t( tracy::stream::RecordType::ClientToServer ), flags,
        marker.journalMonotonicNs, state.compressed, error ) ) return false;
    if( tracy::LZ4_saveDict( state.compressor, state.dictionary.data(),
        int( state.dictionary.size() ) ) < 0 )
    {
        error = "session_export_protocol_dictionary_save_failed";
        return false;
    }
    state.frame.clear();
    state.stats->outputFrames++;
    return true;
}

bool VisitWindowProtocolRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    auto& state = *static_cast<WindowProtocolState*>( userData );
    if( record.kind == TraceSessionCanonicalRecordKind::TransportRecord )
    {
        if( !state.frame.empty() )
        {
            error = "session_export_transport_inside_protocol_frame";
            return false;
        }
        const auto type = uint16_t( record.type );
        if( type < uint16_t( tracy::stream::RecordType::SessionBegin ) ||
            type > uint16_t( tracy::stream::RecordType::Diagnostic ) )
        {
            error = "session_export_transport_type_invalid";
            return false;
        }
        if( !EmitWindowProtocolRecord( state, type, record.flags,
            record.journalMonotonicNs, record.payload, error ) ) return false;
        state.stats->transportRecords++;
        return true;
    }
    if( record.kind == TraceSessionCanonicalRecordKind::ProtocolFrame )
        return FlushWindowProtocolFrame( state, record, error );

    state.stats->scannedProtocolEvents++;
    bool selected = !record.hasSemanticTime;
    if( record.hasSemanticTime )
    {
        const auto timeNs = state.transform->ToNanoseconds( record.semanticTime );
        selected = timeNs >= state.range->beginNs && timeNs < state.range->endNs;
        if( selected ) state.stats->selectedSemanticEvents++;
        else state.stats->omittedSemanticEvents++;
    }
    else
    {
        state.stats->selectedTimelessEvents++;
    }
    if( !selected ) return true;
    if( record.payload.size() > tracy::TargetFrameSize - state.frame.size() )
    {
        error = "session_export_protocol_frame_exceeds_limit";
        return false;
    }
    state.frame.insert( state.frame.end(), record.payload.begin(), record.payload.end() );
    return true;
}

void ClassifyBoundary( TraceSessionExportBoundaryDomain& domain,
    int64_t beginNs, const std::optional<int64_t>& endNs,
    const TraceSessionExportRange& range )
{
    const bool openBefore = beginNs < range.beginNs &&
        ( !endNs || *endNs > range.beginNs );
    const bool openAfter = beginNs < range.endNs &&
        ( !endNs || *endNs >= range.endNs );
    domain.openBefore += openBefore ? 1 : 0;
    domain.openAfter += openAfter ? 1 : 0;
    domain.spanning += openBefore && openAfter ? 1 : 0;
}

}

bool ResolveTraceSessionExportRange( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionExportSelection& selection,
    TraceSessionExportRange& range, std::string& error )
{
    error.clear();
    range = {};
    const bool anyTime = selection.timeBeginNs.has_value() || selection.timeEndNs.has_value();
    const bool allTime = selection.timeBeginNs.has_value() && selection.timeEndNs.has_value();
    const bool anyFrame = selection.frameSet.has_value() || selection.frameBegin.has_value() ||
        selection.frameEnd.has_value();
    const bool allFrame = selection.frameSet.has_value() && selection.frameBegin.has_value() &&
        selection.frameEnd.has_value();
    if( anyTime && anyFrame )
    {
        error = "session_export_selection_mixed";
        return false;
    }
    if( anyTime && !allTime )
    {
        error = "session_export_time_selection_incomplete";
        return false;
    }
    if( anyFrame && !allFrame )
    {
        error = "session_export_frame_selection_incomplete";
        return false;
    }
    if( !allTime && !allFrame )
    {
        error = "session_export_selection_missing";
        return false;
    }
    if( !VerifyCurrentGeneration( sessionRoot, manifest, error ) ) return false;

    if( allTime )
    {
        if( *selection.timeBeginNs >= *selection.timeEndNs )
        {
            error = "session_export_time_range_invalid";
            return false;
        }
        range.beginNs = *selection.timeBeginNs;
        range.endNs = *selection.timeEndNs;
        return true;
    }

    if( *selection.frameSet > std::numeric_limits<size_t>::max() ||
        *selection.frameBegin > std::numeric_limits<size_t>::max() ||
        *selection.frameEnd > std::numeric_limits<size_t>::max() )
    {
        error = "session_export_frame_range_platform_overflow";
        return false;
    }
    auto frames = TraceSessionFrameReader::Open( sessionRoot, manifest, error );
    if( !frames ) return false;
    const auto frameSet = size_t( *selection.frameSet );
    const auto frameBegin = size_t( *selection.frameBegin );
    const auto frameEnd = size_t( *selection.frameEnd );
    if( frameSet >= frames->Sets().size() )
    {
        error = "session_export_frame_set_not_found";
        return false;
    }
    const auto& values = frames->Sets()[frameSet].frames;
    if( frameBegin >= frameEnd || frameEnd > values.size() )
    {
        error = "session_export_frame_range_invalid";
        return false;
    }
    const auto beginNs = values[frameBegin].beginNs;
    const auto endNs = values[frameEnd - 1].endNs;
    if( beginNs >= endNs )
    {
        error = "session_export_frame_time_range_invalid";
        return false;
    }
    range.beginNs = beginNs;
    range.endNs = endNs;
    range.frameSelection = true;
    range.frameSet = *selection.frameSet;
    range.frameBegin = *selection.frameBegin;
    range.frameEnd = *selection.frameEnd;
    return true;
}

bool BuildTraceSessionExportPlan( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionExportRange& range,
    const TraceSessionExportControl& control, TraceSessionExportPlan& plan,
    std::string& error )
{
    error.clear();
    plan = {};
    plan.range = range;
    plan.workerMemoryLimitBytes = control.workerMemoryLimitBytes;
    plan.workerFixedOverheadBytes = control.workerFixedOverheadBytes;
    plan.workerExpansionNumerator = control.workerExpansionNumerator;
    plan.workerExpansionDenominator = control.workerExpansionDenominator;
    plan.estimateMethod = "canonical_uncompressed_x" +
        std::to_string( control.workerExpansionNumerator ) + "_over_" +
        std::to_string( control.workerExpansionDenominator ) + "_plus_" +
        std::to_string( control.workerFixedOverheadBytes ) + "B_v1";
    if( range.beginNs >= range.endNs )
    {
        error = "session_export_time_range_invalid";
        return false;
    }
    if( control.workerMemoryLimitBytes == 0 || control.workerExpansionNumerator == 0 ||
        control.workerExpansionDenominator == 0 )
    {
        error = "session_export_control_invalid";
        return false;
    }
    if( !VerifyCurrentGeneration( sessionRoot, manifest, error ) ) return false;
    TraceSessionTimeTransform transform;
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, transform, error ) ) return false;

    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        plan.scannedDataShards++;
        ExportShardScan scan { &range, &transform };
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard,
            VisitExportShardRecord, &scan, error ) ) return false;
        const bool window = scan.windowSemanticRecords != 0;
        const bool dependency = scan.timelessRecords != 0;
        if( !window && !dependency ) continue;
        plan.shardIds.emplace_back( shard.shardId );
        plan.windowSemanticShards += window ? 1 : 0;
        plan.dependencySourceShards += dependency ? 1 : 0;
        AddSaturating( plan.windowSemanticRecords, scan.windowSemanticRecords );
        AddSaturating( plan.timelessDependencyRecords, scan.timelessRecords );
        AddSaturating( plan.recordCount, shard.recordCount );
        AddSaturating( plan.canonicalUncompressedBytes, shard.uncompressedBytes );
        AddSaturating( plan.canonicalFileBytes, shard.fileBytes );
    }

    if( plan.shardIds.empty() )
    {
        error = "session_export_range_has_no_canonical_evidence";
        return false;
    }
    const auto numerator = uint64_t( control.workerExpansionNumerator );
    const auto denominator = uint64_t( control.workerExpansionDenominator );
    uint64_t expanded = std::numeric_limits<uint64_t>::max();
    if( plan.canonicalUncompressedBytes <=
        ( std::numeric_limits<uint64_t>::max() - ( denominator - 1 ) ) / numerator )
    {
        expanded = ( plan.canonicalUncompressedBytes * numerator + denominator - 1 ) /
            denominator;
    }
    plan.estimatedWorkerBytes = control.workerFixedOverheadBytes;
    AddSaturating( plan.estimatedWorkerBytes, expanded );
    plan.memoryAllowed = plan.estimatedWorkerBytes <= control.workerMemoryLimitBytes;
    if( !plan.memoryAllowed )
    {
        error = "session_export_worker_memory_limit";
        return false;
    }
    return true;
}

bool BuildTraceSessionWindowProtocol( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionExportRange& range,
    TraceSessionWindowProtocolSink sink, void* sinkUserData,
    TraceSessionWindowProtocolStats& stats, std::string& error )
{
    error.clear();
    stats = {};
    if( !sink )
    {
        error = "session_export_protocol_sink_missing";
        return false;
    }
    if( range.beginNs >= range.endNs )
    {
        error = "session_export_time_range_invalid";
        return false;
    }
    if( !VerifyCurrentGeneration( sessionRoot, manifest, error ) ) return false;
    TraceSessionTimeTransform transform;
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, transform, error ) ) return false;
    auto* compressor = tracy::LZ4_createStream();
    if( !compressor )
    {
        error = "session_export_protocol_compressor_alloc_failed";
        return false;
    }
    WindowProtocolState state;
    state.range = &range;
    state.transform = &transform;
    state.sink = sink;
    state.sinkUserData = sinkUserData;
    state.stats = &stats;
    state.compressor = compressor;
    const auto success = VisitTraceSessionCanonicalOrdered( sessionRoot, manifest,
        VisitWindowProtocolRecord, &state, error );
    tracy::LZ4_freeStream( compressor );
    if( !success ) return false;
    if( !state.frame.empty() )
    {
        error = "session_export_protocol_frame_missing_marker";
        return false;
    }
    if( stats.outputFrames == 0 || stats.selectedSemanticEvents == 0 )
    {
        error = "session_export_range_has_no_protocol_events";
        return false;
    }
    return true;
}

bool BuildTraceSessionExportBoundaryPlan( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionExportRange& range,
    TraceSessionExportBoundaryPlan& plan, std::string& error )
{
    error.clear();
    plan = {};
    if( range.beginNs >= range.endNs )
    {
        error = "session_export_time_range_invalid";
        return false;
    }
    if( !VerifyCurrentGeneration( sessionRoot, manifest, error ) ) return false;
    constexpr size_t PageSize = 4096;
    try
    {
        auto cpu = TraceSessionCpuZoneReader::Open( sessionRoot, manifest, error );
        if( !cpu ) return false;
        for( uint64_t offset = 0; offset < cpu->Stats().zones; offset += PageSize )
        {
            const auto page = cpu->ScanById( size_t( offset ), PageSize );
            if( page.empty() ) { error = "session_export_cpu_zone_page_missing"; return false; }
            for( const auto& value : page )
                ClassifyBoundary( plan.cpuZones, value.startNs, value.endNs, range );
            plan.scannedCpuZones += page.size();
        }

        auto gpu = TraceSessionGpuZoneReader::Open( sessionRoot, manifest, error );
        if( !gpu ) return false;
        for( uint64_t offset = 0; offset < gpu->Stats().zones; offset += PageSize )
        {
            const auto page = gpu->ScanById( size_t( offset ), PageSize );
            if( page.empty() ) { error = "session_export_gpu_zone_page_missing"; return false; }
            for( const auto& value : page )
                ClassifyBoundary( plan.gpuZones, value.cpuStartNs, value.cpuEndNs, range );
            plan.scannedGpuZones += page.size();
        }

        auto memory = TraceSessionMemoryReader::Open( sessionRoot, manifest, error );
        if( !memory ) return false;
        for( uint64_t offset = 0; offset < memory->Stats().events; offset += PageSize )
        {
            if( offset > std::numeric_limits<size_t>::max() )
            { error = "session_export_memory_offset_platform_overflow"; return false; }
            const auto page = memory->ScanByStorageOrder( size_t( offset ), PageSize );
            if( page.empty() ) { error = "session_export_memory_page_missing"; return false; }
            for( const auto& value : page )
                ClassifyBoundary( plan.allocations, value.allocationNs, value.freeNs, range );
            plan.scannedAllocations += page.size();
        }

        auto jobs = TraceSessionJobReader::Open( sessionRoot, manifest, error );
        if( !jobs ) return false;
        for( uint64_t offset = 0; offset < jobs->Count(); offset += PageSize )
        {
            if( offset > std::numeric_limits<size_t>::max() )
            { error = "session_export_job_offset_platform_overflow"; return false; }
            const auto page = jobs->ScanBySchedule( size_t( offset ), PageSize );
            if( page.empty() ) { error = "session_export_job_page_missing"; return false; }
            for( const auto& value : page )
                ClassifyBoundary( plan.jobs, value.scheduleNs, value.completedNs, range );
            plan.scannedJobs += page.size();
        }

        auto io = TraceSessionIoGfxReader::Open( sessionRoot, manifest, error );
        if( !io ) return false;
        for( uint64_t offset = 0; offset < io->Stats().ioRequestIds; offset += PageSize )
        {
            if( offset > std::numeric_limits<size_t>::max() )
            { error = "session_export_io_offset_platform_overflow"; return false; }
            const auto page = io->ScanIoRequestsByQueue( size_t( offset ), PageSize );
            if( page.empty() ) { error = "session_export_io_page_missing"; return false; }
            for( const auto& value : page )
                ClassifyBoundary( plan.ioRequests, value.queueNs, value.endNs, range );
            plan.scannedIoRequests += page.size();
        }
    }
    catch( const std::exception& exception )
    {
        error = "session_export_boundary_read_failed:" + std::string( exception.what() );
        return false;
    }
    return true;
}

}
