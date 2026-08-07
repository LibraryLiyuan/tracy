#ifndef __TRACYQUERYSERVICE_HPP__
#define __TRACYQUERYSERVICE_HPP__

#include "TracySessionManager.hpp"

#include <nlohmann/json.hpp>

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
inline constexpr const char* QuerySchemaVersion = "1.19.0";
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

    nlohmann::json Dispatch( const nlohmann::json& id, const std::string& method, const nlohmann::json& params, const std::optional<std::string>& defaultTraceId, std::stop_token stopToken );
    std::shared_ptr<const analysis::GpuMemoryAttribution> CachedGpuAttribution( const std::string& traceId, const std::shared_ptr<analysis::TraceSource>& source );
    std::shared_ptr<const analysis::MemoryFrameSnapshot> CachedMemorySnapshot( const std::string& traceId, const std::shared_ptr<analysis::TraceSource>& source, size_t frameSet, size_t frame, std::vector<std::string> poolRefs, bool allGpu );
    void EvictCache( size_t incomingBytes );
    void EraseTraceCache( const std::string& traceId );

    SessionManager& m_sessions;
    std::mutex m_queryMutex;
    size_t m_cacheBudget = DefaultAnalysisCacheBytes;
    size_t m_cacheBytes = 0;
    uint64_t m_cacheClock = 0;
    std::unordered_map<std::string, GpuCacheEntry> m_gpuCache;
    std::unordered_map<std::string, MemoryCacheEntry> m_memoryCache;
};

const std::vector<std::string>& QueryMethodRegistry();
const nlohmann::json& QueryOperationSchemaRegistry();
const nlohmann::json& QueryEnvelopeOutputSchema();
bool IsPublicQueryMethod( std::string_view method );
std::string DumpProtocolJson( const nlohmann::json& value );

}

#endif
