#include "TracyGpuAnalysisSidecar.hpp"
#include "TracyGpuAnalysisStore.hpp"
#include "TracyHash.hpp"
#include "../../public/common/TracyQueue.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

using namespace tracy;
using namespace tracy::analysis;

namespace
{

bool ParseU64( std::string_view text, uint64_t& value )
{
    const auto result = std::from_chars( text.data(), text.data() + text.size(), value );
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

void Usage()
{
    std::fprintf( stderr, "Usage: tracy-gpu-analysis-corpus --trace output.tracy [--frames N] [--frame-base N] [--passes-per-frame N] [--resources-per-pass N] [--resources N]\n" );
}

}

int main( int argc, char** argv )
{
    std::filesystem::path tracePath;
    uint64_t frames = 216000, frameBase = uint64_t( 1 ) << 40, passesPerFrame = 2, resourcesPerPass = 8, resourceCount = 65536;
    for( int i = 1; i < argc; ++i )
    {
        const std::string_view arg = argv[i];
        if( arg == "--trace" && i + 1 < argc ) tracePath = std::filesystem::u8path( argv[++i] );
        else if( i + 1 < argc && ( arg == "--frames" || arg == "--frame-base" || arg == "--passes-per-frame" ||
            arg == "--resources-per-pass" || arg == "--resources" ) )
        {
            uint64_t value = 0; if( !ParseU64( argv[++i], value ) ) { Usage(); return 1; }
            if( arg == "--frames" ) frames = value; else if( arg == "--frame-base" ) frameBase = value;
            else if( arg == "--passes-per-frame" ) passesPerFrame = value;
            else if( arg == "--resources-per-pass" ) resourcesPerPass = value; else resourceCount = value;
        }
        else { Usage(); return 1; }
    }
    if( tracePath.empty() || frames == 0 || passesPerFrame == 0 || resourcesPerPass == 0 || resourceCount == 0 ||
        frames > std::numeric_limits<uint64_t>::max() / passesPerFrame ) { Usage(); return 1; }
    const uint64_t passCount = frames * passesPerFrame;
    if( passCount > std::numeric_limits<size_t>::max() || passCount > std::numeric_limits<uint64_t>::max() / resourcesPerPass )
    { std::fprintf( stderr, "Synthetic corpus size overflows this process.\n" ); return 1; }
    const uint64_t useCount = passCount * resourcesPerPass;
    if( useCount > std::numeric_limits<size_t>::max() ) { std::fprintf( stderr, "Synthetic relation count exceeds address space.\n" ); return 1; }

    const auto start = std::chrono::steady_clock::now();
    JnTraceData data; data.present = true; data.schemaVersion = 12; data.gpuCatalogPresent = true; data.gpuCatalogValid = true;
    data.gpuCatalogSchemaVersion = JnGpuCatalogSchemaVersion; data.gpuDetailedEvidenceSchemaVersion = JnGpuDetailedEvidenceSchemaVersion;
    data.gpuCatalogResources.reserve( size_t( resourceCount ) ); data.gpuCatalogAllocations.reserve( size_t( resourceCount ) );
    for( uint64_t index = 0; index < resourceCount; ++index )
    {
        JnGpuCatalogAllocationRecordV1 allocation {}; allocation.time = 1; allocation.allocationId = index + 1;
        allocation.pointerToken = index + 1; allocation.sizeBytes = 1024 * 1024; allocation.residentBytes = allocation.sizeBytes;
        allocation.operation = uint8_t( JnGpuCatalogRecordOperation::Create ); allocation.exactness = uint8_t( JnGpuCatalogExactness::Exact );
        data.gpuCatalogAllocations.push_back( allocation );
        JnGpuCatalogResourceRecordV1 resource {}; resource.time = 2; resource.resourceId = index + 1; resource.pointerToken = index + 1;
        resource.allocationId = index + 1; resource.capacityBytes = allocation.sizeBytes; resource.width = 512; resource.height = 512;
        resource.mipLevels = 1; resource.primaryKind = uint16_t( index % 4 == 0 ? JnGpuCatalogPrimaryKind::StructuredBuffer : JnGpuCatalogPrimaryKind::Texture );
        resource.operation = uint8_t( JnGpuCatalogRecordOperation::Create ); resource.exactness = uint8_t( JnGpuCatalogExactness::Exact );
        data.gpuCatalogResources.push_back( resource );
    }
    data.gpuCatalogBatches.push_back( { 1, 0, 0, 0, 1, uint32_t( resourceCount ), uint32_t( resourceCount * sizeof( JnGpuCatalogResourceRecordV1 ) ),
        uint8_t( JnGpuCatalogBatchKind::Resource ), 1, 0, 1 } );
    data.gpuCatalogBatches.push_back( { 1, 0, 0, 0, 2, uint32_t( resourceCount ), uint32_t( resourceCount * sizeof( JnGpuCatalogAllocationRecordV1 ) ),
        uint8_t( JnGpuCatalogBatchKind::Allocation ), 1, 0, 1 } );
    JnGpuCatalogGenerationData generation {}; generation.generation = 1; generation.lastSequence = 2; generation.batchCount = 2;
    generation.recordCount = resourceCount * 2; generation.payloadBytes = resourceCount * ( sizeof( JnGpuCatalogResourceRecordV1 ) + sizeof( JnGpuCatalogAllocationRecordV1 ) );
    generation.state = uint8_t( JnGpuCatalogGenerationState::Complete ); generation.began = generation.ended = generation.valid = 1;
    data.gpuCatalogGenerations.push_back( generation );

    data.gpuReferencePasses.reserve( size_t( passCount ) ); data.gpuReferenceEnds.reserve( size_t( passCount ) );
    data.gpuReferenceUses.reserve( size_t( useCount ) );
    uint64_t passId = 1;
    for( uint64_t frame = 0; frame < frames; ++frame ) for( uint64_t pass = 0; pass < passesPerFrame; ++pass, ++passId )
    {
        const int64_t begin = int64_t( frame * 16666667ull + pass * 1000 );
        data.gpuReferencePasses.push_back( { begin, passId, frameBase + frame, 1, uint32_t( pass ), 1, 0 } );
        for( uint64_t use = 0; use < resourcesPerPass; ++use )
        {
            const auto resourceId = ( passId * 1315423911ull + use * 2654435761ull ) % resourceCount + 1;
            data.gpuReferenceUses.push_back( { begin + int64_t( use ), passId, resourceId, 1, 1, uint32_t( resourceId ), 0, 2 } );
        }
        data.gpuReferenceEnds.push_back( { begin + 900, passId, passId, 1, uint32_t( resourcesPerPass ), 0, 0 } );
    }

    std::filesystem::create_directories( tracePath.parent_path() );
    { std::ofstream trace( tracePath, std::ios::binary | std::ios::trunc ); trace << "N29 synthetic one-hour equivalent corpus\n" << frames << ' ' << passCount << ' ' << useCount << '\n'; }
    auto identity = ComputeGpuAnalysisQuickIdentity( tracePath ); identity.sha256 = Sha256File( tracePath );
    const auto sidecar = GpuAnalysisSidecarPath( tracePath ); std::error_code ec; std::filesystem::remove_all( sidecar, ec );
    GpuAnalysisSidecarControl control; control.minimumFreeBytes = 0; std::string error;
    if( !WriteGpuAnalysisRawSidecar( sidecar, identity, data, control, error ) ) { std::fprintf( stderr, "raw sidecar failed: %s\n", error.c_str() ); return 2; }
    data = {};
    if( !BuildGpuAnalysisDerived( tracePath, control, error ) ) { std::fprintf( stderr, "derived sidecar failed: %s\n", error.c_str() ); return 3; }
    const auto elapsed = std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count();
    const auto reader = GpuAnalysisStoreReader::Open( tracePath, true, nullptr, error );
    if( !reader || reader->Manifest().passCount != passCount || reader->Manifest().framePassRelationCount != passCount )
    { std::fprintf( stderr, "sidecar verification failed: %s\n", error.c_str() ); return 4; }
    std::printf( "{\"schema\":1,\"frames\":\"%llu\",\"frame_base\":\"%llu\",\"passes\":\"%llu\",\"relations\":\"%llu\",\"seconds\":%.3f,\"sidecar_bytes\":\"%llu\"}\n",
        static_cast<unsigned long long>( frames ), static_cast<unsigned long long>( frameBase ), static_cast<unsigned long long>( passCount ),
        static_cast<unsigned long long>( useCount ), elapsed, static_cast<unsigned long long>( reader->Manifest().totalBytes ) );
    return 0;
}
