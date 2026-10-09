// Schema 16 stores the high-volume event streams as compressed blocks in the
// append-only file `events.blocks`. SQLite keeps the block index (`blocks`,
// `block_range`) and small side tables for state that changes after a block
// was written. Every block is an LZ4 frame addressed by (offset,size) and
// verified with its XXH3 checksum on each read.
enum class BlockStream : uint8_t
{
    CpuZones = 1,
    ZoneText = 2,
    // Raw JN protocol packets, one stream per queue type (the block key).
    Packets = 3,
};
void PutVar( std::vector<uint8_t>& b, uint64_t v )
{
    while( v >= 0x80 )
    {
        b.push_back( uint8_t( v ) | 0x80 );
        v >>= 7;
    }
    b.push_back( uint8_t( v ) );
}
uint64_t GetVar( const uint8_t*& p, const uint8_t* e )
{
    uint64_t v = 0;
    for( int shift = 0;; shift += 7 )
    {
        Require( p < e && shift < 64, "Corrupt event block" );
        const auto c = *p++;
        v |= uint64_t( c & 0x7f ) << shift;
        if( !( c & 0x80 ) ) return v;
    }
}
uint64_t Zig( int64_t v ) { return ( uint64_t( v ) << 1 ) ^ uint64_t( v >> 63 ); }
int64_t Unzig( uint64_t v ) { return int64_t( v >> 1 ) ^ -int64_t( v & 1 ); }
#ifdef _WIN32
// Writers append from one thread. Readers use positioned reads, so any number
// of connections may share a file while it grows.
class BlockFile
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    uint64_t end = 0;

