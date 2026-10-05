#ifndef __TRACYHASH_HPP__
#define __TRACYHASH_HPP__

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tracy::analysis
{

class Sha256Builder
{
public:
    Sha256Builder();
    ~Sha256Builder();
    Sha256Builder( Sha256Builder&& ) noexcept;
    Sha256Builder& operator=( Sha256Builder&& ) noexcept;
    Sha256Builder( const Sha256Builder& ) = delete;
    Sha256Builder& operator=( const Sha256Builder& ) = delete;

    void Update( const void* data, size_t size );
    std::string FinalHex();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

std::string Sha256File( const std::filesystem::path& path );

// Remembers the SHA-256 hex digest of each distinct preimage. A preimage is the
// exact byte sequence a Sha256Builder would receive, built with Add() and
// AddString(); equal preimages have equal digests, so a remembered digest is
// the one a new builder would compute. Memory grows with the number of
// distinct preimages; callers account it.
class Sha256HexMemo
{
public:
    void Begin() { m_key.clear(); }
    void Add( const void* data, size_t size ) { m_key.append( static_cast<const char*>( data ), size ); }
    // A length-prefixed string, the encoding analysis signature ids hash.
    void AddString( std::string_view value )
    {
        const uint64_t size = value.size();
        Add( &size, sizeof( size ) );
        m_key.append( value );
    }
    // Digest of the current preimage; `added` tells whether it was kept as a
    // new entry. A preimage longer than MaxPreimage is hashed every time and
    // never kept, so the memo stays small beside the signatures it serves.
    const std::string& Digest( bool& added );
    size_t PreimageBytes() const { return m_key.size(); }
    static constexpr size_t MaxPreimage = 2048;

private:
    std::string m_key;
    std::string m_last;
    std::unordered_map<std::string, std::string> m_digests;
};

}

#endif
