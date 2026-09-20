#include "TracyQueryGpuReferenceStore.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{

void Require( bool condition, const char* message )
{
    if( !condition ) throw std::runtime_error( message );
}

class Input final : public tracy::query::QueryGpuReferenceInput
{
public:
    uint64_t PassCount() const override { return passes.size(); }
    uint64_t UseCount() const override { return uses.size(); }
    uint64_t EndCount() const override { return ends.size(); }
    tracy::JnGpuReferencePassData Pass( uint64_t ordinal ) const override { return passes.at( size_t( ordinal ) ); }
    tracy::JnGpuReferenceUseData Use( uint64_t ordinal ) const override { return uses.at( size_t( ordinal ) ); }
    tracy::JnGpuReferenceEndData End( uint64_t ordinal ) const override { return ends.at( size_t( ordinal ) ); }
    uint64_t ResolveCatalogResource( uint64_t pointerToken, int64_t time ) const override
    {
        if( pointerToken != 100 ) return 0;
        return time <= 20 ? 200 : 201;
    }

    std::vector<tracy::JnGpuReferencePassData> passes;
    std::vector<tracy::JnGpuReferenceUseData> uses;
    std::vector<tracy::JnGpuReferenceEndData> ends;
};

}

int main()
{
    try
    {
        namespace fs = std::filesystem;
        const auto root = fs::temp_directory_path() / "tracy-query-gpu-reference-store";
        std::error_code ignored;
        fs::remove_all( root, ignored );
        fs::create_directories( root );
        Input input;
        input.passes = { { 10, 1, 0, 1, 0, 0, 0 }, { 30, 2, 0, 1, 0, 0, 0 } };
        input.ends = { { 20, 1, 0, 1, 0, 0, 0 } };
        input.uses = {
            { 30, 2, 100, 1, 5, 8, 0, 1 },
            { 10, 1, 100, 1, 3, 7, 0, 2 },
            { 10, 1, 0, 1, 1, 0, 0, 1 },
            { 40, 999, 100, 1, 1, 0, 0, 1 }
        };
        const std::string identity( 64, 'a' );
        tracy::query::QueryGpuReferenceStore::Build( root, identity, input );
        Require( !fs::exists( root / "reverse-scratch" ), "GPU reference scratch was retained" );
        tracy::query::QueryGpuReferenceStoreReader reader( root, identity );
        reader.ValidateAll( [&]( uint64_t pointerToken, int64_t time ) {
            return input.ResolveCatalogResource( pointerToken, time );
        } );
        const auto first = reader.UsesByResource( 100, 0, 1 );
        const auto second = reader.UsesByResource( 100, 1, 1 );
        Require( first.total == 2 && first.values.size() == 1 && first.values[0].passId == 2 &&
            first.values[0].passStart == 30 && first.values[0].passEnd == 30 &&
            first.values[0].usageMask == 5 && first.values[0].resourceSetId == 8,
            "GPU reference first resource page changed" );
        Require( second.total == 2 && second.values.size() == 1 && second.values[0].passId == 1 &&
            second.values[0].passStart == 10 && second.values[0].passEnd == 20 &&
            second.values[0].encoding == 2,
            "GPU reference continuation or open pass boundary changed" );
        Require( reader.UsesByResource( 999, 0, 10 ).total == 0,
            "GPU reference missing resource behavior changed" );
        const auto catalog200 = reader.UsesByCatalogResource( 200, 0, 10 );
        const auto catalog201 = reader.UsesByCatalogResource( 201, 0, 10 );
        Require( catalog200.total == 1 && catalog200.values.size() == 1 &&
            catalog200.values[0].passId == 1 && catalog200.values[0].resourceId == 100 &&
            catalog201.total == 1 && catalog201.values.size() == 1 &&
            catalog201.values[0].passId == 2 && catalog201.values[0].resourceId == 100,
            "GPU reference Catalog resource lifetime resolution changed" );
        fs::remove_all( root, ignored );
        std::cout << "query GPU reference store passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
