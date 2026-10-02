#include "../src/OfflineValidationSink.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

using namespace tracy;

static void Check( bool value )
{
    if( !value ) throw std::runtime_error( "offline validation string source invariant" );
}

int main( int argc, char** argv )
{
    try
    {
        if( argc != 2 ) return 2;

        storage::ResourceLimits limits;
        limits.streamBytes = 64ull << 20;
        limits.diskReserveBytes = 0;
        storage::ResourceSample sample;
        sample.systemCommitLimit = 64ull << 30;
        storage::ConversionResources resources( limits, [&] { return sample; }, [] { return 1ull << 40; } );

        const auto root = std::filesystem::path( argv[1] );
        std::filesystem::create_directories( root );
        conversion::OfflineConversionStorage storage( root / "store", std::string( 64, 's' ), resources );
        uint32_t alpha = UINT32_MAX, beta = UINT32_MAX;
        Check( storage.InternString( "alpha", 5, alpha ) != nullptr && alpha == 0 );
        Check( storage.InternString( "beta", 4, beta ) != nullptr && beta == 1 );
        CallstackFrameId payload[2] {};
        payload[0].data = 0x1111;
        payload[1].data = 0x2222;
        uint32_t callstack = 0;
        Check( storage.InternCallstack( reinterpret_cast<const uint64_t*>( payload ), 2, callstack ) != nullptr && callstack == 1 );
        storage.Seal();
        storage.SealCatalog( {} );

        conversion::OfflineValidationSink sink( storage, resources );
        sink.GlobalStringsBegin( 2 );
        sink.GlobalStringBegin( 0, 1, 5 );
        sink.GlobalStringBytes( 0, 0, "alpha", 5 );
        sink.GlobalStringEnd( 0 );
        sink.GlobalStringBegin( 1, 2, 4 );
        sink.GlobalStringBytes( 1, 0, "beta", 4 );
        sink.GlobalStringEnd( 1 );
        sink.GlobalStringsEnd();

        sink.GlobalPointerTableBegin( 0, 1 );
        sink.GlobalPointerRecord( 0, 0, 101, 1, 0 );
        sink.GlobalPointerTableEnd( 0 );
        sink.GlobalPointerTableBegin( 1, 1 );
        sink.GlobalPointerRecord( 1, 0, 202, 2, 0 );
        sink.GlobalPointerTableEnd( 1 );
        sink.GlobalPointerTableBegin( 2, 1 );
        sink.GlobalPointerRecord( 2, 0, 303, 1, 2 );
        sink.GlobalPointerTableEnd( 2 );

        sink.CallstacksBegin( 1 );
        sink.CallstackBegin( 1, 2 );
        sink.CallstackFrames( 1, 0, payload, 2 );
        sink.CallstackEnd( 1 );
        sink.CallstacksEnd();
        CallstackFrameId definitionId {};
        definitionId.data = 0x3333;
        CallstackFrame definition {};
        definition.name = StringIdx( alpha );
        definition.file = StringIdx( beta );
        definition.line = 42;
        definition.symAddr = 0x4444;
        sink.CallstackFrameDefinitionsBegin( 1 );
        sink.CallstackFrameDefinitionBegin( 0, definitionId, 1, StringIdx( alpha ) );
        sink.CallstackFrameDefinitionRecords( 0, 0, &definition, 1 );
        sink.CallstackFrameDefinitionEnd( 0 );
        sink.CallstackFrameDefinitionsEnd();

        auto* source = sink.GlobalStringSource();
        Check( source != nullptr );
        Check( source->StringCount() == 2 );
        Check( source->PointerCount( 0 ) == 1 && source->PointerCount( 1 ) == 1 && source->PointerCount( 2 ) == 1 );
        Check( std::strcmp( source->StringAt( 0 ), "alpha" ) == 0 );
        Check( std::strcmp( source->StringAt( 1 ), "beta" ) == 0 );
        Check( std::strcmp( source->PointerString( 0, 101 ), "alpha" ) == 0 );
        Check( std::strcmp( source->PointerString( 1, 202 ), "beta" ) == 0 );
        const auto external = source->ExternalName( 303 );
        Check( external.first && external.second && std::strcmp( external.first, "alpha" ) == 0 && std::strcmp( external.second, "beta" ) == 0 );
        Check( source->HasPointer( 0, 101 ) && source->HasPointer( 1, 202 ) && source->HasPointer( 2, 303 ) );
        std::vector<uint64_t> ids;
        source->VisitPointerIds( 0, [&]( uint64_t id ) { ids.push_back( id ); } );
        Check( ids.size() == 1 && ids[0] == 101 );

        auto* callstackSource = sink.GlobalCallstackSource();
        Check( callstackSource != nullptr && callstackSource->CallstackCount() == 1 && callstackSource->FrameCount() == 1 );
        uint32_t payloadCount = 0;
        const auto* storedPayload = callstackSource->Frames( 1, payloadCount );
        Check( storedPayload && payloadCount == 2 && storedPayload[0].data == payload[0].data && storedPayload[1].data == payload[1].data );
        SerializedCallstackSource::FrameView frameView {};
        Check( callstackSource->Frame( definitionId, frameView ) );
        Check( frameView.imageName.Idx() == alpha && frameView.frames && frameView.count == 1 );
        Check( frameView.frames[0].name.Idx() == alpha && frameView.frames[0].file.Idx() == beta && frameView.frames[0].line == 42 && frameView.frames[0].symAddr == 0x4444 );

        std::puts( "Offline validation string source passed" );
    }
    catch( const std::exception& error )
    {
        std::fprintf( stderr, "Offline validation string source failed: %s\n", error.what() );
        return 1;
    }
}
