#include "TracyHash.hpp"
#include "TracyAnalysisIoPath.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#  include <Windows.h>
#  include <bcrypt.h>
#endif

namespace tracy::analysis
{
namespace
{
#ifdef _WIN32
// Windows' SHA-256 provider (hardware accelerated where the CPU supports it);
// its algorithm handle is opened once per process and shared by every hash.
BCRYPT_ALG_HANDLE Sha256Provider()
{
    static const BCRYPT_ALG_HANDLE provider = [] {
        BCRYPT_ALG_HANDLE handle = nullptr;
        if( BCryptOpenAlgorithmProvider( &handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) != 0 )
            throw std::runtime_error( "unable to open SHA-256 provider" );
        return handle;
    }();
    return provider;
}

class Sha256
{
public:
    Sha256()
    {
        if( BCryptCreateHash( Sha256Provider(), &m_hash, nullptr, 0, nullptr, 0, 0 ) != 0 )
            throw std::runtime_error( "unable to create SHA-256 hash" );
    }
    ~Sha256()
    {
        if( m_hash ) BCryptDestroyHash( m_hash );
    }
    Sha256( const Sha256& ) = delete;
    Sha256& operator=( const Sha256& ) = delete;

    void Update( const uint8_t* data, size_t size )
    {
        while( size != 0 )
        {
            const auto chunk = ULONG( std::min<size_t>( size, 1u << 30 ) );
            if( BCryptHashData( m_hash, const_cast<PUCHAR>( data ), chunk, 0 ) != 0 )
                throw std::runtime_error( "unable to hash SHA-256 data" );
            data += chunk;
            size -= chunk;
        }
    }

    std::array<uint8_t, 32> Final()
    {
        std::array<uint8_t, 32> result {};
        if( BCryptFinishHash( m_hash, result.data(), ULONG( result.size() ), 0 ) != 0 )
            throw std::runtime_error( "unable to finish SHA-256 hash" );
        return result;
    }

private:
    BCRYPT_HASH_HANDLE m_hash = nullptr;
};
#else

constexpr std::array<uint32_t, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

uint32_t RotateRight( uint32_t value, uint32_t count )
{
    return ( value >> count ) | ( value << ( 32 - count ) );
}

class Sha256
{
public:
    void Update( const uint8_t* data, size_t size )
    {
        m_totalBytes += size;
        while( size != 0 )
        {
            const auto copy = std::min( size, m_block.size() - m_blockSize );
            std::copy_n( data, copy, m_block.data() + m_blockSize );
            data += copy;
            size -= copy;
            m_blockSize += copy;
            if( m_blockSize == m_block.size() )
            {
                Transform( m_block.data() );
                m_blockSize = 0;
            }
        }
    }

