#include "TracyGpuJobManagedScanner.hpp"
#include "TracyAnalysisDictionary.hpp"

#include "TracyHash.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace tracy::analysis
{
namespace
{

constexpr size_t RepresentativeLimit = 8;

void CheckCancelled( const std::function<bool()>& cancelled )
{
    if( cancelled && cancelled() ) throw BoundedScanError( "gpu_job_managed_scan_cancelled" );
}

struct FrameBoundary
{
    std::string ref;
    std::string frameSetRef;
    int64_t beginNs = 0;
    int64_t endNs = 0;
};

struct GpuLookupKey
{
    uint32_t context = 0;
    uint16_t query = 0;

    bool operator==( const GpuLookupKey& other ) const
    {
        return context == other.context && query == other.query;
    }

    bool operator<( const GpuLookupKey& other ) const
    {
        return std::tie( context, query ) < std::tie( other.context, other.query );
    }
};

struct GpuLookupHash
{
    size_t operator()( const GpuLookupKey& key ) const
    {
        return ( size_t( key.context ) << 16 ) ^ key.query;
    }
};

struct WorkingGpuZone
{
    AnalysisWorkspaceReservation workspace;
    std::string ref;
    std::string parentRef;
    std::string signature;
    std::string logicalSignature;
    std::string contextRef;
    GpuQueueClass queueClass = GpuQueueClass::Unknown;
    int64_t beginNs = 0;
    int64_t endNs = 0;
    uint32_t depth = 0;
    bool exact = true;
    bool timingValid = true;
    bool subtreeComplete = true;
    uint64_t l0SegmentOrdinal = 0;
    std::vector<std::pair<int64_t, int64_t>> directChildren;
    std::set<uint64_t> ownFrameIds;
    std::set<uint64_t> descendantFrameIds;
};

struct WorkingGpuStack
{
    AnalysisWorkspaceReservation workspace;
    std::vector<WorkingGpuZone> zones;
    explicit WorkingGpuStack( std::shared_ptr<AnalysisWorkspaceBudget> budget = {} )
        : workspace( std::move( budget ) ) {}
    void ReserveNext()
    {
        if( zones.size() != zones.capacity() ) return;
        const auto capacity = std::max<size_t>( 64, zones.capacity() * 2 );
        if( capacity > UINT64_MAX / ( 2 * sizeof( WorkingGpuZone ) ) )
            throw std::runtime_error( "analysis_workspace_budget" );
        workspace.Resize( uint64_t( capacity ) * 2 * sizeof( WorkingGpuZone ) );
        zones.reserve( capacity );
    }
};

struct ScriptStackBuild
{
    uint64_t id = 0;
    uint8_t runtime = 0;
    uint32_t expected = 0;
    bool hasHeader = false;
    std::string marker;
    std::vector<uint32_t> frameIds;
};

struct QueryEntity
{
    GpuLookupKey key;
    uint64_t entity = 0;
    bool operator<( const QueryEntity& other ) const
    {
        if( key < other.key ) return true;
        if( other.key < key ) return false;
        return entity < other.entity;
    }
};

struct EntityFrame
{
    uint64_t entity = 0;
    uint64_t frame = 0;
    bool operator<( const EntityFrame& other ) const
    {
        return std::tie( entity, frame ) < std::tie( other.entity, other.frame );
    }
    bool operator==( const EntityFrame& other ) const = default;
};

struct RelationTarget
{
    uint64_t source = 0;
    uint64_t target = 0;
    uint8_t nameSpace = 0;
    uint8_t relation = 0;
    bool operator<( const RelationTarget& other ) const
    {
        return std::tie( source, nameSpace, relation, target ) <
            std::tie( other.source, other.nameSpace, other.relation, other.target );
    }
};

struct RelationKey
{
    uint64_t source = 0;
    uint8_t nameSpace = 0;
    uint8_t relation = 0;
    bool operator<( const RelationKey& other ) const
    {
        return std::tie( source, nameSpace, relation ) <
            std::tie( other.source, other.nameSpace, other.relation );
    }
    bool operator==( const RelationKey& other ) const = default;
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

GpuQueueClass QueueClass( const GpuContextDto& context )
{
    const auto value = Lower( context.name + " " + context.typeName + " " + context.customName.value_or( "" ) );
    if( Contains( value, "copy" ) ) return GpuQueueClass::Copy;
    if( Contains( value, "compute" ) ) return GpuQueueClass::Compute;
    if( Contains( value, "direct" ) || Contains( value, "graphics" ) ) return GpuQueueClass::Direct;
    return GpuQueueClass::Unknown;
}

void HashString( Sha256Builder& hash, std::string_view value )
{
    const auto size = uint64_t( value.size() );
    hash.Update( &size, sizeof( size ) );
    if( !value.empty() ) hash.Update( value.data(), value.size() );
}

std::string SignatureId( bool logical, std::string_view queueIdentity,
    std::string_view parent, const GpuZoneDto& zone, bool stableSite = false )
{
    Sha256Builder hash;
    const uint8_t kind = logical ? 1 : 0;
    hash.Update( &kind, sizeof( kind ) );
    HashString( hash, queueIdentity );
    if( !stableSite ) HashString( hash, parent );
    HashString( hash, zone.sourceLocationRef );
    if( !stableSite || zone.sourceLocationRef.empty() )
    {
        HashString( hash, zone.name );
        HashString( hash, zone.function );
        HashString( hash, zone.file );
        hash.Update( &zone.line, sizeof( zone.line ) );
    }
    return std::string( logical ? "gpu-logical:" : "gpu-exact:" ) + hash.FinalHex();
}

int64_t IntervalUnion( std::vector<std::pair<int64_t, int64_t>> intervals,
    const std::function<bool()>& cancelled )
{
    CheckCancelled( cancelled );
    if( intervals.empty() ) return 0;
    std::sort( intervals.begin(), intervals.end(), [&]( const auto& a, const auto& b ) {
        CheckCancelled( cancelled ); return a < b;
    } );
    auto begin = intervals.front().first;
    auto end = intervals.front().second;
    int64_t total = 0;
    for( size_t i = 1; i < intervals.size(); ++i )
    {
        CheckCancelled( cancelled );
        if( intervals[i].first <= end ) end = std::max( end, intervals[i].second );
        else
        {
            total += end - begin;
            begin = intervals[i].first;
            end = intervals[i].second;
        }
    }
    return total + end - begin;
}

bool IsSingleValuedRelation( uint8_t relation )
{
    return relation == uint8_t( JnRelationKind::LogicalParent ) ||
        relation == uint8_t( JnRelationKind::OwnedBy ) ||
        relation == uint8_t( JnRelationKind::ContinuesAs );
}

template<typename T, typename Callback>
void ReadBatches( const BoundedTraceScanner& scanner, BoundedScanDomain domain,
    size_t batchSize, size_t& maximumBatch, const std::function<bool()>& cancelled, Callback&& callback )
{
    BoundedScanRequest request;
    request.domain = domain;
    request.limit = batchSize;
    for( ;; )
    {
        CheckCancelled( cancelled );
        const auto batch = scanner.Read( request );
        CheckCancelled( cancelled );
        maximumBatch = std::max( maximumBatch, batch.Count() );
        const auto& values = std::get<std::vector<T>>( batch.records );
        for( const auto& value : values )
        {
            CheckCancelled( cancelled );
            callback( value );
        }
        CheckCancelled( cancelled );
        if( batch.cursor.complete ) break;
        request.cursor = batch.cursor;
    }
}

}

GpuJobManagedScanner::GpuJobManagedScanner( const TraceSource& source, size_t batchSize )
    : m_source( source )
    , m_batchSize( batchSize )
{
    if( batchSize == 0 || batchSize > NativeBoundedScanMaximumBatch )
        throw BoundedScanError( "gpu_job_managed_scan_batch_out_of_range" );
}

GpuJobManagedScanResult GpuJobManagedScanner::Scan( const GpuJobManagedScanOptions& options ) const
{
    const auto checkCancelled = [&] { CheckCancelled( options.cancelled ); };
    checkCancelled();
    if( !options.includeExactGpuSignatures && !options.includeLogicalGpuSignatures )
        throw BoundedScanError( "gpu_scan_signature_mode_empty" );
    GpuJobManagedScanResult result;
    result.outputWorkspace = AnalysisWorkspaceReservation( options.workspace );
    AnalysisWorkspaceReservation metadataWorkspace( options.workspace );
    AnalysisWorkspaceReservation managedWorkspace( options.workspace );
    BoundedTraceScanner scanner( m_source );

    auto report = [&]( std::string code, std::string message, const std::string& ref ) {
        result.qualityComplete = false;
        auto it = std::find_if( result.qualityFindings.begin(), result.qualityFindings.end(), [&]( const auto& value ) {
            return value.code == code;
        } );
        if( it == result.qualityFindings.end() )
        {
            result.outputWorkspace.Add( 1024 + 4ull * ( code.size() + message.size() ) );
            result.qualityFindings.push_back( { std::move( code ), std::move( message ), 0, {} } );
            it = std::prev( result.qualityFindings.end() );
        }
        ++it->count;
        if( it->representativeRefs.size() < RepresentativeLimit )
        {
            result.outputWorkspace.Add( 128 + 4ull * ref.size() );
            it->representativeRefs.emplace_back( ref );
        }
    };

    std::vector<FrameBoundary> frames;
    ReadBatches<FrameDto>( scanner, BoundedScanDomain::Frame,
        m_batchSize, result.maximumBatchObserved, options.cancelled, [&]( const auto& frame ) {
        if( frame.complete && frame.endNs && *frame.endNs > frame.beginNs )
        {
            metadataWorkspace.Add( 512 + 8ull * ( frame.ref.size() + frame.frameSetRef.size() ) );
            frames.push_back( { frame.ref, frame.frameSetRef, frame.beginNs, *frame.endNs } );
        }
    } );
    std::sort( frames.begin(), frames.end(), [&]( const auto& left, const auto& right ) {
        checkCancelled();
        return std::tie( left.frameSetRef, left.beginNs, left.endNs, left.ref ) <
            std::tie( right.frameSetRef, right.beginNs, right.endNs, right.ref );
    } );
    std::map<std::string, std::vector<const FrameBoundary*>> framesBySet;
    for( const auto& frame : frames ) { checkCancelled(); framesBySet[frame.frameSetRef].push_back( &frame ); }

    std::unordered_map<std::string, uint32_t> contextIndexes;
    std::unordered_map<std::string, GpuQueueClass> contextClasses;
    std::unordered_map<std::string, bool> contextCalibrated;
    for( const auto& context : m_source.GetGpuContexts() )
    {
        checkCancelled();
        metadataWorkspace.Add( 1024 + 8ull * ( context.ref.size() + context.name.size() +
            context.typeName.size() + ( context.customName ? context.customName->size() : 0 ) ) );
        contextIndexes[context.ref] = uint32_t( context.index );
        contextClasses[context.ref] = QueueClass( context );
        contextCalibrated[context.ref] = context.calibrated;
    }

    std::vector<QueryEntity> entitiesByQuery;
    ReadBatches<GfxEntityDto>( scanner, BoundedScanDomain::GfxEntity,
        m_batchSize, result.maximumBatchObserved, options.cancelled, [&]( const auto& entity ) {
        if( entity.gpuQueryId == 0 ) return;
        metadataWorkspace.Add( 128 );
        entitiesByQuery.push_back( { { entity.gpuContext, uint16_t( entity.gpuQueryId ) }, entity.entityId } );
    } );
    const auto cancellableLess = [&]( const auto& a, const auto& b ) { checkCancelled(); return a < b; };
    std::sort( entitiesByQuery.begin(), entitiesByQuery.end(), cancellableLess );

    std::vector<EntityFrame> framesByEntity;
    ReadBatches<GfxLinkDto>( scanner, BoundedScanDomain::GfxLink,
        m_batchSize, result.maximumBatchObserved, options.cancelled, [&]( const auto& link ) {
        if( link.relation == uint8_t( JnGfxRelation::BelongsToFrame ) )
        {
            metadataWorkspace.Add( 128 );
            framesByEntity.push_back( { link.sourceId, link.targetId } );
        }
    } );
    std::vector<RelationTarget> singleValueTargets;
    ReadBatches<RelationDto>( scanner, BoundedScanDomain::Relation,
        m_batchSize, result.maximumBatchObserved, options.cancelled, [&]( const auto& relation ) {
        AnalysisWorkspaceReservation currentRelation( options.workspace, 512 + 4ull * relation.ref.size() );
        if( relation.sourceKind == uint8_t( JnEntityKind::GpuPass ) &&
            relation.targetKind == uint8_t( JnEntityKind::Frame ) )
        {
            metadataWorkspace.Add( 128 );
            framesByEntity.push_back( { relation.sourceId, relation.targetId } );
        }
        if( IsSingleValuedRelation( relation.relation ) )
        {
            metadataWorkspace.Add( 256 ); // Includes the later ambiguity-key vector.
            singleValueTargets.push_back( { relation.sourceId, relation.targetId,
                relation.relationNamespace, relation.relation } );
        }
    } );
    std::sort( framesByEntity.begin(), framesByEntity.end(), cancellableLess );
    framesByEntity.erase( std::unique( framesByEntity.begin(), framesByEntity.end(), [&]( const auto& a, const auto& b ) {
        checkCancelled(); return a == b;
    } ), framesByEntity.end() );
    std::sort( singleValueTargets.begin(), singleValueTargets.end(), cancellableLess );
    std::vector<RelationKey> ambiguousRelationKeys;
    for( size_t first = 0; first < singleValueTargets.size(); )
    {
        checkCancelled();
        size_t last = first + 1;
        while( last < singleValueTargets.size() &&
            singleValueTargets[last].source == singleValueTargets[first].source &&
            singleValueTargets[last].nameSpace == singleValueTargets[first].nameSpace &&
            singleValueTargets[last].relation == singleValueTargets[first].relation ) { checkCancelled(); ++last; }
        if( std::adjacent_find( singleValueTargets.begin() + first,
            singleValueTargets.begin() + last, [&]( const auto& left, const auto& right ) {
                checkCancelled();
                return left.target != right.target;
            } ) != singleValueTargets.begin() + last )
            ambiguousRelationKeys.push_back( { singleValueTargets[first].source,
                singleValueTargets[first].nameSpace, singleValueTargets[first].relation } );
        first = last;
    }
    singleValueTargets.clear();
    singleValueTargets.shrink_to_fit();

    AnalysisDictionary dictionary( options.workspace );
    AnalysisWorkspaceReservation signatureWorkspace( options.workspace );
    std::map<std::string_view, AnalysisDictionary::PathId, std::less<>> signatureIndex;
    std::unordered_map<std::string, WorkingGpuStack> stacks;
    std::unordered_map<std::string, uint64_t> nextL0Ordinal;
    const auto addEntityFrames = [&]( uint64_t entity, WorkingGpuZone& active ) {
        auto found = std::lower_bound( framesByEntity.begin(), framesByEntity.end(),
            EntityFrame { entity, 0 } );
        while( found != framesByEntity.end() && found->entity == entity )
        {
            checkCancelled();
            if( !active.ownFrameIds.contains( found->frame ) )
            {
                active.workspace.Add( 128 );
                active.ownFrameIds.insert( found->frame );
            }
            ++found;
        }
    };
    auto ensureDefinition = [&]( const GpuZoneDto& zone, bool logical, const std::string& signature,
        const std::string& parent, GpuQueueClass queueClass, uint32_t depth ) {
        checkCancelled();
        if( signatureIndex.find( signature ) != signatureIndex.end() ) return;
        signatureWorkspace.Add( 256 );
        AnalysisWorkspaceReservation currentDefinition( options.workspace, 2048 + 8ull *
            ( signature.size() + parent.size() + zone.name.size() + zone.sourceLocationRef.size() +
              zone.function.size() + zone.file.size() + zone.contextRef.size() ) );
        AnalysisDictionary::PathId parentPath = 0;
        if( !parent.empty() )
        {
            const auto found = signatureIndex.find( parent );
            if( found == signatureIndex.end() ) throw BoundedScanError( "gpu_signature_parent_missing" );
            parentPath = found->second;
        }
        const std::string_view leaf = zone.name.empty() ? zone.function : zone.name;
        const auto path = dictionary.AppendPath( parentPath, dictionary.Intern( leaf ) );
        signatureIndex.emplace( dictionary.Text( dictionary.Intern( signature ) ), path );
        auto resolved = dictionary.ResolvePath( path );
        GpuSignatureDefinition definition { signature, parent, zone.name, zone.sourceLocationRef,
            zone.function, zone.file, zone.line, logical ? std::string {} : zone.contextRef,
            queueClass, std::move( resolved.text ), depth, logical };
        if( options.definitionSink )
        {
            if( !options.definitionSink( definition ) ) throw BoundedScanError( "gpu_definition_sink_cancelled" );
            checkCancelled();
        }
        else
        {
            result.outputWorkspace.Add( currentDefinition.Bytes() + 4ull * definition.path.size() );
            result.gpuSignatures.emplace_back( std::move( definition ) );
        }
    };

    auto closeTop = [&]( std::vector<WorkingGpuZone>& stack ) {
        checkCancelled();
        auto zone = std::move( stack.back() );
        stack.pop_back();
        if( !zone.subtreeComplete && !stack.empty() )
        {
            stack.back().subtreeComplete = false;
            // A deeper interval may extend into this ancestor's apparent
            // residual. Do not certify that residual using an incomplete tree.
            stack.back().exact = false;
        }
        GpuZoneScanFact fact;
        fact.zoneRef = zone.ref;
        fact.signatureId = zone.signature;
        fact.logicalSignatureId = zone.logicalSignature;
        fact.parentZoneRef = zone.parentRef;
        fact.contextRef = zone.contextRef;
        fact.queueClass = zone.queueClass;
        fact.beginNs = zone.beginNs;
        fact.endNs = zone.endNs;
        fact.inclusiveNs = zone.timingValid ? zone.endNs - zone.beginNs : 0;
        fact.directChildUnionNs = zone.timingValid ? IntervalUnion( zone.directChildren, options.cancelled ) : 0;
        fact.exclusiveNs = fact.inclusiveNs - fact.directChildUnionNs;
        fact.depth = zone.depth;
        fact.exact = zone.exact && fact.directChildUnionNs <= fact.inclusiveNs;
        fact.physicalTimingExact = fact.exact;
        fact.l0SegmentOrdinal = zone.l0SegmentOrdinal;
        fact.l0Complete = zone.subtreeComplete && fact.physicalTimingExact;
        if( fact.depth == 0 && !fact.l0Complete )
            report( "gpu_l0_incomplete", "GPU L0 is excluded from complete-segment distributions; independently valid Pass observations remain available.", zone.ref );

        if( zone.ownFrameIds.size() == 1 )
        {
            fact.frameEvidence = GpuFrameEvidence::ExactFrameRelation;
            fact.frameId = *zone.ownFrameIds.begin();
        }
        else if( zone.ownFrameIds.size() > 1 )
        {
            fact.exact = false;
            report( "gpu_frame_relation_ambiguous", "A GPU zone maps to multiple exact frame identities.", zone.ref );
        }
        else if( zone.descendantFrameIds.size() == 1 )
        {
            fact.frameEvidence = GpuFrameEvidence::InferredFromChildPasses;
            fact.frameId = *zone.descendantFrameIds.begin();
        }
        else if( zone.descendantFrameIds.size() > 1 )
        {
            fact.exact = false;
            report( "gpu_child_frame_relation_ambiguous", "Child passes map to multiple frame identities.", zone.ref );
        }

        if( !fact.frameId )
        {
            AnalysisWorkspaceReservation overlapWorkspace( options.workspace );
            std::vector<std::string> overlaps;
            if( contextCalibrated[zone.contextRef] )
            {
                for( const auto& [frameSet, candidates] : framesBySet )
                {
                    checkCancelled();
                    size_t first = 0, last = candidates.size();
                    while( first < last )
                    {
                        const auto middle = first + ( last - first ) / 2;
                        if( candidates[middle]->endNs <= zone.beginNs ) first = middle + 1;
                        else last = middle;
                    }
                    for( auto index = first; index < candidates.size(); ++index )
                    {
                        checkCancelled();
                        const auto& frame = *candidates[index];
                        if( frame.beginNs >= zone.endNs ) break;
                        if( std::min( zone.endNs, frame.endNs ) > std::max( zone.beginNs, frame.beginNs ) )
                        {
                            overlapWorkspace.Add( 128 + 4ull * frame.ref.size() );
                            overlaps.emplace_back( frame.ref );
                        }
                    }
                }
            }
            if( overlaps.size() == 1 )
            {
                fact.frameEvidence = GpuFrameEvidence::TemporalCandidate;
                fact.temporalFrameRef = overlaps.front();
                ++result.temporalGpuCandidateCount;
            }
            else
            {
                fact.frameEvidence = GpuFrameEvidence::Unassigned;
                ++result.unassignedGpuZoneCount;
                report( overlaps.empty() ? "gpu_zone_unassigned" : "gpu_temporal_frame_ambiguous",
                    overlaps.empty() ? "GPU zone has no exact, inferred, or temporal frame candidate." :
                    "GPU zone overlaps multiple CPU frame intervals; no deterministic frame is assigned.", zone.ref );
            }
        }

        if( !stack.empty() && fact.frameId && !stack.back().descendantFrameIds.contains( *fact.frameId ) )
        {
            stack.back().workspace.Add( 128 );
            stack.back().descendantFrameIds.insert( *fact.frameId );
        }
        ++result.gpuZoneCount;
        if( options.gpuZoneSink && !options.gpuZoneSink( fact ) )
            throw BoundedScanError( "gpu_zone_scan_sink_cancelled" );
        checkCancelled();
        if( options.retainDetails )
        {
            result.outputWorkspace.Add( 2048 + 8ull * ( fact.zoneRef.size() + fact.signatureId.size() +
                fact.logicalSignatureId.size() + fact.parentZoneRef.size() + fact.contextRef.size() +
                ( fact.temporalFrameRef ? fact.temporalFrameRef->size() : 0 ) ) );
            if( options.includeExactGpuSignatures )
                result.gpuTracks.push_back( { GpuAnalysisTrack::PhysicalQueueA, fact.zoneRef,
                    fact.signatureId, fact.contextRef, fact.queueClass, fact.inclusiveNs, fact.exclusiveNs,
                    fact.frameEvidence, std::nullopt, std::nullopt, fact.exact } );
            if( options.includeLogicalGpuSignatures )
                result.gpuTracks.push_back( { GpuAnalysisTrack::LogicalFrameB, fact.zoneRef,
                    fact.logicalSignatureId, fact.contextRef, fact.queueClass, fact.inclusiveNs, fact.exclusiveNs,
                    fact.frameEvidence, fact.frameId, fact.temporalFrameRef,
                    fact.exact && ( fact.frameEvidence == GpuFrameEvidence::ExactFrameRelation ||
                        fact.frameEvidence == GpuFrameEvidence::InferredFromChildPasses ) } );
            result.gpuZones.emplace_back( std::move( fact ) );
        }
    };

    ReadBatches<GpuZoneDto>( scanner, BoundedScanDomain::GpuZone,
        m_batchSize, result.maximumBatchObserved, options.cancelled, [&]( const auto& zone ) {
        auto stackEntry = stacks.find( zone.contextRef );
        if( stackEntry == stacks.end() )
        {
            metadataWorkspace.Add( 1024 + 8ull * zone.contextRef.size() );
            stackEntry = stacks.try_emplace( zone.contextRef, options.workspace ).first;
        }
        auto& state = stackEntry->second;
        auto& stack = state.zones;
        size_t parentPosition = std::numeric_limits<size_t>::max();
        if( zone.parentRef )
        {
            for( size_t i = stack.size(); i > 0; --i )
            {
                checkCancelled();
                if( stack[i-1].ref == *zone.parentRef ) { parentPosition = i - 1; break; }
            }
            if( parentPosition != std::numeric_limits<size_t>::max() )
                while( stack.size() > parentPosition + 1 ) closeTop( stack );
            else
                while( !stack.empty() ) closeTop( stack );
        }
        else
        {
            while( !stack.empty() ) closeTop( stack );
        }

        const bool timingValid = zone.complete && zone.gpuEndNs &&
            zone.gpuStartNs >= 0 && *zone.gpuEndNs >= zone.gpuStartNs;
        if( !timingValid )
        {
            ++result.invalidGpuZoneCount;
            report( zone.gpuEndNs ? "invalid_gpu_zone_timing" : "missing_gpu_zone_end",
                "Invalid GPU zone timing is excluded from exact duration facts.", zone.ref );
            // Keep the structural shell: later valid children still belong to
            // this actual recorded parent and this one L0 ordinal.
        }

        const bool parentResolved = !zone.parentRef || parentPosition != std::numeric_limits<size_t>::max();
        const auto queueIt = contextClasses.find( zone.contextRef );
        const auto queueClass = queueIt == contextClasses.end() ? GpuQueueClass::Unknown : queueIt->second;
        const auto stableLogicalSite =
            options.logicalSignatureMode == GpuLogicalSignatureMode::StableSite;
        const auto parentSignature = options.includeExactGpuSignatures && zone.parentRef &&
            parentResolved ? stack.back().signature : std::string {};
        const auto parentLogical = options.includeLogicalGpuSignatures && !stableLogicalSite &&
            zone.parentRef && parentResolved ? stack.back().logicalSignature : std::string {};
        const auto signature = options.includeExactGpuSignatures ?
            SignatureId( false, zone.contextRef, parentSignature, zone ) : std::string {};
        const auto logicalSignature = options.includeLogicalGpuSignatures ?
            SignatureId( true, GpuQueueClassName( queueClass ), parentLogical, zone,
                stableLogicalSite ) : std::string {};
        const auto depth = zone.parentRef && parentResolved ? uint32_t( stack.size() ) : 0u;
        result.maximumGpuDepth = std::max( result.maximumGpuDepth, size_t( depth ) );
        if( options.includeExactGpuSignatures )
            ensureDefinition( zone, false, signature, parentSignature, queueClass, depth );
        if( options.includeLogicalGpuSignatures )
            ensureDefinition( zone, true, logicalSignature, parentLogical, queueClass, depth );

        WorkingGpuZone active;
        active.workspace = AnalysisWorkspaceReservation( options.workspace, 2048 + 8ull *
            ( zone.ref.size() + ( zone.parentRef ? zone.parentRef->size() : 0 ) +
              signature.size() + logicalSignature.size() + zone.contextRef.size() ) );
        active.ref = zone.ref;
        active.parentRef = zone.parentRef.value_or( "" );
        active.signature = signature;
        active.logicalSignature = logicalSignature;
        active.contextRef = zone.contextRef;
        active.queueClass = queueClass;
        active.beginNs = zone.gpuStartNs;
        active.endNs = zone.gpuEndNs.value_or( -1 );
        active.depth = depth;
        active.timingValid = timingValid;
        active.exact = parentResolved && timingValid;
        active.subtreeComplete = parentResolved && timingValid;
        active.l0SegmentOrdinal = parentResolved && zone.parentRef ?
            stack.front().l0SegmentOrdinal : ++nextL0Ordinal[zone.contextRef];
        if( !parentResolved ) report( "missing_gpu_parent", "GPU parent zone is unavailable on the same queue stack.", zone.ref );

        const auto contextIt = contextIndexes.find( zone.contextRef );
        if( contextIt != contextIndexes.end() && zone.queryIdAvailability.available )
        {
            const GpuLookupKey key { contextIt->second, zone.queryId };
            auto found = std::lower_bound( entitiesByQuery.begin(), entitiesByQuery.end(), QueryEntity { key, 0 } );
            while( found != entitiesByQuery.end() && found->key == key )
            { checkCancelled(); addEntityFrames( found->entity, active ); ++found; }
        }
        if( const auto id = m_source.ParseEntityRef( zone.ref, "gpu-zone" ) )
            addEntityFrames( *id, active );

        if( zone.parentRef && parentResolved )
        {
            auto& parent = stack.back();
            if( !timingValid )
            {
                parent.exact = false; // Its residual cannot subtract an unknown child.
                parent.subtreeComplete = false;
            }
            if( timingValid && parent.timingValid &&
                ( zone.gpuStartNs < parent.beginNs || *zone.gpuEndNs > parent.endNs ) )
            {
                active.exact = false;
                parent.exact = false;
                active.subtreeComplete = false;
                parent.subtreeComplete = false;
                report( "gpu_child_outside_parent", "GPU child interval exceeds its parent interval.", zone.ref );
            }
            const auto begin = std::max( zone.gpuStartNs, parent.beginNs );
            const auto end = std::min( active.endNs, parent.endNs );
            if( timingValid && parent.timingValid && end > begin )
            {
                // Includes vector growth and the temporary interval-union copy.
                parent.workspace.Add( 64 );
                parent.directChildren.emplace_back( begin, end );
            }
        }
        else
        {
            ++result.physicalL0SegmentCount;
        }
        state.ReserveNext();
        stack.emplace_back( std::move( active ) );
    } );
    for( auto& [context, state] : stacks ) while( !state.zones.empty() ) closeTop( state.zones );

    std::vector<uint64_t> handleGenerations;
    ReadBatches<JobDto>( scanner, BoundedScanDomain::Job,
        m_batchSize, result.maximumBatchObserved, options.cancelled, [&]( const auto& job ) {
        AnalysisWorkspaceReservation currentJob( options.workspace, 2048 + 8ull *
            ( job.ref.size() + job.name.size() + job.scheduleStackProvenance.size() +
              ( job.scheduleStackUnavailableReason ? job.scheduleStackUnavailableReason->size() : 0 ) ) +
            32ull * job.dependencies.size() );
        JobLifecycleFact fact;
        fact.ref = job.ref;
        fact.jobId = job.jobId;
        fact.packedHandle = job.packedHandle;
        fact.handleSlot = uint32_t( job.packedHandle );
        fact.handleGeneration = uint32_t( job.packedHandle >> 32 );
        fact.name = job.name;
        fact.typeId = job.typeId;
        fact.scheduleNs = job.scheduleNs;
        fact.readyNs = job.readyNs;
        fact.queueEnterNs = job.queueEnterNs;
        fact.firstRunNs = job.firstRunNs;
        fact.completedNs = job.completedNs;
        fact.executionNs = job.executionNs;
        fact.waitNs = job.waitNs;
        fact.waitActiveHelpNs = job.waitActiveHelpNs;
        fact.waitSpinYieldNs = job.waitSpinYieldNs;
        fact.waitSleepNs = job.waitSleepNs;
        fact.originFrameId = job.originFrameId;
        fact.stageCount = uint32_t( job.stages.size() );
        fact.scheduleCallsiteId = job.scheduleCallsiteId;
        fact.scheduleCallstack = job.scheduleCallstack;
        fact.stackProvenance = job.scheduleStackProvenance;
        fact.scheduleStackAvailable = job.scheduleCallsiteId != 0 || job.scheduleCallstack != 0;
        fact.stackUnavailableReason = fact.scheduleStackAvailable ? std::string {} :
            job.scheduleStackUnavailableReason.value_or( "job_schedule_stack_unavailable" );
        fact.hasWaiter = job.waitNs > 0 || job.waitEndCount > 0 || !job.waitCallstacks.empty() ||
            std::any_of( job.stages.begin(), job.stages.end(), [&]( const auto& stage ) {
                checkCancelled();
                return stage.stage == uint8_t( JnJobStage::WaitBegin ) || stage.stage == uint8_t( JnJobStage::WaitEnd );
            } );
        fact.hasContinuation = job.continuationCount > 0 ||
            std::any_of( job.stages.begin(), job.stages.end(), [&]( const auto& stage ) {
                checkCancelled();
                return stage.stage == uint8_t( JnJobStage::Continuation );
            } );
        fact.exact = !job.incomplete && !job.orphan && !job.truncated;
        for( const auto& dependency : job.dependencies )
        {
            checkCancelled();
            if( dependency.prerequisiteJobId != 0 ) fact.dependencyJobIds.emplace_back( dependency.prerequisiteJobId );
        }
        std::sort( fact.dependencyJobIds.begin(), fact.dependencyJobIds.end(), cancellableLess );
        fact.dependencyJobIds.erase( std::unique( fact.dependencyJobIds.begin(), fact.dependencyJobIds.end() ), fact.dependencyJobIds.end() );
        if( job.completedNs && *job.completedNs < job.scheduleNs )
        {
            fact.exact = false;
            report( "job_completion_before_schedule", "Job completion precedes schedule time.", job.ref );
        }
        metadataWorkspace.Add( 32 );
        handleGenerations.push_back( ( uint64_t( fact.handleSlot ) << 32 ) | fact.handleGeneration );
        ++result.jobCount;
        if( options.jobSink && !options.jobSink( fact ) )
            throw BoundedScanError( "job_scan_sink_cancelled" );
        if( options.retainDetails )
        {
            result.outputWorkspace.Add( currentJob.Bytes() );
            result.jobs.emplace_back( std::move( fact ) );
        }
    } );
    std::sort( handleGenerations.begin(), handleGenerations.end(), cancellableLess );
    for( size_t first = 0; first < handleGenerations.size(); )
    {
        checkCancelled();
        size_t last = first + 1;
        const auto slot = uint32_t( handleGenerations[first] >> 32 );
        uint32_t distinct = 1;
        while( last < handleGenerations.size() && uint32_t( handleGenerations[last] >> 32 ) == slot )
        {
            checkCancelled();
            if( uint32_t( handleGenerations[last] ) != uint32_t( handleGenerations[last-1] ) ) ++distinct;
            ++last;
        }
        if( distinct > 1 ) ++result.handleReuseCount;
        first = last;
    }
    handleGenerations.clear();
    handleGenerations.shrink_to_fit();

    std::unordered_map<uint32_t, ScriptFrameDto> frameDefinitions;
    ReadBatches<ScriptFrameDto>( scanner, BoundedScanDomain::ScriptFrame,
        m_batchSize, result.maximumBatchObserved, options.cancelled, [&]( const auto& frame ) {
            managedWorkspace.Add( 1024 + 8ull * ( frame.ref.size() + frame.function.size() + frame.file.size() + frame.threadRef.size() ) );
            frameDefinitions[frame.frameId] = frame;
        } );
    std::map<uint64_t, ScriptStackBuild> stackBuilds;
    std::map<uint64_t, ManagedZoneFact> zoneBuilds;
    ReadBatches<ScriptStackEventDto>( scanner, BoundedScanDomain::ScriptStack,
        m_batchSize, result.maximumBatchObserved, options.cancelled, [&]( const auto& event ) {
        managedWorkspace.Add( 1024 + 8ull * ( event.ref.size() + event.threadRef.size() + event.text.size() ) );
        switch( JnScriptRecordKind( event.kind ) )
        {
        case JnScriptRecordKind::StackHeader:
            stackBuilds[event.primaryId] = { event.primaryId, event.runtime, event.value, true, {}, {} };
            break;
        case JnScriptRecordKind::StackFrame:
            stackBuilds[event.primaryId].id = event.primaryId;
            stackBuilds[event.primaryId].runtime = event.runtime;
            stackBuilds[event.primaryId].frameIds.push_back( uint32_t( event.secondaryId ) );
            break;
        case JnScriptRecordKind::Marker:
            stackBuilds[event.primaryId].id = event.primaryId;
            stackBuilds[event.primaryId].runtime = event.runtime;
            stackBuilds[event.primaryId].marker = event.text;
            break;
        case JnScriptRecordKind::ZoneBegin:
            zoneBuilds[event.primaryId] = { event.primaryId, event.secondaryId, event.runtime,
                event.threadRef, event.timeNs, std::nullopt, {}, ManagedProducerKind::DirectSourceStack,
                false, "Unavailable", {} };
            break;
        case JnScriptRecordKind::ZoneEnd:
            if( const auto found = zoneBuilds.find( event.primaryId ); found != zoneBuilds.end() )
                found->second.endNs = event.timeNs;
            else report( "script_zone_end_without_begin", "Script ZoneEnd has no matching ZoneBegin.", event.ref );
            break;
        }
    } );

    std::unordered_map<uint64_t, size_t> managedStackIndex;
    uint64_t retainedManagedBytes = 0;
    for( auto& [id, build] : stackBuilds )
    {
        checkCancelled();
        AnalysisWorkspaceReservation currentStack( options.workspace, 2048 + 8ull * build.marker.size() );
        ManagedSourceStackFact fact;
        fact.stackId = id;
        fact.runtime = build.runtime;
        fact.expectedDepth = build.expected;
        fact.marker = std::move( build.marker );
        bool allResolved = true;
        for( const auto frameId : build.frameIds )
        {
            checkCancelled();
            const auto found = frameDefinitions.find( frameId );
            if( found == frameDefinitions.end() ) { allResolved = false; continue; }
            currentStack.Add( 1024 + 8ull * ( found->second.ref.size() + found->second.function.size() +
                found->second.file.size() + found->second.threadRef.size() ) );
            fact.frames.emplace_back( found->second );
        }
        fact.complete = build.hasHeader && allResolved && fact.frames.size() == fact.expectedDepth;
        if( !fact.complete )
        {
            fact.unavailableReason = "script_stack_incomplete";
            report( "script_stack_incomplete", "Script source stack depth or frame definitions are incomplete.", std::to_string( id ) );
        }
        managedStackIndex[id] = result.managedStacks.size();
        result.outputWorkspace.Add( currentStack.Bytes() );
        retainedManagedBytes += currentStack.Bytes();
        result.managedStacks.emplace_back( std::move( fact ) );
    }
    for( auto& [id, zone] : zoneBuilds )
    {
        checkCancelled();
        if( const auto stack = managedStackIndex.find( zone.stackId ); stack == managedStackIndex.end() )
        {
            zone.stackUnavailableReason = "script_stack_missing";
            report( "script_stack_missing", "Script zone references an unknown source stack.", std::to_string( id ) );
        }
        else
        {
            const auto& sourceStack = result.managedStacks[stack->second];
            managedWorkspace.Add( 1024 + 8ull * ( sourceStack.marker.size() + sourceStack.unavailableReason.size() ) );
            zone.marker = sourceStack.marker;
            zone.stackAvailable = sourceStack.complete;
            zone.stackProvenance = sourceStack.complete ? "ExactSource" : "Unavailable";
            zone.stackUnavailableReason = sourceStack.complete ? std::string {} : sourceStack.unavailableReason;
        }
        if( !zone.endNs ) report( "script_zone_missing_end", "Script source zone has no matching ZoneEnd.", std::to_string( id ) );
        ++result.managedZoneCount;
        if( options.managedZoneSink && !options.managedZoneSink( zone ) )
            throw BoundedScanError( "managed_zone_scan_sink_cancelled" );
        checkCancelled();
        if( options.retainDetails )
        {
            result.outputWorkspace.Add( 2048 + 8ull * ( zone.threadRef.size() + zone.marker.size() +
                zone.stackProvenance.size() + zone.stackUnavailableReason.size() ) );
            result.managedZones.emplace_back( std::move( zone ) );
        }
    }
    if( !options.retainDetails )
    {
        result.managedStacks.clear();
        result.managedStacks.shrink_to_fit();
        result.outputWorkspace.Resize( result.outputWorkspace.Bytes() - retainedManagedBytes );
    }

    ReadBatches<RelationDto>( scanner, BoundedScanDomain::Relation,
        m_batchSize, result.maximumBatchObserved, options.cancelled, [&]( const auto& relation ) {
        AnalysisWorkspaceReservation currentRelation( options.workspace, 1024 + 4ull * relation.ref.size() );
        TypedRelationFact fact { relation.ref, relation.sourceId, relation.targetId, relation.timeNs,
            relation.sourceKind, relation.targetKind, relation.relationNamespace, relation.relation, true, false };
        if( relation.sourceKind == uint8_t( JnEntityKind::Unknown ) || relation.targetKind == uint8_t( JnEntityKind::Unknown ) )
        {
            fact.exact = false;
            report( "relation_entity_kind_unknown", "Typed relation contains an unknown endpoint kind.", relation.ref );
        }
        const RelationKey key { relation.sourceId, relation.relationNamespace, relation.relation };
        if( std::binary_search( ambiguousRelationKeys.begin(), ambiguousRelationKeys.end(), key ) )
        {
            fact.exact = false;
            fact.ambiguous = true;
            ++result.ambiguousRelationCount;
            report( "relation_single_value_ambiguous", "A single-valued typed relation has multiple targets.", relation.ref );
        }
        ++result.relationCount;
        if( options.relationSink && !options.relationSink( fact ) )
            throw BoundedScanError( "relation_scan_sink_cancelled" );
        if( options.retainDetails )
        {
            result.outputWorkspace.Add( currentRelation.Bytes() );
            result.relations.emplace_back( std::move( fact ) );
        }
    } );
    checkCancelled();
    return result;
}

const char* GpuQueueClassName( GpuQueueClass value )
{
    switch( value )
    {
    case GpuQueueClass::Direct: return "direct";
    case GpuQueueClass::Compute: return "compute";
    case GpuQueueClass::Copy: return "copy";
    case GpuQueueClass::Unknown: return "unknown";
    }
    return "unknown";
}

const char* GpuFrameEvidenceName( GpuFrameEvidence value )
{
    switch( value )
    {
    case GpuFrameEvidence::ExactFrameRelation: return "exact_frame_relation";
    case GpuFrameEvidence::InferredFromChildPasses: return "inferred_from_child_passes";
    case GpuFrameEvidence::TemporalCandidate: return "temporal_candidate";
    case GpuFrameEvidence::Unassigned: return "unassigned";
    }
    return "unassigned";
}

}
