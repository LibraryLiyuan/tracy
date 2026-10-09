#include <zstd.h>

#include "TracyBc1.hpp"
#include "TracyEvent.hpp"
#include "TracyTextureCompression.hpp"

namespace tracy
{

TextureCompression::TextureCompression()
    : m_buf( nullptr )
    , m_bufSize( 0 )
    , m_cctx( ZSTD_createCCtx() )
    , m_dctx( ZSTD_createDCtx() )
    , m_dict( nullptr )
{
}

TextureCompression::~TextureCompression()
{
    delete[] m_buf;
    ZSTD_freeCCtx( m_cctx );
    ZSTD_freeDCtx( m_dctx );
    ZSTD_freeDDict( m_dict );
}

uint32_t TextureCompression::Pack( struct ZSTD_CCtx_s* ctx, char*& buf, size_t& bufsz, const char* image, uint32_t inBytes )
{
    const auto maxout = ZSTD_COMPRESSBOUND( inBytes );
    if( bufsz < maxout )
    {
        bufsz = maxout;
        delete[] buf;
        buf = new char[maxout];
    }
    assert( ctx );
    auto ret = (uint32_t)ZSTD_compressCCtx( ctx, buf, maxout, image, inBytes, 3 );
#ifndef TRACY_NO_STATISTICS
    m_inputBytes.fetch_add( inBytes, std::memory_order_relaxed );
    m_outputBytes.fetch_add( ret, std::memory_order_relaxed );
#endif
    return ret;
}

uint32_t TextureCompression::Pack( struct ZSTD_CCtx_s* ctx, const struct ZSTD_CDict_s* dict, char*& buf, size_t& bufsz, const char* image, uint32_t inBytes )
{
    const auto maxout = ZSTD_COMPRESSBOUND( inBytes );
    if( bufsz < maxout )
    {
        bufsz = maxout;
        delete[] buf;
        buf = new char[maxout];
    }
    assert( ctx );
    auto ret = (uint32_t)ZSTD_compress_usingCDict( ctx, buf, maxout, image, inBytes, dict );
#ifndef TRACY_NO_STATISTICS
    m_inputBytes.fetch_add( inBytes, std::memory_order_relaxed );
    m_outputBytes.fetch_add( ret, std::memory_order_relaxed );
#endif
    return ret;
}

const char* TextureCompression::Unpack( const FrameImage& image )
{
    const auto outsz = size_t( image.w ) * size_t( image.h ) / 2;
    if( m_bufSize < outsz )
    {
        m_bufSize = outsz;
        delete[] m_buf;
        m_buf = new char[outsz];
    }
    assert( m_dctx );
    if( m_dict )
    {
        ZSTD_decompress_usingDDict( m_dctx, m_buf, outsz, image.ptr, image.csz, m_dict );
    }
    else
    {
        ZSTD_decompressDCtx( m_dctx, m_buf, outsz, image.ptr, image.csz );
    }
    return m_buf;
}

void TextureCompression::Rdo( char* data, size_t blocks ) { bc1::Rdo( data, blocks ); }
void TextureCompression::FixOrder( char* data, size_t blocks ) { bc1::FixOrder( data, blocks ); }
}
