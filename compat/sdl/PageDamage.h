#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace PageMirror
{
    struct Region { int left, top, right, bottom; };
    // A page's bits describe differences from the latest completed back page.
    // Writes dirty the other pages; a copy carries the source's remaining bits.
    class Damage
    {
        static constexpr int tile = 16, maxColumns = 128, maxRows = 96, words = maxColumns / 32;
        using Row = std::array<uint32_t, words>;
        using Plane = std::array<Row, maxRows>;
        std::array<Plane, 3> stale{};
        int width{}, height{}, columns{}, rows{};
    public:
        bool enabled{};
        void Reset(int w, int h,unsigned source=0)
        {
            width=w; height=h; columns=(w+tile-1)/tile; rows=(h+tile-1)/tile;
            enabled=w>0 && h>0 && columns<=maxColumns && rows<=maxRows;
            stale={};
            if (enabled) Write(source,{0,0,w,h});
        }
        void Write(unsigned page, Region rect)
        {
            if (!enabled || page>=3) return;
            Mark(7u^(1u<<page),rect);
        }
        void Dirty(unsigned page,Region rect) { if (enabled && page<3) Mark(1u<<page,rect); }
        void Mark(unsigned pages,Region rect)
        {
            if (rect.right<=rect.left || rect.bottom<=rect.top) return;
            const int left=std::clamp(rect.left,0,width)/tile;
            const int right=(std::clamp(rect.right,0,width)+tile-1)/tile;
            const int top=std::clamp(rect.top,0,height)/tile;
            const int bottom=(std::clamp(rect.bottom,0,height)+tile-1)/tile;
            if (right<=left || bottom<=top) return;
            for (int y=top;y<bottom;++y)
                for (int w=left/32;w<=(right-1)/32;++w)
                {
                    const unsigned lo=std::max(left-w*32,0), hi=std::min(right-w*32,32);
                    const uint32_t mask=(~uint32_t{0}<<lo)&(~uint32_t{0}>>(32-hi));
                    for (unsigned p=0;p<3;++p) if (pages&(1u<<p)) stale[p][y][w]|=mask;
                }
        }
        void Copied(unsigned from,unsigned to) { if (enabled) stale[to]=stale[from]; }
        // One horizontal bound per tile row; equal bounds coalesce vertically.
        // Copying clean gaps is safe and avoids a command per small dirty run.
        unsigned Regions(unsigned page,std::array<Region,maxRows>& result,uint64_t& pixels) const
        {
            unsigned count=0; pixels=0;
            for (int y=0;y<rows;++y)
            {
                int left=columns,right=0;
                for (int w=0;w<words;++w) if (stale[page][y][w])
                {
                    const auto bits=stale[page][y][w];
                    left=std::min(left,w*32+std::countr_zero(bits));
                    right=std::max(right,w*32+32-std::countl_zero(bits));
                }
                if (right<=left) continue;
                Region r{left*tile,y*tile,std::min(right*tile,width),std::min((y+1)*tile,height)};
                pixels+=uint64_t(r.right-r.left)*(r.bottom-r.top);
                if (count && result[count-1].left==r.left && result[count-1].right==r.right && result[count-1].bottom==r.top)
                    result[count-1].bottom=r.bottom;
                else result[count++]=r;
            }
            return count;
        }
        using RegionsBuffer = std::array<Region,maxRows>;
    };
}
