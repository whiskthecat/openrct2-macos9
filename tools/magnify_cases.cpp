// Independent pixel oracle for sparse RLE magnification, clipping and owner planes.
#include <openrct2/drawing/Drawing.Sprite.h>
#include <openrct2/drawing/RenderTarget.h>
#include <openrct2/drawing/SpriteRasterCache.h>
#include <openrct2/drawing/Colour.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;

static unsigned BitmapAndMaskCases()
{
    unsigned tests = 0;
    for (int w : {31, 301}) for (int scale : {2, 4})
    {
        const int h = 17, stride = w * 4 + 19, rows = 79;
        std::vector<uint8_t> source(w * h), colour((w + 5) * (h + 2));
        uint32_t random = 987654321;
        auto next = [&] { random = random * 1664525 + 1013904223; return random; };
        for (auto& pixel : source) pixel = next() % 4 ? next() % 256 : 0;
        for (auto& pixel : colour) pixel = next() % 256;
        G1Element images[2]{};
        images[0].offset = source.data(); images[0].width = w; images[0].height = h;
        images[1].offset = colour.data(); images[1].width = w + 5; images[1].height = h + 2;
        gPaintFastSpriteCatalogue = true; gGfxBaseElements = images; gGfxBaseElementCount = 2;
        std::vector<PaletteIndex> palette(256 * 255);
        for (size_t i = 0; i < palette.size(); ++i) palette[i] = static_cast<PaletteIndex>((i * 13 + i / 256 * 7) % 256);
        PaletteMap map(palette.data(), 255, 256);
        for (int masked = 0; masked < 2; ++masked) for (int mode = 0; mode < 5; ++mode)
        for (int plane = 0; plane < 4; ++plane) for (int clip = 0; clip < 19; ++clip)
        {
            if (masked && mode) continue;
            if (!masked && (plane >= 2 || (plane == 1 && mode >= 3))) continue;
            const int sx = clip ? (clip * 79) % (scale * w - 1) : 0, sy = clip % 5;
            const int width = clip ? std::min(scale * w - sx, 19 * clip) : scale * w;
            const int height = scale * h - sy - clip % 7;
            std::vector<PaletteIndex> actual(stride * rows), expect;
            std::vector<uint16_t> owners(actual.size()), eo, ranks(16);
            for (size_t i = 0; i < actual.size(); ++i) { actual[i] = static_cast<PaletteIndex>(next() % 256); owners[i] = next() % 16; }
            for (int i = 0; i < 16; ++i) ranks[i] = i * 7 % 16;
            expect = actual; eo = owners;
            ImageId id(0); if (mode == 2 || mode == 3) id = id.WithPrimary(Colour::brightRed);
            if (mode >= 3) id = id.WithBlended(true);
            images[0].flags = {}; if (mode) images[0].flags.set(G1Flag::hasTransparency);
            RenderTarget rt{}; rt.bits = actual.data() + stride * 3 + 5;
            rt.width = width; rt.height = height; rt.pitch = stride - width;
            rt.zoom_level = ZoomLevel{static_cast<int8_t>(scale == 2 ? -1 : -2)};
            rt.x = sx - 9 * scale; rt.y = sy - 7 * scale;
            gSpriteOwnerTarget = {}; gSpriteOcclusionTarget = {};
            if (plane == 1) gSpriteOwnerTarget = {actual.data(), owners.data(), 13};
            if (plane >= 2)
            {
                gSpriteOcclusionTarget.target = rt.bits; gSpriteOcclusionTarget.owners = owners.data() + stride * 3 + 5;
                gSpriteOcclusionTarget.ownerStride = stride; gSpriteOcclusionTarget.mapIndex = 7;
                if (plane == 3) gSpriteOcclusionTarget.ranks = ranks.data();
            }
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            {
                const size_t at = (y + 3) * stride + x + 5;
                const int sourceX = (sx + x) / scale, sourceY = (sy + y) / scale;
                uint8_t pixel = source[sourceY * w + sourceX];
                if (masked)
                {
                    pixel &= colour[sourceY * (w + 5) + sourceX];
                    if (!pixel || (plane >= 2 && (plane == 3 ? ranks[eo[at]] : eo[at]) > 7)) continue;
                }
                else
                {
                    if (mode && !pixel) continue;
                    if (mode == 3) pixel = static_cast<uint8_t>(palette[(pixel - 1) * 256 + static_cast<uint8_t>(expect[at])]);
                    else if (mode == 4) pixel = static_cast<uint8_t>(palette[static_cast<uint8_t>(expect[at])]);
                    else if (mode == 2) pixel = static_cast<uint8_t>(palette[pixel]);
                    if (mode && !pixel) continue;
                }
                expect[at] = static_cast<PaletteIndex>(pixel); if (plane == 1) eo[at] = 13;
            }
            if (masked) GfxDrawSpriteRawMaskedSoftware(rt, {-9, -7}, ImageId(0), ImageId(1));
            else { DrawSpriteArgs args(id, map, images[0], sx, sy, width, height, rt.bits); GfxSpriteToBuffer(rt, args); }
            if (actual != expect || owners != eo)
            {
                std::fprintf(stderr, "FAIL bitmap/mask scale=%d mask=%d mode=%d plane=%d clip=%d\n", scale, masked, mode, plane, clip);
                throw std::runtime_error("bitmap/mask oracle");
            }
            ++tests;
        }
    }
    gGfxBaseElements = nullptr; gGfxBaseElementCount = 0;
    return tests;
}

