#include "../../server/TracyWorker.hpp"

#include <cstdio>
#include <exception>
#include <string>

int main()
{
    try
    {
        throw tracy::LoadFailure( "validation detail" );
    }
    catch( const std::exception& exception )
    {
        if( std::string( exception.what() ) != "validation detail" )
        {
            std::fprintf( stderr, "LoadFailure lost its diagnostic: %s\n", exception.what() );
            return 1;
        }
    }
    return 0;
}
