#define _CRT_SECURE_NO_WARNINGS
#include <cassert>
#include <cstdio>
#include <vector>

#include "../profiler/src/profiler/TracyThreadLayout.hpp"

struct Thread
{
    uint64_t id;
    const char* name;
    int32_t groupHint;
};

int main()
{
    using namespace tracy;
    Thread other { 1, "Audio Thread", 0 };
    Thread job0 { 2, "Job.Worker 0", 0 };
    Thread render { 3, "Render Thread", 0 };
    Thread main { 4, "Main Thread", 0 };
    Thread job1 { 5, "Job.Worker 1", 0 };
    Thread lowHint { 6, "Job.Worker 2", -2 };
    std::vector<const Thread*> threads { &other, &job0, &render, &main, &job1, &lowHint };
    ThreadLayoutState layout;
    auto sort = [&] {
        SortThreadLayout( threads.begin(), threads.end(), layout,
            []( const Thread* thread ) { return thread->name; },
            []( const Thread* thread ) { return thread->id; },
            []( const Thread* thread ) { return thread->groupHint; } );
    };
    sort();
    assert( ( std::vector<uint64_t> { threads[0]->id, threads[1]->id, threads[2]->id,
        threads[3]->id, threads[4]->id, threads[5]->id } == std::vector<uint64_t> { 4, 3, 6, 2, 5, 1 } ) );

    layout.customOrder = true;
    layout.threads = { { 5, false }, { 4, true }, { 2, true } };
    sort();
    assert( ( std::vector<uint64_t> { threads[0]->id, threads[1]->id, threads[2]->id,
        threads[3]->id, threads[4]->id, threads[5]->id } == std::vector<uint64_t> { 5, 4, 2, 3, 6, 1 } ) );

    FILE* file = tmpfile();
    assert( file );
    assert( WriteThreadLayout( file, layout ) );
    rewind( file );
    ThreadLayoutState loaded;
    assert( ReadThreadLayout( file, loaded ) );
    assert( loaded.customOrder && loaded.threads.size() == 3 );
    assert( loaded.threads[0].id == 5 && !loaded.threads[0].visible );
    assert( loaded.threads[1].id == 4 && loaded.threads[1].visible );
    fclose( file );

    file = tmpfile();
    assert( file );
    const uint32_t incompleteVersion = 1;
    fwrite( &incompleteVersion, sizeof( incompleteVersion ), 1, file );
    rewind( file );
    ThreadLayoutState invalid = layout;
    assert( !ReadThreadLayout( file, invalid ) );
    assert( invalid.threads.size() == layout.threads.size() && invalid.customOrder == layout.customOrder );
    fclose( file );
}
