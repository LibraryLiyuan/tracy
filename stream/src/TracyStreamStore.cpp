#include "TracyStreamStore.hpp"

#include <fstream>
#include <limits>

namespace tracy::stream
{

JournalStore::JournalStore( std::filesystem::path path, ScanOptions options )
    : m_path( std::move( path ) )
    , m_identity( std::make_shared<const uint8_t>( 0 ) )
    , m_options( options )
{
    // Store views retain only aggregate boundaries. Domain-specific readers
    // can scan records inside the fixed validSize without an unbounded index.
    m_options.maxCollectedRecords = 0;
}

std::unique_ptr<JournalStore> JournalStore::Open( const std::filesystem::path& path, const ScanOptions& options, std::string& error )
{
    error.clear();
    if( options.maxPayloadSize == 0 )
    {
        error = "maximum payload size must be non-zero";
        return {};
    }

    std::error_code filesystemError;
    const auto absolute = std::filesystem::absolute( path, filesystemError ).lexically_normal();
    if( filesystemError )
    {
        error = "cannot resolve journal path: " + filesystemError.message();
        return {};
    }

    auto store = std::unique_ptr<JournalStore>( new JournalStore( absolute, options ) );
    const auto result = store->Refresh();
    if( result.code != RefreshCode::Published )
    {
        error = result.message.empty() ? "journal has no readable committed prefix" : result.message;
        return {};
    }
    return store;
}

std::unique_ptr<JournalStore> JournalStore::Open( const std::filesystem::path& path, std::string& error )
{
    return Open( path, ScanOptions {}, error );
}

bool JournalStore::SameHeader( const FileHeader& left, const FileHeader& right )
{
    return left.flags == right.flags
        && left.protocolVersion == right.protocolVersion
        && left.sessionId == right.sessionId
        && left.createdUnixNs == right.createdUnixNs;
}

RefreshResult JournalStore::Refresh()
{
    std::lock_guard lock( m_lock );
    return RefreshLocked();
}

RefreshResult JournalStore::RefreshLocked()
{
    const auto scan = ScanJournal( m_path, m_options );
    m_lastObservation = scan;

    RefreshResult result;
    result.scanCode = scan.code;
    result.previousRevision = m_view ? m_view->revision : 0;
    result.revision = result.previousRevision;
    result.validSize = m_view ? m_view->validSize : 0;
    result.message = scan.message;

    if( scan.code == ScanCode::IoError )
    {
        result.code = RefreshCode::IoError;
        return result;
    }
    if( !scan.HasRecoverablePrefix() )
    {
        result.code = RefreshCode::Rejected;
        return result;
    }

    if( m_view )
    {
        if( !SameHeader( m_view->header, scan.header ) )
        {
            result.code = RefreshCode::Rejected;
            result.message = "journal identity changed; refusing to replace the published session";
            return result;
        }
        if( scan.validSize < m_view->validSize || scan.lastSequence < m_view->revision )
        {
            result.code = RefreshCode::Rejected;
            result.message = "journal committed prefix moved backwards; retaining the previous read view";
            return result;
        }
        if( scan.validSize > m_view->validSize )
        {
            const auto previousPrefix = ScanJournalPrefix( m_path, m_view->validSize, m_options );
            if( !previousPrefix.HasRecoverablePrefix()
                || previousPrefix.validSize != m_view->validSize
                || previousPrefix.lastSequence != m_view->revision
                || previousPrefix.prefixCrc32c != m_view->prefixCrc32c )
            {
                result.code = RefreshCode::Rejected;
                result.message = "previously published committed bytes changed; retaining the previous read view";
                return result;
            }
        }
        if( m_view->complete && ( scan.fileSize != m_view->observedFileSize || scan.code != ScanCode::Ok ) )
        {
            result.code = RefreshCode::Rejected;
            result.message = "terminal journal changed after SessionEnd; retaining the complete read view";
            return result;
        }
        if( scan.validSize == m_view->validSize )
        {
            if( scan.lastSequence != m_view->revision
                || scan.recordCount != m_view->recordCount
                || scan.lastMonotonicNs != m_view->watermarkNs
                || scan.prefixCrc32c != m_view->prefixCrc32c
                || scan.complete != m_view->complete )
            {
                result.code = RefreshCode::Rejected;
                result.message = "journal metadata changed without advancing the committed prefix";
                return result;
            }
            result.code = RefreshCode::Unchanged;
            return result;
        }
        if( m_view->complete )
        {
            result.code = RefreshCode::Rejected;
            result.message = "journal grew after a terminal SessionEnd; retaining the complete read view";
            return result;
        }
    }

    auto view = std::make_shared<JournalReadView>();
    view->path = m_path;
    view->header = scan.header;
    view->revision = scan.lastSequence;
    view->validSize = scan.validSize;
    view->recordCount = scan.recordCount;
    view->watermarkNs = scan.lastMonotonicNs;
    view->prefixCrc32c = scan.prefixCrc32c;
    view->observedFileSize = scan.fileSize;
    view->observedCode = scan.code;
    view->complete = scan.complete;
    view->storeIdentity = m_identity;
    m_view = std::move( view );

    result.code = RefreshCode::Published;
    result.revision = m_view->revision;
    result.validSize = m_view->validSize;
    return result;
}

std::shared_ptr<const JournalReadView> JournalStore::AcquireReadView() const
{
    std::lock_guard lock( m_lock );
    return m_view;
}

ScanResult JournalStore::LastObservation() const
{
    std::lock_guard lock( m_lock );
    return m_lastObservation;
}

bool JournalStore::ReadCommitted( const JournalReadView& view, uint64_t offset, std::span<uint8_t> output, std::string& error ) const
{
    error.clear();
    if( view.path != m_path )
    {
        error = "read view belongs to a different journal store";
        return false;
    }
    if( view.storeIdentity.get() != m_identity.get() )
    {
        error = "read view was not published by this journal store";
        return false;
    }
    if( offset > view.validSize || uint64_t( output.size() ) > view.validSize - offset )
    {
        error = "read exceeds the immutable committed prefix";
        return false;
    }
    if( offset > uint64_t( std::numeric_limits<std::streamoff>::max() )
        || output.size() > size_t( std::numeric_limits<std::streamsize>::max() ) )
    {
        error = "committed read exceeds the stream API range";
        return false;
    }

    std::error_code filesystemError;
    const auto currentSize = std::filesystem::file_size( m_path, filesystemError );
    if( filesystemError )
    {
        error = "cannot stat journal before committed read: " + filesystemError.message();
        return false;
    }
    if( currentSize < view.validSize )
    {
        error = "journal was truncated below the immutable committed prefix";
        return false;
    }

    // Revalidate exactly this view's causal prefix before serving bytes. The
    // prefix fingerprint detects a validly re-encoded in-place replacement,
    // not only broken record CRCs.
    const auto prefix = ScanJournalPrefix( m_path, view.validSize, m_options );
    if( !prefix.HasRecoverablePrefix()
        || !SameHeader( prefix.header, view.header )
        || prefix.validSize != view.validSize
        || prefix.lastSequence != view.revision
        || prefix.prefixCrc32c != view.prefixCrc32c )
    {
        error = "journal no longer contains the immutable committed prefix";
        return false;
    }

    std::ifstream file( m_path, std::ios::binary );
    if( !file )
    {
        error = "cannot open journal for committed read";
        return false;
    }
    file.seekg( std::streamoff( offset ), std::ios::beg );
    if( !file )
    {
        error = "cannot seek within committed journal prefix";
        return false;
    }
    if( !output.empty() )
    {
        file.read( reinterpret_cast<char*>( output.data() ), std::streamsize( output.size() ) );
        if( !file || file.gcount() != std::streamsize( output.size() ) )
        {
            error = "journal changed during committed read";
            return false;
        }
    }

    const auto afterRead = ScanJournalPrefix( m_path, view.validSize, m_options );
    if( !afterRead.HasRecoverablePrefix()
        || afterRead.validSize != view.validSize
        || afterRead.lastSequence != view.revision
        || afterRead.prefixCrc32c != view.prefixCrc32c )
    {
        error = "journal changed during committed read";
        return false;
    }
    return true;
}

const char* RefreshCodeName( RefreshCode code )
{
    switch( code )
    {
    case RefreshCode::Unchanged: return "UNCHANGED";
    case RefreshCode::Published: return "PUBLISHED";
    case RefreshCode::Rejected: return "REJECTED";
    case RefreshCode::IoError: return "IO_ERROR";
    default: return "UNKNOWN";
    }
}

}
