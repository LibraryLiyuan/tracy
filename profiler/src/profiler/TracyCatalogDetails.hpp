#pragma once
#include "../../../public/common/TracyJnGpuCatalog.hpp"
#include "../../../stream/src/TracyEventStore.hpp"
#include <cstring>
#include <sstream>
#include <stdexcept>
namespace tracy
{
inline std::string CatalogRecordText( const stream::StoreCatalogRecord& record )
{
    std::ostringstream out;
    out << "Generation: " << record.generation << "\nSequence: " << record.sequence << "\nEntry: " << record.ordinal << "\n";
    switch( record.kind )
    {
    case 0: {
        if( record.data.size() != sizeof( JnGpuCatalogResourceRecordV1 ) ) throw std::runtime_error( "Invalid Catalog record width" );
        JnGpuCatalogResourceRecordV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "time: " << +value.time << "\n";
        out << "resourceId: " << +value.resourceId << "\n";
        out << "pointerToken: " << +value.pointerToken << "\n";
        out << "familyId: " << +value.familyId << "\n";
        out << "allocationId: " << +value.allocationId << "\n";
        out << "allocationOffsetBytes: " << +value.allocationOffsetBytes << "\n";
        out << "capacityBytes: " << +value.capacityBytes << "\n";
        out << "width: " << +value.width << "\n";
        out << "nameHash: " << +value.nameHash << "\n";
        out << "height: " << +value.height << "\n";
        out << "format: " << +value.format << "\n";
        out << "sampleCount: " << +value.sampleCount << "\n";
        out << "declaredUsageMask: " << +value.declaredUsageMask << "\n";
        out << "observedUsageMask: " << +value.observedUsageMask << "\n";
        out << "backendFlags: " << +value.backendFlags << "\n";
        out << "nameId: " << +value.nameId << "\n";
        out << "nameOriginalLength: " << +value.nameOriginalLength << "\n";
        out << "definitionRevision: " << +value.definitionRevision << "\n";
        out << "createCallsiteId: " << +value.createCallsiteId << "\n";
        out << "depthOrArraySize: " << +value.depthOrArraySize << "\n";
        out << "mipLevels: " << +value.mipLevels << "\n";
        out << "primaryKind: " << +value.primaryKind << "\n";
        out << "operation: " << +value.operation << "\n";
        out << "resourceClass: " << +value.resourceClass << "\n";
        out << "dimension: " << +value.dimension << "\n";
        out << "memoryDomain: " << +value.memoryDomain << "\n";
        out << "allocationKind: " << +value.allocationKind << "\n";
        out << "classificationProvenance: " << +value.classificationProvenance << "\n";
        out << "nameProvenance: " << +value.nameProvenance << "\n";
        out << "stackProvenance: " << +value.stackProvenance << "\n";
        out << "exactness: " << +value.exactness << "\n";
        out << "flags: " << +value.flags << "\n";
        break;
    }
    case 1: {
        if( record.data.size() != sizeof( JnGpuCatalogAllocationRecordV1 ) ) throw std::runtime_error( "Invalid Catalog record width" );
        JnGpuCatalogAllocationRecordV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "time: " << +value.time << "\n";
        out << "allocationId: " << +value.allocationId << "\n";
        out << "pointerToken: " << +value.pointerToken << "\n";
        out << "heapId: " << +value.heapId << "\n";
        out << "parentAllocationId: " << +value.parentAllocationId << "\n";
        out << "sizeBytes: " << +value.sizeBytes << "\n";
        out << "offsetBytes: " << +value.offsetBytes << "\n";
        out << "alignmentBytes: " << +value.alignmentBytes << "\n";
        out << "residentBytes: " << +value.residentBytes << "\n";
        out << "flags: " << +value.flags << "\n";
        out << "primaryKind: " << +value.primaryKind << "\n";
        out << "operation: " << +value.operation << "\n";
        out << "memoryDomain: " << +value.memoryDomain << "\n";
        out << "allocationKind: " << +value.allocationKind << "\n";
        out << "residencyState: " << +value.residencyState << "\n";
        out << "exactness: " << +value.exactness << "\n";
        out << "reserved: " << +value.reserved << "\n";
        break;
    }
    case 2: {
        if( record.data.size() != sizeof( JnGpuCatalogViewRecordV1 ) ) throw std::runtime_error( "Invalid Catalog record width" );
        JnGpuCatalogViewRecordV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "time: " << +value.time << "\n";
        out << "viewId: " << +value.viewId << "\n";
        out << "viewToken: " << +value.viewToken << "\n";
        out << "resourceId: " << +value.resourceId << "\n";
        out << "pointerToken: " << +value.pointerToken << "\n";
        out << "bufferOffsetBytes: " << +value.bufferOffsetBytes << "\n";
        out << "bufferLengthBytes: " << +value.bufferLengthBytes << "\n";
        out << "format: " << +value.format << "\n";
        out << "firstSubresource: " << +value.firstSubresource << "\n";
        out << "subresourceCount: " << +value.subresourceCount << "\n";
        out << "strideBytes: " << +value.strideBytes << "\n";
        out << "operation: " << +value.operation << "\n";
        out << "viewKind: " << +value.viewKind << "\n";
        out << "exactness: " << +value.exactness << "\n";
        out << "flags: " << +value.flags << "\n";
        break;
    }
    case 3: {
        if( record.data.size() != sizeof( JnGpuCatalogLogicalRecordV1 ) ) throw std::runtime_error( "Invalid Catalog record width" );
        JnGpuCatalogLogicalRecordV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "time: " << +value.time << "\n";
        out << "logicalResourceId: " << +value.logicalResourceId << "\n";
        out << "stableKey: " << +value.stableKey << "\n";
        out << "familyId: " << +value.familyId << "\n";
        out << "resourceId: " << +value.resourceId << "\n";
        out << "pointerToken: " << +value.pointerToken << "\n";
        out << "physicalOffsetBytes: " << +value.physicalOffsetBytes << "\n";
        out << "lengthBytes: " << +value.lengthBytes << "\n";
        out << "aliasGroupId: " << +value.aliasGroupId << "\n";
        out << "frameId: " << +value.frameId << "\n";
        out << "nameHash: " << +value.nameHash << "\n";
        out << "nameId: " << +value.nameId << "\n";
        out << "nameOriginalLength: " << +value.nameOriginalLength << "\n";
        out << "definitionRevision: " << +value.definitionRevision << "\n";
        out << "primaryKind: " << +value.primaryKind << "\n";
        out << "operation: " << +value.operation << "\n";
        out << "nameProvenance: " << +value.nameProvenance << "\n";
        out << "exactness: " << +value.exactness << "\n";
        out << "flags: " << +value.flags << "\n";
        break;
    }
    case 4: {
        if( record.data.size() != sizeof( JnGpuCatalogPartRecordV1 ) ) throw std::runtime_error( "Invalid Catalog record width" );
        JnGpuCatalogPartRecordV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "time: " << +value.time << "\n";
        out << "partId: " << +value.partId << "\n";
        out << "resourceId: " << +value.resourceId << "\n";
        out << "logicalResourceId: " << +value.logicalResourceId << "\n";
        out << "offsetBytes: " << +value.offsetBytes << "\n";
        out << "lengthBytes: " << +value.lengthBytes << "\n";
        out << "episodeId: " << +value.episodeId << "\n";
        out << "firstSubresource: " << +value.firstSubresource << "\n";
        out << "subresourceCount: " << +value.subresourceCount << "\n";
        out << "definitionRevision: " << +value.definitionRevision << "\n";
        out << "semanticIndex: " << +value.semanticIndex << "\n";
        out << "elementCount: " << +value.elementCount << "\n";
        out << "strideBytes: " << +value.strideBytes << "\n";
        out << "format: " << +value.format << "\n";
        out << "reserved: " << +value.reserved << "\n";
        out << "operation: " << +value.operation << "\n";
        out << "partKind: " << +value.partKind << "\n";
        out << "exactness: " << +value.exactness << "\n";
        out << "flags: " << +value.flags << "\n";
        break;
    }
    case 5: {
        if( record.data.size() != sizeof( JnGpuCatalogRelationRecordV1 ) ) throw std::runtime_error( "Invalid Catalog record width" );
        JnGpuCatalogRelationRecordV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "time: " << +value.time << "\n";
        out << "sourceId: " << +value.sourceId << "\n";
        out << "targetId: " << +value.targetId << "\n";
        out << "frameId: " << +value.frameId << "\n";
        out << "value0: " << +value.value0 << "\n";
        out << "value1: " << +value.value1 << "\n";
        out << "operation: " << +value.operation << "\n";
        out << "relation: " << +value.relation << "\n";
        out << "exactness: " << +value.exactness << "\n";
        out << "flags: " << +value.flags << "\n";
        break;
    }
    case 6: {
        if( record.data.size() != sizeof( JnGpuCatalogVgRecordV1 ) ) throw std::runtime_error( "Invalid Catalog record width" );
        JnGpuCatalogVgRecordV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "time: " << +value.time << "\n";
        out << "runtimeResourceId: " << +value.runtimeResourceId << "\n";
        out << "pageDefinitionId: " << +value.pageDefinitionId << "\n";
        out << "episodeId: " << +value.episodeId << "\n";
        out << "resourceId: " << +value.resourceId << "\n";
        out << "pointerToken: " << +value.pointerToken << "\n";
        out << "offsetBytes: " << +value.offsetBytes << "\n";
        out << "lengthBytes: " << +value.lengthBytes << "\n";
        out << "frameId: " << +value.frameId << "\n";
        out << "pageIndex: " << +value.pageIndex << "\n";
        out << "gpuPageIndex: " << +value.gpuPageIndex << "\n";
        out << "clusterIndex: " << +value.clusterIndex << "\n";
        out << "operation: " << +value.operation << "\n";
        out << "pageKind: " << +value.pageKind << "\n";
        out << "exactness: " << +value.exactness << "\n";
        out << "flags: " << +value.flags << "\n";
        break;
    }
    case 7: {
        if( record.data.size() != sizeof( JnGpuRangeSetRecordV1 ) ) throw std::runtime_error( "Invalid Catalog record width" );
        JnGpuRangeSetRecordV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "passInstanceId: " << +value.passInstanceId << "\n";
        out << "pointerToken: " << +value.pointerToken << "\n";
        out << "resourceId: " << +value.resourceId << "\n";
        out << "viewDefinitionId: " << +value.viewDefinitionId << "\n";
        out << "offsetBytes: " << +value.offsetBytes << "\n";
        out << "lengthBytes: " << +value.lengthBytes << "\n";
        out << "firstSubresource: " << +value.firstSubresource << "\n";
        out << "subresourceCount: " << +value.subresourceCount << "\n";
        out << "usageMask: " << +value.usageMask << "\n";
        out << "rangeKind: " << +value.rangeKind << "\n";
        out << "exactness: " << +value.exactness << "\n";
        out << "flags: " << +value.flags << "\n";
        out << "reserved: " << +value.reserved << "\n";
        break;
    }
    case 8: {
        if( record.data.size() != sizeof( JnGpuDetailedEvidenceRecordV1 ) ) throw std::runtime_error( "Invalid Catalog record width" );
        JnGpuDetailedEvidenceRecordV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "time: " << +value.time << "\n";
        out << "requestId: " << +value.requestId << "\n";
        out << "evidenceFrameId: " << +value.evidenceFrameId << "\n";
        out << "frameId: " << +value.frameId << "\n";
        out << "sourceId: " << +value.sourceId << "\n";
        out << "targetId: " << +value.targetId << "\n";
        out << "value0: " << +value.value0 << "\n";
        out << "value1: " << +value.value1 << "\n";
        out << "flags: " << +value.flags << "\n";
        out << "sequence: " << +value.sequence << "\n";
        out << "state: " << +value.state << "\n";
        out << "kind: " << +value.kind << "\n";
        break;
    }
    case 9: {
        if( record.data.size() < sizeof( JnGpuCatalogStringRecordHeaderV1 ) ) throw std::runtime_error( "Invalid Catalog string" );
        JnGpuCatalogStringRecordHeaderV1 value;
        memcpy( &value, record.data.data(), sizeof( value ) );
        out << "stringId: " << value.stringId << "\noriginalLength: " << value.originalLength << "\nhash: " << value.hash << "\nprovenance: " << +value.provenance << "\nflags: " << +value.flags << "\nvalue: ";
        out.write( reinterpret_cast<const char*>( record.data.data() + sizeof( value ) ), record.data.size() - sizeof( value ) );
        break;
    }
    default: throw std::runtime_error( "Unknown Catalog record kind" );
    }
    return out.str();
}
}
