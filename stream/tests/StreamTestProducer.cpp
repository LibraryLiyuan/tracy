#include "tracy/Tracy.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace
{

void NestedWork( int value )
{
    ZoneScopedN( "Synthetic nested work" );
    ZoneValue( value );
    std::this_thread::sleep_for( std::chrono::microseconds( 250 ) );
}

}

int main( int argc, char** argv )
{
    int durationSeconds = 5;
    if( argc == 2 ) durationSeconds = std::max( 1, std::atoi( argv[1] ) );

    tracy::SetThreadName( "tracy-stream-test-producer" );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( durationSeconds );
    int frame = 0;
    while( std::chrono::steady_clock::now() < deadline )
    {
        ZoneScopedN( "Synthetic frame" );
        NestedWork( frame );
        TracyPlot( "Synthetic frame index", int64_t( frame ) );
        if( frame % 32 == 0 ) TracyMessageL( "tracy-stream protocol acceptance tick" );
        FrameMark;
        frame++;
        std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
    }
    return 0;
}
