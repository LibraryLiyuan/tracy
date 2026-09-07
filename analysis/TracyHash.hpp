#ifndef __TRACYHASH_HPP__
#define __TRACYHASH_HPP__

#include <filesystem>
#include <memory>
#include <string>

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

}

#endif