int main()
{
    unsigned tests=0;
    for (int w : {31,301})
    {
    const int h=17,stride=w*4+13,rows=79;
    std::vector<uint8_t> encoded(h*2),source(w*h);
    uint32_t random=123456789;
    auto next=[&] { random=random*1664525+1013904223; return random; };
    for(int y=0;y<h;y++)
    {
        const auto offset=encoded.size();encoded[y*2]=offset;encoded[y*2+1]=offset>>8;
        // Include zero data within runs, empty rows, one-pixel runs and gaps.
        for(int block=0;block<3;block++)
        {
            const int step=w==31?11:127;
            const int x=block*step,n=block==2?w-2*step:(w==31?7:126);
            encoded.push_back(n|(block==2?128:0));encoded.push_back(x);
            for(int i=0;i<n;i++)
            {
                const uint8_t value=(y%5==0||i%4==0)?0:1+next()%254;
                encoded.push_back(value);source[y*w+x+i]=value;
            }
        }
    }
    G1Element image;image.offset=encoded.data();image.width=w;image.height=h;image.flags={G1Flag::hasRLECompression};
    std::vector<PaletteIndex> palette(256*255);
    for(size_t i=0;i<palette.size();i++)palette[i]=static_cast<PaletteIndex>((i*13+i/256*7)%256);
    PaletteMap map(palette.data(),255,256);
    for(int scale:{2,4})for(int mode=0;mode<4;mode++)for(int plane=0;plane<6;plane++)
    for(int clip=0;clip<19;clip++)for(int uniform=0;uniform<2;uniform++)
    {
        const int sx=clip?(clip*79)%(scale*w-1):0,sy=clip%5;
        const int width=clip?std::min(scale*w-sx,19*clip):scale*w,height=scale*h-sy-clip%7;
        std::vector<PaletteIndex> actual(stride*rows),expect(actual.size());
        std::vector<uint16_t> owners(actual.size()),coverage(actual.size()),eo,ec,ranks(16);
        for(size_t i=0;i<actual.size();i++) {actual[i]=static_cast<PaletteIndex>(next()%256);owners[i]=next()%16;coverage[i]=next()%8;}
        for(int i=0;i<16;i++)ranks[i]=(i*7)%16;
        if (uniform)
        {
            std::fill(actual.begin(), actual.end(), static_cast<PaletteIndex>(clip % 2 ? 128 : 0));
            std::fill(owners.begin(), owners.end(), clip % 4 * 3);
            std::fill(coverage.begin(), coverage.end(), clip % 4 * 3);
        }
        expect=actual;eo=owners;ec=coverage;
        ImageId id(0);if(mode&1)id=id.WithPrimary(Colour::brightRed);if(mode&2)id=id.WithBlended(true);
        // Owned rendering is opaque; blend-only follows the public occlusion route.
        if(plane==1 && (mode&2))continue;
        if(plane>=2 && ((plane==5)!=(bool)(mode&2)))continue;
        RenderTarget rt{};rt.bits=actual.data();rt.width=stride-5;rt.height=rows;rt.pitch=5;rt.zoom_level=ZoomLevel{static_cast<int8_t>(scale==2?-1:-2)};
        auto* dst=actual.data()+stride*3+5;
        DrawSpriteArgs args(id,map,image,sx,sy,width,height,dst);
        gSpriteOwnerTarget={};gSpriteOcclusionTarget={};
        if(plane==1)gSpriteOwnerTarget={actual.data(),owners.data(),13};
        if(plane>=2)
        {
            gSpriteOcclusionTarget.target=actual.data();gSpriteOcclusionTarget.owners=owners.data();
            gSpriteOcclusionTarget.ownerStride=stride;gSpriteOcclusionTarget.mapIndex=7;
            if(plane==3)gSpriteOcclusionTarget.ranks=ranks.data();
            if(plane>=4){gSpriteOcclusionTarget.coverage=coverage.data();gSpriteOcclusionTarget.coverageStride=stride;}
            if(plane==5)gSpriteOcclusionTarget.blendOnly=true;
        }
        for(int y=0;y<height;y++)for(int x=0;x<width;x++)
        {
            const size_t at=(y+3)*stride+x+5;const int pixel=source[(sy+y)/scale*w+(sx+x)/scale];
            if(!pixel)continue;
            if(plane>=2)
            {
                const auto owner=plane==5?ec[at]:eo[at];
                if(plane==5 ? owner==0||owner>7 : (plane==3?ranks[owner]:owner)>7)continue;
            }
            uint8_t value=pixel;
            if((mode&3)==3)value=static_cast<uint8_t>(palette[(pixel-1)*256+static_cast<uint8_t>(expect[at])]);
            else if(mode&2)value=static_cast<uint8_t>(palette[static_cast<uint8_t>(expect[at])]);
            else if(mode&1)value=static_cast<uint8_t>(palette[pixel]);
            if(!value)continue;
            expect[at]=static_cast<PaletteIndex>(value);
            if(plane==1)eo[at]=13;
            if(plane==4)ec[at]=8;
        }
        GfxSpriteToBuffer(rt,args);
        if(actual!=expect||owners!=eo||coverage!=ec)
        {
            std::fprintf(stderr,"FAIL scale=%d mode=%d plane=%d clip=%d\n",scale,mode,plane,clip);return 1;
        }
        tests++;
    }
    }
    const unsigned bitmapTests = BitmapAndMaskCases();
    gSpriteOwnerTarget={};gSpriteOcclusionTarget={};
    std::printf("PASS %u bitmap/mask cases: 2x/4x, opaque zero, remap/blend, mismatched strides, clipped blocks, owners/ranks\n", bitmapTests);
    std::printf("PASS %u sparse RLE cases: 2x/4x, clipped blocks, zero colours, remap/blend, owners/ranks/coverage\n",tests);
}
