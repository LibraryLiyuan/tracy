#include "../src/TracyStreamJournal.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace tracy::stream;
void Check( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}
std::vector<uint8_t> Read( const std::filesystem::path& path )
{
    std::ifstream input( path, std::ios::binary );
    return { std::istreambuf_iterator<char>( input ), std::istreambuf_iterator<char>() };
}
void Write( const std::filesystem::path& path, std::span<const uint8_t> bytes )
{
    std::ofstream output( path, std::ios::binary | std::ios::trunc );
    output.write( reinterpret_cast<const char*>( bytes.data() ), bytes.size() );
    Check( bool( output ), "write test journal" );
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-tail-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto path = root / "live.tracy-stream";
        const auto header = MakeFileHeader( 90 );
        std::string error;
        auto writer = JournalWriter::CreateFileJournal( path, header, false, {}, error );
        Check( bool( writer ), error.c_str() );
        Check( writer->Append( RecordType::SessionBegin, 0, std::string_view( "init" ), 1, error ), error.c_str() );
        JournalTailReader tail( path );
        std::vector<uint64_t> delivered;
        ScanOptions options;
        options.maxCollectedRecords = 0;
        options.recordVisitor = [&]( const RecordInfo& record ) { delivered.push_back( record.sequence ); };
        auto first = tail.Poll( options );
        Check( first.lastSequence == 1 && delivered == std::vector<uint64_t>{ 1 }, "initial committed prefix" );
        delivered.clear();
        tail.Poll( options );
        Check( delivered.empty(), "temporary EOF must not replay existing records" );
        Check( writer->Append( RecordType::Diagnostic, 0, std::string_view( "second" ), 2, error ), error.c_str() );
        auto second = tail.Poll( options );
        Check( second.lastSequence == 2 && delivered == std::vector<uint64_t>{ 2 }, "only appended records are delivered" );
        delivered.clear();
        const auto secondBytes = Read( path );
        Check( writer->Append( RecordType::Diagnostic, 0, std::string_view( "third" ), 3, error ), error.c_str() );
        writer.reset();
        const auto complete = Read( path );
        std::filesystem::resize_file( path, complete.size() - 1 );
        auto partial = tail.Poll( options );
        Check( partial.code == ScanCode::TruncatedTail && partial.lastSequence == 2 && delivered.empty(), "uncommitted trailer remains invisible" );
        {
            std::ofstream output( path, std::ios::binary | std::ios::app );
            output.put( char( complete.back() ) );
        }
        auto third = tail.Poll( options );
        Check( third.lastSequence == 3 && delivered == std::vector<uint64_t>{ 3 }, "completed tail is delivered once" );
        Check( third.prefixCrc32c == ScanJournal( path ).prefixCrc32c, "incremental checksum equals full scan" );
        bool cancel = true;
        options.cancelled = [&] { return cancel; };
        bool cancelled = false;
        try
        {
            tail.Poll( options );
        }
        catch( const std::exception& )
        {
            cancelled = true;
        }
        Check( cancelled && tail.Snapshot().lastSequence == 3, "cancellation preserves the accepted cursor" );
        cancel = false;
        delivered.clear();
        tail.Poll( options );
        Check( delivered.empty(), "reader can continue after cancellation" );
        options.cancelled = {};
        const auto quotaPath = root / "quota.tracy-stream";
        Write( quotaPath, complete );
        JournalTailReader quota( quotaPath );
        auto bounded = options;
        bounded.maxNewRecords = 1;
        for( uint64_t sequence = 1; sequence <= 3; ++sequence )
        {
            delivered.clear();
            auto value = quota.Poll( bounded );
            Check( value.lastSequence == sequence && delivered == std::vector<uint64_t>{ sequence }, "batch budget advances exactly one committed record" );
        }
        JournalTailReader retried( quotaPath );
        auto failing = options;
        failing.recordVisitor = []( const RecordInfo& ) { throw std::runtime_error( "consumer rolled back" ); };
        bool aborted = false;
        try
        {
            retried.Poll( failing );
        }
        catch( const std::exception& )
        {
            aborted = true;
        }
        Check( aborted && retried.Snapshot().lastSequence == 0, "visitor failure does not commit the reader cursor" );
        delivered.clear();
        retried.Poll( options );
        Check( delivered == std::vector<uint64_t>{ 1, 2, 3 }, "failed consumer batch can be retried" );
        const auto expectFailure = [&]( JournalTailReader& reader, uint64_t last, const char* reason ) {
            delivered.clear();
            bool failed = false;
            try
            {
                reader.Poll( options );
            }
            catch( const std::exception& )
            {
                failed = true;
            }
            Check( failed && delivered.empty() && reader.Snapshot().lastSequence == last, reason );
        };
        const auto corrupt = root / "corrupt.tracy-stream";
        Write( corrupt, secondBytes );
        JournalTailReader corruptReader( corrupt );
        corruptReader.Poll();
        auto bad = complete;
        bad[secondBytes.size() + RecordHeaderSize] ^= 1;
        Write( corrupt, bad );
        expectFailure( corruptReader, 2, "corrupt append cannot publish or deliver records" );
        const auto truncated = root / "truncated.tracy-stream";
        Write( truncated, complete );
        JournalTailReader truncatedReader( truncated );
        truncatedReader.Poll();
        std::filesystem::resize_file( truncated, secondBytes.size() );
        expectFailure( truncatedReader, 3, "committed truncation preserves previous revision" );
        const auto replacement = root / "replacement.tracy-stream";
        Write( replacement, complete );
        JournalTailReader replacedReader( replacement );
        replacedReader.Poll();
        std::filesystem::rename( replacement, root / "replacement.previous" );
        Write( replacement, complete );
        expectFailure( replacedReader, 3, "replacement is detected even when bytes and session match" );
        const auto rewritten = root / "rewritten.tracy-stream";
        Write( rewritten, complete );
        JournalTailReader rewrittenReader( rewritten );
        rewrittenReader.Poll();
        const auto time = std::filesystem::last_write_time( rewritten );
        const auto alternate = root / "alternate.tracy-stream";
        auto other = JournalWriter::CreateFileJournal( alternate, header, false, {}, error );
        Check( bool( other ), error.c_str() );
        Check( other->Append( RecordType::SessionBegin, 0, std::string_view( "EDIT" ), 1, error ), error.c_str() );
        Check( other->Append( RecordType::Diagnostic, 0, std::string_view( "second" ), 2, error ), error.c_str() );
        Check( other->Append( RecordType::Diagnostic, 0, std::string_view( "third" ), 3, error ), error.c_str() );
        other.reset();
        const auto alternateBytes = Read( alternate );
        Check( alternateBytes.size() == complete.size(), "same-size rewrite fixture" );
        Write( rewritten, alternateBytes );
        std::filesystem::last_write_time( rewritten, time );
        expectFailure( rewrittenReader, 3, "valid-CRC prefix rewrite with restored timestamp is detected" );
        // An unpublished follower can validate old bytes once at publication.
        // Its I/O must stay linear even with one committed record per poll.
        const auto linearPath = root / "linear.tracy-stream";
        auto linearWriter = JournalWriter::CreateFileJournal( linearPath, header, false, {}, error );
        Check( bool( linearWriter ), error.c_str() );
        const std::string payload( 256 * 1024, 'x' );
        constexpr uint64_t records = 32;
        for( uint64_t n = 0; n < records; ++n )
            Check( linearWriter->Append( RecordType::Diagnostic, 0, payload, n + 1, error ), error.c_str() );
        Check( linearWriter->Append( RecordType::SessionEnd, 0, "end", records + 1, error ), error.c_str() );
        linearWriter.reset();
        JournalTailReader linear( linearPath, JournalTailReader::PrefixValidation::Deferred );
        uint64_t verified = 0;
        for( uint64_t n = 1; n <= records + 1; ++n )
        {
            delivered.clear();
            auto value = linear.Poll( bounded );
            verified += linear.VerifiedBytes();
            Check( value.lastSequence == n && delivered == std::vector<uint64_t>{ n }, "deferred hash advances with each accepted record" );
        }
        Check( linear.Snapshot().complete, "deferred reader sees terminal record" );
        JournalTailReader reference( linearPath );
        reference.Poll();
        Check( linear.PrefixDigest() == reference.PrefixDigest(), "incremental SHA256 equals whole-file SHA256" );
        linear.VerifyPrefix();
        Check( verified + linear.Snapshot().validSize <= 7 * std::filesystem::file_size( linearPath ), "follower verification reads a linear amount of data" );
        delivered.clear();
        linear.Poll( bounded );
        Check( delivered.empty() && linear.VerifiedBytes() < 1024, "unchanged EOF does not reread historical payload" );

        JournalTailReader deferredRetry( quotaPath, JournalTailReader::PrefixValidation::Deferred );
        aborted = false;
        try { deferredRetry.Poll( failing ); }
        catch( const std::exception& ) { aborted = true; }
        Check( aborted && deferredRetry.Snapshot().lastSequence == 0, "deferred visitor failure rolls back the hash and cursor" );
        deferredRetry.Poll( bounded );
        deferredRetry.VerifyPrefix();
        cancel = true;
        cancelled = false;
        try { deferredRetry.VerifyPrefix( [&] { return cancel; } ); }
        catch( const std::exception& ) { cancelled = true; }
        Check( cancelled && deferredRetry.Snapshot().lastSequence == 1, "cancelled final verification retains accepted cursor" );
        deferredRetry.Poll( options );
        deferredRetry.VerifyPrefix();
        JournalTailReader quotaReference( quotaPath );
        quotaReference.Poll();
        Check( deferredRetry.PrefixDigest() == quotaReference.PrefixDigest(), "retried deferred hash has no duplicated bytes" );

        const auto deferredLivePath = root / "deferred-live.tracy-stream";
        Write( deferredLivePath, secondBytes );
        JournalTailReader deferredLive( deferredLivePath, JournalTailReader::PrefixValidation::Deferred );
        deferredLive.Poll();
        Write( deferredLivePath, std::span<const uint8_t>( complete.data(), complete.size() - 1 ) );
        delivered.clear();
        auto torn = deferredLive.Poll( options );
        Check( torn.code == ScanCode::TruncatedTail && delivered.empty(), "deferred reader does not hash an incomplete record" );
        Write( deferredLivePath, complete );
        deferredLive.Poll( options );
        deferredLive.VerifyPrefix();
        Check( delivered == std::vector<uint64_t>{ 3 } && deferredLive.PrefixDigest() == quotaReference.PrefixDigest(), "completed append is hashed and delivered once" );
        std::filesystem::resize_file( deferredLivePath, secondBytes.size() );
        expectFailure( deferredLive, 3, "deferred reader still rejects truncation immediately" );
        Write( deferredLivePath, complete );
        JournalTailReader deferredReplaced( deferredLivePath, JournalTailReader::PrefixValidation::Deferred );
        deferredReplaced.Poll();
        std::filesystem::rename( deferredLivePath, root / "deferred-live.previous" );
        Write( deferredLivePath, complete );
        expectFailure( deferredReplaced, 3, "deferred reader still rejects replacement immediately" );

        const auto deferredRewrite = root / "deferred-rewrite.tracy-stream";
        Write( deferredRewrite, complete );
        JournalTailReader deferredChanged( deferredRewrite, JournalTailReader::PrefixValidation::Deferred );
        deferredChanged.Poll();
        Write( deferredRewrite, alternateBytes );
        std::filesystem::last_write_time( deferredRewrite, time );
        bool finalRejected = false;
        try { deferredChanged.VerifyPrefix(); }
        catch( const std::exception& ) { finalRejected = true; }
        Check( finalRejected && deferredChanged.Snapshot().lastSequence == 3, "deferred verification rejects valid-CRC historical rewrites before publication" );
        expectFailure( deferredChanged, 3, "failed deferred reader cannot resume" );
        std::cout << "Journal tail tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
