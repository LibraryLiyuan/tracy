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
        manifestInput.close();
        Require( json.at( "sections" ).contains( "gpu_catalog" ),
            "Query index did not publish a GPU Catalog sidecar" );
        const auto catalogRoot = manifest.manifestPath.parent_path() /
            json.at( "sections" ).at( "gpu_catalog" ).at( "file" ).get<std::string>();
        {
            query::QueryCatalogStoreReader reader( catalogRoot, manifest.sourceFingerprint );
            Require( reader.Count( OfflineCatalogDomain::CatalogResource ) == expectedResources,
                "Catalog resource sidecar count changed" );
            Require( reader.Count( OfflineCatalogDomain::CatalogBatch ) == expectedBatches,
                "Catalog batch sidecar count changed" );
            Require( reader.Count( OfflineCatalogDomain::CatalogString ) == expectedStrings,
                "Catalog string sidecar count changed" );
        }

        bool rejectedUnboundCompact = false;
        try { auto compact = analysis::WorkerTraceSource::Open( manifest.dataPath ); }
        catch( const analysis::TraceLoadError& error )
        { rejectedUnboundCompact = error.code == analysis::TraceLoadErrorCode::Corrupt; }
        Require( rejectedUnboundCompact, "manifest-bound compact payload opened without its external dictionary" );
        auto indexed = query::QueryIndex::Open( manifest );
        Require( !indexed->CatalogDerivedDataReady(),
            "indexed trace silently claims GPU Catalog indices are ready" );
        const auto* statusSource = dynamic_cast<const query::QueryCatalogStatusSource*>( indexed.get() );
        Require( statusSource != nullptr, "indexed trace does not expose bounded Catalog status" );
        const auto status = statusSource->GetQueryCatalogStatus( 64, 16 );
        const auto& validation = statusSource->GetQueryCatalogValidation();
        Require( validation.complete && validation.healthy && status.metadata[0] == 1 &&
            status.counts[size_t( OfflineCatalogDomain::CatalogResource )] == expectedResources &&
            status.counts[size_t( OfflineCatalogDomain::CatalogBatch )] == expectedBatches,
            "indexed bounded Catalog status changed" );
        const auto activeData = manifest.dataPath;
        const auto second = query::QueryIndex::Build( input );
        Require( fs::exists( activeData ), "published rebuild removed a generation held by an active indexed reader" );
        indexed.reset();
        const std::string generationPrefix = "jnidx." + manifest.sourceFingerprint.substr( 0, 16 ) +
            ".s" + std::to_string( query::QueryIndexSchemaVersion ) + ".g";
        auto currentBase = second.dataPath.filename().string();
        const std::string dataSuffix = ".jnidx.data";
        currentBase.erase( currentBase.size() - dataSuffix.size() );
        std::vector<fs::path> retained;
        std::string cleanupError;
        Require( query::QueryIndex::CleanupUnreferencedGenerationsForTesting(
                root, generationPrefix, currentBase, retained, cleanupError ) &&
            !fs::exists( activeData ) && fs::exists( second.dataPath ) && retained.empty(),
            "unused Query generations were not reclaimed after their reader lease ended" );
        std::cout << "GPU Catalog index integration passed resources=" << expectedResources << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
