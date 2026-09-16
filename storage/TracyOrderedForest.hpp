#ifndef TRACY_ORDERED_FOREST_HPP
#define TRACY_ORDERED_FOREST_HPP

#include "TracyMutableRows.hpp"
#include <array>

namespace tracy::storage
{

// Mutable construction and constant-stack preorder traversal for an ordered forest.
// Handles are logical IDs; callers must never retain pointers into the page cache.
class OrderedForest
{
    struct Header
    {
        uint64_t first = 0;
        uint64_t last = 0;
        uint64_t next = 0;
        uint64_t parent = 0;
        uint32_t children = 0;
        uint32_t flags = 0;
    };
    static_assert( sizeof( Header ) == 40 );
    static constexpr uint32_t RootFlag = 1;
    static constexpr uint32_t MaxPayload = 4096 - sizeof( Header );

public:
    OrderedForest( std::filesystem::path path, uint32_t payloadBytes, ConversionResources& resources, uint64_t cacheBytes = 2ull << 20 )
        : m_payloadBytes( payloadBytes )
        , m_rows( std::move( path ), uint32_t( sizeof( Header ) + payloadBytes ), resources, cacheBytes )
    {
        Require( payloadBytes > 0 && payloadBytes <= MaxPayload, "ordered_forest_configuration" );
    }

    uint64_t CreateRoot()
    {
        Require( !m_sealed, "ordered_forest_sealed" );
        std::array<unsigned char, 4096> row{};
        Header header;
        header.flags = RootFlag;
        std::memcpy( row.data(), &header, sizeof( header ) );
        const auto handle = ToHandle( m_rows.Append( row.data() ) );
        ++m_roots;
        return handle;
    }

    uint64_t Append( uint64_t parent, const void* payload )
    {
        Require( !m_sealed && payload, "ordered_forest_append_state" );
        auto parentRow = ReadRow( parent );
        auto parentHeader = HeaderOf( parentRow );

        std::array<unsigned char, 4096> row{};
        Header header;
        header.parent = parent;
        std::memcpy( row.data(), &header, sizeof( header ) );
        std::memcpy( row.data() + sizeof( Header ), payload, m_payloadBytes );
        const auto handle = ToHandle( m_rows.Append( row.data() ) );

        if( parentHeader.last )
        {
            auto sibling = ReadRow( parentHeader.last );
            auto siblingHeader = HeaderOf( sibling );
            Require( siblingHeader.next == 0 && siblingHeader.parent == parent, "ordered_forest_sibling" );
            siblingHeader.next = handle;
            SetHeader( sibling, siblingHeader );
            WriteRow( parentHeader.last, sibling );
        }
        else
        {
            Require( parentHeader.first == 0, "ordered_forest_first_child" );
            parentHeader.first = handle;
            ++m_branches;
        }
        parentHeader.last = handle;
        Require( parentHeader.children < UINT32_MAX, "ordered_forest_child_count" );
        ++parentHeader.children;
        SetHeader( parentRow, parentHeader );
        WriteRow( parent, parentRow );
        ++m_count;
        return handle;
    }

    void Read( uint64_t handle, void* payload )
    {
        Require( payload, "ordered_forest_read" );
        const auto row = ReadRow( handle );
        const auto header = HeaderOf( row );
        Require( ( header.flags & RootFlag ) == 0, "ordered_forest_root_payload" );
        std::memcpy( payload, row.data() + sizeof( Header ), m_payloadBytes );
    }

    void Put( uint64_t handle, const void* payload )
    {
        Require( !m_sealed && payload, "ordered_forest_update_state" );
        auto row = ReadRow( handle );
        const auto header = HeaderOf( row );
        Require( ( header.flags & RootFlag ) == 0, "ordered_forest_root_payload" );
        std::memcpy( row.data() + sizeof( Header ), payload, m_payloadBytes );
        WriteRow( handle, row );
    }

    uint32_t Children( uint64_t handle )
    {
        return HeaderOf( ReadRow( handle ) ).children;
    }

    void Seal()
    {
        Require( !m_sealed, "ordered_forest_already_sealed" );
        m_rows.Seal();
        m_sealed = true;
    }

    template<class Visitor>
    void Visit( uint64_t root, Visitor&& visitor )
    {
        Require( m_sealed, "ordered_forest_not_sealed" );
        auto rootRow = ReadRow( root );
        const auto rootHeader = HeaderOf( rootRow );
        Require( ( rootHeader.flags & RootFlag ) != 0, "ordered_forest_visit_root" );
        uint64_t current = rootHeader.first;
        while( current )
        {
            auto row = ReadRow( current );
            auto header = HeaderOf( row );
            visitor( current, row.data() + sizeof( Header ), header.children, true );
            if( header.first )
            {
                current = header.first;
                continue;
            }

            for( ;; )
            {
                visitor( current, row.data() + sizeof( Header ), header.children, false );
                if( header.next )
                {
                    current = header.next;
                    break;
                }
                if( header.parent == root )
                {
                    current = 0;
                    break;
                }
                current = header.parent;
                row = ReadRow( current );
                header = HeaderOf( row );
            }
        }
    }

    void ReduceCache() { m_rows.ReduceCache(); }
    uint64_t Count() const { return m_count; }
    uint64_t RootCount() const { return m_roots; }
    uint64_t BranchCount() const { return m_branches; }
    uint64_t LogicalBytes() const { return m_rows.LogicalBytes(); }
    bool HasDiskFile() const { return m_rows.HasDiskFile(); }

private:
    static void Require( bool value, const char* message )
    {
        if( !value ) throw std::runtime_error( message );
    }
    static uint64_t ToHandle( uint64_t row )
    {
        Require( row < UINT64_MAX, "ordered_forest_handle" );
        return row + 1;
    }
    std::array<unsigned char, 4096> ReadRow( uint64_t handle )
    {
        Require( handle > 0 && handle <= m_rows.Count(), "ordered_forest_handle" );
        std::array<unsigned char, 4096> row{};
        m_rows.Read( handle - 1, row.data() );
        return row;
    }
    void WriteRow( uint64_t handle, const std::array<unsigned char, 4096>& row )
    {
        Require( handle > 0 && handle <= m_rows.Count(), "ordered_forest_handle" );
        m_rows.Put( handle - 1, row.data() );
    }
    static Header HeaderOf( const std::array<unsigned char, 4096>& row )
    {
        Header header;
        std::memcpy( &header, row.data(), sizeof( header ) );
        return header;
    }
    static void SetHeader( std::array<unsigned char, 4096>& row, const Header& header )
    {
        std::memcpy( row.data(), &header, sizeof( header ) );
    }

    uint32_t m_payloadBytes;
    MutableRows m_rows;
    uint64_t m_count = 0;
    uint64_t m_roots = 0;
    uint64_t m_branches = 0;
    bool m_sealed = false;
};

}
#endif
