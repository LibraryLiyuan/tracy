#include "tracy/Tracy.hpp"
#include "client/TracyJnClient.hpp"
#include "common/TracyJnGpuCatalog.hpp"
#include "common/TracyJnVram.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

// GPU memory as the JN engine sends it (TracyJnVram.hpp): totals and DXGI
// every 250 ms, a category when it changes. Over the run a level streams its
// textures in, a resolution change adds render targets and aliasing heaps for
// a while, the level unloads and streams again, another process takes video
// memory, and the driver evicts some near the end.
class VramScenario
{
public:
    explicit VramScenario( double durationSeconds ) : m_duration( durationSeconds ) {}

    void Emit( double seconds )
    {
        using namespace tracy::JnVram;
        constexpr double MiB = 1024.0 * 1024.0;
        constexpr double GiB = 1024.0 * MiB;
        const double u = std::min( seconds / m_duration, 1.0 );
        const auto quantize = []( double bytes ) { return std::round( bytes / MiB ) * MiB; };
        const bool highResolution = u >= 0.35 && u < 0.5;
        double texture;
        if( u < 0.5 ) texture = 1.6 * GiB + 1.8 * GiB * ( u / 0.5 );
        else if( u < 0.6 ) texture = 3.4 * GiB;
        else if( u < 0.65 ) texture = 1.9 * GiB;
        else texture = 1.9 * GiB + 0.9 * GiB * std::min( ( u - 0.65 ) / 0.25, 1.0 );
        const double heap = highResolution ? 1280 * MiB : 1024 * MiB;
        Values values {};
        auto& physical = values[0][int( Ledger::Physical )];
        auto& logical = values[0][int( Ledger::Logical )];
        auto& resources = values[0][int( Ledger::Resources )];
        physical[int( Category::Texture )] = quantize( texture );
        physical[int( Category::RenderTarget )] = highResolution ? 1152 * MiB : 640 * MiB;
        physical[int( Category::DepthStencil )] = highResolution ? 256 * MiB : 160 * MiB;
        physical[int( Category::Buffer )] = quantize( 280 * MiB + 60 * MiB * u );
        physical[int( Category::RayTracing )] = u >= 0.2 ? 96 * MiB : 0;
        physical[int( Category::Heap )] = heap;
        physical[int( Category::Other )] = 12 * MiB;
        // Placed resources live in the heaps.
        logical[int( Category::Texture )] = physical[int( Category::Texture )] + quantize( 0.55 * heap );
        logical[int( Category::RenderTarget )] = physical[int( Category::RenderTarget )] + quantize( 0.3 * heap );
        logical[int( Category::DepthStencil )] = physical[int( Category::DepthStencil )];
        logical[int( Category::Buffer )] = physical[int( Category::Buffer )] + quantize( 0.1 * heap );
        logical[int( Category::RayTracing )] = physical[int( Category::RayTracing )];
        resources[int( Category::Texture )] = std::round( physical[int( Category::Texture )] / ( 1.5 * MiB ) + 0.55 * heap / ( 4 * MiB ) );
        resources[int( Category::RenderTarget )] = highResolution ? 52 : 40;
        resources[int( Category::DepthStencil )] = highResolution ? 9 : 6;
        resources[int( Category::Buffer )] = std::round( 900 + 200 * u );
        resources[int( Category::RayTracing )] = u >= 0.2 ? 3 : 0;
        resources[int( Category::Heap )] = heap / ( 64 * MiB );
        // Upload bursts every five seconds.
        const bool burst = std::fmod( seconds, 5.0 ) < 1.0;
        auto& upload = values[1];
        upload[int( Ledger::Physical )][int( Category::Buffer )] = burst ? 244 * MiB : 180 * MiB;
        upload[int( Ledger::Physical )][int( Category::Other )] = 8 * MiB;
        upload[int( Ledger::Logical )][int( Category::Buffer )] = upload[int( Ledger::Physical )][int( Category::Buffer )];
        upload[int( Ledger::Resources )][int( Category::Buffer )] = burst ? 38 : 30;

        const double evicted = u >= 0.8 && u < 0.85 ? 384 * MiB : 0;
        const std::array<double, Segments> untracked = { 350 * MiB + 40 * MiB * std::sin( seconds ), 120 * MiB };
        const std::array<double, Segments> budget = { u >= 0.7 ? 6.8 * GiB : 7.5 * GiB, 16 * GiB };
        for( int segment = 0; segment < Segments; ++segment )
        {
            double physicalTotal = 0, logicalTotal = 0;
            for( int category = 0; category < Categories; ++category )
            {
                physicalTotal += values[segment][int( Ledger::Physical )][category];
                logicalTotal += values[segment][int( Ledger::Logical )][category];
            }
            const double usage = physicalTotal + untracked[segment] - ( segment == 0 ? evicted : 0 );
            TracyPlot( DxgiUsagePlots[segment], int64_t( quantize( usage ) ) );
            TracyPlot( DxgiBudgetPlots[segment], int64_t( budget[segment] ) );
            TracyPlot( PhysicalTotalPlots[segment], int64_t( physicalTotal ) );
            TracyPlot( LogicalTotalPlots[segment], int64_t( logicalTotal ) );
            for( int ledger = 0; ledger < Ledgers; ++ledger )
                for( int category = 0; category < Categories; ++category )
                {
                    // The engine has no logical bytes for heaps.
                    if( ledger == int( Ledger::Logical ) && category == int( Category::Heap ) ) continue;
                    const auto value = values[segment][ledger][category];
                    if( m_sent && value == m_last[segment][ledger][category] ) continue;
                    TracyPlot( PlotNames[segment][ledger][category], int64_t( value ) );
                }
        }
        TracyPlot( EvictedBytesPlot, int64_t( evicted ) );
        if( !m_sent ) Configure();
        m_last = values;
        m_sent = true;
        EmitResources();
    }

private:
    using Values = std::array<std::array<std::array<double, tracy::JnVram::Categories>, tracy::JnVram::Ledgers>, tracy::JnVram::Segments>;

