#ifndef __TRACYJNGPUCATALOG_HPP__
#define __TRACYJNGPUCATALOG_HPP__

#include <stddef.h>
#include <stdint.h>

namespace tracy
{

static constexpr uint32_t JnGpuCatalogBatchMagic = 0x3143474A; // JGC1
static constexpr uint16_t JnGpuCatalogSchemaVersion = 1;
static constexpr uint16_t JnGpuDetailedEvidenceSchemaVersion = 1;

inline uint64_t JnGpuCatalogChecksum64( const void* data, size_t size )
{
    const auto* bytes = static_cast<const uint8_t*>( data );
    uint64_t hash = 14695981039346656037ull;
    for( size_t i = 0; i < size; ++i )
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

enum class JnGpuCatalogRecordOperation : uint8_t
{
    Create = 0,
    Update = 1,
    Destroy = 2,
    Bind = 3,
    Unbind = 4,
    Open = 5,
    Close = 6,
    Snapshot = 7
};

enum class JnGpuCatalogResourceClass : uint8_t
{
    Unknown = 0,
    Buffer = 1,
    Texture = 2,
    AccelerationStructure = 3,
    Heap = 4
};

enum class JnGpuCatalogDimension : uint8_t
{
    Unknown = 0,
    Buffer = 1,
    Texture1D = 2,
    Texture2D = 3,
    Texture3D = 4
};

enum class JnGpuCatalogPrimaryKind : uint16_t
{
    Unknown = 0,
    Texture = 1,
    RenderTarget = 2,
    DepthStencil = 3,
    BackBuffer = 4,
    VertexBuffer = 5,
    IndexBuffer = 6,
    ConstantBuffer = 7,
    StructuredBuffer = 8,
    RawBuffer = 9,
    IndirectBuffer = 10,
    UploadBuffer = 11,
    ReadbackBuffer = 12,
    Blas = 13,
    Tlas = 14,
    RtasScratch = 15,
    ShaderTable = 16,
    InstanceBuffer = 17,
    DynamicVbo = 18,
    VirtualGeometryPool = 19,
    Heap = 20
};

enum class JnGpuCatalogMemoryDomain : uint8_t
{
    Unknown = 0,
    Local = 1,
    NonLocal = 2,
    Upload = 3,
    Readback = 4,
    Custom = 5
};

enum class JnGpuCatalogAllocationKind : uint8_t
{
    Unknown = 0,
    Committed = 1,
    Placed = 2,
    Suballocated = 3,
    Reserved = 4,
    Imported = 5,
    Borrowed = 6
};

enum class JnGpuCatalogExactness : uint8_t
{
    Exact = 0,
    OpenBoundary = 1,
    Partial = 2,
    Unknown = 3,
    Invalid = 4
};

enum class JnGpuCatalogPartKind : uint8_t
{
    Unknown = 0,
    BufferRange = 1,
    TextureSubresource = 2,
    MeshVertexStream = 3,
    MeshIndex = 4,
    VgPage = 5,
    RtasContributor = 6
};

enum class JnGpuCatalogRelationKind : uint8_t
{
    BackedBy = 0,
    ViewOf = 1,
    LogicalBinds = 2,
    FamilyContains = 3,
    PartOf = 4,
    AliasParticipant = 5,
    RtasUses = 6,
    VgResidency = 7,
    PrimaryOwner = 8
};

enum class JnGpuRangeKind : uint8_t
{
    Unknown = 0,
    Buffer = 1,
    TextureSubresource = 2,
    WholeResource = 3
};

enum class JnGpuEvidenceState : uint8_t
{
    ContinuousExact = 0,
    PeriodicExact = 1,
    ManualExact = 2,
    NotSampled = 3,
    Processing = 4,
    InvalidCapacity = 5,
    SkippedBacklog = 6,
    UnavailableUnsafeResourceState = 7
};

enum class JnGpuCatalogGenerationState : uint8_t
{
    Building = 0,
    Complete = 1,
    InvalidCoreGap = 2,
    InvalidCapacity = 3,
    InvalidBootstrapTimeout = 4
};

#pragma pack( push, 1 )

struct JnGpuCatalogBatchEnvelopeV1
{
    uint32_t magic;
    uint16_t catalogSchema;
    uint16_t evidenceSchema;
    uint16_t recordBytes;
    uint16_t flags;
    uint32_t recordCount;
    uint32_t payloadBytes;
    uint64_t checksum;
};

struct JnGpuCatalogResourceRecordV1
{
    int64_t time;
    uint64_t resourceId;
    uint64_t pointerToken;
    uint64_t familyId;
    uint64_t allocationId;
    uint64_t allocationOffsetBytes;
    uint64_t capacityBytes;
    uint64_t width;
    uint64_t nameHash;
    uint32_t height;
    uint32_t format;
    uint32_t sampleCount;
    uint32_t declaredUsageMask;
    uint32_t observedUsageMask;
    uint32_t backendFlags;
    uint32_t nameId;
    uint32_t nameOriginalLength;
    uint32_t definitionRevision;
    uint32_t createCallsiteId;
    uint16_t depthOrArraySize;
    uint16_t mipLevels;
    uint16_t primaryKind;
    uint8_t operation;
    uint8_t resourceClass;
    uint8_t dimension;
    uint8_t memoryDomain;
    uint8_t allocationKind;
    uint8_t classificationProvenance;
    uint8_t nameProvenance;
    uint8_t stackProvenance;
    uint8_t exactness;
    uint8_t flags;
};

struct JnGpuCatalogAllocationRecordV1
{
    int64_t time;
    uint64_t allocationId;
    uint64_t pointerToken;
    uint64_t heapId;
    uint64_t parentAllocationId;
    uint64_t sizeBytes;
    uint64_t offsetBytes;
    uint64_t alignmentBytes;
    uint64_t residentBytes;
    uint32_t flags;
    uint16_t primaryKind;
    uint8_t operation;
    uint8_t memoryDomain;
    uint8_t allocationKind;
    uint8_t residencyState;
    uint8_t exactness;
    uint8_t reserved;
};

struct JnGpuCatalogViewRecordV1
{
    int64_t time;
    uint64_t viewId;
    uint64_t viewToken;
    uint64_t resourceId;
    uint64_t pointerToken;
    uint64_t bufferOffsetBytes;
    uint64_t bufferLengthBytes;
    uint32_t format;
    uint32_t firstSubresource;
    uint32_t subresourceCount;
    uint32_t strideBytes;
    uint8_t operation;
    uint8_t viewKind;
    uint8_t exactness;
    uint8_t flags;
};

struct JnGpuCatalogLogicalRecordV1
{
    int64_t time;
    uint64_t logicalResourceId;
    uint64_t stableKey;
    uint64_t familyId;
    uint64_t resourceId;
    uint64_t pointerToken;
    uint64_t physicalOffsetBytes;
    uint64_t lengthBytes;
    uint64_t aliasGroupId;
    uint64_t frameId;
    uint64_t nameHash;
    uint32_t nameId;
    uint32_t nameOriginalLength;
    uint32_t definitionRevision;
    uint16_t primaryKind;
    uint8_t operation;
    uint8_t nameProvenance;
    uint8_t exactness;
    uint8_t flags;
};

struct JnGpuCatalogPartRecordV1
{
    int64_t time;
    uint64_t partId;
    uint64_t resourceId;
    uint64_t logicalResourceId;
    uint64_t offsetBytes;
    uint64_t lengthBytes;
    uint64_t episodeId;
    uint32_t firstSubresource;
    uint32_t subresourceCount;
    uint32_t definitionRevision;
    uint32_t semanticIndex;
    uint32_t elementCount;
    uint32_t strideBytes;
    uint32_t format;
    uint32_t reserved;
    uint8_t operation;
    uint8_t partKind;
    uint8_t exactness;
    uint8_t flags;
};

struct JnGpuCatalogRelationRecordV1
{
    int64_t time;
    uint64_t sourceId;
    uint64_t targetId;
    uint64_t frameId;
    uint64_t value0;
    uint64_t value1;
    uint8_t operation;
    uint8_t relation;
    uint8_t exactness;
    uint8_t flags;
};

struct JnGpuCatalogVgRecordV1
{
    int64_t time;
    uint64_t runtimeResourceId;
    uint64_t pageDefinitionId;
    uint64_t episodeId;
    uint64_t resourceId;
    uint64_t offsetBytes;
    uint64_t lengthBytes;
    uint64_t frameId;
    uint32_t pageIndex;
    uint32_t gpuPageIndex;
    uint32_t clusterIndex;
    uint8_t operation;
    uint8_t pageKind;
    uint8_t exactness;
    uint8_t flags;
};

struct JnGpuRangeSetRecordV1
{
    uint64_t passInstanceId;
    uint64_t pointerToken;
    uint64_t viewDefinitionId;
    uint64_t offsetBytes;
    uint64_t lengthBytes;
    uint32_t firstSubresource;
    uint32_t subresourceCount;
    uint32_t usageMask;
    uint8_t rangeKind;
    uint8_t exactness;
    uint8_t flags;
    uint8_t reserved;
};

struct JnGpuDetailedEvidenceRecordV1
{
    int64_t time;
    uint64_t requestId;
    uint64_t evidenceFrameId;
    uint64_t frameId;
    uint64_t sourceId;
    uint64_t targetId;
    uint64_t value0;
    uint64_t value1;
    uint32_t flags;
    uint16_t sequence;
    uint8_t state;
    uint8_t kind;
};

struct JnGpuCatalogStringRecordHeaderV1
{
    uint32_t stringId;
    uint32_t originalLength;
    uint64_t hash;
    uint16_t byteLength;
    uint8_t provenance;
    uint8_t flags;
};

#pragma pack( pop )

static_assert( sizeof( JnGpuCatalogBatchEnvelopeV1 ) == 28, "GPU Catalog envelope ABI mismatch" );
static_assert( sizeof( JnGpuRangeSetRecordV1 ) == 56, "GPU RangeSetV1 ABI mismatch" );

}

#endif
