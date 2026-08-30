#ifndef __TRACYSESSIONMANAGER_HPP__
#define __TRACYSESSIONMANAGER_HPP__

#include "TracyTraceSource.hpp"
#include "TracyTraceSessionStore.hpp"

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
    std::string loadStage;
    uint64_t loadCompleted = 0;
    uint64_t loadTotal = 0;
    uint64_t loadSubCompleted = 0;
    uint64_t loadSubTotal = 0;
};

struct TraceSessionBuildSnapshot
{
    std::filesystem::path path;
    analysis::TraceSessionState state = analysis::TraceSessionState::InvalidSource;
    std::string sessionId;
    std::string generation;
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    uint64_t sourceRevision = 0;
    uint64_t shardCount = 0;
    uint64_t committedShardCount = 0;
    uint64_t currentShardId = 0;
    uint64_t lastCheckpointShardId = 0;
    uint64_t sourceRecordsProcessed = 0;
    uint64_t canonicalBytes = 0;
    double stageProgress = 0;
    bool mandatoryDerivedComplete = false;
    bool auditComplete = false;
    bool published = false;
    std::string reason;
};

class SessionManager
{
public:
    using StateCallback = std::function<void( analysis::TraceSourceState )>;
    using SourceLoader = std::function<std::unique_ptr<analysis::TraceSource>( const std::filesystem::path&, StateCallback )>;

    explicit SessionManager( std::vector<std::filesystem::path> allowRoots = {}, size_t maxSessions = 2, SourceLoader sourceLoader = {}, bool preferIndex = false );
    ~SessionManager();

    SessionManager( const SessionManager& ) = delete;
    SessionManager& operator=( const SessionManager& ) = delete;

    TraceSessionSnapshot Open( const std::filesystem::path& path );
    TraceSessionSnapshot Status( const std::string& id ) const;
    std::vector<TraceSessionSnapshot> List() const;
    TraceSessionSnapshot Close( const std::string& id );
    TraceSessionSnapshot WaitReady( const std::string& id, std::chrono::milliseconds timeout );

    std::shared_ptr<analysis::TraceSource> GetReadySource( const std::string& id ) const;
    std::filesystem::path ResolveTracePath( const std::filesystem::path& path ) const;
    TraceSessionBuildSnapshot InspectBuild( const std::filesystem::path& path ) const;
    const std::vector<std::filesystem::path>& AllowRoots() const { return m_allowRoots; }

private:
    struct Session;

    TraceSessionSnapshot SnapshotLocked( const Session& session ) const;
    std::shared_ptr<Session> FindLocked( const std::string& id ) const;
    void RefreshSegmentSession( const std::shared_ptr<Session>& session ) const;
    void LoaderLoop( std::stop_token stopToken );
    void UpdateState( const std::shared_ptr<Session>& session, analysis::TraceSourceState state );
    std::filesystem::path ResolveBuildStatusPath( const std::filesystem::path& path ) const;

    std::vector<std::filesystem::path> m_allowRoots;
    size_t m_maxSessions;
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
    std::deque<std::shared_ptr<Session>> m_queue;
    uint64_t m_nextSession = 1;
    SourceLoader m_sourceLoader;
    std::jthread m_loader;
};

const char* ToString( SessionErrorCode code );

}

#endif
