#pragma once

#include "TracyGpuAnalysisTraceSource.hpp"
#include <nlohmann/json.hpp>
#include <set>

// Read-only acceptance helper. No Worker, Query service, source replay or writes.
// Expected values come from pinned immutable readers; Python compares the public
// Query/MCP projection separately. Global correctness remains the full audit.
inline bool EmitSessionAcceptanceAnchors( const std::filesystem::path& path, std::string& error )
{
    using namespace tracy::analysis;
    using json = nlohmann::json;
    auto source = GpuAnalysisTraceSource::OpenSessionIfReady( path, {}, &error );
    if( !source || !source->SessionManifest() ) return false;
    const auto& manifest = *source->SessionManifest();
    auto store = source->StoreReader();
    json output = { { "schema", 1 }, { "generation", manifest.generation },
        { "source_reason", manifest.reason }, { "shards", json::array() },
        { "frames", json::array() }, { "resources", json::array() },
        { "images", json::array() }, { "windows", json::array() }, { "cross_shard_resource", nullptr },
        { "largest_allocation", nullptr }, { "max_fanout_pass", nullptr } };
    const auto positions = []( size_t count ) {
        return count ? std::set<size_t>{ 0, count / 2, count - 1 } : std::set<size_t>{};
    };
    std::vector<TraceSessionShard> shards;
    for( const auto& shard : manifest.shards ) if( shard.domain != "checkpoint" ) shards.push_back( shard );
    if( shards.empty() ) { error = "acceptance_no_data_shards"; return false; }
    const auto frames = TraceSessionFrameReader::Open( path, manifest, error );
    if( !frames ) return false;
    for( const auto ordinal : positions( shards.size() ) )
    {
        const auto& shard = shards[ordinal];
        output["shards"].push_back( { { "id", shard.shardId }, { "start_ns", shard.timeBeginNs },
            { "end_ns", shard.timeEndNs } } );
        for( size_t set = 0; set < frames->Sets().size(); ++set )
        {
            const auto& values = frames->Sets()[set].frames;
            size_t first = values.size(), last = values.size();
            for( size_t i = 0; i < values.size(); ++i )
            {
                if( values[i].beginNs < shard.timeBeginNs || values[i].beginNs > shard.timeEndNs ) continue;
                if( first == values.size() ) first = i;
                last = i;
            }
            if( first == values.size() ) continue;
            for( const auto index : std::set<size_t>{ first, last } )
            {
                const auto& value = values[index];
                output["frames"].push_back( { { "shard_id", shard.shardId },
                    { "frame_set_index", set }, { "frame_set_name", frames->Sets()[set].name },
                    { "index", index }, { "begin_ns", value.beginNs },
                    { "end_ns", value.complete ? json( value.endNs ) : json( nullptr ) },
                    { "complete", value.complete } } );
            }
        }
    }
    std::set<uint64_t> windowShards;
    for( const auto& frame : output["frames"] )
    {
        const auto shardId = frame["shard_id"].get<uint64_t>();
        if( frame["end_ns"].is_null() || !windowShards.insert( shardId ).second ) continue;
        ScanRange range;
        range.startNs = frame["begin_ns"].get<int64_t>();
        range.endNs = frame["end_ns"].get<int64_t>();
        range.limit = 3;
        const auto refs = []( const auto& values ) { json result = json::array();
            for( const auto& value : values ) result.push_back( value.ref ); return result; };
        output["windows"].push_back( { { "shard_id", shardId },
            { "zone.cpu.search", refs( source->ScanCpuZones( range ) ) },
            { "zone.gpu.search", refs( source->ScanGpuZones( range ) ) },
            { "sample.list", refs( source->ScanSampleEvents( range ) ) },
            { "context_switch.range", refs( source->ScanContextSwitchEvents( range ) ) } } );
    }
    const auto resourceJson = []( const auto& value ) {
        return json { { "resource_id", value.resourceId }, { "capacity_bytes", value.capacityBytes },
            { "allocation_id", value.allocationId ? json( value.allocationId ) : json( nullptr ) },
            { "name", value.name.empty() ? json( nullptr ) : json( value.name ) },
            { "create_time_ns", value.openBoundary ? json( nullptr ) : json( value.createTime ) },
            { "destroy_time_ns", value.destroyTime ? json( value.destroyTime ) : json( nullptr ) },
            { "alive_at_capture_end", value.aliveAtEnd } };
    };
    const auto resourcePositions = positions( size_t( store->Manifest().resourceCount ) );
    uint64_t count = 0, previousId = 0;
    for( size_t page = 0; page < store->ResourceSummaryPageCount(); ++page )
    {
        std::vector<GpuAnalysisResourceSummary> values;
        if( !store->LoadResourceSummaryPage( page, values, error ) ) return false;
        for( const auto& value : values )
        {
            if( count && value.resourceId <= previousId ) { error = "acceptance_resource_id_order"; return false; }
            previousId = value.resourceId;
            if( resourcePositions.count( size_t( count ) ) ) output["resources"].push_back( resourceJson( value ) );
            ++count;
            if( output["cross_shard_resource"].is_null() && !value.openBoundary && value.destroyTime > value.createTime )
                for( const auto& shard : shards )
                    if( value.createTime < uint64_t( std::max<int64_t>( 0, shard.timeEndNs ) ) &&
                        value.destroyTime > uint64_t( std::max<int64_t>( 0, shard.timeEndNs ) ) )
                    { output["cross_shard_resource"] = resourceJson( value ); break; }
        }
        std::cerr << "[Anchors.ResourceSummary] page=" << page + 1 << '/' << store->ResourceSummaryPageCount() << std::endl;
    }
    if( count != store->Manifest().resourceCount ) { error = "acceptance_resource_summary_count"; return false; }
    uint64_t largest = 0, largestId = 0;
    for( size_t page = 0; page < store->AllocationPageCount(); ++page )
    {
        std::vector<GpuAllocationAnalysisRecord> values;
        if( !store->LoadAllocationPage( page, values, error ) ) return false;
        for( const auto& value : values ) if( !largestId || value.sizeBytes > largest ||
            ( value.sizeBytes == largest && value.allocationId < largestId ) )
        {
            largest = value.sizeBytes; largestId = value.allocationId;
            output["largest_allocation"] = { { "allocation_id", largestId }, { "size_bytes", largest },
                { "resource_id", value.resources.empty() ? json( nullptr ) : json( *std::min_element( value.resources.begin(), value.resources.end() ) ) } };
        }
        std::cerr << "[Anchors.Allocation] page=" << page + 1 << '/' << store->AllocationPageCount() << std::endl;
    }
    uint64_t fanout = 0, passId = 0;
    for( size_t page = 0; page < store->PassSummaryPageCount(); ++page )
    {
        std::vector<GpuAnalysisPassSummary> values;
        if( !store->LoadPassSummaryPage( page, values, error ) ) return false;
        for( const auto& value : values ) if( !passId || value.directResourceCount > fanout ||
            ( value.directResourceCount == fanout && value.passId < passId ) )
        {
            fanout = value.directResourceCount; passId = value.passId;
            output["max_fanout_pass"] = { { "pass_id", passId }, { "frame_id", value.frameId },
                { "resource_count", fanout }, { "referenced_physical_bytes", value.directPhysicalBytes },
                { "complete", value.complete != 0 }, { "truncated", value.truncated != 0 } };
        }
        std::cerr << "[Anchors.PassSummary] page=" << page + 1 << '/' << store->PassSummaryPageCount() << std::endl;
    }
    if( passId )
    {
        const auto pass = store->FindPass( passId, error ); if( !pass ) return false;
        output["max_fanout_pass"]["start_ns"] = pass->startNs;
        output["max_fanout_pass"]["end_ns"] = pass->endNs;
        output["max_fanout_pass"]["resource_ids"] = json::array();
        for( const auto index : positions( pass->directResources.size() ) )
            output["max_fanout_pass"]["resource_ids"].push_back( pass->directResources[index] );
        // Catalog stores the GPU domain index, not the packed JN FrameIdentity.
        // Export only explicit alias records. Python rejects absent/ambiguous
        // mappings; never infer a join from equal numbers or nearby timestamps.
        auto& aliases = output["max_fanout_pass"]["frame_identity_aliases"];
        aliases = json::array();
        for( const auto& frame : source->GetCorrelatedFrameEvents() )
            if( frame.domain == 4 && frame.domainIndex == pass->frameId )
                aliases.push_back( { { "frame_id", frame.frameId },
                    { "domain_id", frame.domain }, { "domain_index", frame.domainIndex } } );
    }
    const auto images = source->GetFrameImageResources();
    for( const auto index : positions( images.size() ) )
    {
        const auto& value = images[index];
        output["images"].push_back( { { "ref", value.ref }, { "width", value.width },
            { "height", value.height }, { "raw_frame_index", value.rawFrameIndex },
            { "raw_bc1_bytes", value.rawBc1Bytes }, { "frame_ref", value.frameRef ? json( *value.frameRef ) : json( nullptr ) } } );
    }
    output["peak"] = { { "engine_known_physical_peak_bytes", store->Overview().engineKnownPhysicalPeakBytes },
        { "peak_time_ns", store->Overview().engineKnownPhysicalPeakTimeNs } };
    output["degraded_interval_availability"] = "query_quality_and_representative_windows;no_inferred_interval";
    std::cout << output.dump() << std::endl;
    return true;
}
