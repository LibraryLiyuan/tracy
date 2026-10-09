namespace
{
template<class T>
size_t CodePageLimit( Db& db, size_t limit, size_t textBytes = 0 )
{
    Require( limit <= 4096, "Resource page limit exceeded" );
    return std::min<size_t>( limit, ( db.options.resultBytes - sizeof( StorePage<T> ) ) / ( sizeof( T ) + textBytes ) );
}
StoreBinaryChunk ResourceChunk( Db& db, const std::string& table, const std::string& column, uint64_t id, uint64_t offset, size_t maxBytes, bool code )
{
    Require( offset < INT64_MAX && maxBytes > 0 && maxBytes <= db.options.resultBytes - sizeof( StoreBinaryChunk ), "Resource chunk exceeds result budget" );
    auto q = db.Prep( "SELECT length(CAST(" + column + " AS BLOB)),substr(CAST(" + column + " AS BLOB),?2,?3) FROM " + table + " WHERE id=?1" + ( code ? " AND status=2" : "" ) );
    Int( q, 1, id );
    Int( q, 2, offset + 1 );
    Int( q, 3, maxBytes );
    Require( db.Step( q ), "Resource was not recorded or is unavailable" );
    StoreBinaryChunk chunk;
    chunk.offset = offset;
    chunk.totalBytes = sqlite3_column_int64( q, 0 );
    const auto n = sqlite3_column_bytes( q, 1 );
    const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( q, 1 ) );
    if( n ) chunk.data.assign( p, p + n );
    chunk.more = offset < chunk.totalBytes && uint64_t( n ) < chunk.totalBytes - offset;
    sqlite3_reset( q );
    return chunk;
}
}
StorePage<StoreHardwareSample> EventStore::HardwareSamples( I begin, I end, std::optional<uint8_t> kind, std::optional<uint64_t> address, uint64_t after, size_t limit ) const
{
    if( kind ) Require( *kind < 6, "Unknown hardware counter" );
    auto& db = m_impl->db;
    limit = CodePageLimit<StoreHardwareSample>( db, limit );
    StorePage<StoreHardwareSample> page;
    if( !limit || begin >= end ) return page;
    page.values.reserve( limit );
    const bool all = begin == INT64_MIN && end == INT64_MAX && !kind && !address;
    std::string sql = "SELECT id,address,kind,time FROM hardware_samples ";
    sql += all ? "NOT INDEXED " : address ? ( kind ? "INDEXED BY hardware_address_kind_time " : "INDEXED BY hardware_address_time " )
                              : kind      ? "INDEXED BY hardware_kind_time "
                                          : "INDEXED BY hardware_time ";
    sql += "WHERE time>=?1 AND time<?2 AND id>?3 ";
    if( kind ) sql += "AND kind=?5 ";
    if( address ) sql += "AND address=?6 ";
    sql += "ORDER BY id LIMIT ?4";
    auto q = db.Prep( sql );
    Int( q, 1, uint64_t( begin ) );
    Int( q, 2, uint64_t( end ) );
    Int( q, 3, after );
    Int( q, 4, limit + 1 );
    if( kind ) Int( q, 5, *kind );
    if( address ) Int( q, 6, *address );
    while( db.Step( q ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreHardwareSample v;
        v.id = sqlite3_column_int64( q, 0 );
        v.address = sqlite3_column_int64( q, 1 );
        v.kind = uint8_t( sqlite3_column_int( q, 2 ) );
        v.time = sqlite3_column_int64( q, 3 );
        page.next = v.id;
        page.values.push_back( v );
    }
    sqlite3_reset( q );
    return page;
}
StorePage<StoreHardwareAddress> EventStore::HardwareAddresses( uint64_t after, size_t limit ) const
{
    auto& db = m_impl->db;
    limit = CodePageLimit<StoreHardwareAddress>( db, limit );
    StorePage<StoreHardwareAddress> page;
    if( !limit ) return page;
    page.values.reserve( limit );
    auto q = db.Prep( "SELECT id,address,c0,c1,c2,c3,c4,c5 FROM hardware_addresses WHERE id>? ORDER BY id LIMIT ?" );
    Int( q, 1, after );
    Int( q, 2, limit + 1 );
    while( db.Step( q ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreHardwareAddress v;
        v.id = sqlite3_column_int64( q, 0 );
        v.address = sqlite3_column_int64( q, 1 );
        for( int n = 0; n < 6; ++n ) v.counts[n] = sqlite3_column_int64( q, n + 2 );
        page.next = v.id;
        page.values.push_back( v );
    }
    sqlite3_reset( q );
    return page;
}
StorePage<StoreSourceResource> EventStore::SourceResources( uint64_t after, size_t limit ) const
{
    auto& db = m_impl->db;
    limit = CodePageLimit<StoreSourceResource>( db, limit, 4096 );
    StorePage<StoreSourceResource> page;
    if( !limit ) return page;
    page.values.reserve( limit );
    auto q = db.Prep( "SELECT id,status,length(data),length(path),substr(path,1,4096) FROM source_resources WHERE id>? ORDER BY id LIMIT ?" );
    Int( q, 1, after );
    Int( q, 2, limit + 1 );
    while( db.Step( q ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreSourceResource v;
        v.id = sqlite3_column_int64( q, 0 );
        v.status = StoreCodeStatus( sqlite3_column_int( q, 1 ) );
        v.bytes = sqlite3_column_int64( q, 2 );
        v.pathBytes = sqlite3_column_int64( q, 3 );
        v.path = Col( q, 4 );
        v.pathMore = v.path.size() < v.pathBytes;
        page.next = v.id;
        page.values.push_back( std::move( v ) );
    }
    sqlite3_reset( q );
    return page;
}
StorePage<StoreSymbolResource> EventStore::SymbolResources( uint64_t after, size_t limit ) const
{
    auto& db = m_impl->db;
    limit = CodePageLimit<StoreSymbolResource>( db, limit, 4 * 4096 );
    StorePage<StoreSymbolResource> page;
    if( !limit ) return page;
    page.values.reserve( limit );
    auto q = db.Prep( "SELECT id,address,line,call_line,size,inlined,status,length(data),substr(CAST(name AS BLOB),1,4096),substr(CAST(file AS BLOB),1,4096),substr(CAST(image AS BLOB),1,4096),substr(CAST(call_file AS BLOB),1,4096),length(CAST(name AS BLOB)),length(CAST(file AS BLOB)),length(CAST(image AS BLOB)),length(CAST(call_file AS BLOB)) FROM symbol_resources WHERE id>? ORDER BY id LIMIT ?" );
    Int( q, 1, after );
    Int( q, 2, limit + 1 );
    while( db.Step( q ) )
    {
        if( page.values.size() == limit )
        {
            page.more = true;
            break;
        }
        StoreSymbolResource v;
        v.id = sqlite3_column_int64( q, 0 );
        v.address = sqlite3_column_int64( q, 1 );
        v.line = uint32_t( sqlite3_column_int64( q, 2 ) );
        v.callLine = uint32_t( sqlite3_column_int64( q, 3 ) );
        v.size = sqlite3_column_int64( q, 4 );
        v.inlineFrame = sqlite3_column_int( q, 5 ) != 0;
        v.status = StoreCodeStatus( sqlite3_column_int( q, 6 ) );
        v.codeBytes = sqlite3_column_int64( q, 7 );
        std::string* strings[] = { &v.name, &v.file, &v.image, &v.callFile };
        for( int n = 0; n < 4; ++n )
        {
            *strings[n] = Col( q, 8 + n );
            v.textBytes[n] = sqlite3_column_int64( q, 12 + n );
            v.textMore |= strings[n]->size() < v.textBytes[n];
        }
        page.next = v.id;
        page.values.push_back( std::move( v ) );
    }
    sqlite3_reset( q );
    return page;
}
StoreBinaryChunk EventStore::CodeBytes( StoreCodeKind kind, uint64_t id, uint64_t offset, size_t maxBytes ) const
{
    Require( kind == StoreCodeKind::Source || kind == StoreCodeKind::Symbol, "Unknown code resource capability" );
    return ResourceChunk( m_impl->db, kind == StoreCodeKind::Source ? "source_resources" : "symbol_resources", "data", id, offset, maxBytes, true );
}
StoreBinaryChunk EventStore::CodeTextBytes( StoreCodeKind kind, uint64_t id, uint8_t field, uint64_t offset, size_t maxBytes ) const
{
    Require( kind == StoreCodeKind::Source || kind == StoreCodeKind::Symbol, "Unknown code text capability" );
    Require( kind == StoreCodeKind::Source ? field == 0 : field < 4, "Unknown resource text field" );
    constexpr const char* columns[] = { "name", "file", "image", "call_file" };
    return ResourceChunk( m_impl->db, kind == StoreCodeKind::Source ? "source_resources" : "symbol_resources", kind == StoreCodeKind::Source ? "path" : columns[field], id, offset, maxBytes, false );
}
