#include "TracyQueryCatalogStore.hpp"
#include "TracyQueryIndex.hpp"
#include "TracyWorkerTraceSource.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
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
        if( argc != 3 ) throw std::runtime_error( "expected Catalog Trace and isolated output directory" );
        namespace fs = std::filesystem;
        using namespace tracy;
        const fs::path root = argv[2];
        if( fs::exists( root ) ) throw std::runtime_error( "refusing existing output directory" );
        fs::create_directories( root );
        const auto input = root / "catalog.tracy";
        fs::copy_file( argv[1], input );

        auto original = analysis::WorkerTraceSource::Open( input );
        auto catalog = original->GetGpuCatalogData();
        Require( catalog && catalog->gpuCatalogPresent && !catalog->gpuCatalogResources.empty(),
            "fixture has no GPU Catalog resources" );
        const auto expectedResources = catalog->gpuCatalogResources.size();
        const auto expectedBatches = catalog->gpuCatalogBatches.size();
        const auto expectedStrings = catalog->gpuCatalogStrings.size();
        original.reset(); catalog.reset();

        const auto manifest = query::QueryIndex::Build( input );
        std::ifstream manifestInput( manifest.manifestPath );
        const auto json = nlohmann::json::parse( manifestInput );
        Require( json.at( "sections" ).contains( "gpu_catalog" ),
            "Query index did not publish a GPU Catalog sidecar" );
        const auto catalogRoot = manifest.manifestPath.parent_path() /
            json.at( "sections" ).at( "gpu_catalog" ).at( "file" ).get<std::string>();
        query::QueryCatalogStoreReader reader( catalogRoot, manifest.sourceFingerprint );
        Require( reader.Count( OfflineCatalogDomain::CatalogResource ) == expectedResources,
            "Catalog resource sidecar count changed" );
        Require( reader.Count( OfflineCatalogDomain::CatalogBatch ) == expectedBatches,
            "Catalog batch sidecar count changed" );
        Require( reader.Count( OfflineCatalogDomain::CatalogString ) == expectedStrings,
            "Catalog string sidecar count changed" );

        auto compact = analysis::WorkerTraceSource::Open( manifest.dataPath );
        Require( compact->GetGpuCatalogData()->gpuCatalogResources.empty(),
            "compact payload still retains the full GPU Catalog" );
        compact.reset();
        auto indexed = query::QueryIndex::Open( manifest );
        Require( !indexed->CatalogDerivedDataReady(),
            "indexed trace silently claims GPU Catalog indices are ready" );
        std::cout << "GPU Catalog index integration passed resources=" << expectedResources << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
