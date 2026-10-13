#ifndef __TRACYTEXTURE_HPP__
#define __TRACYTEXTURE_HPP__

#include <functional>
#include <stdint.h>
#include <imgui.h>

namespace tracy
{

void InitTexture();
ImTextureID MakeTexture( bool zigzag = false );
void FreeTexture( ImTextureID tex, void(*runOnMainThread)(const std::function<void()>&, bool) );
void UpdateTexture( ImTextureID tex, const char* data, int w, int h );
void UpdateTextureRGBA( ImTextureID tex, void* data, int w, int h );
void UpdateTextureRGBAMips( ImTextureID tex, void** data, int* w, int* h, size_t mips );
// A texture for a picture drawn at any size: filtered between mip levels and
// clamped at its edges. Fill it with UpdateTextureRGBAMips, down to 1x1.
ImTextureID MakeImageTexture();
// Decodes a DXT1 (BC1) image whose sides are multiples of four into RGBA, as
// UpdateTexture uploads it; on any thread.
void DecodeBc1( const char* data, int w, int h, uint32_t* out );

}

#endif
