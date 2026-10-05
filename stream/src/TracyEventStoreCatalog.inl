// Included inside Ingest: Catalog state and pending relations live in SQLite.
void CatalogInvalid( uint64_t generation )
{
    db.Exec( "UPDATE catalog_state SET present=1,valid=0" );
    auto s = db.Prep( "UPDATE catalog_generations SET valid=0,state=2 WHERE id=?" );
    Int( s, 1, generation );
    db.Run( s );
}
void CatalogPassTime( uint64_t pass, I time, uint8_t priority )
{
    auto s = db.Prep( "INSERT INTO catalog_pass_times VALUES(?,?,?) ON CONFLICT(pass) DO UPDATE SET time=excluded.time,priority=excluded.priority WHERE priority<=excluded.priority" );
    Int( s, 1, pass );
    Int( s, 2, uint64_t( time ) );
    Int( s, 3, priority );
    db.Run( s );
}
void CatalogPayload( uint64_t id, std::span<const uint8_t> data )
{
    auto s = db.Prep( "SELECT COUNT(*) FROM catalog_pending WHERE id=?" );
    Int( s, 1, id );
    db.Step( s );
    const bool duplicate = sqlite3_column_int( s, 0 ) != 0;
    sqlite3_reset( s );
    if( !id || data.size() < sizeof( JnGpuCatalogBatchEnvelopeV1 ) || data.size() > 64ull * 1024 * 1024 || duplicate || db.Scalar( "SELECT COUNT(*) FROM catalog_pending" ) >= 16 )
    {
        CatalogInvalid( 0 );
        return;
    }
    s = db.Prep( "INSERT INTO catalog_pending VALUES(?,?)" );
    Int( s, 1, id );
    Blob( s, 2, data );
    db.Run( s );
}
void CatalogControl( uint64_t id, const QueueJnGpuCatalogControl& e )
{
    if( !e.generation || e.kind > 2 || e.state > 4 )
    {
        CatalogInvalid( e.generation );
        return;
    }
    auto s = db.Prep( "SELECT sequence,ended FROM catalog_generations WHERE id=?" );
    Int( s, 1, e.generation );
    const bool exists = db.Step( s );
    const auto sequence = exists ? uint32_t( sqlite3_column_int64( s, 0 ) ) : 0;
    const bool ended = exists && sqlite3_column_int( s, 1 );
    sqlite3_reset( s );
    const auto time = Time( e.time );
    if( e.kind == 0 )
    {
        if( exists || e.sequence != 1 )
        {
            CatalogInvalid( e.generation );
            return;
        }
        const bool valid = e.state == 0 && !( e.flags & 1 );
        if( !db.Scalar( "SELECT COUNT(*) FROM catalog_generations" ) ) db.Exec( "UPDATE catalog_state SET valid=1" );
        s = db.Prep( "INSERT INTO catalog_generations(id,event,begin_value,begin,sequence,state,flags,valid) VALUES(?,?,?,?,?,?,?,?)" );
        Int( s, 1, e.generation );
        Int( s, 2, id );
        Int( s, 3, e.value );
        Int( s, 4, uint64_t( time ) );
        Int( s, 5, e.sequence );
        Int( s, 6, e.state );
        Int( s, 7, e.flags );
        Int( s, 8, valid );
        db.Run( s );
    }
    else
    {
        if( !exists || ( e.kind == 2 && ended ) || sequence == UINT32_MAX || e.sequence != sequence + 1 )
        {
            CatalogInvalid( e.generation );
            return;
        }
        const bool valid = !( e.flags & 1 ) && e.state != 2 && e.state != 3 && e.state != 4;
        s = db.Prep( "UPDATE catalog_generations SET sequence=?1,state=?2,flags=flags|?3,valid=valid&?4,ended=CASE WHEN ?5=2 THEN 1 ELSE ended END,end=CASE WHEN ?5=2 THEN ?6 ELSE end END,end_value=CASE WHEN ?5=2 THEN ?7 ELSE end_value END WHERE id=?8" );
        Int( s, 1, e.sequence );
        Int( s, 2, e.state );
        Int( s, 3, e.flags );
        Int( s, 4, valid );
        Int( s, 5, e.kind );
        Int( s, 6, uint64_t( time ) );
        Int( s, 7, e.value );
        Int( s, 8, e.generation );
        db.Run( s );
    }
    s = db.Prep( "INSERT INTO catalog_controls VALUES(?,?,?,?,?,?,?,?,?)" );
    Int( s, 1, id );
    Int( s, 2, e.generation );
    Int( s, 3, uint64_t( time ) );
    Int( s, 4, e.value );
    Int( s, 5, thread );
    Int( s, 6, e.sequence );
    Int( s, 7, e.kind );
    Int( s, 8, e.state );
    Int( s, 9, e.flags );
    db.Run( s );
    db.Exec( "UPDATE catalog_state SET present=1,valid=valid AND NOT EXISTS(SELECT 1 FROM catalog_generations WHERE valid=0)" );
    s = db.Prep( "INSERT OR IGNORE INTO catalog_dirty VALUES(?)" );
    Int( s, 1, e.generation );
    db.Run( s );
    if( e.kind == 2 ) CatalogResolve( e.generation, false );
    info.lastNs = std::max( info.lastNs, time );
}
void CatalogResourceInterval( uint64_t generation, const JnGpuCatalogResourceRecordV1& record )
{
    if( !record.resourceId ) return;
    const auto operation = JnGpuCatalogRecordOperation( record.operation );
    auto life = db.Prep( "SELECT id FROM catalog_live_resources WHERE generation=? AND resource=?" );
    Int( life, 1, generation );
    Int( life, 2, record.resourceId );
    const bool active = db.Step( life );
    const uint64_t activeId = active ? sqlite3_column_int64( life, 0 ) : 0;
    sqlite3_reset( life );
    if( operation == JnGpuCatalogRecordOperation::Destroy )
    {
        if( active )
        {
            life = db.Prep( "UPDATE catalog_lifetimes SET end=? WHERE id=?" );
            Int( life, 1, uint64_t( record.time ) );
            Int( life, 2, activeId );
            db.Run( life );
            life = db.Prep( "SELECT begin FROM catalog_lifetimes WHERE id=?" );
            Int( life, 1, activeId );
            db.Step( life );
            const auto begin = sqlite3_column_int64( life, 0 );
            sqlite3_reset( life );
            Range( "UPDATE catalog_lifetime_range SET mint=?2,maxt=?3 WHERE id=?1", activeId, begin, record.time );
            life = db.Prep( "DELETE FROM catalog_live_resources WHERE generation=? AND resource=?" );
            Int( life, 1, generation );
            Int( life, 2, record.resourceId );
            db.Run( life );
        }
    }
    else if( !active )
    {
        const I begin = operation == JnGpuCatalogRecordOperation::Create ? record.time : INT64_MIN;
        life = db.Prep( "INSERT INTO catalog_lifetimes(generation,resource,begin) VALUES(?,?,?)" );
        Int( life, 1, generation );
        Int( life, 2, record.resourceId );
        Int( life, 3, uint64_t( begin ) );
        db.Run( life );
        const auto id = sqlite3_last_insert_rowid( db.handle );
        life = db.Prep( "INSERT INTO catalog_live_resources VALUES(?,?,?)" );
        Int( life, 1, generation );
        Int( life, 2, record.resourceId );
        Int( life, 3, id );
        db.Run( life );
        Range( "INSERT INTO catalog_lifetime_range VALUES(?1,?2,?3)", id, begin, {} );
    }
    if( !record.pointerToken ) return;
    bool create = operation == JnGpuCatalogRecordOperation::Create || operation == JnGpuCatalogRecordOperation::Open || operation == JnGpuCatalogRecordOperation::Snapshot;
    if( operation == JnGpuCatalogRecordOperation::Destroy )
    {
        auto s = db.Prep( "UPDATE catalog_intervals SET end=?1 WHERE id=(SELECT id FROM catalog_active WHERE generation=?2 AND token=?3) AND resource=?4" );
        Int( s, 1, uint64_t( record.time ) );
        Int( s, 2, generation );
        Int( s, 3, record.pointerToken );
        Int( s, 4, record.resourceId );
        db.Run( s );
        s = db.Prep( "DELETE FROM catalog_active WHERE generation=? AND token=?" );
        Int( s, 1, generation );
        Int( s, 2, record.pointerToken );
        db.Run( s );
        return;
    }
    if( !create )
    {
        auto s = db.Prep( "SELECT 1 FROM catalog_intervals WHERE generation=? AND token=? LIMIT 1" );
        Int( s, 1, generation );
        Int( s, 2, record.pointerToken );
        create = !db.Step( s );
        sqlite3_reset( s );
    }
    if( create )
    {
        auto s = db.Prep( "INSERT INTO catalog_intervals(generation,token,resource,begin) VALUES(?,?,?,?)" );
        Int( s, 1, generation );
        Int( s, 2, record.pointerToken );
        Int( s, 3, record.resourceId );
        Int( s, 4, uint64_t( operation == JnGpuCatalogRecordOperation::Create ? record.time : INT64_MIN ) );
        db.Run( s );
        const auto id = sqlite3_last_insert_rowid( db.handle );
        s = db.Prep( "INSERT INTO catalog_active VALUES(?,?,?) ON CONFLICT(generation,token) DO UPDATE SET id=excluded.id" );
        Int( s, 1, generation );
        Int( s, 2, record.pointerToken );
        Int( s, 3, id );
        db.Run( s );
    }
}
static uint16_t CatalogWidth( uint8_t kind )
{
    constexpr uint16_t sizes[] = { sizeof( JnGpuCatalogResourceRecordV1 ), sizeof( JnGpuCatalogAllocationRecordV1 ), sizeof( JnGpuCatalogViewRecordV1 ), sizeof( JnGpuCatalogLogicalRecordV1 ), sizeof( JnGpuCatalogPartRecordV1 ), sizeof( JnGpuCatalogRelationRecordV1 ), sizeof( JnGpuCatalogVgRecordV1 ), sizeof( JnGpuRangeSetRecordV1 ), sizeof( JnGpuDetailedEvidenceRecordV1 ), 0 };
    return kind < std::size( sizes ) ? sizes[kind] : 0;
}
void CatalogBatch( uint64_t id, const QueueJnGpuCatalogBatch& e )
{
    auto s = db.Prep( "SELECT data FROM catalog_pending WHERE id=?" );
    Int( s, 1, e.payloadId );
    if( !e.generation || !e.payloadId || !e.recordCount || e.kind > 9 || !db.Step( s ) )
    {
        sqlite3_reset( s );
        CatalogInvalid( e.generation );
        return;
    }
    const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( s, 0 ) );
    const auto size = sqlite3_column_bytes( s, 0 );
    std::vector<uint8_t> payload( p, p + size );
    sqlite3_reset( s );
    s = db.Prep( "DELETE FROM catalog_pending WHERE id=?" );
    Int( s, 1, e.payloadId );
    db.Run( s );
    s = db.Prep( "SELECT sequence,ended FROM catalog_generations WHERE id=?" );
    Int( s, 1, e.generation );
    if( !db.Step( s ) )
    {
        sqlite3_reset( s );
        CatalogInvalid( e.generation );
        return;
    }
    const auto sequence = uint32_t( sqlite3_column_int64( s, 0 ) );
    const bool ended = sqlite3_column_int( s, 1 ) != 0;
    sqlite3_reset( s );
    if( sequence == UINT32_MAX || e.sequence != sequence + 1 || payload.size() != e.payloadBytes || payload.size() < sizeof( JnGpuCatalogBatchEnvelopeV1 ) )
    {
        CatalogInvalid( e.generation );
        return;
    }
    JnGpuCatalogBatchEnvelopeV1 envelope;
    memcpy( &envelope, payload.data(), sizeof( envelope ) );
    auto records = std::span<const uint8_t>( payload ).subspan( sizeof( envelope ) );
    const auto width = CatalogWidth( e.kind );
    const uint8_t encoding = e.kind == 7 ? 2 : e.kind == 8 ? 3
                                                           : 0;
    if( envelope.magic != JnGpuCatalogBatchMagic || envelope.catalogSchema != JnGpuCatalogSchemaVersion || envelope.evidenceSchema != JnGpuDetailedEvidenceSchemaVersion || envelope.recordCount != e.recordCount || envelope.payloadBytes != records.size() || envelope.flags != e.flags || envelope.recordBytes != width || e.encoding != encoding || JnGpuCatalogChecksum64( records.data(), records.size() ) != envelope.checksum || ( width && uint64_t( width ) * e.recordCount != records.size() ) )
    {
        CatalogInvalid( e.generation );
        return;
    }
    if( e.kind == 9 )
    {
        auto check = records;
        for( uint32_t n = 0; n < e.recordCount; ++n )
        {
            if( check.size() < sizeof( JnGpuCatalogStringRecordHeaderV1 ) )
            {
                CatalogInvalid( e.generation );
                return;
            }
            JnGpuCatalogStringRecordHeaderV1 header;
            memcpy( &header, check.data(), sizeof( header ) );
            check = check.subspan( sizeof( header ) );
            if( !header.stringId || header.byteLength > 256 || check.size() < header.byteLength || ( header.byteLength && header.originalLength == header.byteLength && JnGpuCatalogChecksum64( check.data(), header.byteLength ) != header.hash ) )
            {
                CatalogInvalid( e.generation );
                return;
            }
            check = check.subspan( header.byteLength );
        }
        if( !check.empty() )
        {
            CatalogInvalid( e.generation );
            return;
        }
    }
    s = db.Prep( "SELECT count FROM catalog_kind_counts WHERE kind=?" );
    Int( s, 1, e.kind );
    const uint64_t first = db.Step( s ) ? sqlite3_column_int64( s, 0 ) : 0;
    sqlite3_reset( s );
    s = db.Prep( "INSERT INTO catalog_batches(id,generation,transport,first_record,sequence,records,bytes,kind,encoding,flags) VALUES(?,?,?,?,?,?,?,?,?,?)" );
    Int( s, 1, id );
    Int( s, 2, e.generation );
    Int( s, 3, envelope.checksum );
    Int( s, 4, first );
    Int( s, 5, e.sequence );
    Int( s, 6, e.recordCount );
    Int( s, 7, records.size() );
    Int( s, 8, e.kind );
    Int( s, 9, e.encoding );
    Int( s, 10, e.flags );
    db.Run( s );
    s = db.Prep( "INSERT OR IGNORE INTO catalog_dirty_batches VALUES(?)" );
    Int( s, 1, id );
    db.Run( s );
    for( uint32_t n = 0; n < e.recordCount; ++n )
    {
        size_t bytes = width;
        uint64_t object = 0;
        std::optional<I> time;
        if( e.kind == 9 )
        {
            JnGpuCatalogStringRecordHeaderV1 header;
            memcpy( &header, records.data(), sizeof( header ) );
            bytes = sizeof( header ) + header.byteLength;
            object = header.stringId;
        }
        std::vector<uint8_t> data( records.begin(), records.begin() + bytes );
        records = records.subspan( bytes );
        if( e.kind < 9 )
        {
            memcpy( &object, data.data() + ( e.kind == 7 ? 0 : 8 ), 8 );
            if( e.kind != 7 )
            {
                I raw;
                memcpy( &raw, data.data(), 8 );
                time = Time( raw );
                memcpy( data.data(), &*time, 8 );
            }
        }
        s = db.Prep( "INSERT INTO catalog_records(batch,generation,kind,ordinal,object,time,data) VALUES(?,?,?,?,?,?,?)" );
        Int( s, 1, id );
        Int( s, 2, e.generation );
        Int( s, 3, e.kind );
        Int( s, 4, n );
        Int( s, 5, object );
        if( time ) Int( s, 6, uint64_t( *time ) );
        Blob( s, 7, data );
        db.Run( s );
        const auto recordId = sqlite3_last_insert_rowid( db.handle );
        if( e.kind == 2 || e.kind == 3 || e.kind == 5 || e.kind == 6 || e.kind == 7 || e.kind == 8 )
        {
            s = db.Prep( "INSERT INTO catalog_unresolved VALUES(?,?)" );
            Int( s, 1, recordId );
            Int( s, 2, e.generation );
            db.Run( s );
        }
        if( e.kind == 0 )
        {
            JnGpuCatalogResourceRecordV1 record;
            memcpy( &record, data.data(), sizeof( record ) );
            CatalogResourceInterval( e.generation, record );
        }
    }
    s = db.Prep( "INSERT INTO catalog_kind_counts VALUES(?,?) ON CONFLICT(kind) DO UPDATE SET count=excluded.count" );
    Int( s, 1, e.kind );
    Int( s, 2, first + e.recordCount );
    db.Run( s );
    s = db.Prep( "UPDATE catalog_generations SET sequence=?,batches=batches+1,records=records+?,bytes=bytes+? WHERE id=?" );
    Int( s, 1, e.sequence );
    Int( s, 2, e.recordCount );
    Int( s, 3, envelope.payloadBytes );
    Int( s, 4, e.generation );
    db.Run( s );
    s = db.Prep( "INSERT OR IGNORE INTO catalog_dirty VALUES(?)" );
    Int( s, 1, e.generation );
    db.Run( s );
    if( ended ) CatalogResolve( e.generation, false );
}
uint64_t CatalogResolvePointer( uint64_t generation, uint64_t token, I time )
{
    if( !token ) return 0;
    auto s = db.Prep( "SELECT MIN(resource),MAX(resource) FROM catalog_intervals WHERE generation=?1 AND token=?2 AND (?3=?4 OR(begin<=?3 AND(end IS NULL OR end>=?3)))" );
    Int( s, 1, generation );
    Int( s, 2, token );
    Int( s, 3, uint64_t( time ) );
    Int( s, 4, uint64_t( INT64_MIN ) );
    db.Step( s );
    const auto low = uint64_t( sqlite3_column_int64( s, 0 ) ), high = uint64_t( sqlite3_column_int64( s, 1 ) );
    sqlite3_reset( s );
    return low == high ? low : 0;
}
uint64_t CatalogDescriptor( uint64_t token )
{
    auto s = db.Prep( "SELECT id FROM catalog_descriptor_heaps WHERE token=?" );
    Int( s, 1, token );
    if( db.Step( s ) )
    {
        const auto id = uint64_t( sqlite3_column_int64( s, 0 ) );
        sqlite3_reset( s );
        return id;
    }
    sqlite3_reset( s );
    const auto id = 0xd000000000000000ull | catalogDescriptorId++;
    s = db.Prep( "INSERT INTO catalog_descriptor_heaps VALUES(?,?)" );
    Int( s, 1, token );
    Int( s, 2, id );
    db.Run( s );
    return id;
}
void CatalogResolve( uint64_t generation, bool final )
{
    uint64_t unresolved = 0, core = 0;
    auto records = db.Prep( "SELECT id,kind,data,batch FROM catalog_records WHERE id IN(SELECT id FROM catalog_unresolved WHERE generation=?) ORDER BY kind,id" );
    Int( records, 1, generation );
    while( db.Step( records ) )
    {
        const auto id = uint64_t( sqlite3_column_int64( records, 0 ) );
        const auto kind = uint8_t( sqlite3_column_int( records, 1 ) );
        const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( records, 2 ) );
        std::vector<uint8_t> data( p, p + sqlite3_column_bytes( records, 2 ) );
        bool remains = false;
        const auto resource = [&]( auto& record, I time, uint8_t missing ) {
            if( !record.resourceId && record.pointerToken ) record.resourceId = CatalogResolvePointer( generation, record.pointerToken, time );
            if( record.pointerToken && !record.resourceId )
            {
                ++unresolved;
                remains = true;
                record.exactness = missing;
            }
            if( record.resourceId ) record.pointerToken = 0;
        };
        switch( kind )
        {
        case 2: {
            JnGpuCatalogViewRecordV1 record;
            memcpy( &record, data.data(), sizeof( record ) );
            resource( record, record.time, 2 );
            memcpy( data.data(), &record, sizeof( record ) );
            break;
        }
        case 3: {
            JnGpuCatalogLogicalRecordV1 record;
            memcpy( &record, data.data(), sizeof( record ) );
            resource( record, record.time, 2 );
            memcpy( data.data(), &record, sizeof( record ) );
            break;
        }
        case 6: {
            JnGpuCatalogVgRecordV1 record;
            memcpy( &record, data.data(), sizeof( record ) );
            resource( record, record.time, 2 );
            memcpy( data.data(), &record, sizeof( record ) );
            break;
        }
        case 7: {
            JnGpuRangeSetRecordV1 record;
            memcpy( &record, data.data(), sizeof( record ) );
            I time = INT64_MIN;
            auto s = db.Prep( "SELECT time FROM catalog_pass_times WHERE pass=?" );
            Int( s, 1, record.passInstanceId );
            if( db.Step( s ) ) time = sqlite3_column_int64( s, 0 );
            sqlite3_reset( s );
            resource( record, time, 3 );
            memcpy( data.data(), &record, sizeof( record ) );
            break;
        }
        case 5: {
            JnGpuCatalogRelationRecordV1 record;
            memcpy( &record, data.data(), sizeof( record ) );
            const auto reference = [&]( uint64_t& object, uint8_t bit ) {
                if( !( record.flags & bit ) ) return;
                const auto resolved = CatalogResolvePointer( generation, object, record.time );
                if( !resolved )
                {
                    ++unresolved;
                    remains = true;
                    if( record.relation == 0 || record.relation == 8 ) ++core;
                    record.exactness = 2;
                }
                else
                {
                    object = resolved;
                    record.flags &= ~bit;
                }
            };
            reference( record.sourceId, 1 );
            reference( record.targetId, 2 );
            memcpy( data.data(), &record, sizeof( record ) );
            break;
        }
        case 8: {
            JnGpuDetailedEvidenceRecordV1 record;
            memcpy( &record, data.data(), sizeof( record ) );
            const auto reference = [&]( uint64_t& object, uint32_t bit ) {
                if( !( record.flags & bit ) ) return;
                const auto resolved = CatalogResolvePointer( generation, object, record.time );
                if( !resolved )
                {
                    ++unresolved;
                    remains = true;
                    record.flags |= 128;
                }
                else
                {
                    object = resolved;
                    record.flags &= ~bit;
                }
            };
            reference( record.sourceId, 1 );
            reference( record.targetId, 2 );
            if( record.flags & 4 )
            {
                record.sourceId = CatalogDescriptor( record.sourceId );
                record.flags &= ~uint32_t( 4 );
            }
            if( record.flags & 8 )
            {
                record.targetId = CatalogDescriptor( record.targetId );
                record.flags &= ~uint32_t( 8 );
            }
            memcpy( data.data(), &record, sizeof( record ) );
            break;
        }
        }
        if( !std::equal( data.begin(), data.end(), p ) )
        {
            auto update = db.Prep( "UPDATE catalog_records SET data=? WHERE id=?" );
            Blob( update, 1, data );
            Int( update, 2, id );
            db.Run( update );
            update = db.Prep( "INSERT OR IGNORE INTO catalog_dirty_batches VALUES(?)" );
            Int( update, 1, sqlite3_column_int64( records, 3 ) );
            db.Run( update );
        }
        if( !remains )
        {
            auto remove = db.Prep( "DELETE FROM catalog_unresolved WHERE id=?" );
            Int( remove, 1, id );
            db.Run( remove );
        }
    }
    sqlite3_reset( records );
    auto s = db.Prep( "UPDATE catalog_generations SET unresolved=? WHERE id=?" );
    Int( s, 1, unresolved );
    Int( s, 2, generation );
    db.Run( s );
    if( final )
    {
        s = db.Prep( "UPDATE catalog_generations SET valid=0,state=2 WHERE id=? AND(ended=0 OR ?!=0)" );
        Int( s, 1, generation );
        Int( s, 2, core );
        db.Run( s );
        db.Exec( "UPDATE catalog_state SET valid=valid AND NOT EXISTS(SELECT 1 FROM catalog_generations WHERE valid=0)" );
    }
}
void CatalogFinish( bool partial )
{
    if( !partial ) db.Exec( "INSERT OR IGNORE INTO catalog_dirty SELECT id FROM catalog_generations" );
    if( !partial && db.Scalar( "SELECT COUNT(*) FROM catalog_pending" ) )
    {
        CatalogInvalid( 0 );
        db.Exec( "DELETE FROM catalog_pending" );
    }
    auto generations = db.Prep( partial ? "SELECT id FROM catalog_generations WHERE id IN(SELECT generation FROM catalog_dirty) AND ended=1 ORDER BY event" : "SELECT id FROM catalog_generations ORDER BY event" );
    while( db.Step( generations ) ) CatalogResolve( sqlite3_column_int64( generations, 0 ), !partial );
    sqlite3_reset( generations );
    auto batches = db.Prep( "SELECT id FROM catalog_batches WHERE id IN(SELECT id FROM catalog_dirty_batches) ORDER BY id" );
    while( db.Step( batches ) )
    {
        const auto id = uint64_t( sqlite3_column_int64( batches, 0 ) );
        uint64_t checksum = 14695981039346656037ull;
        auto rows = db.Prep( "SELECT data FROM catalog_records WHERE batch=? ORDER BY ordinal" );
        Int( rows, 1, id );
        while( db.Step( rows ) )
        {
            const auto p = static_cast<const uint8_t*>( sqlite3_column_blob( rows, 0 ) );
            for( int n = 0; n < sqlite3_column_bytes( rows, 0 ); ++n )
            {
                checksum ^= p[n];
                checksum *= 1099511628211ull;
            }
        }
        sqlite3_reset( rows );
        auto update = db.Prep( "UPDATE catalog_batches SET stored=? WHERE id=?" );
        Int( update, 1, checksum );
        Int( update, 2, id );
        db.Run( update );
        if( checksum == 0 )
        {
            update = db.Prep( "UPDATE catalog_batches SET valid=0 WHERE id=? AND records!=0" ); Int( update, 1, id ); db.Run( update );
            db.Exec( "UPDATE catalog_state SET valid=0" );
        }
    }
    sqlite3_reset( batches );
    db.Exec( "DELETE FROM catalog_dirty;DELETE FROM catalog_dirty_batches;" );
}
