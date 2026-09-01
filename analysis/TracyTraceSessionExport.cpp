#include "TracyTraceSessionExport.hpp"

#include "TracyTraceSessionFrames.hpp"

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

}
