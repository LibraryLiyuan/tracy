#pragma once

#include <algorithm>
#include <cstdint>

#ifdef _WIN32
#  include <Windows.h>
#  include <Psapi.h>
#endif

// Physical memory load and system commit are independent pressure signals.
struct ProcessMemoryState
{
    uint64_t workingSet = 0;
    uint64_t commit = 0;
    uint64_t physicalAvailable = 0;
    uint64_t physicalTotal = 0;
    uint64_t systemCommitTotal = 0;
    uint64_t systemCommitLimit = 0;
    uint32_t physicalLoadPercent = 0;
    uint32_t commitLoadPercent = 0;
    bool systemCommitAvailable = false;
};

inline uint32_t SystemCommitLoadPercent( uint64_t total, uint64_t limit )
{
    if( limit == 0 ) return 0;
    if( total >= limit ) return 100;
    // Find floor(100 * total / limit) without overflow or rounding across
    // the pressure threshold. The threshold calculation never exceeds limit.
    uint32_t low = 0, high = 100;
    while( low + 1 < high )
    {
        const auto percent = ( low + high ) / 2;
        const auto threshold = ( limit / 100 ) * percent + ( ( limit % 100 ) * percent + 99 ) / 100;
        if( total >= threshold ) low = percent;
        else high = percent;
    }
    return low;
}

inline bool MemoryPressureExceeded( const ProcessMemoryState& state )
{
    const bool commitPressure = state.systemCommitAvailable && state.commitLoadPercent >= 85;
    if( state.physicalTotal == 0 ) return commitPressure;
    const auto minimumAvailable = std::max<uint64_t>( 8ull << 30, state.physicalTotal * 15 / 100 );
    return commitPressure || state.commit > state.physicalTotal * 60 / 100 ||
        state.physicalAvailable < minimumAvailable;
}

inline ProcessMemoryState GetProcessMemoryState()
{
    ProcessMemoryState state;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof( counters );
    if( GetProcessMemoryInfo( GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>( &counters ), sizeof( counters ) ) )
    {
        state.workingSet = counters.WorkingSetSize;
        state.commit = counters.PrivateUsage;
    }
    MEMORYSTATUSEX memory {};
    memory.dwLength = sizeof( memory );
    if( GlobalMemoryStatusEx( &memory ) )
    {
        state.physicalAvailable = memory.ullAvailPhys;
        state.physicalTotal = memory.ullTotalPhys;
        state.physicalLoadPercent = memory.dwMemoryLoad;
    }
    PERFORMANCE_INFORMATION performance {};
    performance.cb = sizeof( performance );
    if( GetPerformanceInfo( &performance, sizeof( performance ) ) )
    {
        state.systemCommitTotal = uint64_t( performance.CommitTotal ) * performance.PageSize;
        state.systemCommitLimit = uint64_t( performance.CommitLimit ) * performance.PageSize;
        state.systemCommitAvailable = performance.CommitLimit != 0;
        state.commitLoadPercent = SystemCommitLoadPercent( performance.CommitTotal, performance.CommitLimit );
    }
#endif
    return state;
}

struct ProcessActivityState
{
    uint64_t cpuTicks100ns = 0, readBytes = 0, writeBytes = 0;
    bool available = false;
};

inline ProcessActivityState GetProcessActivityState()
{
    ProcessActivityState state;
#ifdef _WIN32
    FILETIME creation {}, exit {}, kernel {}, user {};
    IO_COUNTERS io {};
    if( GetProcessTimes( GetCurrentProcess(), &creation, &exit, &kernel, &user ) &&
        GetProcessIoCounters( GetCurrentProcess(), &io ) )
    {
        state.cpuTicks100ns = ( uint64_t( kernel.dwHighDateTime ) << 32 ) + kernel.dwLowDateTime +
            ( uint64_t( user.dwHighDateTime ) << 32 ) + user.dwLowDateTime;
        state.readBytes = io.ReadTransferCount;
        state.writeBytes = io.WriteTransferCount;
        state.available = true;
    }
#endif
    return state;
}
