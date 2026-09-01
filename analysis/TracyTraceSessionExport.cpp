#include "TracyTraceSessionExport.hpp"

#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionFrames.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"

#include <algorithm>
#include <limits>

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

}
