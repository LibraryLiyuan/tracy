    // At most two bounded transfer strings (image and path) are staged. Copies
     // are part of the live parser checkpoint; all resource history is on disk.
std::vector<uint8_t> transfer, transferImage;
size_t transferOffset = 0;
uint64_t symbolCodeAcks = 0;
void FinishCodeQueries( bool partial )
{
         // A failed code response has no address. Kernel queries are asynchronous
         // and may be overtaken by user-code replies, so FIFO attribution is not
         // sound. Associate failures only after a complete session identifies
         // the requests for which no successful payload arrived.
    if( partial || !info.cleanEnd ) return;
    const auto pending = uint64_t( db.Scalar( "SELECT COUNT(*) FROM queries WHERE type=12 AND used=0" ) );
    Require( pending == symbolCodeAcks, "Symbol code query/response count mismatch" );
    while( symbolCodeAcks )
    {
        const auto address = Response( ServerQuerySymbolCode, {} );
        auto q = db.Prep( "UPDATE symbol_resources SET status=3 WHERE address=? AND status!=2" );
        Int( q, 1, address );
        db.Run( q );
        --symbolCodeAcks;
    }
}
void CodeQuery( const ServerQueryPacket& query )
{
    if( query.type == ServerQueryDataTransfer )
    {
        Require( query.ptr > 0 && query.ptr <= TargetFrameSize, "Code query transfer exceeds decoder budget" );
        Require( transfer.empty() || transferOffset == transfer.size(), "Overlapping code query transfer" );
        Require( transferImage.empty(), "Too many code query transfer strings" );
        transferImage = std::move( transfer );
        transfer.assign( size_t( query.ptr ), 0 );
        transferOffset = 0;
    }
    else if( query.type == ServerQueryDataTransferPart )
    {
        Require( transferOffset < transfer.size(), "Code query transfer part has no active transfer" );
        uint8_t bytes[12];
        memcpy( bytes, &query.ptr, 8 );
        memcpy( bytes + 8, &query.extra, 4 );
        const auto count = std::min<size_t>( 12, transfer.size() - transferOffset );
        memcpy( transfer.data() + transferOffset, bytes, count );
        transferOffset += count;
    }
    else if( query.type == ServerQuerySourceCode )
    {
        Require( query.ptr <= UINT32_MAX && !transfer.empty() && transferOffset == transfer.size() && transfer.back() == 0, "Source query has no complete path" );
        const auto length = size_t( std::find( transfer.begin(), transfer.end(), uint8_t( 0 ) ) - transfer.begin() );
        const std::span<const uint8_t> path( transfer.data(), length );
        auto q = db.Prep( "INSERT OR IGNORE INTO source_resources(path,status) VALUES(?,1)" );
        Blob( q, 1, path );
        db.Run( q );
        if( sqlite3_changes( db.handle ) ) ++info.sourceFiles;
        q = db.Prep( "INSERT INTO source_requests(query,resource) SELECT ?1,id FROM source_resources WHERE path=?2" );
        Int( q, 1, query.ptr );
        Blob( q, 2, path );
        db.Run( q );
        Require( sqlite3_changes( db.handle ) == 1, "Source path association failed" );
        transfer.clear();
        transferImage.clear();
        transferOffset = 0;
    }
    else if( query.type == ServerQuerySymbolCode )
    {
        auto q = db.Prep( "UPDATE symbol_resources SET status=1 WHERE address=? AND status!=2" );
        Int( q, 1, query.ptr );
        db.Run( q );
    }
}
void HardwareSample( uint64_t id, const QueueHwSample& event, uint8_t kind )
{
    const auto time = event.time == 0 ? 0 : Time( event.time );
    auto q = db.Prep( "INSERT INTO hardware_samples VALUES(?,?,?,?)" );
    Int( q, 1, id );
    Int( q, 2, event.ip );
    Int( q, 3, kind );
    Int( q, 4, uint64_t( time ) );
    db.Run( q );
    q = db.Prep( "INSERT OR IGNORE INTO hardware_addresses(address) VALUES(?)" );
    Int( q, 1, event.ip );
    db.Run( q );
    if( sqlite3_changes( db.handle ) ) ++info.hardwareAddresses;
    const auto column = "c" + std::to_string( kind );
    q = db.Prep( "UPDATE hardware_addresses SET " + column + "=" + column + "+1 WHERE address=?" );
    Int( q, 1, event.ip );
    db.Run( q );
    ++info.hardwareSamples;
    info.lastNs = std::max( info.lastNs, time );
}
void SymbolCandidate( const QueueCallstackFrame& event, const std::string& name, const std::string& file, const std::string& image, bool inlined )
{
    if( !event.symAddr ) return;
    auto q = db.Prep( "INSERT OR IGNORE INTO symbol_candidates VALUES(?,?,?,?,?,?,?)" );
    Int( q, 1, event.symAddr );
    Text( q, 2, name );
    Text( q, 3, file );
    Int( q, 4, event.line );
    Text( q, 5, image );
    Int( q, 6, event.symLen );
    Int( q, 7, inlined );
    db.Run( q );
}
void SymbolInformation( uint64_t id, const QueueSymbolInformation& event )
{
    Response( ServerQuerySymbol, event.symAddr );
    const auto file = Take();
    auto q = db.Prep( "INSERT INTO symbol_resources SELECT ?1,address,name,?2,?3,image,file,line,size,inlined,CASE WHEN EXISTS(SELECT 1 FROM queries WHERE type=?5 AND key=?4) THEN 1 ELSE 0 END,NULL FROM symbol_candidates WHERE address=?4" );
    Int( q, 1, id );
    Text( q, 2, file );
    Int( q, 3, event.line );
    Int( q, 4, event.symAddr );
    Int( q, 5, ServerQuerySymbolCode );
    db.Run( q );
    Require( sqlite3_changes( db.handle ) == 1, "Symbol response has no recorded callstack definition" );
    ++info.symbols;
}
void SymbolCode( uint64_t address, std::span<const uint8_t> bytes )
{
    Response( ServerQuerySymbolCode, address );
    auto q = db.Prep( "UPDATE symbol_resources SET status=2,data=?2 WHERE address=?1 AND status!=2" );
    Int( q, 1, address );
    Blob( q, 2, bytes );
    db.Run( q );
    Require( sqlite3_changes( db.handle ) == 1, "Symbol code has no pending symbol" );
}
void SourceCode( uint32_t query, std::span<const uint8_t> bytes, bool available )
{
    Response( ServerQuerySourceCode, query );
    auto q = db.Prep( "UPDATE source_resources SET status=?2,data=?3 WHERE id=(SELECT resource FROM source_requests WHERE query=?1 AND done=0) AND status!=2" );
    Int( q, 1, query );
    Int( q, 2, available ? 2 : 3 );
    Blob( q, 3, bytes );
    db.Run( q );
    q = db.Prep( "UPDATE source_requests SET done=1 WHERE query=? AND done=0" );
    Int( q, 1, query );
    db.Run( q );
    Require( sqlite3_changes( db.handle ) == 1, "Source response has no recorded path request" );
}
