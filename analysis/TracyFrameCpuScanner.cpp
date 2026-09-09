#include "TracyFrameCpuScanner.hpp"

#include "TracyHash.hpp"
#include "TracyAnalysisDictionary.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace tracy::analysis
{
namespace
{

constexpr size_t RepresentativeLimit = 8;

void CheckCancelled( const std::function<bool()>& cancelled )
{
    if( cancelled && cancelled() ) throw BoundedScanError( "frame_cpu_scan_cancelled" );
}

struct FrameBoundary
{
    std::string ref;
    std::string frameSetRef;
    size_t index = 0;
    int64_t beginNs = 0;
    int64_t endNs = 0;
    uint32_t frameSetOrdinal = 0;
};

struct FrameContribution
{
    const FrameBoundary* frame = nullptr;
    int64_t beginNs = 0;
    int64_t endNs = 0;
};

struct WorkingZone
{
    AnalysisWorkspaceReservation workspace;
    std::string ref;
    std::string exactSignature;
    std::string logicalSignature;
    uint32_t exactSignatureOrdinal = 0;
    uint32_t logicalSignatureOrdinal = 0;
    std::string threadRef;
    int64_t beginNs = 0;
    int64_t endNs = 0;
    bool exact = true;
    std::vector<FrameContribution> frames;
    std::unordered_map<std::string, std::vector<std::pair<int64_t, int64_t>>> directChildren;
};

struct WorkingStack
{
    AnalysisWorkspaceReservation workspace;
    std::vector<WorkingZone> zones;
    explicit WorkingStack( std::shared_ptr<AnalysisWorkspaceBudget> budget = {} )
        : workspace( std::move( budget ) ) {}
    void ReserveNext()
    {
        if( zones.size() != zones.capacity() ) return;
        const auto capacity = std::max<size_t>( 64, zones.capacity() * 2 );
        if( capacity > UINT64_MAX / ( 2 * sizeof( WorkingZone ) ) ) throw std::runtime_error( "analysis_workspace_budget" );
        workspace.Resize( uint64_t( capacity ) * 2 * sizeof( WorkingZone ) );
        zones.reserve( capacity );
    }
};

uint64_t TextBytes( const std::optional<std::string>& value ) { return value ? value->size() : 0; }
uint64_t ZoneBytes( const CpuZoneDto& zone )
{
    return 2048 + 4 * ( zone.ref.size() + zone.threadRef.size() + zone.sourceLocationRef.size() + zone.name.size() +
        zone.function.size() + zone.file.size() + TextBytes( zone.parentRef ) + TextBytes( zone.callstackRef ) +
        TextBytes( zone.stackRef ) + zone.stackProvenance.size() + TextBytes( zone.stackUnavailableReason ) +
        TextBytes( zone.timingInvalidReason ) + TextBytes( zone.extraName ) + TextBytes( zone.extraText ) );
}

struct RunKey
{
    std::string signature;
    std::string frame;

    bool operator<( const RunKey& other ) const
    {
        if( signature != other.signature ) return signature < other.signature;
        return frame < other.frame;
    }
};

std::string Lower( std::string value )
{
    std::transform( value.begin(), value.end(), value.begin(), []( unsigned char c ) {
        return char( std::tolower( c ) );
    } );
    return value;
}

bool Contains( const std::string& value, std::string_view needle )
{
    return value.find( needle ) != std::string::npos;
}

std::string ThreadRole( const ThreadDto& thread )
{
    const auto name = Lower( thread.name );
    if( name == "main" || name == "main thread" || Contains( name, "mainthread" ) ) return "Main";
    if( Contains( name, "render" ) || Contains( name, "gfx" ) ) return "Render";
    if( Contains( name, "job.worker" ) || Contains( name, "job worker" ) || Contains( name, "worker" ) ) return "Worker";
    return "Other";
}

uint8_t ThreadRoleCacheIndex( std::string_view role )
{
    if( role == "Main" ) return 0;
    if( role == "Render" ) return 1;
    if( role == "Worker" ) return 2;
    return 3;
}

CpuWorkClass Classify( const CpuZoneDto& zone )
{
    const auto name = Lower( zone.name.empty() ? zone.function : zone.name );
    if( Contains( name, "waitfortargetfps" ) || Contains( name, "wait for target fps" ) ||
        Contains( name, "targetframerate" ) || Contains( name, "frame pacing" ) )
        return CpuWorkClass::IntentionalPacing;
    if( Contains( name, "wait" ) || Contains( name, "sleep" ) || Contains( name, "idle" ) ||
        Contains( name, "semaphore" ) || Contains( name, "fence" ) )
        return CpuWorkClass::Wait;
    return CpuWorkClass::ActiveWork;
}

void HashString( Sha256Builder& hash, std::string_view value )
{
    const auto size = uint64_t( value.size() );
    hash.Update( &size, sizeof( size ) );
    if( !value.empty() ) hash.Update( value.data(), value.size() );
}

std::string SignatureId( bool logical, std::string_view threadIdentity,
    std::string_view parent, const CpuZoneDto& zone, bool stableSite = false )
{
    Sha256Builder hash;
    const uint8_t kind = logical ? 1 : 0;
    hash.Update( &kind, sizeof( kind ) );
    HashString( hash, threadIdentity );
    if( !stableSite ) HashString( hash, parent );
    HashString( hash, zone.sourceLocationRef );
    if( !stableSite || zone.sourceLocationRef.empty() )
    {
        HashString( hash, zone.name );
        HashString( hash, zone.function );
        HashString( hash, zone.file );
        hash.Update( &zone.line, sizeof( zone.line ) );
    }
    return std::string( logical ? "cpu-logical:" : "cpu-exact:" ) + hash.FinalHex();
}

int64_t IntervalUnion( std::vector<std::pair<int64_t, int64_t>> intervals,
    const std::function<bool()>& cancelled )
{
    CheckCancelled( cancelled );
    if( intervals.empty() ) return 0;
    std::sort( intervals.begin(), intervals.end(), [&]( const auto& a, const auto& b ) {
        CheckCancelled( cancelled ); return a < b;
    } );
    int64_t total = 0;
    auto begin = intervals.front().first;
    auto end = intervals.front().second;
    for( size_t i = 1; i < intervals.size(); ++i )
    {
        CheckCancelled( cancelled );
        if( intervals[i].first <= end )
        {
            end = std::max( end, intervals[i].second );
        }
        else
        {
            total += end - begin;
            begin = intervals[i].first;
            end = intervals[i].second;
        }
    }
    return total + end - begin;
}

void AddRepresentative( std::vector<std::string>& refs, const std::string& ref )
{
    if( refs.size() >= RepresentativeLimit ) return;
    if( std::find( refs.begin(), refs.end(), ref ) == refs.end() ) refs.emplace_back( ref );
}

}

ExactFrameCpuScanner::ExactFrameCpuScanner( const TraceSource& source, size_t batchSize )
    : m_source( source )
    , m_batchSize( batchSize )
{
    if( batchSize == 0 || batchSize > NativeBoundedScanMaximumBatch )
        throw BoundedScanError( "frame_cpu_scan_batch_out_of_range" );
}

CpuFrameScanResult ExactFrameCpuScanner::Scan( const CpuSignatureFrameSink& sink,
    const CpuFrameScanOptions& options ) const
{
    return ScanInternal( sink ? &sink : nullptr, nullptr, options );
}

CpuFrameScanResult ExactFrameCpuScanner::ScanView( const CpuSignatureFrameViewSink& sink,
    const CpuFrameScanOptions& options ) const
{
    if( !sink ) throw BoundedScanError( "frame_cpu_scan_view_sink_missing" );
    return ScanInternal( nullptr, &sink, options );
}

CpuFrameScanResult ExactFrameCpuScanner::ScanInternal( const CpuSignatureFrameSink* sink,
    const CpuSignatureFrameViewSink* viewSink, const CpuFrameScanOptions& options ) const
{
    const auto checkCancelled = [&] { CheckCancelled( options.cancelled ); };
    checkCancelled();
    if( !options.includeExactSignatures && !options.includeLogicalSignatures )
        throw BoundedScanError( "frame_cpu_scan_signature_mode_empty" );
    CpuFrameScanResult result( options.workspace );
    AnalysisWorkspaceReservation metadataWorkspace( options.workspace ), frameWorkspace( options.workspace ),
        signatureWorkspace( options.workspace ), presenceWorkspace( options.workspace ), runWorkspace( options.workspace );
    BoundedTraceScanner scanner( m_source );

    const auto sourceFrameSets = m_source.GetFrameSets();
    checkCancelled();
    std::unordered_map<std::string, uint64_t> completeFramesBySet;
    std::vector<FrameBoundary> completeFrames;
    std::unordered_set<std::string> continuousFrameSets;
    for( const auto& frameSet : sourceFrameSets )
    {
        checkCancelled();
        metadataWorkspace.Add( 2048 + 8 * ( frameSet.ref.size() + frameSet.name.size() ) );
        if( frameSet.continuous ) continuousFrameSets.emplace( frameSet.ref );
    }
    std::unordered_map<std::string, FrameBoundary> pendingContinuousFrames;
    auto reportIncompleteFrame = [&]( const std::string& code,
        const std::string& message, const std::string& ref ) {
        ++result.incompleteFrameCount;
        result.qualityComplete = false;
        auto it = std::find_if( result.qualityFindings.begin(), result.qualityFindings.end(),
            [&]( const auto& finding ) { return finding.code == code; } );
        if( it == result.qualityFindings.end() )
        {
            result.outputWorkspace.Add( 1024 + 4 * ( code.size() + message.size() ) );
            result.qualityFindings.push_back( { code, message, 0, {} } );
            it = std::prev( result.qualityFindings.end() );
        }
        ++it->count;
        if( it->representativeRefs.size() < RepresentativeLimit &&
            std::find( it->representativeRefs.begin(), it->representativeRefs.end(), ref ) == it->representativeRefs.end() )
            result.outputWorkspace.Add( 128 + 4 * ref.size() );
        AddRepresentative( it->representativeRefs, ref );
    };
    auto acceptCompleteFrame = [&]( FrameBoundary frame ) {
        checkCancelled();
        if( options.completeFrameSink ) options.completeFrameSink(
            frame.frameSetRef, frame.index, frame.beginNs, frame.endNs );
        checkCancelled();
        ++completeFramesBySet[frame.frameSetRef];
        ++result.completeFrameCount;
        completeFrames.emplace_back( std::move( frame ) );
    };
    BoundedScanRequest frameRequest;
    frameRequest.domain = BoundedScanDomain::Frame;
    frameRequest.limit = m_batchSize;
    for( ;; )
    {
        checkCancelled();
        AnalysisWorkspaceReservation pageWorkspace( options.workspace );
        const auto batch = scanner.Read( frameRequest );
        checkCancelled();
        result.maximumBatchObserved = std::max( result.maximumBatchObserved, batch.Count() );
        const auto& frames = std::get<std::vector<FrameDto>>( batch.records );
        for( const auto& frame : frames )
        {
            checkCancelled();
            pageWorkspace.Add( 1024 + 4 * ( frame.ref.size() + frame.frameSetRef.size() + TextBytes( frame.imageRef ) ) );
        }
        for( const auto& frame : frames )
        {
            checkCancelled();
            const auto valid = frame.complete && frame.endNs && *frame.endNs > frame.beginNs;
            // Every valid frame remains either a completed boundary or the
            // pending continuous tail. Includes indexes and vector growth.
            if( valid ) frameWorkspace.Add( 1024 + 8 * ( frame.ref.size() + frame.frameSetRef.size() ) );
            if( continuousFrameSets.contains( frame.frameSetRef ) )
            {
                // A continuous FrameSet's previous frame is proven complete by
                // the current FrameMark.  Its final entry has no following mark;
                // Worker/Session may attach the capture end to it, but that is
                // not an exact frame boundary and must stay out of denominators.
                auto pending = pendingContinuousFrames.find( frame.frameSetRef );
                if( pending != pendingContinuousFrames.end() )
                {
                    acceptCompleteFrame( std::move( pending->second ) );
                    pendingContinuousFrames.erase( pending );
                }
                if( valid )
                {
                    pendingContinuousFrames.emplace( frame.frameSetRef,
                        FrameBoundary { frame.ref, frame.frameSetRef, frame.index,
                            frame.beginNs, *frame.endNs } );
                }
                else
                {
                    reportIncompleteFrame( "incomplete_frame_boundary",
                        "Incomplete or invalid frame boundaries are excluded from exact denominators.",
                        frame.ref );
                }
            }
            else if( valid )
            {
                acceptCompleteFrame( { frame.ref, frame.frameSetRef, frame.index,
                    frame.beginNs, *frame.endNs } );
            }
            else
            {
                reportIncompleteFrame( "incomplete_frame_boundary",
                    "Incomplete or invalid frame boundaries are excluded from exact denominators.",
                    frame.ref );
            }
        }
        if( batch.cursor.complete ) break;
        frameRequest.cursor = batch.cursor;
    }
    for( const auto& [frameSet, frame] : pendingContinuousFrames )
    {
        checkCancelled();
        (void)frameSet;
        reportIncompleteFrame( "continuous_frame_tail_without_next_mark",
            "The final continuous frame has no following FrameMark; a synthetic capture-end boundary is excluded from exact denominators.",
            frame.ref );
    }
    for( const auto& frameSet : sourceFrameSets )
        result.outputWorkspace.Add( 512 + 4 * ( frameSet.ref.size() + frameSet.name.size() ) );
    result.frameSetDenominators.reserve( sourceFrameSets.size() );
    for( const auto& frameSet : sourceFrameSets )
    {
        checkCancelled();
        const auto found = completeFramesBySet.find( frameSet.ref );
        result.frameSetDenominators.push_back( { frameSet.ref, frameSet.name,
            frameSet.continuous, found == completeFramesBySet.end() ? 0 : found->second } );
    }
    std::sort( completeFrames.begin(), completeFrames.end(), [&]( const auto& lhs, const auto& rhs ) {
        checkCancelled();
        if( lhs.beginNs != rhs.beginNs ) return lhs.beginNs < rhs.beginNs;
        if( lhs.endNs != rhs.endNs ) return lhs.endNs < rhs.endNs;
        return lhs.ref < rhs.ref;
    } );
    std::map<std::string, std::vector<FrameBoundary*>> framesBySet;
    for( auto& frame : completeFrames ) { checkCancelled(); framesBySet[frame.frameSetRef].push_back( &frame ); }
    std::vector<std::string> frameSetRefs;
    frameSetRefs.reserve( framesBySet.size() );
    for( auto& [frameSetRef, frames] : framesBySet )
    {
        checkCancelled();
        if( frameSetRefs.size() >= std::numeric_limits<uint32_t>::max() )
            throw BoundedScanError( "frame_cpu_scan_frame_set_limit" );
        const auto ordinal = uint32_t( frameSetRefs.size() );
        frameSetRefs.emplace_back( frameSetRef );
        for( auto* frame : frames ) { checkCancelled(); frame->frameSetOrdinal = ordinal; }
    }

    struct ThreadRoleInfo
    {
        std::string name;
        uint8_t cacheIndex = 3;
    };
    std::unordered_map<std::string, ThreadRoleInfo> threadRoles;
    for( const auto& thread : m_source.GetThreads() )
    {
        checkCancelled();
        metadataWorkspace.Add( 2048 + 8 * ( thread.ref.size() + thread.name.size() + TextBytes( thread.externalProcessName ) +
            TextBytes( thread.externalThreadName ) + TextBytes( thread.localName ) + thread.groupHintAvailability.reason.size() ) );
        auto role = ThreadRole( thread );
        threadRoles[thread.ref] = { role, ThreadRoleCacheIndex( role ) };
    }

    // Stable-site logical signatures depend only on the thread role and a
    // non-empty SourceLocation reference.  A real HighEvidence capture has
    // millions of zone instances but usually only thousands of locations;
    // compute SHA-256 once per unique site instead of once per occurrence.
    std::array<std::unordered_map<std::string, std::string>, 4> stableLogicalSignatures;

    AnalysisDictionary dictionary( options.workspace );
    struct SignatureEntry
    {
        AnalysisDictionary::StringId signature;
        AnalysisDictionary::PathId path;
        CpuWorkClass workClass;
    };
    std::map<std::string_view, uint32_t, std::less<>> signatureIndex;
    std::vector<SignatureEntry> signatureEntries;
    std::map<RunKey, size_t> runIndex;
    std::unordered_set<uint64_t> streamedPresence;
    std::unordered_map<std::string, WorkingStack> stacks;

    auto finding = [&]( std::string code, std::string message, const std::string& ref ) {
        result.qualityComplete = false;
        auto it = std::find_if( result.qualityFindings.begin(), result.qualityFindings.end(), [&]( const auto& value ) {
            return value.code == code;
        } );
        if( it == result.qualityFindings.end() )
        {
            result.outputWorkspace.Add( 1024 + 4 * ( code.size() + message.size() ) );
            result.qualityFindings.push_back( { std::move( code ), std::move( message ), 0, {} } );
            it = std::prev( result.qualityFindings.end() );
        }
        ++it->count;
        if( it->representativeRefs.size() < RepresentativeLimit &&
            std::find( it->representativeRefs.begin(), it->representativeRefs.end(), ref ) == it->representativeRefs.end() )
            result.outputWorkspace.Add( 128 + 4 * ref.size() );
        AddRepresentative( it->representativeRefs, ref );
    };

    auto ensureDefinition = [&]( const CpuZoneDto& zone, bool logical, const std::string& id,
        const std::string& parent, const std::string& role, uint32_t depth,
        AnalysisDictionary::PathId parentPath ) -> uint32_t {
        checkCancelled();
        const auto found = signatureIndex.find( id );
        if( found != signatureIndex.end() ) return uint32_t( found->second );
        if( signatureEntries.size() >= std::numeric_limits<uint32_t>::max() )
            throw BoundedScanError( "frame_cpu_scan_signature_limit" );
        signatureWorkspace.Add(256); // map node plus transient vector growth
        const auto ordinal = uint32_t( signatureEntries.size() );
        const auto signature = dictionary.Intern( id );
        const auto leaf = dictionary.Intern( zone.name.empty() ? zone.function : zone.name );
        const auto path = dictionary.AppendPath( parentPath, leaf );
        AnalysisWorkspaceReservation definitionWorkspace( options.workspace, 2048 + 8ull *
            ( id.size() + parent.size() + zone.name.size() + zone.sourceLocationRef.size() +
              zone.function.size() + zone.file.size() + zone.threadRef.size() + role.size() ) );
        const auto workClass = Classify( zone );
        signatureEntries.push_back( { signature, path, workClass } );
        signatureIndex.emplace( dictionary.Text( signature ), ordinal );
        auto resolved = dictionary.ResolvePath( path );
        CpuSignatureDefinition definition { id, parent, zone.name, zone.sourceLocationRef,
            zone.function, zone.file, zone.line, logical ? std::string {} : zone.threadRef,
            role, std::move( resolved.text ), depth, workClass, logical };
        if( options.definitionSink )
        {
            if( !options.definitionSink( definition ) )
                throw std::runtime_error( "frame_cpu_scan_definition_sink_cancelled" );
            checkCancelled();
        }
        else
        {
            result.outputWorkspace.Add( definitionWorkspace.Bytes() + 4 * definition.path.size() );
            result.signatures.push_back( std::move( definition ) );
        }
        return ordinal;
    };

    auto updateRun = [&]( const std::string& signature, uint32_t signatureOrdinal,
        const FrameContribution& contribution, int64_t directChildUnion,
        bool exact, const std::string& ref ) {
        if( signature.empty() ) return;
        AnalysisWorkspaceReservation currentRun( options.workspace, 1024 + 4 *
            ( signature.size() + contribution.frame->frameSetRef.size() + contribution.frame->ref.size() + ref.size() ) );
        const auto inclusive = contribution.endNs - contribution.beginNs;
        if( sink || viewSink )
        {
            const auto workClass = signatureEntries[signatureOrdinal].workClass;
            bool accepted = true;
            if( viewSink )
            {
                const CpuSignatureFrameRunView run { signature,
                    contribution.frame->frameSetRef, contribution.frame->ref,
                    signatureOrdinal, contribution.frame->frameSetOrdinal,
                    contribution.frame->index, inclusive, directChildUnion,
                    inclusive - directChildUnion, 1,
                    exact && directChildUnion <= inclusive, ref, workClass };
                accepted = ( *viewSink )( run );
            }
            else
            {
                const CpuSignatureFrameRun run { signature, contribution.frame->frameSetRef,
                    contribution.frame->ref, contribution.frame->index, inclusive,
                    directChildUnion, inclusive - directChildUnion, 1,
                    exact && directChildUnion <= inclusive, { ref }, workClass };
                accepted = ( *sink )( run );
            }
            if( !accepted ) throw std::runtime_error( "frame_cpu_scan_sink_cancelled" );
            checkCancelled();
            const auto presence = ( uint64_t( signatureOrdinal ) << 32 ) | contribution.frame->frameSetOrdinal;
            if( !streamedPresence.contains( presence ) )
            {
                presenceWorkspace.Add( 128 );
                streamedPresence.emplace( presence );
            }
            return;
        }
        const RunKey key { signature, contribution.frame->ref };
        if( !runIndex.contains( key ) ) runWorkspace.Add( 256 + 4 * ( key.signature.size() + key.frame.size() ) );
        auto [it, inserted] = runIndex.emplace( key, result.runs.size() );
        if( inserted )
        {
            result.outputWorkspace.Add( 1024 + 4 * ( signature.size() + contribution.frame->frameSetRef.size() + contribution.frame->ref.size() ) );
            result.runs.push_back( { signature, contribution.frame->frameSetRef, contribution.frame->ref,
                contribution.frame->index, 0, 0, 0, 0, true, {} } );
        }
        auto& run = result.runs[it->second];
        run.inclusiveNs += inclusive;
        run.directChildUnionNs += directChildUnion;
        run.exclusiveNs += inclusive - directChildUnion;
        ++run.occurrenceCount;
        run.exact = run.exact && exact && directChildUnion <= inclusive;
        run.workClass = signatureEntries[signatureIndex.at(signature)].workClass;
        if( run.representativeEventRefs.size() < RepresentativeLimit &&
            std::find( run.representativeEventRefs.begin(), run.representativeEventRefs.end(), ref ) == run.representativeEventRefs.end() )
            result.outputWorkspace.Add( 128 + 4 * ref.size() );
        AddRepresentative( run.representativeEventRefs, ref );
    };

    auto closeTop = [&]( std::vector<WorkingZone>& stack ) {
        checkCancelled();
        auto zone = std::move( stack.back() );
        stack.pop_back();
        for( const auto& contribution : zone.frames )
        {
            checkCancelled();
            const auto childIt = zone.directChildren.find( contribution.frame->ref );
            const auto childUnion = childIt == zone.directChildren.end() ? 0 : IntervalUnion( childIt->second, options.cancelled );
            auto exact = zone.exact;
            if( childUnion > contribution.endNs - contribution.beginNs )
            {
                exact = false;
                finding( "child_union_exceeds_parent", "Direct child interval union exceeds its clipped parent interval.", zone.ref );
            }
            if( options.includeExactSignatures )
                updateRun( zone.exactSignature, zone.exactSignatureOrdinal,
                    contribution, childUnion, exact, zone.ref );
            if( options.includeLogicalSignatures )
                updateRun( zone.logicalSignature, zone.logicalSignatureOrdinal,
                    contribution, childUnion, exact, zone.ref );
        }
    };

    BoundedScanRequest zoneRequest;
    zoneRequest.domain = BoundedScanDomain::CpuZone;
    zoneRequest.limit = m_batchSize;
    for( ;; )
    {
        checkCancelled();
        AnalysisWorkspaceReservation pageWorkspace( options.workspace );
        const auto batch = scanner.Read( zoneRequest );
        checkCancelled();
        result.maximumBatchObserved = std::max( result.maximumBatchObserved, batch.Count() );
        const auto& zones = std::get<std::vector<CpuZoneDto>>( batch.records );
        for( const auto& zone : zones ) { checkCancelled(); pageWorkspace.Add( ZoneBytes( zone ) ); }
        for( const auto& zone : zones )
        {
            checkCancelled();
            ++result.inputZoneCount;
            if( !stacks.contains( zone.threadRef ) ) metadataWorkspace.Add( 1024 + 4 * zone.threadRef.size() );
            auto& storage = stacks.try_emplace( zone.threadRef, options.workspace ).first->second;
            auto& stack = storage.zones;

            size_t parentPosition = std::numeric_limits<size_t>::max();
            if( zone.parentRef )
            {
                for( size_t i = stack.size(); i > 0; --i )
                {
                    checkCancelled();
                    if( stack[i-1].ref == *zone.parentRef )
                    {
                        parentPosition = i - 1;
                        break;
                    }
                }
                if( parentPosition != std::numeric_limits<size_t>::max() )
                {
                    while( stack.size() > parentPosition + 1 ) closeTop( stack );
                }
            }
            else
            {
                while( !stack.empty() ) closeTop( stack );
            }

            const bool timingValid = zone.complete && zone.endNs && zone.timingValid && *zone.endNs >= zone.startNs;
            if( !timingValid )
            {
                ++result.invalidZoneCount;
                finding( zone.endNs ? "invalid_zone_timing" : "missing_zone_end",
                    zone.timingInvalidReason.value_or( zone.endNs ? "Zone end precedes its begin." : "Zone has no closing timestamp." ), zone.ref );
                if( parentPosition != std::numeric_limits<size_t>::max() && !stack.empty() ) stack.back().exact = false;
                continue;
            }

            const bool parentResolved = !zone.parentRef || parentPosition != std::numeric_limits<size_t>::max();
            bool structuralExact = true;
            if( zone.parentRef && !parentResolved )
            {
                structuralExact = false;
                finding( "missing_parent_zone", "Parent zone was not available on the same bounded thread stack.", zone.ref );
            }

            ++result.validZoneCount;
            const auto roleIt = threadRoles.find( zone.threadRef );
            const auto role = roleIt == threadRoles.end() ? std::string( "Other" ) : roleIt->second.name;
            const auto roleCacheIndex = roleIt == threadRoles.end() ? uint8_t( 3 ) : roleIt->second.cacheIndex;
            const auto parentExact = options.includeExactSignatures && !stack.empty() &&
                zone.parentRef && parentResolved ? stack.back().exactSignature : std::string {};
            const auto stableLogicalSite =
                options.logicalSignatureMode == CpuLogicalSignatureMode::StableSite;
            const auto parentLogical = options.includeLogicalSignatures && !stableLogicalSite &&
                !stack.empty() && zone.parentRef && parentResolved ?
                stack.back().logicalSignature : std::string {};
            const auto exactId = options.includeExactSignatures ?
                SignatureId( false, zone.threadRef, parentExact, zone ) : std::string {};
            std::string logicalId;
            if( options.includeLogicalSignatures )
            {
                if( stableLogicalSite && !zone.sourceLocationRef.empty() )
                {
                    auto& cache = stableLogicalSignatures[roleCacheIndex];
                    const auto found = cache.find( zone.sourceLocationRef );
                    if( found != cache.end() ) logicalId = found->second;
                    else
                    {
                        logicalId = SignatureId( true, role, parentLogical, zone, true );
                        signatureWorkspace.Add(128+4ull*(zone.sourceLocationRef.size()+logicalId.size()));
                        cache.emplace( zone.sourceLocationRef, logicalId );
                    }
                }
                else logicalId = SignatureId( true, role, parentLogical, zone, stableLogicalSite );
            }
            const auto depth = zone.parentRef && parentResolved && !stack.empty() ? uint32_t( stack.size() ) : 0u;
            result.maximumDepth = std::max( result.maximumDepth, size_t( depth ) );
            const auto exactParentPath = parentExact.empty() ? 0 :
                signatureEntries[signatureIndex.at(parentExact)].path;
            const auto logicalParentPath = parentLogical.empty() ? 0 :
                signatureEntries[signatureIndex.at(parentLogical)].path;
            uint32_t exactSignatureOrdinal = 0;
            uint32_t logicalSignatureOrdinal = 0;
            if( options.includeExactSignatures )
                exactSignatureOrdinal = ensureDefinition( zone, false, exactId, parentExact, role, depth,
                    exactParentPath );
            if( options.includeLogicalSignatures )
                logicalSignatureOrdinal = ensureDefinition( zone, true, logicalId, parentLogical, role, depth,
                    stableLogicalSite ? 0 : logicalParentPath );

            WorkingZone active;
            active.workspace = AnalysisWorkspaceReservation( options.workspace, 2048 + 8 *
                ( zone.ref.size() + zone.threadRef.size() + exactId.size() + logicalId.size() ) );
            active.ref = zone.ref;
            active.exactSignature = exactId;
            active.logicalSignature = logicalId;
            active.exactSignatureOrdinal = exactSignatureOrdinal;
            active.logicalSignatureOrdinal = logicalSignatureOrdinal;
            active.threadRef = zone.threadRef;
            active.beginNs = zone.startNs;
            active.endNs = *zone.endNs;
            active.exact = structuralExact;
            for( const auto& [frameSet, candidates] : framesBySet )
            {
                checkCancelled();
                size_t first = 0;
                size_t last = candidates.size();
                while( first < last )
                {
                    ++result.frameBoundaryProbeCount;
                    const auto middle = first + ( last - first ) / 2;
                    if( candidates[middle]->endNs <= zone.startNs ) first = middle + 1;
                    else last = middle;
                }
                for( auto index = first; index < candidates.size(); ++index )
                {
                    checkCancelled();
                    ++result.frameBoundaryProbeCount;
                    const auto& frame = *candidates[index];
                    if( frame.beginNs >= *zone.endNs ) break;
                    const auto begin = std::max( zone.startNs, frame.beginNs );
                    const auto end = std::min( *zone.endNs, frame.endNs );
                    if( end > begin )
                    {
                        active.workspace.Add( 128 );
                        active.frames.push_back( { &frame, begin, end } );
                    }
                }
            }
            if( active.frames.empty() )
            {
                ++result.unassignedZoneCount;
                finding( "zone_unassigned_to_complete_frame",
                    "A valid zone does not intersect any complete frame and is excluded from exact frame denominators.", zone.ref );
            }

            if( zone.parentRef && parentResolved && !stack.empty() )
            {
                auto& parent = stack.back();
                if( zone.startNs < parent.beginNs || *zone.endNs > parent.endNs )
                {
                    parent.exact = false;
                    active.exact = false;
                    finding( "child_outside_parent", "Child zone is not contained by its declared parent interval.", zone.ref );
                }
                for( const auto& parentFrame : parent.frames )
                {
                    checkCancelled();
                    const auto begin = std::max( zone.startNs, parentFrame.beginNs );
                    const auto end = std::min( *zone.endNs, parentFrame.endNs );
                    if( end > begin )
                    {
                        if( !parent.directChildren.contains( parentFrame.frame->ref ) )
                            parent.workspace.Add( 256 + 4 * parentFrame.frame->ref.size() );
                        parent.workspace.Add( 64 ); // capacity plus IntervalUnion's current copy
                        parent.directChildren[parentFrame.frame->ref].emplace_back( begin, end );
                    }
                }
            }
            storage.ReserveNext();
            stack.emplace_back( std::move( active ) );
        }
        if( batch.cursor.complete ) break;
        zoneRequest.cursor = batch.cursor;
    }

    for( auto& [thread, storage] : stacks )
        while( !storage.zones.empty() ) closeTop( storage.zones );

    if( sink || viewSink )
    {
        for( const auto key : streamedPresence )
        {
            checkCancelled();
            const auto signatureOrdinal = uint32_t( key >> 32 );
            const auto frameSetOrdinal = uint32_t( key );
            if( signatureOrdinal >= signatureEntries.size() || frameSetOrdinal >= frameSetRefs.size() )
                throw BoundedScanError( "frame_cpu_scan_stream_presence_invalid" );
            const auto signature = dictionary.Text( signatureEntries[signatureOrdinal].signature );
            const auto& frameSet = frameSetRefs[frameSetOrdinal];
            result.outputWorkspace.Add( 512 + 4 * ( signature.size() + frameSet.size() ) );
            result.denominators.push_back( { std::string(signature), frameSet,
                completeFramesBySet[frameSet], 0 } );
        }
    }
    else
    {
        AnalysisWorkspaceReservation presentWorkspace( options.workspace );
        std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> present;
        for( const auto& run : result.runs )
        {
            checkCancelled();
            const auto signature = present.find( run.signatureId );
            if( signature == present.end() || !signature->second.contains( run.frameSetRef ) )
                presentWorkspace.Add( 512 + 4 * ( run.signatureId.size() + run.frameSetRef.size() ) );
            ++present[run.signatureId][run.frameSetRef];
        }
        for( const auto& [signature, bySet] : present )
            for( const auto& [frameSet, count] : bySet )
            {
                checkCancelled();
                result.outputWorkspace.Add( 512 + 4 * ( signature.size() + frameSet.size() ) );
                result.denominators.push_back( { signature, frameSet,
                    completeFramesBySet[frameSet], count } );
            }
    }
    std::sort( result.denominators.begin(), result.denominators.end(), [&]( const auto& lhs, const auto& rhs ) {
        checkCancelled();
        if( lhs.signatureId != rhs.signatureId ) return lhs.signatureId < rhs.signatureId;
        return lhs.frameSetRef < rhs.frameSetRef;
    } );
    checkCancelled();
    return result;
}

const char* CpuWorkClassName( CpuWorkClass value )
{
    switch( value )
    {
    case CpuWorkClass::ActiveWork: return "active_work";
    case CpuWorkClass::Wait: return "wait";
    case CpuWorkClass::IntentionalPacing: return "intentional_pacing";
    case CpuWorkClass::Unknown: return "unknown";
    }
    return "unknown";
}

}
