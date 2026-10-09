#ifndef __TRACYEVENTSTORETRACESOURCE_HPP__
#define __TRACYEVENTSTORETRACESOURCE_HPP__

#include "TracyWorkerTraceSource.hpp"

#include <filesystem>
#include <memory>

namespace tracy::query
{

// A trace source over the event store of a `.tracy-stream` journal, for
// recordings too long to load as a standard snapshot. The small domains come
// from a skeleton snapshot that the store exports next to the journal on
// first use (`<journal>.query-skeleton.tracy`), loaded by Tracy's Worker so
// they keep its semantics. CPU and GPU zones, Jobs, graphics records and
// relations are read from the store, numbered as the Worker numbers them in a
// full snapshot of the same store.
class EventStoreTraceSource
{
public:
    // Whether `journal` has a finished event store of the current schema.
    static bool Available( const std::filesystem::path& journal );
    static std::unique_ptr<analysis::TraceSource> Open( const std::filesystem::path& journal,
        analysis::WorkerTraceSource::StateCallback stateCallback = {} );
    static std::filesystem::path SkeletonPath( const std::filesystem::path& journal );
};

}

#endif
