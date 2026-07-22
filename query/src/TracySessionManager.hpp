#ifndef __TRACYSESSIONMANAGER_HPP__
#define __TRACYSESSIONMANAGER_HPP__

#include "TracyWorkerTraceSource.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tracy::query
{

enum class SessionErrorCode
{
    PathNotAllowed,
    TraceNotFound,
    TraceOpenFailed,
    UnsupportedTraceVersion,
    CorruptTrace,
    TraceLoading,
    TraceNotReady,
    TraceClosed,
    ResourceLimit,
    InternalError
};

class SessionError : public std::runtime_error
{
public:
    SessionError( SessionErrorCode code, std::string message, bool retryable = false )
        : std::runtime_error( std::move( message ) )
        , code( code )
        , retryable( retryable )
    {}

    SessionErrorCode code;
    bool retryable;
};

struct TraceSessionSnapshot
{
    std::string id;
    std::filesystem::path path;
    std::string fingerprint;
    analysis::TraceSourceKind sourceKind = analysis::TraceSourceKind::Snapshot;
    analysis::TraceSourceState state = analysis::TraceSourceState::Queued;
    uint64_t revision = 0;
    int64_t watermarkNs = 0;
    bool complete = false;
    bool closePending = false;
    std::string errorCode;
    std::string errorMessage;
};

class SessionManager
{
public:
    explicit SessionManager( std::vector<std::filesystem::path> allowRoots = {}, size_t maxSessions = 2 );
    ~SessionManager();

    SessionManager( const SessionManager& ) = delete;
    SessionManager& operator=( const SessionManager& ) = delete;

    TraceSessionSnapshot Open( const std::filesystem::path& path );
    TraceSessionSnapshot Status( const std::string& id ) const;
    std::vector<TraceSessionSnapshot> List() const;
    TraceSessionSnapshot Close( const std::string& id );
    TraceSessionSnapshot WaitReady( const std::string& id, std::chrono::milliseconds timeout );

    std::shared_ptr<analysis::WorkerTraceSource> GetReadySource( const std::string& id ) const;
    std::filesystem::path ResolveTracePath( const std::filesystem::path& path ) const;
    const std::vector<std::filesystem::path>& AllowRoots() const { return m_allowRoots; }

private:
    struct Session;

    TraceSessionSnapshot SnapshotLocked( const Session& session ) const;
    std::shared_ptr<Session> FindLocked( const std::string& id ) const;
    void LoaderLoop( std::stop_token stopToken );
    void UpdateState( const std::shared_ptr<Session>& session, analysis::TraceSourceState state );

    std::vector<std::filesystem::path> m_allowRoots;
    size_t m_maxSessions;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
    std::deque<std::shared_ptr<Session>> m_queue;
    uint64_t m_nextSession = 1;
    std::jthread m_loader;
};

const char* ToString( SessionErrorCode code );

}

#endif
