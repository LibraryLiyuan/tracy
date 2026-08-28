#include "tracy/Tracy.hpp"
#include "client/TracyJnClient.hpp"
#include "common/TracyJnGpuCatalog.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <thread>

namespace
{

void NestedWork( int value )
{
    ZoneScopedN( "Synthetic nested work" );
    ZoneValue( value );
    std::this_thread::sleep_for( std::chrono::microseconds( 250 ) );
}

bool EmitGpuCatalogResourceBatch( uint64_t generation, uint32_t sequence,
    const tracy::JnGpuCatalogResourceRecordV1& record )
{
    tracy::JnGpuCatalogBatchEnvelopeV1 envelope {};
    envelope.magic = tracy::JnGpuCatalogBatchMagic;
    envelope.catalogSchema = tracy::JnGpuCatalogSchemaVersion;
    envelope.evidenceSchema = tracy::JnGpuDetailedEvidenceSchemaVersion;
    envelope.recordBytes = sizeof( record );
    envelope.recordCount = 1;
    envelope.payloadBytes = sizeof( record );
    envelope.checksum = tracy::JnGpuCatalogChecksum64( &record, sizeof( record ) );

    std::array<uint8_t, sizeof( envelope ) + sizeof( record )> payload {};
    memcpy( payload.data(), &envelope, sizeof( envelope ) );
    memcpy( payload.data() + sizeof( envelope ), &record, sizeof( record ) );
    return tracy::EmitJnGpuCatalogBatch( generation, sequence, 1,
        uint8_t( tracy::JnGpuCatalogBatchKind::Resource ),
        uint8_t( tracy::JnGpuCatalogBatchEncoding::FixedV1 ), 0,
        payload.data(), uint32_t( payload.size() ) );
}

bool EmitGpuCatalogRelationBatch( uint64_t generation, uint32_t sequence,
    const tracy::JnGpuCatalogRelationRecordV1& record )
{
    tracy::JnGpuCatalogBatchEnvelopeV1 envelope {};
    envelope.magic = tracy::JnGpuCatalogBatchMagic;
    envelope.catalogSchema = tracy::JnGpuCatalogSchemaVersion;
    envelope.evidenceSchema = tracy::JnGpuDetailedEvidenceSchemaVersion;
    envelope.recordBytes = sizeof( record );
    envelope.recordCount = 1;
    envelope.payloadBytes = sizeof( record );
    envelope.checksum = tracy::JnGpuCatalogChecksum64( &record, sizeof( record ) );

    std::array<uint8_t, sizeof( envelope ) + sizeof( record )> payload {};
    memcpy( payload.data(), &envelope, sizeof( envelope ) );
    memcpy( payload.data() + sizeof( envelope ), &record, sizeof( record ) );
    return tracy::EmitJnGpuCatalogBatch( generation, sequence, 1,
        uint8_t( tracy::JnGpuCatalogBatchKind::Relation ),
        uint8_t( tracy::JnGpuCatalogBatchEncoding::FixedV1 ), 0,
        payload.data(), uint32_t( payload.size() ) );
}

bool EmitGpuCatalogFixture( uint32_t postEndBatchCount, bool unresolvedEnrichment, bool unresolvedCore )
{
    constexpr uint64_t Generation = 0xC470000000000001ull;
    constexpr uint64_t ResourceId = 1;
    constexpr uint64_t PointerToken = 0xC4701000ull;
    uint32_t sequence = 1;
    if( !tracy::EmitJnGpuCatalogControl( Generation, postEndBatchCount + 1, sequence++,
        uint8_t( tracy::JnGpuCatalogControlKind::GenerationBegin ),
        uint8_t( tracy::JnGpuCatalogGenerationState::Building ),
        uint8_t( tracy::JnGpuCatalogControlFlags::Bootstrap ) ) ) return false;

    tracy::JnGpuCatalogResourceRecordV1 resource {};
    resource.time = tracy::Profiler::GetTime();
    resource.resourceId = ResourceId;
    resource.pointerToken = PointerToken;
    resource.capacityBytes = 4 * 1024 * 1024;
    resource.width = resource.capacityBytes;
    resource.definitionRevision = 1;
    resource.primaryKind = uint16_t( tracy::JnGpuCatalogPrimaryKind::StructuredBuffer );
    resource.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
    resource.resourceClass = uint8_t( tracy::JnGpuCatalogResourceClass::Buffer );
    resource.dimension = uint8_t( tracy::JnGpuCatalogDimension::Buffer );
    resource.memoryDomain = uint8_t( tracy::JnGpuCatalogMemoryDomain::Local );
    resource.exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
    if( !EmitGpuCatalogResourceBatch( Generation, sequence++, resource ) ) return false;

    if( !tracy::EmitJnGpuCatalogControl( Generation, uint64_t( tracy::Profiler::GetTime() ), sequence++,
        uint8_t( tracy::JnGpuCatalogControlKind::GenerationEnd ),
        uint8_t( tracy::JnGpuCatalogGenerationState::Complete ), 0 ) ) return false;

    resource.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Update );
    for( uint32_t i = 0; i < postEndBatchCount; ++i )
    {
        resource.time = tracy::Profiler::GetTime();
        resource.definitionRevision = i + 2;
        resource.observedUsageMask = i;
        if( !EmitGpuCatalogResourceBatch( Generation, sequence++, resource ) ) return false;
    }
    if( unresolvedEnrichment || unresolvedCore )
    {
        tracy::JnGpuCatalogRelationRecordV1 relation {};
        relation.time = tracy::Profiler::GetTime();
        relation.sourceId = 0xBAD0000012345678ull;
        relation.targetId = 0x7400000000000001ull;
        relation.operation = uint8_t( tracy::JnGpuCatalogRecordOperation::Create );
        relation.relation = uint8_t( unresolvedCore ? tracy::JnGpuCatalogRelationKind::BackedBy :
            tracy::JnGpuCatalogRelationKind::RtasUses );
        relation.exactness = uint8_t( tracy::JnGpuCatalogExactness::Exact );
        relation.flags = 1;
        if( !EmitGpuCatalogRelationBatch( Generation, sequence++, relation ) ) return false;
    }
    return true;
}

}

