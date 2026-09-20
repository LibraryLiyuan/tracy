#include "TracyQueryCatalogStore.hpp"
#include "TracyQueryService.hpp"
#include "TracySessionManager.hpp"
#include "FakeTraceSource.hpp"

#include <cassert>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace
{

using namespace tracy;
using namespace tracy::query;

class BoundedCatalogSource final : public test::FakeTraceSource,
    public QueryCatalogStatusSource, public QueryCatalogResourceSource
{
public:
    BoundedCatalogSource()
    {
        m_validation.complete = true;
        m_validation.present = true;
        m_validation.originalValid = true;
        m_validation.healthy = true;
        for( uint64_t id = 100; id <= 102; id++ )
        {
            QueryCatalogResourceStateData state {};
            state.latest.resourceId = id;
            state.latest.allocationId = id == 102 ? 600 : 500;
            state.latest.operation = uint8_t( JnGpuCatalogRecordOperation::Create );
            state.generation = 9;
            state.createTime = int64_t( id );
            m_states[id - 100] = state;
            m_associations[id - 100].resourceId = id;
        }
        m_associations[0].meshPartCount = 2;
        m_associations[0].rangeCount = 1;
        m_associations[0].logicalBindingCount = 1;
        m_associations[0].allocationResourceCount = 2;
        m_associations[0].hasAliasGroup = 1;
        m_associations[1].logicalBindingCount = 1;
        m_associations[1].allocationResourceCount = 2;
        m_associations[2].rangeCount = 1;
        m_associations[2].allocationResourceCount = 1;
    }

    bool CatalogDerivedDataReady() const override { return false; }
    QueryCatalogStatusData GetQueryCatalogStatus( size_t, size_t ) const override { return {}; }
    const QueryCatalogValidationResult& GetQueryCatalogValidation() const override { return m_validation; }
    uint64_t GetQueryCatalogResourceCount() const override { return m_states.size(); }
    QueryCatalogResourceStateData GetQueryCatalogResourceStateAt( uint64_t ordinal ) const override
    {
        return m_states.at( size_t( ordinal ) );
    }
    std::optional<QueryCatalogResourceStateData> GetQueryCatalogResourceState( uint64_t resourceId ) const override
    {
        return resourceId >= 100 && resourceId <= 102 ?
            std::optional( m_states[size_t( resourceId - 100 )] ) : std::nullopt;
    }
    std::optional<std::string> GetQueryCatalogResourceName( uint64_t, uint32_t ) const override
    {
        return std::nullopt;
    }
    QueryCatalogResourceAssociationData GetQueryCatalogResourceAssociationsAt( uint64_t ordinal ) const override
    {
        return m_associations.at( size_t( ordinal ) );
    }

private:
    QueryCatalogValidationResult m_validation;
    std::array<QueryCatalogResourceStateData, 3> m_states;
    std::array<QueryCatalogResourceAssociationData, 3> m_associations;
};

nlohmann::json Request( int id, std::string method, nlohmann::json params )
{
    return { { "protocol", "tracy-query/1" }, { "id", id },
        { "method", std::move( method ) }, { "params", std::move( params ) } };
}

}

int main()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "tracy-query-catalog-association-search";
    std::error_code ignored;
    fs::remove_all( root, ignored );
    fs::create_directories( root );
    const auto trace = root / "catalog.tracy";
    std::ofstream( trace, std::ios::binary ).put( '\0' );

    tracy::query::SessionManager sessions( { root }, 1,
        []( const fs::path&, tracy::query::SessionManager::StateCallback callback )
            -> std::unique_ptr<tracy::analysis::TraceSource> {
            callback( tracy::analysis::TraceSourceState::Indexing );
            return std::make_unique<BoundedCatalogSource>();
        } );
    tracy::query::QueryService service( sessions, 1024 * 1024 );
    const auto opened = service.Execute( Request( 1, "trace.open", { { "path", trace.string() } } ) );
    assert( opened.at( "ok" ) );
    const auto traceId = opened.at( "data" ).at( "trace_id" ).get<std::string>();
    assert( sessions.WaitReady( traceId, std::chrono::seconds( 5 ) ).state ==
        tracy::analysis::TraceSourceState::Ready );

    const auto search = [&]( int id, const char* filter ) {
        auto response = service.Execute( Request( id, "gpu.resource.search", {
            { "trace_id", traceId }, { "filter", { { filter, true } } }, { "limit", 10 }
        } ) );
        assert( response.at( "ok" ) );
        return response.at( "data" );
    };
    const auto mesh = search( 2, "has_mesh_parts" );
    assert( mesh.at( "total" ) == "1" && mesh.at( "resources" )[0].at( "resource_id" ) == "100" &&
        mesh.at( "resources" )[0].at( "mesh_part_count" ) == "2" );
    const auto ranges = search( 3, "has_ranges" );
    assert( ranges.at( "total" ) == "2" && ranges.at( "resources" )[0].at( "pass_range_count" ) == "1" );
    const auto logical = search( 4, "has_logical_bindings" );
    assert( logical.at( "total" ) == "2" && logical.at( "resources" )[0].at( "logical_binding_count" ) == "1" );
    const auto aliases = search( 5, "has_alias_group" );
    assert( aliases.at( "total" ) == "1" && aliases.at( "resources" )[0].at( "resource_id" ) == "100" );
    const auto shared = search( 6, "has_shared_allocation" );
    assert( shared.at( "total" ) == "2" && shared.at( "resources" )[0].at( "allocation_resource_count" ) == "2" );

    assert( service.Execute( Request( 7, "trace.close", { { "trace_id", traceId } } ) ).at( "ok" ) );
    fs::remove_all( root, ignored );
    return 0;
}
