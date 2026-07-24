#include "TracySessionManager.hpp"

#include "TracySegmentTraceSource.hpp"
#include "TracyWorkerTraceSource.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace tracy::query
{
namespace
{

std::string FoldPathPart( const std::filesystem::path& value )
{
    auto result = value.generic_string();
#ifdef _WIN32
    std::transform( result.begin(), result.end(), result.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
#endif
    return result;
}

bool IsWithin( const std::filesystem::path& path, const std::filesystem::path& root )
{
    auto pathIt = path.begin();
    auto rootIt = root.begin();
    for( ; rootIt != root.end(); ++rootIt, ++pathIt )
    {
        if( pathIt == path.end() || FoldPathPart( *pathIt ) != FoldPathPart( *rootIt ) ) return false;
    }
    return true;
}

std::string Lower( std::string value )
{
    std::transform( value.begin(), value.end(), value.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
    return value;
}

}

struct SessionManager::Session
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
    std::shared_ptr<analysis::TraceSource> source;
    bool refreshInProgress = false;
    std::chrono::steady_clock::time_point lastRefresh;
};

const char* ToString( SessionErrorCode code )
{
    switch( code )
    {
    case SessionErrorCode::PathNotAllowed: return "PATH_NOT_ALLOWED";
    case SessionErrorCode::TraceNotFound: return "TRACE_NOT_FOUND";
    case SessionErrorCode::TraceOpenFailed: return "TRACE_OPEN_FAILED";
    case SessionErrorCode::UnsupportedTraceVersion: return "UNSUPPORTED_TRACE_VERSION";
    case SessionErrorCode::CorruptTrace: return "CORRUPT_TRACE";
    case SessionErrorCode::TraceLoading: return "TRACE_LOADING";
    case SessionErrorCode::TraceNotReady: return "TRACE_NOT_READY";
    case SessionErrorCode::TraceClosed: return "TRACE_CLOSED";
    case SessionErrorCode::ResourceLimit: return "RESOURCE_LIMIT";
    case SessionErrorCode::InternalError: return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}

SessionManager::SessionManager( std::vector<std::filesystem::path> allowRoots, size_t maxSessions, SourceLoader sourceLoader )
    : m_maxSessions( maxSessions )
    , m_sourceLoader( std::move( sourceLoader ) )
{
    if( allowRoots.empty() ) allowRoots.emplace_back( std::filesystem::current_path() );
    for( const auto& root : allowRoots )
    {
        std::error_code error;
        const auto canonical = std::filesystem::canonical( root, error );
        if( error || !std::filesystem::is_directory( canonical ) )
        {
            throw SessionError( SessionErrorCode::PathNotAllowed, "allow root is not an existing directory: " + root.string() );
        }
        m_allowRoots.emplace_back( canonical );
    }
    std::sort( m_allowRoots.begin(), m_allowRoots.end() );
    m_allowRoots.erase( std::unique( m_allowRoots.begin(), m_allowRoots.end() ), m_allowRoots.end() );
    if( !m_sourceLoader )
    {
        m_sourceLoader = []( const std::filesystem::path& path, StateCallback callback ) -> std::unique_ptr<analysis::TraceSource> {
            if( Lower( path.extension().string() ) == ".tracy-stream" )
            {
                return SegmentTraceSource::Open( path, std::move( callback ) );
            }
            return analysis::WorkerTraceSource::Open( path, std::move( callback ) );
        };
    }
    m_loader = std::jthread( [this]( std::stop_token token ) { LoaderLoop( token ); } );
}

SessionManager::~SessionManager()
{
    m_loader.request_stop();
    m_changed.notify_all();
}

std::filesystem::path SessionManager::ResolveTracePath( const std::filesystem::path& requested ) const
{
    std::error_code error;
    auto absolute = requested.is_absolute() ? requested : std::filesystem::current_path() / requested;
    if( !std::filesystem::exists( absolute, error ) || error ) throw SessionError( SessionErrorCode::TraceNotFound, "trace file does not exist" );
    const auto canonical = std::filesystem::canonical( absolute, error );
    if( error ) throw SessionError( SessionErrorCode::TraceOpenFailed, "unable to resolve final trace path" );
    if( !std::filesystem::is_regular_file( canonical, error ) || error ) throw SessionError( SessionErrorCode::TraceOpenFailed, "trace path is not a regular file" );
    const auto extension = Lower( canonical.extension().string() );
    if( extension != ".tracy" && extension != ".tracy-stream" )
    {
        throw SessionError( SessionErrorCode::TraceOpenFailed, "only .tracy and .tracy-stream files may be opened" );
    }

    const bool allowed = std::any_of( m_allowRoots.begin(), m_allowRoots.end(), [&]( const auto& root ) { return IsWithin( canonical, root ); } );
    if( !allowed ) throw SessionError( SessionErrorCode::PathNotAllowed, "trace path is outside every --allow-root" );
    return canonical;
}

TraceSessionSnapshot SessionManager::Open( const std::filesystem::path& requested )
{
    const auto path = ResolveTracePath( requested );
    std::lock_guard lock( m_mutex );
    for( const auto& [id, session] : m_sessions )
    {
        if( session->path == path && session->state != analysis::TraceSourceState::Closed && session->state != analysis::TraceSourceState::Failed )
        {
            return SnapshotLocked( *session );
        }
    }

    const size_t active = std::count_if( m_sessions.begin(), m_sessions.end(), []( const auto& pair ) {
        return pair.second->state != analysis::TraceSourceState::Closed && pair.second->state != analysis::TraceSourceState::Failed;
    } );
    if( active >= m_maxSessions ) throw SessionError( SessionErrorCode::ResourceLimit, "at most two trace sessions may be active" );

    auto session = std::make_shared<Session>();
    session->id = "trace-" + std::to_string( m_nextSession++ );
    session->path = path;
    m_sessions.emplace( session->id, session );
    m_queue.emplace_back( session );
    m_changed.notify_all();
    return SnapshotLocked( *session );
}

TraceSessionSnapshot SessionManager::SnapshotLocked( const Session& session ) const
{
    TraceSessionSnapshot result;
    result.id = session.id;
    result.path = session.path;
    result.fingerprint = session.fingerprint;
    result.sourceKind = session.sourceKind;
    result.state = session.state;
    result.revision = session.revision;
    result.watermarkNs = session.watermarkNs;
    result.complete = session.complete;
    result.closePending = session.closePending;
    result.errorCode = session.errorCode;
    result.errorMessage = session.errorMessage;
    if( session.state == analysis::TraceSourceState::Loading )
    {
        const auto progress = analysis::WorkerTraceSource::GetLoadProgress();
        result.loadStage = progress.stage;
        result.loadCompleted = progress.completed;
        result.loadTotal = progress.total;
        result.loadSubCompleted = progress.subCompleted;
        result.loadSubTotal = progress.subTotal;
    }
    else if( session.state == analysis::TraceSourceState::Indexing )
    {
        result.loadStage = "analysis_indexes";
        result.loadCompleted = result.loadTotal = 1;
    }
    return result;
}

std::shared_ptr<SessionManager::Session> SessionManager::FindLocked( const std::string& id ) const
{
    const auto it = m_sessions.find( id );
    if( it == m_sessions.end() ) throw SessionError( SessionErrorCode::TraceNotFound, "trace session was not found" );
    return it->second;
}

TraceSessionSnapshot SessionManager::Status( const std::string& id ) const
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock( m_mutex );
        session = FindLocked( id );
    }
    RefreshSegmentSession( session );
    std::lock_guard lock( m_mutex );
    return SnapshotLocked( *session );
}

std::vector<TraceSessionSnapshot> SessionManager::List() const
{
    std::lock_guard lock( m_mutex );
    std::vector<TraceSessionSnapshot> result;
    result.reserve( m_sessions.size() );
    for( const auto& [id, session] : m_sessions ) result.emplace_back( SnapshotLocked( *session ) );
    std::sort( result.begin(), result.end(), []( const auto& lhs, const auto& rhs ) { return lhs.id < rhs.id; } );
    return result;
}

TraceSessionSnapshot SessionManager::Close( const std::string& id )
{
    std::lock_guard lock( m_mutex );
    const auto session = FindLocked( id );
    if( session->state == analysis::TraceSourceState::Closed ) return SnapshotLocked( *session );
    session->closePending = true;
    session->state = analysis::TraceSourceState::Closing;
    if( session->source )
    {
        session->source.reset();
        session->state = analysis::TraceSourceState::Closed;
        session->complete = false;
        session->closePending = false;
    }
    m_changed.notify_all();
    return SnapshotLocked( *session );
}

TraceSessionSnapshot SessionManager::WaitReady( const std::string& id, std::chrono::milliseconds timeout )
{
    std::unique_lock lock( m_mutex );
    const auto session = FindLocked( id );
    m_changed.wait_for( lock, timeout, [&] {
        return session->state == analysis::TraceSourceState::Ready || session->state == analysis::TraceSourceState::Failed || session->state == analysis::TraceSourceState::Closed;
    } );
    return SnapshotLocked( *session );
}

std::shared_ptr<analysis::TraceSource> SessionManager::GetReadySource( const std::string& id ) const
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock( m_mutex );
        session = FindLocked( id );
    }
    RefreshSegmentSession( session );
    std::lock_guard lock( m_mutex );
    if( session->state == analysis::TraceSourceState::Queued || session->state == analysis::TraceSourceState::Loading || session->state == analysis::TraceSourceState::Indexing )
    {
        throw SessionError( SessionErrorCode::TraceLoading, "trace is still loading or indexing", true );
    }
    if( session->state == analysis::TraceSourceState::Closed || session->state == analysis::TraceSourceState::Closing )
    {
        throw SessionError( SessionErrorCode::TraceClosed, "trace session is closed" );
    }
    if( session->state != analysis::TraceSourceState::Ready || !session->source )
    {
        throw SessionError( SessionErrorCode::TraceNotReady, session->errorMessage.empty() ? "trace is not ready" : session->errorMessage );
    }
    return session->source;
}

