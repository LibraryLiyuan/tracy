void TelemetryPlot( uint64_t event, int kind, uint64_t key, I time, double value )
{
    auto q = db.Prep( "SELECT plot,last_time FROM system_plots WHERE kind=? AND key=?" );
    Int( q, 1, kind );
    Int( q, 2, key );
    uint64_t plot = 0;
    I last = 0;
    if( db.Step( q ) )
    {
        plot = sqlite3_column_int64( q, 0 );
        last = sqlite3_column_int64( q, 1 );
    }
    sqlite3_reset( q );
    if( !plot )
    {
        plot = event;
        q = db.Prep( "INSERT INTO plots(id,name,kind,format,sum) VALUES(?,?,?,?,?)" );
        Int( q, 1, plot );
        Text( q, 2, kind == 2 ? "CPU usage" : "???" );
        Int( q, 3, kind );
        Int( q, 4, kind == 2 ? 2 : 3 );
        Scalar( q, 5, 0 );
        db.Run( q );
        q = db.Prep( "INSERT INTO system_plots VALUES(?,?,?,?)" );
        Int( q, 1, kind );
        Int( q, 2, key );
        Int( q, 3, plot );
        Int( q, 4, time );
        db.Run( q );
        if( kind == 3 ) return; // First energy report establishes the time reference.
    }
    else if( kind == 3 )
    {
        const auto delta = Sub( time, last );
        value = delta == 0 ? std::numeric_limits<double>::infinity() : value * 1000. / double( delta );
        q = db.Prep( "UPDATE system_plots SET last_time=? WHERE kind=? AND key=?" );
        Int( q, 1, time );
        Int( q, 2, kind );
        Int( q, 3, key );
        db.Run( q );
        if( !std::isfinite( value ) ) return; // Native InsertPlot ignores non-finite power values.
    }
    q = db.Prep( "INSERT INTO plot_points VALUES(?,?,?,?)" );
    Int( q, 1, event );
    Int( q, 2, plot );
    Int( q, 3, time );
    Scalar( q, 4, value );
    db.Run( q );
    PlotValue( plot, value );
    info.lastNs = std::max( info.lastNs, time );
}
void Vsync( uint64_t event, const QueueFrameVsync& value )
{
    auto q = db.Prep( "SELECT setid FROM vsync_sets WHERE key=?" );
    Int( q, 1, value.id );
    uint64_t set = 0;
    if( db.Step( q ) ) set = sqlite3_column_int64( q, 0 );
    sqlite3_reset( q );
    if( !set )
    {
        set = ( 1ull << 63 ) | uint64_t( db.Scalar( "SELECT COUNT(*) FROM vsync_sets" ) );
        q = db.Prep( "INSERT INTO vsync_sets VALUES(?,?)" );
        Int( q, 1, value.id );
        Int( q, 2, set );
        db.Run( q );
    }
    Frame( set, Time( value.time ), true );
}
void Parameter( uint64_t event, const QueueParamSetup& value )
{
    auto q = db.Prep( "INSERT INTO parameters VALUES(?,?,?,?,?)" );
    Int( q, 1, event );
    Int( q, 2, value.idx );
    Int( q, 3, value.name );
    Int( q, 4, value.isBool != 0 );
    Int( q, 5, uint64_t( int64_t( value.val ) ) );
    db.Run( q );
}
