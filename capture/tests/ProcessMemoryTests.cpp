#include "../src/ProcessMemory.hpp"

#include <cstdio>
#include <limits>

int main()
{
    int failures = 0;
    const auto check = [&]( bool valid, const char* message ) {
        if( !valid ) { std::fprintf( stderr, "FAIL: %s\n", message ); ++failures; }
    };
    check( SystemCommitLoadPercent( 85, 100 ) == 85, "commit uses total / limit" );
    check( SystemCommitLoadPercent( 8499, 10000 ) == 84, "load below pressure threshold stays below it" );
    check( SystemCommitLoadPercent( 1, 0 ) == 0, "unknown limit avoids division by zero" );
    check( SystemCommitLoadPercent( 101, 100 ) == 100, "commit percentage is clamped" );
    check( SystemCommitLoadPercent( std::numeric_limits<uint64_t>::max() / 2,
        std::numeric_limits<uint64_t>::max() ) == 49, "percentage does not overflow or round up" );
    ProcessMemoryState state;
    state.physicalTotal = 64ull << 30;
    state.physicalAvailable = 32ull << 30;
    state.physicalLoadPercent = 90;
    state.systemCommitAvailable = true;
    state.commitLoadPercent = 40;
    check( !MemoryPressureExceeded( state ), "physical load is not mislabeled as commit pressure" );
    state.physicalLoadPercent = 40;
    state.commitLoadPercent = 85;
    check( MemoryPressureExceeded( state ), "system commit pressure is detected with available physical memory" );
    state.systemCommitAvailable = false;
    check( !MemoryPressureExceeded( state ), "failed commit API is not treated as measured pressure" );
    state.physicalAvailable = 1ull << 30;
    check( MemoryPressureExceeded( state ), "existing physical memory protection remains effective" );
#ifdef _WIN32
    const auto actual = GetProcessMemoryState();
    check( actual.systemCommitAvailable && actual.systemCommitLimit > 0, "GetPerformanceInfo supplies commit counters" );
    check( actual.commitLoadPercent == SystemCommitLoadPercent( actual.systemCommitTotal, actual.systemCommitLimit ),
        "reported percentage matches measured system commit bytes" );
    check( actual.physicalTotal > 0 && actual.workingSet > 0, "physical and process counters are available" );
#endif
    if( failures == 0 ) std::puts( "Process memory tests passed." );
    return failures ? 1 : 0;
}