void SessionManager::RefreshSegmentSession( const std::shared_ptr<Session>& session ) const
{
    std::shared_ptr<SegmentTraceSource> current;
    uint64_t currentRevision = 0;
    {
        std::lock_guard lock( m_mutex );
        if( session->state != analysis::TraceSourceState::Ready ||
            session->sourceKind != analysis::TraceSourceKind::Segment ||
            session->refreshInProgress )
        {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if( session->lastRefresh.time_since_epoch().count() != 0 &&
            now - session->lastRefresh < std::chrono::milliseconds( 100 ) )
        {
            return;
        }
        current = std::dynamic_pointer_cast<SegmentTraceSource>( session->source );
        if( !current ) return;
        currentRevision = session->revision;
        session->refreshInProgress = true;
        session->lastRefresh = now;
    }

    try
    {
        const auto view = current->RefreshView();
        if( view && view->revision > currentRevision )
        {
            auto replacement = SegmentTraceSource::OpenRevision( current->Store(), view );
            auto shared = std::shared_ptr<analysis::TraceSource>( std::move( replacement ) );
            const auto sourceView = shared->AcquireReadView();
            const auto fingerprint = shared->GetTraceInfo().fingerprint;
            std::lock_guard lock( m_mutex );
            if( session->state == analysis::TraceSourceState::Ready && sourceView.revision > session->revision )
            {
                session->source = std::move( shared );
                session->sourceKind = sourceView.sourceKind;
                session->revision = sourceView.revision;
                session->watermarkNs = sourceView.watermarkNs;
                session->complete = sourceView.complete;
                session->fingerprint = fingerprint;
            }
        }
    }
    catch( const std::exception& )
    {
        // Preserve and continue serving the last immutable good revision. A
        // partial tail or transient replay failure must not invalidate it.
    }

    {
        std::lock_guard lock( m_mutex );
        session->refreshInProgress = false;
    }
    m_changed.notify_all();
}

void SessionManager::UpdateState( const std::shared_ptr<Session>& session, analysis::TraceSourceState state )
{
    std::lock_guard lock( m_mutex );
    if( session->closePending ) session->state = analysis::TraceSourceState::Closing;
    else session->state = state;
    m_changed.notify_all();
}

void SessionManager::LoaderLoop( std::stop_token stopToken )
{
    while( !stopToken.stop_requested() )
    {
        std::shared_ptr<Session> session;
        {
            std::unique_lock lock( m_mutex );
            m_changed.wait( lock, [&] { return stopToken.stop_requested() || !m_queue.empty(); } );
            if( stopToken.stop_requested() ) return;
            session = m_queue.front();
            m_queue.pop_front();
            if( session->closePending )
            {
                session->state = analysis::TraceSourceState::Closed;
                session->closePending = false;
                m_changed.notify_all();
                continue;
            }
            session->state = analysis::TraceSourceState::Loading;
            m_changed.notify_all();
        }

        try
        {
            auto source = m_sourceLoader( session->path, [this, weak = std::weak_ptr<Session>( session )]( auto state ) {
                // The source reports Ready immediately before returning ownership to
                // the session manager. Keep the externally visible session in
                // Indexing until the source, fingerprint, watermark, and completeness
                // fields have all been published under m_mutex below.
                if( const auto locked = weak.lock() )
                {
                    UpdateState( locked, state == analysis::TraceSourceState::Ready ? analysis::TraceSourceState::Indexing : state );
                }
            } );
            std::lock_guard lock( m_mutex );
            if( session->closePending )
            {
                session->state = analysis::TraceSourceState::Closed;
                session->closePending = false;
                session->complete = false;
            }
            else
            {
                session->source = std::shared_ptr<analysis::TraceSource>( std::move( source ) );
                session->fingerprint = session->source->GetTraceInfo().fingerprint;
                const auto view = session->source->AcquireReadView();
                session->sourceKind = view.sourceKind;
                session->revision = view.revision;
                session->watermarkNs = view.watermarkNs;
                session->complete = view.complete;
                session->state = analysis::TraceSourceState::Ready;
            }
        }
        catch( const analysis::TraceLoadError& error )
        {
            std::lock_guard lock( m_mutex );
            session->state = analysis::TraceSourceState::Failed;
            session->complete = false;
            session->errorMessage = error.what();
            switch( error.code )
            {
            case analysis::TraceLoadErrorCode::NotFound: session->errorCode = "TRACE_NOT_FOUND"; break;
            case analysis::TraceLoadErrorCode::OpenFailed: session->errorCode = "TRACE_OPEN_FAILED"; break;
            case analysis::TraceLoadErrorCode::UnsupportedVersion:
            case analysis::TraceLoadErrorCode::LegacyVersion: session->errorCode = "UNSUPPORTED_TRACE_VERSION"; break;
            case analysis::TraceLoadErrorCode::Corrupt: session->errorCode = "CORRUPT_TRACE"; break;
            case analysis::TraceLoadErrorCode::ResourceLimit: session->errorCode = "RESOURCE_LIMIT"; break;
            case analysis::TraceLoadErrorCode::Internal: session->errorCode = "INTERNAL_ERROR"; break;
            }
        }
        catch( const std::exception& error )
        {
            std::lock_guard lock( m_mutex );
            session->state = analysis::TraceSourceState::Failed;
            session->complete = false;
            session->errorCode = "INTERNAL_ERROR";
            session->errorMessage = error.what();
        }
        m_changed.notify_all();
    }
}

}
