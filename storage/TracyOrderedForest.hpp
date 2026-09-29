#ifndef TRACY_ORDERED_FOREST_HPP
#define TRACY_ORDERED_FOREST_HPP

#include "TracyMutableRows.hpp"
#include <algorithm>
#include <vector>

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
        , m_scratchLease( resources.Workspace(), uint64_t( sizeof( Header ) ) + payloadBytes + 256 )
        , m_row( sizeof( Header ) + payloadBytes )
        , m_rows( std::move( path ), uint32_t( sizeof( Header ) + payloadBytes ), resources, cacheBytes )
    {
        Require( payloadBytes > 0 && payloadBytes <= MaxPayload, "ordered_forest_configuration" );
    }

    uint64_t CreateRoot()
    {
        Require( !m_sealed, "ordered_forest_sealed" );
        ClearRow();Header header;
        header.flags = RootFlag;
        std::memcpy( m_row.data(), &header, sizeof( header ) );
        const auto handle = ToHandle( m_rows.Append( m_row.data() ) );
        ++m_roots;
        return handle;
    }

    uint64_t Append( uint64_t parent, const void* payload )
    {
        Require( !m_sealed && payload, "ordered_forest_append_state" );
        auto parentHeader = ReadHeader( parent );

        ClearRow();Header header;
        header.parent = parent;
        std::memcpy( m_row.data(), &header, sizeof( header ) );
        std::memcpy( m_row.data() + sizeof( Header ), payload, m_payloadBytes );
        const auto handle = ToHandle( m_rows.Append( m_row.data() ) );

        if( parentHeader.last )
        {
            auto siblingHeader = ReadHeader( parentHeader.last );
            Require( siblingHeader.next == 0 && siblingHeader.parent == parent, "ordered_forest_sibling" );
            siblingHeader.next = handle;
            WriteHeader( parentHeader.last, siblingHeader );
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
        WriteHeader( parent, parentHeader );
        ++m_count;
        return handle;
    }

    void Read( uint64_t handle, void* payload )
    {
        Require( payload, "ordered_forest_read" );
        LoadRow( handle );const auto header = HeaderOfRow();
        Require( ( header.flags & RootFlag ) == 0, "ordered_forest_root_payload" );
        std::memcpy( payload, m_row.data() + sizeof( Header ), m_payloadBytes );
    }

    void Put( uint64_t handle, const void* payload )
    {
        Require( !m_sealed && payload, "ordered_forest_update_state" );
        LoadRow( handle );const auto header = HeaderOfRow();
        Require( ( header.flags & RootFlag ) == 0, "ordered_forest_root_payload" );
        std::memcpy( m_row.data() + sizeof( Header ), payload, m_payloadBytes );
        m_rows.Put( handle - 1, m_row.data() );
    }

    uint32_t Children( uint64_t handle )
    {
        return ReadHeader( handle ).children;
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
        const auto rootHeader = ReadHeader( root );
        Require( ( rootHeader.flags & RootFlag ) != 0, "ordered_forest_visit_root" );
        uint64_t current = rootHeader.first;
        while( current )
        {
            LoadRow( current );auto header = HeaderOfRow();
            visitor( current, m_row.data() + sizeof( Header ), header.children, true );
            if( header.first )
            {
                current = header.first;
                continue;
            }

            for( ;; )
            {
                LoadRow( current );header=HeaderOfRow();visitor( current, m_row.data() + sizeof( Header ), header.children, false );
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
                header = ReadHeader( current );
            }
        }
    }

    void ReduceCache() { m_rows.ReduceCache(); }
    uint64_t Count() const { return m_count; }
    uint64_t RootCount() const { return m_roots; }
    uint64_t BranchCount() const { return m_branches; }
    uint64_t LogicalBytes() const { return m_rows.LogicalBytes(); }
    bool HasDiskFile() const { return m_rows.HasDiskFile(); }
    MutableRows::Stats Statistics() const { return m_rows.Statistics(); }

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
    void LoadRow( uint64_t handle )
    {
        Require( handle > 0 && handle <= m_rows.Count(), "ordered_forest_handle" );
        m_rows.Read( handle - 1, m_row.data() );
    }
    Header ReadHeader( uint64_t handle )
    {
        LoadRow(handle);return HeaderOfRow();
    }
    void WriteHeader( uint64_t handle, const Header& header )
    {
        LoadRow(handle);std::memcpy(m_row.data(),&header,sizeof(header));m_rows.Put(handle-1,m_row.data());
    }
    Header HeaderOfRow() const
    {
        Header header;std::memcpy(&header,m_row.data(),sizeof(header));return header;
    }
    void ClearRow(){std::fill(m_row.begin(),m_row.end(),0);}

    uint32_t m_payloadBytes;
    analysis::AnalysisWorkspaceReservation m_scratchLease;
    std::vector<unsigned char> m_row;
    MutableRows m_rows;
    uint64_t m_count = 0;
    uint64_t m_roots = 0;
    uint64_t m_branches = 0;
    bool m_sealed = false;
};

}
#endif
