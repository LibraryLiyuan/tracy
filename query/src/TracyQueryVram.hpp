#ifndef __TRACYQUERYVRAM_HPP__
#define __TRACYQUERYVRAM_HPP__

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../../public/common/TracyJnVram.hpp"

namespace tracy::query
{

// One plot's points in time order. A value holds until the next point.
struct VramPoint
{
    int64_t timeNs = 0;
    double value = 0;
};
using VramSeries = std::vector<VramPoint>;

// The GPU memory plots of a trace (see TracyJnVram.hpp), by segment. An
// absent plot is an empty series.
struct VramSeriesSet
{
    std::array<VramSeries, JnVram::Segments> usage, budget, physical, logical;
    std::array<std::array<std::array<VramSeries, JnVram::Categories>, JnVram::Ledgers>, JnVram::Segments> categories;
    VramSeries evicted;
};

struct VramRequest
{
    std::optional<int64_t> beginNs, endNs, atNs;
    // Time buckets of the returned series; 0 returns no series.
    size_t buckets = 60;
    std::array<bool, JnVram::Segments> segments { true, true };
};

// DXGI usage against budget, the engine's ledgers, and their categories at
// the usage peak, at the end and at atNs, for the requested range.
nlohmann::json VramSummary( const VramSeriesSet& series, const VramRequest& request );
// When a segment's DXGI usage peaked, none without usage.
std::optional<int64_t> VramUsagePeak( const VramSeriesSet& series, size_t segment );
// When the engine's registry first held resources: its first sample after the
// capture connected that counts logical bytes, which follows the records of
// the resources created before the capture.
std::optional<int64_t> VramRegistryStart( const VramSeriesSet& series );

// A GPU resource the engine registered (a GTMEM logical resource).
struct VramResourceInput
{
    std::string name;
    // The engine's category (TracyJnVram.hpp), empty in records that predate it.
    std::string category;
    // 'T' texture, 'B' buffer, 'U' upload, 'R' readback; segment 'L' or 'N'.
    char kind = 'U', segment = 'L';
    uint64_t sizeBytes = 0, allocationId = 0;
    int64_t createdNs = 0;
    std::optional<int64_t> freedNs;
    // Created before the capture connected; createdNs is then the connection.
    bool preCapture = false;
};

// The resources a request lists. A resource's type is its category, or its
// kind in records that predate categories; upload and readback buffers go
// by their kind.
struct VramResourceFilter
{
    // texture, render_target, depth_stencil, buffer, ray_tracing, other,
    // upload or readback; empty for every type.
    std::string type;
    // A part of the name, in any case; empty for every name.
    std::string name;
};
bool IsVramResourceType( const std::string& type );

// The resources of a segment (0 local, 1 non-local) live at a time that pass
// the filter, largest first, `limit` of them from `offset`; their bytes by
// name, and by type before the filter.
nlohmann::json VramResourcesAt( const std::vector<VramResourceInput>& resources, size_t segment, int64_t timeNs,
    const VramResourceFilter& filter, size_t offset, size_t limit );
// Per name, the bytes of a segment's resources that pass the filter live at
// both ends of a range, and the resources created and freed inside it.
nlohmann::json VramResourceGrowth( const std::vector<VramResourceInput>& resources, size_t segment, int64_t beginNs, int64_t endNs,
    const VramResourceFilter& filter, size_t limit );

// A D3D12 resource the GPU reference passes of a range referenced: its GTMEM
// logical id, when the range first referenced it, by how many passes, and
// their usages combined.
struct VramReferencedResource
{
    uint64_t resourceId = 0;
    int64_t firstNs = 0;
    uint32_t passes = 0, usageMask = 0;
};
struct VramFrameReferences
{
    int64_t beginNs = 0, endNs = 0;
    // Each resource once.
    std::vector<VramReferencedResource> resources;
    // The reference passes in the range, and how many of them are a command
    // list's whole set (frame granularity, taxonomy level 0).
    uint64_t passes = 0, commandListSets = 0;
    // The range held more references than were read.
    bool truncated = false;
};
// The resources a range referenced, resolved to the engine's resources live
// when first referenced: by type, segment and name over all of them, then
// those passing the filter, largest first, `limit` of them from `offset`.
nlohmann::json VramFrameResources( const std::vector<VramResourceInput>& resources, const VramFrameReferences& references,
    const VramResourceFilter& filter, size_t offset, size_t limit );

}

#endif
