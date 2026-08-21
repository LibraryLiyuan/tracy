#ifndef __TRACYQUERYSERVICE_HPP__
#define __TRACYQUERYSERVICE_HPP__

#include "TracySessionManager.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace tracy::query
{

inline constexpr const char* QueryProtocol = "tracy-query/1";
inline constexpr const char* QuerySchemaVersion = "1.30.0";
inline constexpr size_t DefaultPageSize = 100;
inline constexpr size_t MaximumPageSize = 1000;
inline constexpr size_t DefaultTopN = 20;
inline constexpr size_t MaximumTopN = 500;
inline constexpr size_t MaximumRequestBytes = 1024 * 1024;
inline constexpr size_t MaximumResponseBytes = 8 * 1024 * 1024;
inline constexpr size_t DefaultAnalysisCacheBytes = size_t( 512 ) * 1024 * 1024;
inline constexpr uint64_t DefaultMaxScanEvents = 5000000;
inline constexpr uint64_t MaximumMaxScanEvents = 100000000;
inline constexpr uint64_t DefaultMaxCpuMs = 5000;
inline constexpr uint64_t MaximumMaxCpuMs = 60000;
inline constexpr uint64_t DefaultMaxNodes = 10000;
inline constexpr uint64_t MaximumMaxNodes = 100000;
inline constexpr uint64_t DefaultMaxEdges = 20000;
inline constexpr uint64_t MaximumMaxEdges = 200000;
inline constexpr uint64_t DefaultMaxGroups = 500;
inline constexpr uint64_t MaximumMaxGroups = 10000;

class QueryError : public std::runtime_error
{
public:
    QueryError( std::string code, std::string message, bool retryable = false, nlohmann::json details = nlohmann::json::object() )
        : std::runtime_error( std::move( message ) )
        , code( std::move( code ) )
        , retryable( retryable )
        , details( std::move( details ) )
    {}

    std::string code;
    bool retryable;
    nlohmann::json details;
};

class QueryService
{
public:
    explicit QueryService( SessionManager& sessions, size_t analysisCacheBytes = DefaultAnalysisCacheBytes );

    nlohmann::json Execute( const nlohmann::json& request, const std::optional<std::string>& defaultTraceId = std::nullopt, std::stop_token stopToken = {} );
    nlohmann::json Failure( const nlohmann::json& id, const QueryError& error ) const;
    nlohmann::json Failure( const nlohmann::json& id, std::string code, std::string message, bool retryable = false, nlohmann::json details = nlohmann::json::object() ) const;

private:
    struct GpuCacheEntry
    {
        std::shared_ptr<const analysis::GpuMemoryAttribution> value;
        size_t bytes = 0;
        uint64_t access = 0;
    };
    struct MemoryCacheEntry
    {
        std::shared_ptr<const analysis::MemoryFrameSnapshot> value;
        size_t bytes = 0;
        uint64_t access = 0;
    };
    struct ResourceEntityKey
    {
        uint64_t id = 0;
        uint32_t epoch = 0;
        uint8_t kind = 0;

        bool operator==( const ResourceEntityKey& other ) const noexcept
        { return id == other.id && epoch == other.epoch && kind == other.kind; }
    };
    struct ResourceEntityKeyHash
    {
        size_t operator()( const ResourceEntityKey& value ) const noexcept
        {
            uint64_t hash = value.id ^ ( uint64_t( value.epoch ) << 17 ) ^ ( uint64_t( value.kind ) << 57 );
            hash ^= hash >> 33; hash *= 0xff51afd7ed558ccdULL;
            hash ^= hash >> 33; hash *= 0xc4ceb9fe1a85ec53ULL;
            return size_t( hash ^ ( hash >> 33 ) );
        }
    };
    struct ResourceRelationKey
    {
        ResourceEntityKey source;
        ResourceEntityKey target;
        uint8_t relation = 0;

        bool operator==( const ResourceRelationKey& other ) const noexcept
        { return source == other.source && target == other.target && relation == other.relation; }
    };
    struct ResourceRelationKeyHash
    {
        size_t operator()( const ResourceRelationKey& value ) const noexcept
        {
            const auto left = ResourceEntityKeyHash {}( value.source );
            const auto right = ResourceEntityKeyHash {}( value.target );
            return left ^ ( right + size_t( 0x9e3779b9 ) + ( left << 6 ) + ( left >> 2 ) ) ^ value.relation;
        }
    };
    struct ResourceRangeKey
    {
        uint64_t partId = 0;
        ResourceEntityKey target;

        bool operator==( const ResourceRangeKey& other ) const noexcept
        { return partId == other.partId && target == other.target; }
    };
    struct ResourceRangeKeyHash
    {
        size_t operator()( const ResourceRangeKey& value ) const noexcept
        { return ResourceEntityKeyHash {}( value.target ) ^ size_t( value.partId ^ ( value.partId >> 32 ) ); }
    };
    struct ResourceGraphIndex
    {
        std::vector<analysis::ResourceAssetDto> assets;
        std::vector<analysis::ResourceAssetUpdateDto> assetUpdates;
        std::vector<analysis::UnityObjectEventDto> objectEvents;
        std::vector<analysis::NativeRootDto> roots;
        std::vector<analysis::GfxResourceBindingDto> gfx;
        std::vector<analysis::ResourcePartDto> parts;
        std::vector<analysis::ResourceRangeDto> ranges;
        std::vector<analysis::ResourceContextDto> contexts;
        std::vector<analysis::ResourceMetadataDto> metadata;
        std::vector<analysis::ResourceRelationDto> relations;
        std::vector<analysis::ResourceBootstrapDto> bootstrap;
        std::vector<analysis::ResourceQualityDto> quality;

        std::unordered_map<uint64_t, const analysis::ResourceAssetDto*> assetByEntity;
        std::unordered_map<uint64_t, std::vector<const analysis::ResourceAssetUpdateDto*>> updatesByAsset;
        std::unordered_map<uint64_t, std::vector<const analysis::UnityObjectEventDto*>> objectEventsById;
        std::unordered_map<uint64_t, std::vector<const analysis::NativeRootDto*>> rootsByObject;
        std::unordered_map<uint64_t, std::vector<const analysis::GfxResourceBindingDto*>> gfxById;
        std::unordered_map<ResourceEntityKey, std::vector<const analysis::GfxResourceBindingDto*>, ResourceEntityKeyHash> gfxByIdentity;
        std::unordered_map<uint64_t, std::vector<const analysis::GfxResourceBindingDto*>> gfxByTarget;
        std::unordered_map<uint64_t, std::vector<const analysis::ResourcePartDto*>> partsByOwner;
        std::unordered_map<uint64_t, std::vector<const analysis::ResourceRangeDto*>> rangesByPart;
        std::array<std::unordered_map<uint64_t, std::vector<const analysis::ResourceMetadataDto*>>, 14> metadataByEntity;
        std::unordered_map<ResourceEntityKey, std::vector<const analysis::ResourceRelationDto*>, ResourceEntityKeyHash> relationsBySource;
        std::unordered_map<ResourceEntityKey, std::vector<const analysis::ResourceRelationDto*>, ResourceEntityKeyHash> relationsByTarget;
        std::unordered_map<ResourceRelationKey, std::vector<const analysis::ResourceRelationDto*>, ResourceRelationKeyHash> relationHistory;
        std::unordered_map<ResourceRangeKey, std::vector<const analysis::ResourceRangeDto*>, ResourceRangeKeyHash> rangeHistory;
    };
    struct ResourceGraphCacheEntry
    {
        std::shared_ptr<const ResourceGraphIndex> value;
        size_t bytes = 0;
        uint64_t access = 0;
    };

    nlohmann::json Dispatch( const nlohmann::json& id, const std::string& method, const nlohmann::json& params, const std::optional<std::string>& defaultTraceId, std::stop_token stopToken );
    std::shared_ptr<const analysis::GpuMemoryAttribution> CachedGpuAttribution( const std::string& traceId,
        const std::shared_ptr<analysis::TraceSource>& source, bool summaryOnly = false );
    std::shared_ptr<const analysis::MemoryFrameSnapshot> CachedMemorySnapshot( const std::string& traceId, const std::shared_ptr<analysis::TraceSource>& source, size_t frameSet, size_t frame, std::vector<std::string> poolRefs, bool allGpu );
    std::shared_ptr<const ResourceGraphIndex> CachedResourceGraphIndex( const std::string& traceId,
        const std::shared_ptr<analysis::TraceSource>& source );
    void EvictCache( size_t incomingBytes );
    void EraseTraceCache( const std::string& traceId );

    SessionManager& m_sessions;
    std::mutex m_queryMutex;
    size_t m_cacheBudget = DefaultAnalysisCacheBytes;
    size_t m_cacheBytes = 0;
    uint64_t m_cacheClock = 0;
    std::unordered_map<std::string, GpuCacheEntry> m_gpuCache;
    std::unordered_map<std::string, MemoryCacheEntry> m_memoryCache;
    std::unordered_map<std::string, ResourceGraphCacheEntry> m_resourceGraphCache;
};

const std::vector<std::string>& QueryMethodRegistry();
const nlohmann::json& QueryOperationSchemaRegistry();
const nlohmann::json& QueryEnvelopeOutputSchema();
bool IsPublicQueryMethod( std::string_view method );
std::string DumpProtocolJson( const nlohmann::json& value );

}

#endif
