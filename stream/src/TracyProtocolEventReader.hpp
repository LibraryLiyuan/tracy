#pragma once
#include "../../public/common/TracyQueue.hpp"
#include <cstring>
#include <span>

namespace tracy
{
struct ProtocolEventView
{
    QueueItem fixed{};
    std::span<const uint8_t> payload;
};

// One event is contained in one decompressed protocol frame. Never dereference
// a QueueItem through an unaligned or truncated network buffer.
inline bool ReadProtocolEvent( std::span<const uint8_t>& bytes, ProtocolEventView& event )
{
    if( bytes.empty() || bytes[0] >= uint8_t( QueueType::NUM_TYPES ) ) return false;
    event = {};
    const auto type = QueueType( bytes[0] );
    size_t fixedSize = QueueDataSize[bytes[0]], lengthSize = 0;
    if( type >= QueueType::StringData )
    {
        fixedSize = sizeof( QueueHeader ) + sizeof( QueueStringTransfer );
        lengthSize = type == QueueType::FrameImageData || type == QueueType::SymbolCode ||
                             type == QueueType::SourceCode || type == QueueType::JnGpuReferenceSetDefinition ||
                             type == QueueType::JnGpuCatalogBatchData
                         ? 4
                         : 2;
    }
    else if( type == QueueType::SingleStringData || type == QueueType::SecondStringData )
    {
        fixedSize = sizeof( QueueHeader );
        lengthSize = 2;
    }
    if( fixedSize > sizeof( event.fixed ) || bytes.size() < fixedSize + lengthSize ) return false;
    memcpy( &event.fixed, bytes.data(), fixedSize );
    uint32_t length = 0;
    if( lengthSize ) memcpy( &length, bytes.data() + fixedSize, lengthSize );
    if( length > bytes.size() - fixedSize - lengthSize ) return false;
    event.payload = bytes.subspan( fixedSize + lengthSize, length );
    bytes = bytes.subspan( fixedSize + lengthSize + length );
    return true;
}
}
