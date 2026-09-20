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
            state.latest.capacityBytes = ( id - 99 ) * 1024;
            state.latest.primaryKind = uint16_t( id == 102 ?
                JnGpuCatalogPrimaryKind::RenderTarget : JnGpuCatalogPrimaryKind::Texture );
            state.latest.operation = uint8_t( JnGpuCatalogRecordOperation::Create );
            state.generation = 9;
            state.createTime = int64_t( id );
            state.aliveAtEnd = true;
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
    QueryCatalogAllocationPage GetQueryCatalogResourceAllocations(
        uint64_t resourceId, uint64_t offset, size_t limit ) const override
    {
        QueryCatalogAllocationPage page;
        if( resourceId != 100 ) return page;
        page.total = 2;
        for( uint64_t index = offset; index < std::min<uint64_t>( 2, offset + limit ); index++ )
        {
            QueryCatalogAllocationData value {};
            value.record.time = int64_t( 10 + index );
            value.record.allocationId = 500;
            value.record.sizeBytes = 4096 * ( index + 1 );
            value.record.operation = uint8_t( index == 0 ?
                JnGpuCatalogRecordOperation::Create : JnGpuCatalogRecordOperation::Update );
            value.generation = 9;
            page.values.emplace_back( value );
        }
        return page;
    }
    QueryCatalogViewPage GetQueryCatalogResourceViews(
        uint64_t resourceId, uint64_t offset, size_t limit ) const override
    {
        QueryCatalogViewPage page;
        if( resourceId != 100 ) return page;
        page.total = 2;
        for( uint64_t index = offset; index < std::min<uint64_t>( 2, offset + limit ); index++ )
        {
            QueryCatalogViewData value {};
            value.record.viewId = 800 + index;
            value.record.resourceId = 100;
            value.record.bufferLengthBytes = 1024 * ( index + 1 );
            value.generation = 11;
            page.values.emplace_back( value );
        }
        return page;
    }
    QueryCatalogPartPage GetQueryCatalogResourceMeshParts(
        uint64_t resourceId, uint64_t offset, size_t limit ) const override
    {
        QueryCatalogPartPage page;
        if( resourceId != 100 ) return page;
        page.total = 2;
        for( uint64_t index = offset; index < std::min<uint64_t>( 2, offset + limit ); index++ )
        {
            QueryCatalogPartData value {};
            value.record.partId = 900 + index;
            value.record.resourceId = 100;
            value.record.partKind = uint8_t( index == 0 ?
                JnGpuCatalogPartKind::MeshVertexStream : JnGpuCatalogPartKind::MeshIndex );
            value.record.lengthBytes = 2048 * ( index + 1 );
            value.generation = 12;
            page.values.emplace_back( value );
        }
        return page;
    }
    QueryCatalogVgPage GetQueryCatalogResourceVgPages(
        uint64_t resourceId, uint64_t offset, size_t limit ) const override
    {
        QueryCatalogVgPage page;
        if( resourceId != 100 ) return page;
        page.total = 2;
        for( uint64_t index = offset; index < std::min<uint64_t>( 2, offset + limit ); index++ )
        {
            QueryCatalogVgData value {};
            value.record.resourceId = 100;
            value.record.runtimeResourceId = 1000 + index;
            value.record.pageIndex = uint32_t( index + 1 );
            value.generation = 13;
            page.values.emplace_back( value );
        }
        return page;
    }
    QueryCatalogRelationPage GetQueryCatalogResourceRtasRelations(
        uint64_t resourceId, uint64_t offset, size_t limit ) const override
    {
        QueryCatalogRelationPage page;
        if( resourceId != 100 ) return page;
        page.total = 1;
        if( offset == 0 && limit != 0 )
        {
            QueryCatalogRelationData value {};
            value.record.time = 60;
            value.record.sourceId = 100;
            value.record.targetId = 102;
            value.record.relation = uint8_t( JnGpuCatalogRelationKind::RtasUses );
            value.generation = 14;
            page.values.emplace_back( value );
        }
        return page;
    }
    QueryCatalogEvidencePage GetQueryCatalogPassEvidence(
        uint64_t passId, uint64_t offset, size_t limit ) const override
    {
        QueryCatalogEvidencePage page;
        if( passId != 200 ) return page;
        page.total = 2;
        page.hasState = true;
        page.lastState = uint8_t( JnGpuEvidenceState::PeriodicExact );
        for( uint64_t index = offset; index < std::min<uint64_t>( 2, offset + limit ); index++ )
        {
            QueryCatalogEvidenceData value {};
            value.record.time = int64_t( 70 + index );
            value.record.requestId = index + 1;
            value.record.evidenceFrameId = 200;
            value.record.state = uint8_t( index == 0 ?
                JnGpuEvidenceState::ContinuousExact : JnGpuEvidenceState::PeriodicExact );
            value.record.kind = uint8_t( JnGpuDetailedEvidenceKind::VgPage );
            value.generation = 15;
            page.values.emplace_back( value );
        }
        return page;
    }
    QueryCatalogGpuMemorySummary GetQueryCatalogGpuMemorySummary() const override
    {
        QueryCatalogGpuMemorySummary value;
        value.livePhysicalBytes = 12288;
        value.peakPhysicalBytes = 12288;
        value.peakTime = 77;
        value.allocationCreateCount = 4;
        value.allocationDestroyCount = 2;
        value.allocatedPhysicalBytes = 16384;
        value.freedPhysicalBytes = 4096;
        return value;
    }
    uint64_t ResolveQueryCatalogResourcePointer( uint64_t, int64_t ) const override { return 0; }
    QueryCatalogPointerIntervalPage GetQueryCatalogResourcePointerIntervals(
        uint64_t, uint64_t, size_t ) const override { return {}; }
    QueryCatalogRangePage GetQueryCatalogResourceRanges(
        uint64_t resourceId, uint64_t offset, size_t limit ) const override
    {
        QueryCatalogRangePage page;
        if( resourceId != 100 ) return page;
        page.total = 2;
        for( uint64_t index = offset; index < std::min<uint64_t>( 2, offset + limit ); index++ )
        {
            QueryCatalogRangeData value {};
            value.record.passInstanceId = 200 + index;
            value.record.resourceId = 100;
            value.record.lengthBytes = 1024 * ( index + 1 );
            value.record.usageMask = uint32_t( index + 1 );
            value.generation = 16;
            page.values.emplace_back( value );
        }
        return page;
    }
    QueryCatalogRangePage GetQueryCatalogPassRanges(
        uint64_t, uint64_t, size_t ) const override { return {}; }
    QueryGpuReferenceUsePage GetQueryCatalogResourceUses(
        uint64_t resourceId, uint64_t offset, size_t limit ) const override
    {
        QueryGpuReferenceUsePage page;
        if( resourceId != 100 ) return page;
        page.total = 2;
        for( uint64_t index = offset; index < std::min<uint64_t>( 2, offset + limit ); index++ )
        {
            QueryGpuReferenceResolvedUseRecord value {};
            value.resourceId = 900 + index;
            value.passId = 300 + index;
            value.passStart = int64_t( 70 + index );
            value.passEnd = int64_t( 80 + index );
            value.usageMask = uint32_t( 4 + index );
            value.resourceSetId = uint32_t( 20 + index );
            value.encoding = uint8_t( index == 0 ? 2 : 1 );
            page.values.emplace_back( value );
        }
        return page;
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

    const auto allocationFirst = service.Execute( Request( 7, "gpu.resource.allocations", {
        { "trace_id", traceId }, { "ref", "fake:gpu-resource:100" }, { "limit", 1 }
    } ) );
    assert( allocationFirst.at( "ok" ) && allocationFirst.at( "data" ).at( "allocations" ).size() == 1 &&
        allocationFirst.at( "data" ).at( "allocations" )[0].at( "allocation_id" ) == "500" &&
        allocationFirst.at( "data" ).at( "allocations" )[0].at( "size_bytes" ) == "4096" &&
        allocationFirst.at( "page" ).at( "next_cursor" ).is_string() );
    const auto allocationSecond = service.Execute( Request( 8, "gpu.resource.allocations", {
        { "trace_id", traceId },
        { "ref", "fake:gpu-resource:100" },
        { "limit", 1 }, { "cursor", allocationFirst.at( "page" ).at( "next_cursor" ) }
    } ) );
    assert( allocationSecond.at( "ok" ) && allocationSecond.at( "data" ).at( "allocations" ).size() == 1 &&
        allocationSecond.at( "data" ).at( "allocations" )[0].at( "size_bytes" ) == "8192" &&
        allocationSecond.at( "page" ).at( "next_cursor" ).is_null() );

    const auto views = service.Execute( Request( 9, "gpu.resource.views", {
        { "trace_id", traceId }, { "ref", "fake:gpu-resource:100" }, { "limit", 1 }
    } ) );
    assert( views.at( "ok" ) && views.at( "data" ).at( "views" ).size() == 1 &&
        views.at( "data" ).at( "views" )[0].at( "view_id" ) == "800" &&
        views.at( "data" ).at( "total" ) == "2" && views.at( "page" ).at( "next_cursor" ).is_string() );
    const auto meshParts = service.Execute( Request( 10, "gpu.resource.mesh_buffers", {
        { "trace_id", traceId }, { "ref", "fake:gpu-resource:100" }, { "limit", 1 }
    } ) );
    assert( meshParts.at( "ok" ) && meshParts.at( "data" ).at( "parts" ).size() == 1 &&
        meshParts.at( "data" ).at( "parts" )[0].at( "part_id" ) == "900" &&
        meshParts.at( "data" ).at( "total" ) == "2" && meshParts.at( "page" ).at( "next_cursor" ).is_string() );
    const auto vgPages = service.Execute( Request( 11, "gpu.resource.vg_pages", {
        { "trace_id", traceId }, { "ref", "fake:gpu-resource:100" }, { "limit", 1 }
    } ) );
    assert( vgPages.at( "ok" ) && vgPages.at( "data" ).at( "pages" ).size() == 1 &&
        vgPages.at( "data" ).at( "pages" )[0].at( "page_index" ) == 1 &&
        vgPages.at( "data" ).at( "page" ).at( "total" ) == "2" );
    const auto rtas = service.Execute( Request( 12, "gpu.resource.raytracing_chain", {
        { "trace_id", traceId }, { "ref", "fake:gpu-resource:100" }, { "limit", 1 }
    } ) );
    assert( rtas.at( "ok" ) && rtas.at( "data" ).at( "relations" ).size() == 1 &&
        rtas.at( "data" ).at( "relations" )[0].at( "source_id" ) == "100" &&
        rtas.at( "data" ).at( "relations" )[0].at( "target_id" ) == "102" &&
        rtas.at( "data" ).at( "page" ).at( "total" ) == "1" );
    const auto evidencePage = service.Execute( Request( 13, "gpu.pass.vg_evidence", {
        { "trace_id", traceId }, { "pass_id", "200" }, { "limit", 1 }
    } ) );
    assert( evidencePage.at( "ok" ) && evidencePage.at( "data" ).at( "records" ).size() == 1 &&
        evidencePage.at( "data" ).at( "records" )[0].at( "request_id" ) == "1" &&
        evidencePage.at( "data" ).at( "sample_status" ) == "periodic_exact" &&
        evidencePage.at( "data" ).at( "page" ).at( "total" ) == "2" );

    const auto referencesFirst = service.Execute( Request( 14, "gpu.resource.references", {
        { "trace_id", traceId }, { "ref", "fake:gpu-resource:100" }, { "limit", 1 }
    } ) );
    assert( referencesFirst.at( "ok" ) && referencesFirst.at( "data" ).at( "ranges" ).size() == 1 &&
        referencesFirst.at( "data" ).at( "ranges" )[0].at( "pass_id" ) == "200" &&
        referencesFirst.at( "data" ).at( "ranges" )[0].at( "generation" ) == "16" &&
        referencesFirst.at( "data" ).at( "resource_set_uses" ).size() == 1 &&
        referencesFirst.at( "data" ).at( "resource_set_uses" )[0].at( "logical_resource_id" ) == "900" &&
        referencesFirst.at( "page" ).at( "next_cursor" ).is_string() );
    const auto referencesSecond = service.Execute( Request( 15, "gpu.resource.references", {
        { "trace_id", traceId }, { "ref", "fake:gpu-resource:100" }, { "limit", 1 },
        { "cursor", referencesFirst.at( "page" ).at( "next_cursor" ) }
    } ) );
    assert( referencesSecond.at( "ok" ) && referencesSecond.at( "data" ).at( "ranges" ).size() == 1 &&
        referencesSecond.at( "data" ).at( "ranges" )[0].at( "pass_id" ) == "201" &&
        referencesSecond.at( "data" ).at( "resource_set_uses" )[0].at( "logical_resource_id" ) == "901" &&
        referencesSecond.at( "page" ).at( "next_cursor" ).is_null() );

    const auto byType = service.Execute( Request( 16, "gpu.memory.by_type", {
        { "trace_id", traceId }
    } ) );
    assert( byType.at( "ok" ) && byType.at( "data" ).at( "groups" ).size() == 2 &&
        byType.at( "data" ).at( "groups" )[0].at( "primary_kind" ) == "render_target" &&
        byType.at( "data" ).at( "groups" )[0].at( "live_resource_count" ) == "1" &&
        byType.at( "data" ).at( "groups" )[0].at( "resource_capacity_bytes" ) == "3072" &&
        byType.at( "data" ).at( "groups" )[1].at( "primary_kind" ) == "texture" &&
        byType.at( "data" ).at( "groups" )[1].at( "live_resource_count" ) == "2" &&
        byType.at( "data" ).at( "groups" )[1].at( "resource_capacity_bytes" ) == "3072" );

    const auto peak = service.Execute( Request( 17, "gpu.memory.peak", {
        { "trace_id", traceId }
    } ) );
    assert( peak.at( "ok" ) &&
        peak.at( "data" ).at( "engine_known_physical_peak_bytes" ) == "12288" &&
        peak.at( "data" ).at( "peak_time_ns" ) == "77" &&
        peak.at( "data" ).at( "peak_provenance" ) == "event_exact" );
    const auto churn = service.Execute( Request( 18, "gpu.memory.churn", {
        { "trace_id", traceId }
    } ) );
    assert( churn.at( "ok" ) && churn.at( "data" ).at( "create_count" ) == "4" &&
        churn.at( "data" ).at( "destroy_count" ) == "2" &&
        churn.at( "data" ).at( "allocated_bytes" ) == "16384" &&
        churn.at( "data" ).at( "freed_bytes" ) == "4096" &&
        churn.at( "data" ).at( "live_bytes_at_end" ) == "12288" );

    assert( service.Execute( Request( 19, "trace.close", { { "trace_id", traceId } } ) ).at( "ok" ) );
    fs::remove_all( root, ignored );
    return 0;
}
