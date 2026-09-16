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
        tracy::conversion::OfflineConversionStorage storage( root / "store", std::string( 64, 'c' ), budget );

        const auto threadA = storage.CreateCpuThread();
        const auto threadB = storage.CreateCpuThread();
        tracy::OfflineCpuZone row { 10, -1, 1, 0 };
        const auto parent = storage.AppendCpuZone( threadA, row );
        row = { 12, -1, 2, 0 };
        const auto child = storage.AppendCpuZone( parent, row );
        row = { 11, 15, 3, 7 };
        storage.AppendCpuZone( threadB, row );
        storage.ReadCpuZone( child, row );
        row.end = 20;
        row.extra = 9;
        storage.PutCpuZone( child, row );
        storage.ReadCpuZone( child, row );
        Check( row.start == 12 && row.end == 20 && row.sourceLocation == 2 && row.extra == 9, "cpu_zone_update" );

        storage.SealCpuZones();
        Check( storage.CpuRootChildren( threadA ) == 1 && storage.CpuRootChildren( threadB ) == 1, "cpu_zone_root_children" );
        std::vector<int64_t> events;
        storage.VisitCpuZones( threadA, [&]( uint64_t, const tracy::OfflineCpuZone& zone, uint32_t children, bool begin ) {
            events.push_back( begin ? zone.start : zone.end );
            if( zone.start == 10 ) Check( children == 1, "cpu_zone_child_count" );
        } );
        Check( events == std::vector<int64_t>( { 10, 12, 20, -1 } ), "cpu_zone_traversal_order" );
        Check( storage.CpuZoneCount() == 3 && storage.CpuThreadCount() == 2, "cpu_zone_counts" );
        std::puts( "offline CPU zone roots, updates and ordered traversal passed" );
        return 0;
    }
    catch( const std::exception& e )
    {
        std::fprintf( stderr, "%s\n", e.what() );
        return 1;
    }
}