public:
    BlockFile( const std::filesystem::path& p, bool write )
    {
        handle = CreateFileW( p.c_str(), write ? ( GENERIC_READ | GENERIC_WRITE ) : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, write ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
        Require( handle != INVALID_HANDLE_VALUE, "Cannot open event block file" );
        LARGE_INTEGER size;
        Require( GetFileSizeEx( handle, &size ) != 0, "Cannot size event block file" );
        end = uint64_t( size.QuadPart );
    }
    BlockFile( const BlockFile& ) = delete;
    BlockFile& operator=( const BlockFile& ) = delete;
    ~BlockFile()
    {
        if( handle != INVALID_HANDLE_VALUE ) CloseHandle( handle );
    }
    uint64_t Size() const { return end; }
    uint64_t Append( std::span<const uint8_t> bytes )
    {
        const auto offset = end;
        for( size_t done = 0; done < bytes.size(); )
        {
            OVERLAPPED o{};
            const auto at = offset + done;
            o.Offset = DWORD( at );
            o.OffsetHigh = DWORD( at >> 32 );
            const auto n = DWORD( std::min<size_t>( bytes.size() - done, 1u << 30 ) );
            DWORD written = 0;
            Require( WriteFile( handle, bytes.data() + done, n, &written, &o ) != 0 && written == n, "Event block write failed" );
            done += n;
        }
        end += bytes.size();
        return offset;
    }
    void Read( uint64_t offset, std::span<uint8_t> out ) const
    {
        for( size_t done = 0; done < out.size(); )
        {
            OVERLAPPED o{};
            const auto at = offset + done;
            o.Offset = DWORD( at );
            o.OffsetHigh = DWORD( at >> 32 );
            const auto n = DWORD( std::min<size_t>( out.size() - done, 1u << 30 ) );
            DWORD read = 0;
            Require( ReadFile( handle, out.data() + done, n, &read, &o ) != 0 && read == n, "Event block read failed" );
            done += n;
        }
    }
    void Flush() { Require( FlushFileBuffers( handle ) != 0, "Event block flush failed" ); }
};
#endif
struct BlockLocation
{
    uint64_t id = 0, offset = 0, size = 0, raw = 0, checksum = 0;
};
// Raw block bytes are LZ4-compressed whole; the checksum covers the stored bytes.
std::vector<uint8_t> Compress( std::span<const uint8_t> raw )
{
    Require( raw.size() <= uint64_t( LZ4_MAX_INPUT_SIZE ), "Event block too large" );
    std::vector<uint8_t> out( size_t( LZ4_compressBound( int( raw.size() ) ) ) );
    const auto n = LZ4_compress_default( reinterpret_cast<const char*>( raw.data() ), reinterpret_cast<char*>( out.data() ), int( raw.size() ), int( out.size() ) );
    Require( n > 0, "Event block compression failed" );
    out.resize( size_t( n ) );
    return out;
}
std::vector<uint8_t> ReadBlock( const BlockFile& file, const BlockLocation& b )
{
    Require( b.size > 0 && b.size <= ( 1ull << 31 ) && b.raw <= uint64_t( LZ4_MAX_INPUT_SIZE ), "Invalid event block bounds" );
    std::vector<uint8_t> stored( size_t( b.size ) );
    file.Read( b.offset, stored );
    Require( XXH3_64bits( stored.data(), stored.size() ) == b.checksum, "Event block checksum mismatch" );
    std::vector<uint8_t> raw( size_t( b.raw ) );
    const auto n = LZ4_decompress_safe( reinterpret_cast<const char*>( stored.data() ), reinterpret_cast<char*>( raw.data() ), int( stored.size() ), int( raw.size() ) );
    Require( n >= 0 && uint64_t( n ) == b.raw, "Event block decompression failed" );
    return raw;
}
// One CPU zone. `open` means the end was unknown when the block was written;
// the final end, child count, child time, name and color then live in
// `zone_late`. `childTime` is the summed duration of the closed direct
// children, so a closed zone's self time is its duration minus childTime.
struct ZoneRow
{
    uint64_t id = 0, parent = 0, skey = 0, stack = 0, callsite = 0, validation = 0;
    int64_t start = 0, end = 0, childTime = 0;
    uint32_t depth = 0, children = 0, color = 0;
    uint8_t skind = 0;
    bool open = true, hasColor = false, hasName = false;
    std::string name;
};
// Hash of a zone source (kind, key) with mixed low bits for power-of-two tables.
struct SourceKeyHash
{
    size_t operator()( const std::pair<uint8_t, uint64_t>& k ) const
    {
        uint64_t x = k.second ^ ( uint64_t( k.first ) << 56 );
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        return size_t( x );
    }
};
// Column order: id delta, parent distance, start delta, duration+1 (0 = open),
// depth, child count, source index, flags, then optional stack, callsite,
// validation delta, color, child time and name for the zones whose flags
// carry them.
// Sources are a per-block dictionary of (kind,key).
std::vector<uint8_t> EncodeZones( const std::vector<ZoneRow>& rows, size_t count )
{
    std::vector<uint8_t> b;
    b.reserve( count * 12 + 64 );
    PutVar( b, count );
    std::vector<std::pair<uint8_t, uint64_t>> sources;
    std::unordered_map<std::pair<uint8_t, uint64_t>, uint32_t, SourceKeyHash> indexes;
    std::vector<uint32_t> sourceIndex( count );
    for( size_t n = 0; n < count; ++n )
    {
        const auto key = std::make_pair( rows[n].skind, rows[n].skey );
        const auto [found, added] = indexes.try_emplace( key, uint32_t( sources.size() ) );
        if( added ) sources.push_back( key );
        sourceIndex[n] = found->second;
    }
    PutVar( b, sources.size() );
    for( const auto& [kind, key] : sources )
    {
        b.push_back( kind );
        PutVar( b, key );
    }
    uint64_t prevId = 0;
    int64_t prevStart = 0;
    for( size_t n = 0; n < count; ++n )
    {
        PutVar( b, rows[n].id - prevId );
        prevId = rows[n].id;
    }
    for( size_t n = 0; n < count; ++n ) PutVar( b, rows[n].parent ? rows[n].id - rows[n].parent : 0 );
    for( size_t n = 0; n < count; ++n )
    {
        PutVar( b, Zig( rows[n].start - prevStart ) );
        prevStart = rows[n].start;
    }
    for( size_t n = 0; n < count; ++n ) PutVar( b, rows[n].open ? 0 : uint64_t( rows[n].end - rows[n].start ) + 1 );
    for( size_t n = 0; n < count; ++n ) PutVar( b, rows[n].depth );
    for( size_t n = 0; n < count; ++n ) PutVar( b, rows[n].children );
    for( size_t n = 0; n < count; ++n ) PutVar( b, sourceIndex[n] );
    for( size_t n = 0; n < count; ++n )
    {
        const auto& r = rows[n];
        b.push_back( uint8_t( ( r.stack ? 1 : 0 ) | ( r.callsite ? 2 : 0 ) | ( r.validation ? 4 : 0 ) | ( r.hasColor ? 8 : 0 ) | ( r.hasName ? 16 : 0 ) | ( r.childTime ? 32 : 0 ) ) );
    }
    for( size_t n = 0; n < count; ++n )
        if( rows[n].stack ) PutVar( b, rows[n].stack );
    for( size_t n = 0; n < count; ++n )
        if( rows[n].callsite ) PutVar( b, rows[n].callsite );
    uint64_t prevValidation = 0;
    for( size_t n = 0; n < count; ++n )
        if( rows[n].validation )
        {
            PutVar( b, Zig( int64_t( rows[n].validation - prevValidation ) ) );
            prevValidation = rows[n].validation;
        }
    for( size_t n = 0; n < count; ++n )
        if( rows[n].hasColor ) PutVar( b, rows[n].color );
    for( size_t n = 0; n < count; ++n )
        if( rows[n].childTime ) PutVar( b, uint64_t( rows[n].childTime ) );
    for( size_t n = 0; n < count; ++n )
        if( rows[n].hasName )
        {
            PutVar( b, rows[n].name.size() );
            b.insert( b.end(), rows[n].name.begin(), rows[n].name.end() );
        }
    return b;
}
void DecodeZones( std::span<const uint8_t> raw, std::vector<ZoneRow>& rows )
{
    const uint8_t *p = raw.data(), *e = p + raw.size();
    const auto count = size_t( GetVar( p, e ) );
    Require( count <= raw.size(), "Corrupt zone block" );
    const auto sourceCount = size_t( GetVar( p, e ) );
    Require( sourceCount <= count + 1, "Corrupt zone block sources" );
    std::vector<std::pair<uint8_t, uint64_t>> sources( sourceCount );
    for( auto& s : sources )
    {
        Require( p < e, "Corrupt zone block sources" );
        s.first = *p++;
        s.second = GetVar( p, e );
    }
    rows.assign( count, ZoneRow{} );
    uint64_t id = 0;
    int64_t start = 0;
    for( auto& r : rows ) r.id = id += GetVar( p, e );
    for( auto& r : rows )
    {
        const auto distance = GetVar( p, e );
        Require( distance <= r.id, "Corrupt zone parent" );
        r.parent = distance ? r.id - distance : 0;
    }
    for( auto& r : rows ) r.start = start += Unzig( GetVar( p, e ) );
    for( auto& r : rows )
    {
        const auto duration = GetVar( p, e );
        r.open = duration == 0;
        if( !r.open ) r.end = r.start + int64_t( duration - 1 );
    }
    for( auto& r : rows ) r.depth = uint32_t( GetVar( p, e ) );
    for( auto& r : rows ) r.children = uint32_t( GetVar( p, e ) );
    for( auto& r : rows )
    {
        const auto index = GetVar( p, e );
        Require( index < sources.size(), "Corrupt zone source index" );
        r.skind = sources[size_t( index )].first;
        r.skey = sources[size_t( index )].second;
    }
    std::vector<uint8_t> flags( count );
    for( auto& f : flags )
    {
        Require( p < e, "Corrupt zone flags" );
        f = *p++;
    }
    for( size_t n = 0; n < count; ++n )
        if( flags[n] & 1 ) rows[n].stack = GetVar( p, e );
    for( size_t n = 0; n < count; ++n )
        if( flags[n] & 2 ) rows[n].callsite = GetVar( p, e );
    uint64_t validation = 0;
    for( size_t n = 0; n < count; ++n )
        if( flags[n] & 4 ) rows[n].validation = validation += uint64_t( Unzig( GetVar( p, e ) ) );
    for( size_t n = 0; n < count; ++n )
        if( flags[n] & 8 )
        {
            rows[n].hasColor = true;
            rows[n].color = uint32_t( GetVar( p, e ) );
        }
    for( size_t n = 0; n < count; ++n )
        if( flags[n] & 32 ) rows[n].childTime = int64_t( GetVar( p, e ) );
    for( size_t n = 0; n < count; ++n )
        if( flags[n] & 16 )
        {
            const auto size = GetVar( p, e );
            Require( size <= uint64_t( e - p ), "Corrupt zone name" );
            rows[n].hasName = true;
            rows[n].name.assign( reinterpret_cast<const char*>( p ), size_t( size ) );
            p += size;
        }
    Require( p == e, "Trailing zone block data" );
}
// Zone text fragments of one logical thread in (zone,event) order.
struct TextRow
{
    uint64_t zone = 0, event = 0;
    std::string text;
};
std::vector<uint8_t> EncodeTexts( const std::vector<TextRow>& rows )
{
    std::vector<uint8_t> b;
    size_t bytes = 0;
    for( const auto& r : rows ) bytes += r.text.size();
    b.reserve( bytes + rows.size() * 6 + 16 );
    PutVar( b, rows.size() );
    uint64_t zone = 0, event = 0;
    for( const auto& r : rows )
    {
        PutVar( b, r.zone - zone );
        PutVar( b, r.event - event );
        PutVar( b, r.text.size() );
        zone = r.zone;
        event = r.event;
    }
    for( const auto& r : rows ) b.insert( b.end(), r.text.begin(), r.text.end() );
    return b;
}
void DecodeTexts( std::span<const uint8_t> raw, std::vector<TextRow>& rows )
{
    const uint8_t *p = raw.data(), *e = p + raw.size();
    const auto count = size_t( GetVar( p, e ) );
    Require( count <= raw.size(), "Corrupt text block" );
    rows.assign( count, TextRow{} );
    uint64_t zone = 0, event = 0;
    std::vector<uint64_t> sizes( count );
    for( size_t n = 0; n < count; ++n )
    {
        rows[n].zone = zone += GetVar( p, e );
        rows[n].event = event += GetVar( p, e );
        sizes[n] = GetVar( p, e );
    }
    for( size_t n = 0; n < count; ++n )
    {
        Require( sizes[n] <= uint64_t( e - p ), "Corrupt zone text" );
        rows[n].text.assign( reinterpret_cast<const char*>( p ), size_t( sizes[n] ) );
        p += sizes[n];
    }
    Require( p == e, "Trailing text block data" );
}
// One raw JN protocol packet: the queue item after its type byte, with the
// ingest thread, the serial stack current at the packet and the event time
// for the packet types that carry one (0 otherwise).
struct PacketRow
{
    uint64_t id = 0, thread = 0, stack = 0;
    int64_t time = 0;
};
// Column order: count, payload size, id delta, time delta, thread, stack, then
// the fixed-size payloads transposed by byte, so the bytes that rarely change
// between packets form long runs for LZ4.
std::vector<uint8_t> EncodePackets( const std::vector<PacketRow>& rows, const std::vector<uint8_t>& payloads, size_t size )
{
    const auto count = rows.size();
    Require( payloads.size() == count * size, "Packet payload size mismatch" );
    std::vector<uint8_t> b;
    b.reserve( count * ( size + 12 ) + 16 );
    PutVar( b, count );
    PutVar( b, size );
    uint64_t id = 0;
    int64_t time = 0;
    for( const auto& r : rows )
    {
        PutVar( b, r.id - id );
        id = r.id;
    }
    for( const auto& r : rows )
    {
        PutVar( b, Zig( r.time - time ) );
        time = r.time;
    }
    for( const auto& r : rows ) PutVar( b, r.thread );
    for( const auto& r : rows ) PutVar( b, r.stack );
    const auto base = b.size();
    b.resize( base + payloads.size() );
    for( size_t byte = 0; byte < size; ++byte )
        for( size_t n = 0; n < count; ++n ) b[base + byte * count + n] = payloads[n * size + byte];
    return b;
}
void DecodePackets( std::span<const uint8_t> raw, std::vector<PacketRow>& rows, std::vector<uint8_t>& payloads, size_t& size )
{
    const uint8_t *p = raw.data(), *e = p + raw.size();
    const auto count = size_t( GetVar( p, e ) );
    size = size_t( GetVar( p, e ) );
    Require( count <= raw.size() && size <= 31, "Corrupt packet block" );
    rows.assign( count, PacketRow{} );
    uint64_t id = 0;
    int64_t time = 0;
    for( auto& r : rows ) r.id = id += GetVar( p, e );
    for( auto& r : rows ) r.time = time += Unzig( GetVar( p, e ) );
    for( auto& r : rows ) r.thread = GetVar( p, e );
    for( auto& r : rows ) r.stack = GetVar( p, e );
    Require( uint64_t( e - p ) == uint64_t( count ) * size, "Corrupt packet payloads" );
    payloads.resize( count * size );
    for( size_t byte = 0; byte < size; ++byte )
        for( size_t n = 0; n < count; ++n ) payloads[n * size + byte] = p[byte * count + n];
}
// Per-block timeline summary: zones per clamped depth plus the lowest start
// and highest final end. A block whose zones all fall into one overview bin
// is counted from this summary without being decoded.
constexpr uint32_t SummaryDepths = 16;
struct ZoneSummary
{
    std::array<uint32_t, SummaryDepths> depth{};
    uint32_t unfinished = 0, invalid = 0, maximumDepth = 0;
};
std::vector<uint8_t> EncodeSummary( const ZoneSummary& s )
{
    std::vector<uint8_t> b;
    for( auto v : s.depth ) PutVar( b, v );
    PutVar( b, s.unfinished );
    PutVar( b, s.invalid );
    PutVar( b, s.maximumDepth );
    return b;
}
ZoneSummary DecodeSummary( std::span<const uint8_t> raw )
{
    ZoneSummary s;
    const uint8_t *p = raw.data(), *e = p + raw.size();
    for( auto& v : s.depth ) v = uint32_t( GetVar( p, e ) );
    s.unfinished = uint32_t( GetVar( p, e ) );
    s.invalid = uint32_t( GetVar( p, e ) );
    s.maximumDepth = uint32_t( GetVar( p, e ) );
    Require( p == e, "Corrupt zone block summary" );
    return s;
}
