#include "TracyQueryIndex.hpp"
#include "TracyWorkerTraceSource.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

}

int main( int argc, char** argv )
{
    try
    {
        if( argc != 3 ) throw std::runtime_error( "expected Job Trace and isolated output directory" );
        namespace fs = std::filesystem;
        const fs::path root = argv[2];
        if( fs::exists( root ) ) throw std::runtime_error( "refusing existing output directory" );
        fs::create_directories( root );
        const auto input = root / "jobs.tracy";
        fs::copy_file( argv[1], input );

        auto original = tracy::analysis::WorkerTraceSource::Open( input );
        const auto expectedStages = original->GetTraceInfo().counts.jobStages;
        Require( expectedStages != 0, "fixture has no Job stages" );
        original.reset();

        const auto manifest = tracy::query::QueryIndex::Build( input );
        Require( manifest.jobStages.count == expectedStages, "Job-stage sidecar count changed" );
        auto compact = tracy::analysis::WorkerTraceSource::Open( manifest.dataPath );
        Require( compact->GetTraceInfo().counts.jobStages == 0,
            "compact payload still retains the full Job-stage table" );
        compact.reset();

        auto indexed = tracy::query::QueryIndex::Open( manifest );
        Require( indexed->GetTraceInfo().counts.jobStages == expectedStages,
            "indexed trace lost Job-stage count" );
        Require( !indexed->JobDerivedDataReady(),
            "indexed trace silently claims Job aggregation is ready" );
        std::cout << "Job-stage index integration passed records=" << expectedStages << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
