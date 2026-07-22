#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "TracyPng.hpp"

#include <algorithm>
#include <stdexcept>

namespace tracy::query
{
namespace
{

void PngWrite( void* context, void* data, int size )
{
    auto& output = *static_cast<std::vector<uint8_t>*>( context );
    const auto* bytes = static_cast<const uint8_t*>( data );
    output.insert( output.end(), bytes, bytes + size );
}

}

std::vector<uint8_t> EncodePng( const analysis::FrameImageDto& image )
{
    if( image.width == 0 || image.height == 0 || image.width > 4096 || image.height > 4096 ) throw std::runtime_error( "invalid frame image dimensions" );
    const size_t expected = size_t( image.width ) * image.height * 4;
    if( image.rgba.size() != expected ) throw std::runtime_error( "invalid RGBA frame image buffer" );

    const uint8_t* pixels = image.rgba.data();
    std::vector<uint8_t> flipped;
    if( image.flipped )
    {
        flipped.resize( expected );
        const size_t stride = size_t( image.width ) * 4;
        for( uint32_t row = 0; row < image.height; row++ )
        {
            std::copy_n( image.rgba.data() + size_t( image.height - row - 1 ) * stride, stride, flipped.data() + size_t( row ) * stride );
        }
        pixels = flipped.data();
    }

    std::vector<uint8_t> output;
    if( stbi_write_png_to_func( PngWrite, &output, int( image.width ), int( image.height ), 4, pixels, int( image.width * 4 ) ) == 0 )
    {
        throw std::runtime_error( "PNG encoding failed" );
    }
    return output;
}

}