    // A resource as the engine registers it: its memory event in the pool of
    // its kind, then its record, a zone named after it.
    static void Allocate( uint64_t id, char kind, const char* name, const char* category, uint64_t bytes )
    {
        const char* pool = kind == 'T' ? "GPU D3D12 Logical Texture" : kind == 'B' ? "GPU D3D12 Logical Buffer" : "GPU D3D12 Logical Upload";
        TracyAllocN( reinterpret_cast<void*>( id ), bytes, pool );
        ZoneScopedN( "GTMEM Request Scope" );
        ZoneName( name, strlen( name ) );
        char record[320];
        const int length = snprintf( record, sizeof( record ),
            "GTMEM1|SCOPE|label=0|frame=0\nGTMEM1|RESOURCE|allocation=%llu|physical=%llu|bytes=%llu|offset=0|owner=0|physical_owner=0|kind=%c|segment=%c|flags=0|category=%s",
            static_cast<unsigned long long>( id ), static_cast<unsigned long long>( id ), static_cast<unsigned long long>( bytes ),
            kind, kind == 'U' ? 'N' : 'L', category );
        ZoneText( record, size_t( length ) );
    }

    static void Free( uint64_t id, char kind )
    {
        TracyFreeN( reinterpret_cast<void*>( id ), kind == 'T' ? "GPU D3D12 Logical Texture" : kind == 'B' ? "GPU D3D12 Logical Buffer" : "GPU D3D12 Logical Upload" );
    }

    // A frame's resources once, and a fog texture made again each time.
    void EmitResources()
    {
        constexpr uint64_t MiB = 1024 * 1024;
        if( !m_resources )
        {
            Allocate( 0x10000, 'T', "GBufferA", "RenderTarget", 32 * MiB );
            Allocate( 0x20000, 'T', "SceneDepth", "DepthStencil", 16 * MiB );
            Allocate( 0x30000, 'T', "Albedo_rock", "Texture", 8 * MiB );
            Allocate( 0x40000, 'B', "TLAS", "RayTracing", 2 * MiB );
            Allocate( 0x50000, 'B', "Clusters", "Buffer", 4 * MiB );
            Allocate( 0x60000, 'U', "Staging", "Buffer", 1 * MiB );
            m_resources = true;
        }
        if( m_fog ) Free( 0x70000, 'T' );
        Allocate( 0x70000, 'T', "SmallMapFog_Texture", "Texture", 1 * MiB );
        m_fog = true;
    }

