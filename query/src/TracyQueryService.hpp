#ifndef __TRACYQUERYSERVICE_HPP__
#define __TRACYQUERYSERVICE_HPP__

#include "TracySessionManager.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace tracy::query
{

inline constexpr const char* QueryProtocol = "tracy-query/1";
inline constexpr const char* QuerySchemaVersion = "1.0.0";
inline constexpr size_t DefaultPageSize = 100;
inline constexpr size_t MaximumPageSize = 1000;
inline constexpr size_t DefaultTopN = 20;
inline constexpr size_t MaximumTopN = 500;
inline constexpr size_t MaximumRequestBytes = 1024 * 1024;
inline constexpr size_t MaximumResponseBytes = 8 * 1024 * 1024;

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
    explicit QueryService( SessionManager& sessions );

    nlohmann::json Execute( const nlohmann::json& request, const std::optional<std::string>& defaultTraceId = std::nullopt );
    nlohmann::json Failure( const nlohmann::json& id, const QueryError& error ) const;
    nlohmann::json Failure( const nlohmann::json& id, std::string code, std::string message, bool retryable = false, nlohmann::json details = nlohmann::json::object() ) const;

private:
    nlohmann::json Dispatch( const nlohmann::json& id, const std::string& method, const nlohmann::json& params, const std::optional<std::string>& defaultTraceId );

    SessionManager& m_sessions;
    std::mutex m_queryMutex;
};

std::string DumpProtocolJson( const nlohmann::json& value );

}

#endif