    std::array<uint8_t, 32> Final()
    {
        const uint64_t bitCount = m_totalBytes * 8;
        m_block[m_blockSize++] = 0x80;
        if( m_blockSize > 56 )
        {
            std::fill( m_block.begin() + m_blockSize, m_block.end(), uint8_t( 0 ) );
            Transform( m_block.data() );
            m_blockSize = 0;
        }
        std::fill( m_block.begin() + m_blockSize, m_block.begin() + 56, uint8_t( 0 ) );
        for( size_t i = 0; i < 8; i++ ) m_block[63 - i] = uint8_t( bitCount >> ( i * 8 ) );
        Transform( m_block.data() );

        std::array<uint8_t, 32> result {};
        for( size_t i = 0; i < m_state.size(); i++ )
        {
            result[i * 4] = uint8_t( m_state[i] >> 24 );
            result[i * 4 + 1] = uint8_t( m_state[i] >> 16 );
            result[i * 4 + 2] = uint8_t( m_state[i] >> 8 );
            result[i * 4 + 3] = uint8_t( m_state[i] );
        }
        return result;
    }

private:
    void Transform( const uint8_t* block )
    {
        std::array<uint32_t, 64> words {};
        for( size_t i = 0; i < 16; i++ )
        {
            words[i] = uint32_t( block[i * 4] ) << 24 | uint32_t( block[i * 4 + 1] ) << 16 |
                uint32_t( block[i * 4 + 2] ) << 8 | uint32_t( block[i * 4 + 3] );
        }
        for( size_t i = 16; i < words.size(); i++ )
        {
            const uint32_t s0 = RotateRight( words[i - 15], 7 ) ^ RotateRight( words[i - 15], 18 ) ^ ( words[i - 15] >> 3 );
            const uint32_t s1 = RotateRight( words[i - 2], 17 ) ^ RotateRight( words[i - 2], 19 ) ^ ( words[i - 2] >> 10 );
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h] = m_state;
        for( size_t i = 0; i < words.size(); i++ )
        {
            const uint32_t sum1 = RotateRight( e, 6 ) ^ RotateRight( e, 11 ) ^ RotateRight( e, 25 );
            const uint32_t choice = ( e & f ) ^ ( ~e & g );
            const uint32_t temp1 = h + sum1 + choice + K[i] + words[i];
            const uint32_t sum0 = RotateRight( a, 2 ) ^ RotateRight( a, 13 ) ^ RotateRight( a, 22 );
            const uint32_t majority = ( a & b ) ^ ( a & c ) ^ ( b & c );
            const uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    std::array<uint32_t, 8> m_state { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::array<uint8_t, 64> m_block {};
    size_t m_blockSize = 0;
    uint64_t m_totalBytes = 0;
};
#endif

}

struct Sha256Builder::Impl
{
#ifdef _WIN32
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<uint8_t> object;

    Impl()
    {
        struct Provider
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            DWORD objectBytes = 0;
            Provider()
            {
                DWORD resultBytes = 0;
                if( BCryptOpenAlgorithmProvider( &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) != 0 )
                    throw std::runtime_error( "unable to open SHA-256 provider" );
                if( BCryptGetProperty( algorithm, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>( &objectBytes ), sizeof( objectBytes ),
                        &resultBytes, 0 ) != 0 || objectBytes == 0 )
                {
                    BCryptCloseAlgorithmProvider( algorithm, 0 );
                    algorithm = nullptr;
                    throw std::runtime_error( "unable to query SHA-256 provider" );
                }
            }
            ~Provider() { if( algorithm ) BCryptCloseAlgorithmProvider( algorithm, 0 ); }
        };
        static Provider provider;
        object.resize( provider.objectBytes );
        if( BCryptCreateHash( provider.algorithm, &hash, object.data(), DWORD( object.size() ), nullptr, 0, 0 ) != 0 )
            throw std::runtime_error( "unable to create SHA-256 hash" );
    }
    ~Impl() { if( hash ) BCryptDestroyHash( hash ); }
#else
    Sha256 value;
#endif
    std::string finalHex;
    // Small updates (callers often hash one record at a time) are gathered
    // and hashed in blocks; the digest does not depend on the split.
    std::array<uint8_t, 4096> pending;
    size_t pendingBytes = 0;
    void UpdateRaw( const uint8_t* data, size_t size )
    {
#ifdef _WIN32
        while( size != 0 )
        {
            const auto chunk = ULONG( std::min<size_t>( size, ULONG_MAX ) );
            if( BCryptHashData( hash, const_cast<PUCHAR>( data ), chunk, 0 ) != 0 )
                throw std::runtime_error( "unable to update SHA-256 hash" );
            data += chunk;
            size -= chunk;
        }
#else
        value.Update( data, size );
#endif
    }
    void Flush()
    {
        if( pendingBytes == 0 ) return;
        UpdateRaw( pending.data(), pendingBytes );
        pendingBytes = 0;
    }
};

Sha256Builder::Sha256Builder()
    : m_impl( std::make_unique<Impl>() )
{}

Sha256Builder::~Sha256Builder() = default;
Sha256Builder::Sha256Builder( Sha256Builder&& ) noexcept = default;
Sha256Builder& Sha256Builder::operator=( Sha256Builder&& ) noexcept = default;

void Sha256Builder::Update( const void* data, size_t size )
{
    if( !m_impl || !m_impl->finalHex.empty() ) throw std::runtime_error( "sha256_builder_already_finalized" );
    if( size == 0 ) return;
    auto& impl = *m_impl;
    if( size >= impl.pending.size() )
    {
        impl.Flush();
        impl.UpdateRaw( static_cast<const uint8_t*>( data ), size );
        return;
    }
    if( impl.pendingBytes + size > impl.pending.size() ) impl.Flush();
    std::memcpy( impl.pending.data() + impl.pendingBytes, data, size );
    impl.pendingBytes += size;
}

std::string Sha256Builder::FinalHex()
{
    if( !m_impl ) throw std::runtime_error( "sha256_builder_moved" );
    if( !m_impl->finalHex.empty() ) return m_impl->finalHex;
    m_impl->Flush();
#ifdef _WIN32
    std::array<uint8_t, 32> digest {};
    if( BCryptFinishHash( m_impl->hash, digest.data(), ULONG( digest.size() ), 0 ) != 0 )
        throw std::runtime_error( "unable to finish SHA-256 hash" );
#else
    const auto digest = m_impl->value.Final();
#endif
    static constexpr char Digits[] = "0123456789abcdef";
    std::string hex( digest.size() * 2, '0' );
    for( size_t i = 0; i < digest.size(); i++ )
    {
        hex[i * 2] = Digits[digest[i] >> 4];
        hex[i * 2 + 1] = Digits[digest[i] & 15];
    }
    m_impl->finalHex = std::move( hex );
    return m_impl->finalHex;
}

const char* Sha256Backend() noexcept
{
#ifdef _WIN32
    return "windows-cng";
#else
    return "portable";
#endif
}

const std::string& Sha256HexMemo::Digest( bool& added )
{
    if( m_key.size() > MaxPreimage )
    {
        added = false;
        Sha256Builder hash;
        hash.Update( m_key.data(), m_key.size() );
        m_last = hash.FinalHex();
        return m_last;
    }
    const auto found = m_digests.find( m_key );
    added = found == m_digests.end();
    if( !added ) return found->second;
    Sha256Builder hash;
    hash.Update( m_key.data(), m_key.size() );
    return m_digests.emplace( m_key, hash.FinalHex() ).first->second;
}

std::string Sha256File( const std::filesystem::path& path )
{
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<uint8_t> object;
    std::array<uint8_t, 32> digest {};
    const auto cleanup = [&] {
        if( hash ) BCryptDestroyHash( hash );
        if( algorithm ) BCryptCloseAlgorithmProvider( algorithm, 0 );
    };
    if( BCryptOpenAlgorithmProvider( &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) != 0 )
        throw std::runtime_error( "unable to open SHA-256 provider" );
    DWORD objectBytes = 0, resultBytes = 0;
    if( BCryptGetProperty( algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>( &objectBytes ), sizeof( objectBytes ),
            &resultBytes, 0 ) != 0 || objectBytes == 0 )
    {
        cleanup();
        throw std::runtime_error( "unable to query SHA-256 provider" );
    }
    object.resize( objectBytes );
    if( BCryptCreateHash( algorithm, &hash, object.data(), DWORD( object.size() ),
            nullptr, 0, 0 ) != 0 )
    {
        cleanup();
        throw std::runtime_error( "unable to create SHA-256 hash" );
    }

    std::ifstream stream( AnalysisIoPath( path ), std::ios::binary );
    if( !stream )
    {
        cleanup();
        throw std::runtime_error( "unable to open trace for fingerprint" );
    }
    std::vector<uint8_t> buffer( 4 * 1024 * 1024 );
    while( stream )
    {
        stream.read( reinterpret_cast<char*>( buffer.data() ),
            std::streamsize( buffer.size() ) );
        const auto read = stream.gcount();
        if( read > 0 && BCryptHashData( hash, buffer.data(), ULONG( read ), 0 ) != 0 )
        {
            cleanup();
            throw std::runtime_error( "unable to hash trace fingerprint" );
        }
    }
    if( !stream.eof() )
    {
        cleanup();
        throw std::runtime_error( "unable to read trace for fingerprint" );
    }
    if( BCryptFinishHash( hash, digest.data(), ULONG( digest.size() ), 0 ) != 0 )
    {
        cleanup();
        throw std::runtime_error( "unable to finish trace fingerprint" );
    }
    cleanup();
#else
    std::ifstream stream( AnalysisIoPath( path ), std::ios::binary );
    if( !stream ) throw std::runtime_error( "unable to open trace for fingerprint" );

    Sha256 hash;
    // Keep the streaming buffer well below the default Windows worker-thread
    // stack size. Trace hashing runs on SessionManager's loader thread.
    std::array<uint8_t, 64 * 1024> buffer {};
    while( stream )
    {
        stream.read( reinterpret_cast<char*>( buffer.data() ), buffer.size() );
        const auto read = stream.gcount();
        if( read > 0 ) hash.Update( buffer.data(), size_t( read ) );
    }
    if( !stream.eof() ) throw std::runtime_error( "unable to read trace for fingerprint" );

    const auto digest = hash.Final();
#endif
    std::ostringstream output;
    output << std::hex << std::setfill( '0' );
    for( const auto byte : digest ) output << std::setw( 2 ) << unsigned( byte );
    return output.str();
}

}