    static void Configure()
    {
        using namespace tracy::JnVram;
        for( int segment = 0; segment < Segments; ++segment )
        {
            for( const auto name : { DxgiUsagePlots[segment], DxgiBudgetPlots[segment], PhysicalTotalPlots[segment], LogicalTotalPlots[segment] } )
                TracyPlotConfig( name, tracy::PlotFormatType::Memory, true, true, 0 );
            for( int ledger = 0; ledger < Ledgers; ++ledger )
                for( int category = 0; category < Categories; ++category )
                {
                    if( ledger == int( Ledger::Logical ) && category == int( Category::Heap ) ) continue;
                    const auto format = ledger == int( Ledger::Resources ) ? tracy::PlotFormatType::Number : tracy::PlotFormatType::Memory;
                    TracyPlotConfig( PlotNames[segment][ledger][category], format, true, true, CategoryColors[category] );
                }
        }
        TracyPlotConfig( EvictedBytesPlot, tracy::PlotFormatType::Memory, true, true, 0 );
    }

    double m_duration;
    bool m_sent = false, m_resources = false, m_fog = false;
    Values m_last {};
};

// Frame-granularity GPU references as the JN engine sends them: per frame, a
// command list's whole resource set (GPU.Frame.Direct, taxonomy level 0),
// defined once per connection and then referenced by id. Odd frames add the
// TLAS; every set holds a pointer the registry does not know, as a swap chain
// buffer.
class ReferenceScenario
{
public:
    void Emit( int frame )
    {
        if( !tracy::GetProfiler().IsConnected() ) return;
        const bool odd = ( frame & 1 ) != 0;
        auto& defined = m_defined[odd ? 1 : 0];
        const uint32_t set = odd ? 0x80000002u : 0x80000001u;
        tracy::JnGpuReferenceSetEntry entries[8] = {};
        uint16_t count = 0;
        for( const uint64_t id : { 0x10000ull, 0x20000ull, 0x30000ull, 0x50000ull, 0x60000ull, 0x70000ull, 0x99990000ull } )
            entries[count++] = { id, 1u << 14 };
        if( odd ) entries[count++] = { 0x40000ull, 1u << 14 };
        std::sort( entries, entries + count, []( const auto& a, const auto& b ) { return a.resourceId < b.resourceId; } );
        const uint64_t pass = ++m_pass;
        tracy::EmitJnGpuReferencePass( pass, uint64_t( frame ), TaxonomyFrameDirect, 0, 0 );
        if( !defined )
        {
            for( uint16_t i = 0; i < count; i += 2 )
                if( !tracy::EmitJnGpuReferenceSetDefinitionChunk( set, count, entries + i, uint8_t( std::min<int>( 2, count - i ) ) ) ) return;
            defined = true;
        }
        tracy::EmitJnGpuReferenceSetReference( pass, set, count, 0 );
        tracy::EmitJnGpuReferenceEnd( pass, 0, count, 0, uint8_t( tracy::JnGpuReferenceFlags::CommandListBoundary ) );
    }

private:
    // JNTracyGpuTaxonomy_L0_Direct.
    static constexpr uint32_t TaxonomyFrameDirect = 0x00010001u;
    uint64_t m_pass = 0;
    bool m_defined[2] = {};
};

}

int main( int argc, char** argv )
{
    int durationSeconds = 5;
    uint32_t postEndBatchCount = 0;
    bool unresolvedEnrichment = false;
    bool unresolvedCore = false;
    bool vram = false, references = false;
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
            else if( strcmp( argv[i], "--vram" ) == 0 )
                vram = true;
            // The resources of --vram, referenced each frame.
            else if( strcmp( argv[i], "--references" ) == 0 )
                vram = references = true;
            else
            {
                std::fprintf( stderr, "Unknown or incomplete argument: %s\n", argv[i] );
                return 2;
            }
        }
    }

    tracy::SetThreadName( "tracy-stream-test-producer" );
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds( durationSeconds );
    VramScenario vramScenario( durationSeconds );
    ReferenceScenario referenceScenario;
    auto vramSent = start - std::chrono::seconds( 1 );
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
        if( const auto now = std::chrono::steady_clock::now(); vram && now - vramSent >= std::chrono::milliseconds( 250 ) )
        {
            vramScenario.Emit( std::chrono::duration<double>( now - start ).count() );
            vramSent = now;
        }
        // After the resources exist, which the first VRAM sample registers.
        if( references && vramSent >= start ) referenceScenario.Emit( frame );
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