int main( int argc, char** argv )
{
    int durationSeconds = 5;
    uint32_t postEndBatchCount = 0;
    bool unresolvedEnrichment = false;
    bool unresolvedCore = false;
    if( argc == 2 && argv[1][0] != '-' )
    {
        durationSeconds = std::max( 1, std::atoi( argv[1] ) );
    }
    else
    {
        for( int i = 1; i < argc; ++i )
        {
            if( strcmp( argv[i], "--duration-seconds" ) == 0 && i + 1 < argc )
                durationSeconds = std::max( 1, std::atoi( argv[++i] ) );
            else if( strcmp( argv[i], "--gpu-catalog-post-end-batches" ) == 0 && i + 1 < argc )
                postEndBatchCount = uint32_t( strtoul( argv[++i], nullptr, 10 ) );
            else if( strcmp( argv[i], "--gpu-catalog-unresolved-enrichment" ) == 0 )
                unresolvedEnrichment = true;
            else if( strcmp( argv[i], "--gpu-catalog-unresolved-core" ) == 0 )
                unresolvedCore = true;
            else
            {
                std::fprintf( stderr, "Unknown or incomplete argument: %s\n", argv[i] );
                return 2;
            }
        }
    }

    tracy::SetThreadName( "tracy-stream-test-producer" );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( durationSeconds );
    int frame = 0;
    bool catalogEmitted = postEndBatchCount == 0;
    while( std::chrono::steady_clock::now() < deadline )
    {
        if( !catalogEmitted && tracy::GetProfiler().IsConnected() )
        {
            catalogEmitted = EmitGpuCatalogFixture( postEndBatchCount, unresolvedEnrichment, unresolvedCore );
            if( !catalogEmitted )
            {
                std::fprintf( stderr, "GPU Catalog fixture emission failed.\n" );
                return 1;
            }
        }
        ZoneScopedN( "Synthetic frame" );
        NestedWork( frame );
        TracyPlot( "Synthetic frame index", int64_t( frame ) );
        if( frame % 32 == 0 ) TracyMessageL( "tracy-stream protocol acceptance tick" );
        FrameMark;
        frame++;
        std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
    }
    if( !catalogEmitted )
    {
        std::fprintf( stderr, "GPU Catalog fixture never observed a capture connection.\n" );
        return 1;
    }
    return 0;
}
