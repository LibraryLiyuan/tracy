#include "../src/OfflineConversionStorage.hpp"
#include <chrono>
#include <cstdio>
#include <vector>

static void Check( bool value, const char* message ) { if( !value ) throw std::runtime_error( message ); }

int main( int argc, char** argv )
{
    try
    {
        if( argc != 2 ) return 2;
        auto root = std::filesystem::path( argv[1] ) / std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
        std::filesystem::create_directories( root );
        tracy::storage::ResourceLimits limits;
        limits.streamBytes = 64ull << 20;
        limits.diskReserveBytes = 0;
        tracy::storage::ResourceSample sample;
        sample.systemCommitLimit = 64ull << 30;
        tracy::storage::ConversionResources budget( limits, [&] { return sample; }, [] { return 1ull << 40; } );
        tracy::conversion::OfflineConversionStorage storage( root / "store", std::string( 64, 'd' ), budget );

        const auto queueA = storage.CreateGpuThread();
        const auto queueB = storage.CreateGpuThread();
        tracy::OfflineGpuZone row { 10, -1, -1, -1, 1, 0, 2, 7 };
        const auto parent = storage.AppendGpuZone( queueA, row );
        row = { 12, -1, -1, -1, 2, 3, 4, 8 };
        const auto child = storage.AppendGpuZone( parent, row );
        row = { 11, 15, 20, 25, 3, 6, 5, 9 };
        storage.AppendGpuZone( queueB, row );
        storage.ReadGpuZone( child, row );
        row.cpuEnd = 21;
        row.gpuStart = 31;
        row.gpuEnd = 41;
        row.callstack = 9;
        storage.PutGpuZone( child, row );
        storage.ReadGpuZone( child, row );
        Check( row.cpuStart == 12 && row.cpuEnd == 21 && row.gpuStart == 31 && row.gpuEnd == 41 && row.callstack == 9, "gpu_zone_update" );

        storage.SealGpuZones();
        Check( storage.GpuRootChildren( queueA ) == 1 && storage.GpuRootChildren( queueB ) == 1, "gpu_zone_root_children" );
        std::vector<int64_t> events;
        storage.VisitGpuZones( queueA, [&]( uint64_t, const tracy::OfflineGpuZone& zone, uint32_t children, bool begin ) {
            events.push_back( begin ? zone.cpuStart : zone.cpuEnd );
            if( zone.cpuStart == 10 ) Check( children == 1, "gpu_zone_child_count" );
        } );
        Check( events == std::vector<int64_t>( { 10, 12, 21, -1 } ), "gpu_zone_traversal" );
        Check( storage.GpuZoneCount() == 3 && storage.GpuThreadCount() == 2 && storage.GpuBranchCount() == 3, "gpu_zone_counts" );
        std::puts( "offline GPU zone roots, updates and ordered traversal passed" );
        return 0;
    }
    catch( const std::exception& e )
    {
        std::fprintf( stderr, "%s\n", e.what() );
        return 1;
    }
}
