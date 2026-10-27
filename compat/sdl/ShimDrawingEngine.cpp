#include <openrct2/core/OgTrace.h>
#include "WeatherRaster.h"
#include <openrct2/paint/PerfMetrics.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/entity/EntityBase.h>
// The display side of the UI: the engine's own 8-bit software renderer, with
// repainted rectangles handed to ShimHost. Viewport scrolling is offered to
// the host first so it can move pixels itself (the ATI engine on the Mac).
#include "ShimHost.h"
#include <openrct2/core/FrameTiming.h>

#include <algorithm>
#ifdef OPENRCT2_PAK_VFS
    #include "ClassicAudio.h"
    #include "ClassicAutosave.h"
    #include "ClassicDiskIo.h"
#endif
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <openrct2-ui/UiContext.h>
#include <openrct2/GameState.h>
#include <openrct2/Context.h>
#include <openrct2-ui/drawing/engines/DrawingEngineFactory.hpp>
#include <openrct2/drawing/Drawing.Screen.h>
#include <openrct2/drawing/Drawing.h>
#include <openrct2/drawing/NewDrawing.h>
#include <openrct2/interface/Screenshot.h>
#include <openrct2/drawing/WeatherDrawer.h>
#include <openrct2/drawing/X8DrawingEngine.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/paint/Paint.Cache.h>
#include <limits>
#include <cmath>
#include <SDL.h>
#include <openrct2/config/Config.h>
#include <openrct2/ui/UiContext.h>

using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;
using namespace OpenRCT2::Ui;

#ifdef OPENRCT2_REFERENCE_BLITTERS
extern void (*gPaintPaletteFilterTraceHook)(const RenderTarget&, const PaletteMap&, int32_t, int32_t, int32_t, int32_t);
extern void (*gPaintSpriteBlendTraceHook)(PaletteIndex*, const PaletteMap&, int, bool);
#endif

namespace
{
#ifdef OPENRCT2_REFERENCE_BLITTERS
    struct PixelTrace
    {
        int x, y;
        uint8_t* pixel = nullptr;
        uint8_t before = 0;
        bool inside = false;
        std::array<uint8_t, 32> colours{};
        // Glass can cover an item that appears later in the reference list.
        // Retain its operation so later opaque colours also have provenance
        // when an ambiguous order puts that item behind the glass.
        std::vector<std::array<uint8_t, 256>> blends;
        bool blended = false;
        std::vector<std::array<int64_t, 7>> objects;
        uint8_t* lensSavedPixel = nullptr;
    };
    std::vector<PixelTrace> _pixelTrace;

    void TraceOpaqueColour(PixelTrace& point, uint8_t colour)
    {
        std::array<uint8_t, 32> covered{};
        covered[colour / 8] |= 1u << (colour % 8);
        // Each preceding glass item is used at most once. A covered opaque
        // item may be before or after it; preserve both choices.
        for (auto blend = point.blends.rbegin(); blend != point.blends.rend(); ++blend)
        {
            const auto prior = covered;
            for (unsigned candidate = 0; candidate < 256; candidate++)
                if ((prior[candidate / 8] & (1u << (candidate % 8))) != 0)
                {
                    const unsigned mapped = (*blend)[candidate];
                    covered[mapped / 8] |= 1u << (mapped % 8);
                }
        }
        for (size_t i = 0; i < covered.size(); i++) point.colours[i] |= covered[i];
    }

    void TraceLens(const RenderTarget& target, const ScreenCoordsXY& origin,
        const ScreenCoordsXY& centre, int32_t radius, bool before)
    {
        for (auto& point : _pixelTrace)
        {
            if (!before)
            {
                point.pixel = point.lensSavedPixel;
                continue;
            }
            point.lensSavedPixel = point.pixel;
            point.pixel = nullptr;
            const int x = static_cast<int>(point.x / Config::Get().general.windowScale);
            const int y = static_cast<int>(point.y / Config::Get().general.windowScale);
            if (x < origin.x || y < origin.y || x >= origin.x + target.width || y >= origin.y + target.height
                || !ViewportLensContainsPixel({x, y}, centre, radius, Config::Get().interface.ditheredLensEdge))
                continue;
            point.pixel = reinterpret_cast<uint8_t*>(target.bits) + (y - origin.y) * target.LineStride() + x - origin.x;
            // The lens replaces normal-view colours in this circle. Classify
            // only current lens writes, including their real glass operations.
            point.colours = {};
            point.colours[*point.pixel / 8] |= 1u << (*point.pixel % 8);
            point.blends.clear();
            point.blended = false;
            point.objects.clear();
        }
    }

    void TraceSpriteBlend(PaletteIndex* destination, const PaletteMap& palette, int source, bool transparent)
    {
        for (auto& point : _pixelTrace)
        {
            if (point.pixel != reinterpret_cast<uint8_t*>(destination)) continue;
            std::array<uint8_t, 256> operation{};
            std::array<uint8_t, 32> filtered{};
            for (unsigned colour = 0; colour < 256; colour++)
                {
                    const auto mapped = static_cast<uint8_t>(source < 0 ? palette[colour]
                        : palette.Blend(static_cast<PaletteIndex>(source), static_cast<PaletteIndex>(colour)));
                    const unsigned visible = transparent && mapped == 0 ? colour : mapped;
                    operation[colour] = visible;
                    if ((point.colours[colour / 8] & (1u << (colour % 8))) != 0)
                        filtered[visible / 8] |= 1u << (visible % 8);
                }
            // Earlier paint-list items also draw here. Keep their colours and
            // add the colours a covered glass/water sprite can blend from them.
            for (size_t i = 0; i < filtered.size(); i++) point.colours[i] |= filtered[i];
            point.blends.push_back(operation);
            point.blended = true;
        }
    }

    void TracePaletteFilter(const RenderTarget& rt, const PaletteMap& palette,
        int32_t left, int32_t top, int32_t width, int32_t height)
    {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(rt.bits);
        const size_t stride = rt.LineStride();
        for (auto& point : _pixelTrace)
        {
            const uintptr_t address = reinterpret_cast<uintptr_t>(point.pixel);
            if (address < begin) continue;
            const size_t y = (address - begin) / stride, x = (address - begin) % stride;
            if (x < static_cast<size_t>(left) || x >= static_cast<size_t>(left + width)
                || y < static_cast<size_t>(top) || y >= static_cast<size_t>(top + height)) continue;
            std::array<uint8_t, 32> filtered{};
            for (unsigned colour = 0; colour < 256; colour++)
                if ((point.colours[colour / 8] & (1u << (colour % 8))) != 0)
                {
                    const unsigned mapped = static_cast<uint8_t>(palette[colour]);
                    filtered[mapped / 8] |= 1u << (mapped % 8);
                }
            point.colours = filtered;
            // A viewport-wide filter applies after all its sprites; later
            // window colours must not acquire that viewport's glass layers.
            point.blends.clear();
        }
    }

    void TracePaintPixel(const RenderTarget& rt, bool before)
    {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(rt.bits);
        const size_t stride = rt.LineStride();
        const uintptr_t end = begin + stride * rt.height;
        for (auto& point : _pixelTrace)
        {
            if (before)
            {
                const uintptr_t address = reinterpret_cast<uintptr_t>(point.pixel);
                point.inside = address >= begin && address < end && (address - begin) % stride < static_cast<size_t>(rt.width);
                if (point.inside)
                {
                    point.before = *point.pixel;
                    point.blended = false;
                }
            }
            else if (point.inside && point.before != *point.pixel)
            {
                const uint8_t colour = *point.pixel;
                const auto* ps = gPaintTraceStruct;
                point.objects.push_back({point.before, colour, ps ? static_cast<int>(ps->InteractionItem) : -1,
                    ps && ps->InteractionItem == ViewportInteractionItem::entity && ps->Entity ? ps->Entity->id.ToUnderlying() : -1,
                    ps ? ps->MapPos.x : -1, ps ? ps->MapPos.y : -1, ps ? static_cast<int64_t>(ps->image_id.GetIndex()) : -1});
                if (point.blended)
                    point.colours[colour / 8] |= 1u << (colour % 8);
                else
                    TraceOpaqueColour(point, colour);
            }
        }
    }
#endif
    // Tracks what changed in the frame and replays it on the screen at the
    // end: first the viewport scrolls (block moves the video card can do),
    // then every repainted rectangle. A rectangle repainted before a scroll
    // is also sent at its moved position, so the order within the frame does
    // not matter and nothing half-drawn is ever shown.
    class ShimDrawingEngine final : public X8DrawingEngine
    {
        struct Rect
        {
            int left, top, right, bottom;
        };
        struct Move
        {
            int x, y, width, height, dx, dy;
            int blockLeft, blockTop, blockRight, blockBottom; // the block the engine asked to shift
        };
        static constexpr int kMaxRects = 256, kMaxMoves = 32;
    public:
        struct LastFrame
        {
            bool all, lazy, stale;
            Rect readback;
            bool recovering;
            int rectCount, moveCount;
            Rect rects[kMaxRects];
            Move moves[kMaxMoves];
        };
        static inline LastFrame gLastFrame{};

    private:
        float _scale = 1;
        int _physicalWidth = 0, _physicalHeight = 0;
        std::vector<uint8_t> _scaledBits;
        std::vector<int> _sourceX, _sourceY;
        Rect _pointerRect{};
        bool _pointerDrawn = false;
        bool _lensWasOn = false;
        int _pointerMouseX = 0, _pointerMouseY = 0, _pointerHotX = 0, _pointerHotY = 0;
        uint16_t _pointerData[16]{}, _pointerMask[16]{};
        uint8_t _pointerBlack = 0, _pointerWhite = 255;
        bool PointerNeedsRestore()
        {
            if (!_pointerDrawn) return false;
            uint16_t data[16], mask[16]; int hotX, hotY, mx, my;
            if (!ShimHost::SoftwarePointer(data, mask, hotX, hotY)) return true;
            SDL_GetMouseState(&mx, &my);
            if (mx != _pointerMouseX || my != _pointerMouseY || hotX != _pointerHotX || hotY != _pointerHotY
                || _black != _pointerBlack || _white != _pointerWhite
                || std::memcmp(data, _pointerData, sizeof(data)) || std::memcmp(mask, _pointerMask, sizeof(mask))) return true;
            auto touches = [&](int l, int t, int r, int b) {
                return Edge(l) < _pointerRect.right && Edge(r) > _pointerRect.left
                    && Edge(t) < _pointerRect.bottom && Edge(b) > _pointerRect.top;
            };
            if (_all || gPaintForceRedraw) return true;
            for (int i = 0; i < _rectCount; ++i)
            {
                const auto& r = _rects[i];
                const int l = _wordRects && !_memoryStale ? r.left & ~7 : r.left;
                const int right = _wordRects && !_memoryStale ? std::min<int>((r.right + 7) & ~7, _width) : r.right;
                if (touches(l, r.top, right, r.bottom)) return true;
            }
            for (int i = 0; i < _moveCount; ++i)
            {
                const auto& m = _moves[i];
                if (touches(m.x, m.y, m.x + m.width, m.y + m.height)
                    || touches(m.x - m.dx, m.y - m.dy, m.x - m.dx + m.width, m.y - m.dy + m.height)) return true;
            }
            return false;
        }
        uint8_t _black = 0, _white = 255;

        int Edge(int logical) const { return static_cast<int>(std::ceil(logical * _scale)); }

        void PresentRect(const uint8_t* bits, int left, int top, int right, int bottom)
        {
            if (_scale == 1)
            {
                ShimHost::Present(bits, _pitch, left, top, right, bottom);
                return;
            }
            left = std::min(Edge(left), _physicalWidth);
            right = std::min(Edge(right), _physicalWidth);
            top = std::min(Edge(top), _physicalHeight);
            bottom = std::min(Edge(bottom), _physicalHeight);
            for (int y = top; y < bottom; y++)
            {
                auto* to = _scaledBits.data() + y * _physicalWidth;
                const auto* from = bits + _sourceY[y] * _pitch;
                for (int x = left; x < right; x++) to[x] = from[_sourceX[x]];
            }
            ShimHost::Present(_scaledBits.data(), _physicalWidth, left, top, right, bottom);
        }

        bool ScrollDisplay(int x, int y, int width, int height, int dx, int dy)
        {
            struct Timer
            {
                uint64_t start = ShimHost::Microseconds();
                ~Timer() { ShimHost::gScreenScrollMicros += ShimHost::Microseconds() - start; }
            } timer;
            if (_scale == 1) return ShimHost::Scroll(x, y, width, height, dx, dy);
            // A translation preserves nearest-neighbour sampling only at integral display deltas.
            const float sx = dx * _scale, sy = dy * _scale;
            if (sx != std::floor(sx) || sy != std::floor(sy)) return false;
            const int left = std::min(Edge(x), _physicalWidth), top = std::min(Edge(y), _physicalHeight);
            const int right = std::min(Edge(x + width), _physicalWidth), bottom = std::min(Edge(y + height), _physicalHeight);
            const int px = sx, py = sy;
            if (!ShimHost::Scroll(left, top, right - left, bottom - top, px, py)) return false;
            // Keep the clean display shadow current for software-pointer restoration.
            for (int i = 0; i < bottom - top; i++)
            {
                const int row = py > 0 ? bottom - 1 - i : top + i;
                MoveRow(_scaledBits.data() + row * _physicalWidth + left,
                        _scaledBits.data() + (row - py) * _physicalWidth + left - px, right - left);
            }
            return true;
        }

        void DrawPointer()
        {
            if (_pointerDrawn) return;
            uint16_t data[16], mask[16];
            int hotX, hotY, mx, my;
            if (!ShimHost::SoftwarePointer(data, mask, hotX, hotY)) return;
            SDL_GetMouseState(&mx, &my);
            _pointerMouseX = mx; _pointerMouseY = my; _pointerHotX = hotX; _pointerHotY = hotY;
            _pointerBlack = _black; _pointerWhite = _white;
            std::memcpy(_pointerData, data, sizeof(data)); std::memcpy(_pointerMask, mask, sizeof(mask));
            const int x = mx - Edge(hotX), y = my - Edge(hotY);
            const int size = Edge(16);
            _pointerRect = { std::max(0, x), std::max(0, y), std::min(_physicalWidth, x + size),
                             std::min(_physicalHeight, y + size) };
            if (_pointerRect.left >= _pointerRect.right || _pointerRect.top >= _pointerRect.bottom) return;
            ShimHost::gPointerPixels += static_cast<uint64_t>(_pointerRect.right - _pointerRect.left)
                * (_pointerRect.bottom - _pointerRect.top);
            std::vector<uint8_t> saved;
            for (int py = _pointerRect.top; py < _pointerRect.bottom; py++)
                for (int px = _pointerRect.left; px < _pointerRect.right; px++)
                {
                    auto& pixel = _scaledBits[py * _physicalWidth + px];
                    saved.push_back(pixel);
                    const int sx = static_cast<int>((px - x) / _scale), sy = static_cast<int>((py - y) / _scale);
                    const uint16_t bit = 0x8000u >> sx;
                    if (mask[sy] & bit) pixel = data[sy] & bit ? _black : _white;
                }
            ShimHost::Present(_scaledBits.data(), _physicalWidth, _pointerRect.left, _pointerRect.top,
                              _pointerRect.right, _pointerRect.bottom);
            size_t i = 0;
            for (int py = _pointerRect.top; py < _pointerRect.bottom; py++)
                for (int px = _pointerRect.left; px < _pointerRect.right; px++)
                    _scaledBits[py * _physicalWidth + px] = saved[i++];
            _pointerDrawn = true;
        }

        Rect _rects[kMaxRects]{};
        Move _moves[kMaxMoves]{};
        int _rectCount = 0, _moveCount = 0;
        bool _all = true;
        static inline ShimDrawingEngine* _instance = nullptr;

        void Add(int left, int top, int right, int bottom)
        {
            if (_all)
                return;
            left = std::max(left, 0);
            top = std::max(top, 0);
            right = std::min(right, static_cast<int>(_width));
            bottom = std::min(bottom, static_cast<int>(_height));
            if (left >= right || top >= bottom)
                return;
            for (int i = 0; i < _rectCount; i++)
                if (_rects[i].left <= left && _rects[i].top <= top && _rects[i].right >= right && _rects[i].bottom >= bottom)
                    return;
            if (_rectCount == kMaxRects)
            {
                _all = true;
                return;
            }
            _rects[_rectCount++] = { left, top, right, bottom };
        }

        // memmove for one row of pixels, four bytes at a time in the direction that is safe.
        static void MoveRow(uint8_t* to, const uint8_t* from, int32_t count)
        {
            typedef uint32_t __attribute__((may_alias, aligned(1))) Word;
            if (to == from)
                return;
            if (to < from || to >= from + count)
            {
                for (; count >= 4; count -= 4, to += 4, from += 4)
                    *reinterpret_cast<Word*>(to) = *reinterpret_cast<const Word*>(from);
                for (; count > 0; count--)
                    *to++ = *from++;
            }
            else if (to - from >= 4)
            {
                to += count;
                from += count;
                for (; count >= 4; count -= 4)
                {
                    to -= 4;
                    from -= 4;
                    *reinterpret_cast<Word*>(to) = *reinterpret_cast<const Word*>(from);
                }
                for (; count > 0; count--)
                    *--to = *--from;
            }
            else
            {
                to += count;
                from += count;
                for (; count > 0; count--)
                    *--to = *--from;
            }
        }

        static void OnRepaint(RenderTarget& rt, int32_t left, int32_t top, int32_t right, int32_t bottom)
        {
            if (_instance != nullptr && rt.bits == _instance->_bits)
            {
                ShimHost::CountRepaint(right - left, bottom - top);
                _instance->Add(left, top, right, bottom);
            }
        }

    public:
        explicit ShimDrawingEngine(IUiContext& uiContext)
            : X8DrawingEngine(uiContext)
        {
#ifdef OPENRCT2_CLASSIC_MAC
            gPaintHistoryFreeBytes = [] {return static_cast<uint64_t>(std::max<long>(0, ShimHost::FreeMemory(false)));};
#endif
            _instance = this;
            gWindowDrawAllHook = OnRepaint;
        }

        ~ShimDrawingEngine() override
        {
            gWindowDrawAllHook = nullptr;
            _instance = nullptr;
        }

#ifdef OPENRCT2_CLASSIC_MAC
        void SetVSync(bool vsync) override { ShimHost::SetVSync(vsync); }
        DrawingEngineFlags GetFlags() override
        {
            auto flags = X8DrawingEngine::GetFlags();
            if (ShimHost::VSyncAvailable()) flags.set(DrawingEngineFlag::verticalBlankSync);
            return flags;
        }
#endif

        void Resize(uint32_t width, uint32_t height) override
        {
            // Clear old display overlays before reallocating a scaled frame.
            if (_pointerDrawn && !_scaledBits.empty())
                ShimHost::Present(_scaledBits.data(), _physicalWidth, _pointerRect.left, _pointerRect.top,
                                  _pointerRect.right, _pointerRect.bottom);
            _pointerDrawn = false;
            _weatherSaved.count = 0;
            _weatherShown.clear();
            _memoryStale = false;
            _readbackActive = false;
            _readbackPending = false;
            _scale = Config::Get().general.windowScale;
            SDL_GetWindowSize(nullptr, &_physicalWidth, &_physicalHeight);
            _scaledBits.resize(_scale == 1 ? 0 : static_cast<size_t>(_physicalWidth) * _physicalHeight);
            _sourceX.resize(_scale == 1 ? 0 : _physicalWidth);
            _sourceY.resize(_scale == 1 ? 0 : _physicalHeight);
            for (int x = 0; x < static_cast<int>(_sourceX.size()); x++) _sourceX[x] = std::min<int>(x / _scale, width - 1);
            for (int y = 0; y < static_cast<int>(_sourceY.size()); y++) _sourceY[y] = std::min<int>(y / _scale, height - 1);
            X8DrawingEngine::Resize(width, height);
            if (_cellWidth != 0)
                _invalidationGrid.reset(static_cast<int32_t>(_width), static_cast<int32_t>(_height), _cellWidth, _cellHeight);
            _all = true;
        }

        // Size of the cells that changes are tracked in. Small cells repaint
        // less of the screen per moving sprite but cost more set-up.
        static inline uint32_t _cellWidth = 16, _cellHeight = 16; // measured best on the G3
        // Off: scrolled areas are copied from the frame like any other change (to measure what the card saves).
        static inline bool _hardwareScroll = true;
        static inline bool _alignCells = true;
        static inline bool _wordRects = true;
        static inline bool _idleRefresh = true;
        int _idleStaleFrames = 0;
        static void SetIdleRefresh(bool on) { _idleRefresh = on; }
        static void SetWordRectangles(bool on) { _wordRects = on; }
        static void SetAlignedCells(bool on) { _alignCells = on; }
        static inline bool _rowShiftOnly = false; // tests: scroll the frame row by row
        static inline bool _lazyScrollAllowed = true;
        bool _lazyThisFrame = false;
        bool _memoryStale = false;
        Rect _staleRect{}; // parts of the frame in memory no longer match the screen
        int _clockFrames = 0;
        std::vector<ShimHost::WeatherSpan> _weatherNew, _weatherShown;
        ShimHost::WeatherPixels _weatherSaved;
        bool _refreshPending = false; // paint and send the whole frame next time
        bool _readbackPending = false;
        bool _readbackActive = false;
        int32_t _readbackTop = 0;
        // Our copy of what the display holds, for presenting only changed pixels. Valid only
        // while memory and display agree (no pending hardware scroll, no software pointer).
        std::vector<uint8_t> _shown;
        bool _shownValid = false;
        ShimHost::Colour _palette[256]{};
        bool _palettePending = false;
        static inline bool _deferPalette = true;
        struct Carried
        {
            int32_t left, top, right, bottom;
            uint8_t kind;
        };
        std::vector<Carried> _carried;
        std::vector<Carried> _pendingDamage, _damageWork;
        std::vector<Rect> _scrollDamage;
        static void RecordScrollDamage(int32_t l, int32_t t, int32_t r, int32_t b)
        {
            _instance->_scrollDamage.push_back({l, t, r, b});
        }

        void AlignDamageGrid(bool bothAxes)
        {
            int32_t x = 0, y = 0;
            if (_alignCells && _cellWidth != 0)
                if (auto* main = WindowGetMain(); main != nullptr && main->viewport != nullptr)
                {
                    const auto* view = main->viewport;
                    const int32_t ox = view->zoom.ApplyInversedTo(view->viewPos.x) - view->pos.x;
                    const int32_t oy = view->zoom.ApplyInversedTo(view->viewPos.y) - view->pos.y;
                    const int32_t w = _cellWidth, h = _cellHeight;
                    x = -((ox % w + w) % w);
                    if (bothAxes) y = -((oy % h + h) % h);
                }
            _invalidationGrid.setOrigin(x, y);
        }

        void TranslateDamage(int movesBefore)
        {
            // Keep pending damage in world-aligned rectangles until the final grid is known.
            // Regridding first expands each guest cell, then carrying expands it a second time.
            _carried.clear();
            for (const auto& area : _pendingDamage)
                if (area.kind == InvalidationGrid::kGeneral)
                    _carried.push_back(area); // window/UI changes also remain at their screen position
            for (int m = movesBefore; m < _moveCount; m++)
            {
                const auto& move = _moves[m];
                const int32_t sl = std::max(move.x - move.dx, move.blockLeft);
                const int32_t st = std::max(move.y - move.dy, move.blockTop);
                const int32_t sr = std::min(move.x - move.dx + move.width, move.blockRight);
                const int32_t sb = std::min(move.y - move.dy + move.height, move.blockBottom);
                _damageWork.clear();
                for (const auto& area : _pendingDamage)
                {
                    const int32_t l = std::max(area.left, sl), t = std::max(area.top, st);
                    const int32_t r = std::min(area.right, sr), b = std::min(area.bottom, sb);
                    if (l >= r || t >= b)
                    {
                        _damageWork.push_back(area);
                        continue;
                    }
                    _carried.push_back({ l + move.dx, t + move.dy, r + move.dx, b + move.dy, area.kind });
                    // Only source pixels inside this scroll move. Leave every other piece dirty.
                    if (area.top < t) _damageWork.push_back({area.left, area.top, area.right, t, area.kind});
                    if (b < area.bottom) _damageWork.push_back({area.left, b, area.right, area.bottom, area.kind});
                    if (area.left < l) _damageWork.push_back({area.left, t, l, b, area.kind});
                    if (r < area.right) _damageWork.push_back({r, t, area.right, b, area.kind});
                }
                _pendingDamage.swap(_damageWork);
            }
            for (const auto& area : _pendingDamage)
                _carried.push_back(area);
            AlignDamageGrid(true);
            for (const auto& area : _carried)
                _invalidationGrid.invalidate(area.left, area.top, area.right, area.bottom, area.kind);
        }
        static void SetHardwareScroll(bool on)
        {
            _hardwareScroll = on;
        }
        static void SetRowShift(bool on)
        {
            _rowShiftOnly = on;
        }
        static void SetCells(uint32_t width, uint32_t height)
        {
            _cellWidth = width;
            _cellHeight = height;
            if (_instance != nullptr)
            {
                _instance->Resize(_instance->_width, _instance->_height);
                GfxInvalidateScreen();
            }
        }

        void SetPalette(const GamePalette& palette) override
        {
            for (int i = 0; i < 256; i++)
                _palette[i] = { palette[i].red, palette[i].green, palette[i].blue };
            if (_scale != 1)
            {
            _black = 0; _white = 0;
            const auto light = [&](int i) { return _palette[i].red * 3 + _palette[i].green * 6 + _palette[i].blue; };
            for (int i = 1; i < 256; i++)
            {
                if (light(i) < light(_black)) _black = i;
                if (light(i) > light(_white)) _white = i;
            }
            }
            _palettePending = true;
            if (!_deferPalette)
                FlushPalette();
        }

        void FlushPalette()
        {
            if (!_palettePending)
                return;
            const uint64_t start = ShimHost::Microseconds();
            ShimHost::SetPalette(_palette);
            ShimHost::CountPalette(ShimHost::Microseconds() - start);
            _palettePending = false;
        }

        static void SetDeferredPalette(bool on)
        {
            _deferPalette = on;
            if (!on && _instance != nullptr)
                _instance->FlushPalette();
        }

        // When the display can scroll by itself, the frame in memory is not moved along (3 ms per
        // scrolled frame on the G3): the screen stays the true picture and memory is only right
        // where something was repainted since. That needs a different order of work than the
        // engine's: scroll first, then carry the pending dirty cells along with the scroll, then
        // repaint. (The engine repaints first so that its memory move does not copy stale pixels.)
        bool LazyScroll() const
        {
            // Saving rain's old colours from an indexed video frame is slower than
            // moving the clean RAM picture on the G3. Keep the compact row kernel,
            // but retain RAM scrolling while rain/snow can be drawn on that display.
            const bool wetVideo = ShimHost::WeatherNeedsCleanCanvas() && Config::Get().general.renderWeatherEffects
                && getGameState().weatherCurrent.level != Weather::Level::none;
            // Keep RAM current until the waiting capture runs. One refresh is
            // insufficient: zero-tick pictures can scroll again before that tick.
            return _scale == 1 && _lazyScrollAllowed && _hardwareScroll && !_rowShiftOnly && ShimHost::CanScroll()
                && !gPaintForceRedraw && !wetVideo && gScreenshotCountdown == 0;
        }

        void PaintWindows() override
        {
            OG_SCOPE_NUMBER(29);
            ShimHost::PhaseTimer timer{ ShimHost::Phase::paint };
#ifdef OPENRCT2_PERF_METRICS
            Perf::Frame(_bits, static_cast<size_t>(_pitch) * _height);
#endif
            // Restore weather before any readback or scroll. Retain the scaled pointer
            // unless those actual weather pixels would overwrite it.
            if (_pointerDrawn && ShimHost::WeatherTouches(_weatherShown.data(), _weatherShown.size(),
                    _pointerRect.left, _pointerRect.top, _pointerRect.right, _pointerRect.bottom, &_weatherSaved))
            {
                ShimHost::Present(_scaledBits.data(), _physicalWidth, _pointerRect.left, _pointerRect.top,
                                  _pointerRect.right, _pointerRect.bottom);
                _pointerDrawn = false;
            }
            ShimHost::RestoreWeather(_weatherShown.data(), _weatherShown.size(), _weatherSaved);
            _weatherShown.clear();
            gPaintDamageViewport = nullptr;
            gPaintEntityDamageMaskOn = false;
            gLastFrame.readback = {};
            if (_readbackPending)
            {
                _readbackPending = false;
                if (_memoryStale)
                {
                    const auto started = ShimHost::Microseconds();
                    // Recovery only enables wider subsequent uploads; the display
                    // already contains the complete current picture. Keep exact
                    // dirty edges until every strip has reached RAM. A new hardware
                    // scroll cancels partial progress below.
                    const bool smooth = Config::Get().general.smoothScrollRecovery;
                    const int32_t top = smooth && _readbackActive ? _readbackTop : _staleRect.top;
                    const int32_t bottom = smooth ? std::min(top + 32, _staleRect.bottom) : _staleRect.bottom;
                    if (ShimHost::ReadPixels(reinterpret_cast<uint8_t*>(_bits), _pitch, _width, _height,
                        _staleRect.left, top, _staleRect.right, bottom))
                    {
                        _readbackTop = bottom;
                        _readbackActive = bottom < _staleRect.bottom;
                        if (!_readbackActive) _memoryStale = false;
                        gLastFrame.readback = {_staleRect.left, top, _staleRect.right, bottom};
                        ShimHost::gIdleReadbackCalls++;
                        ShimHost::gIdleReadbackUs += ShimHost::Microseconds() - started;
                    }
                    else
                    {
                        _readbackActive = false;
                        _refreshPending = true;
                    }
                }
            }
#ifdef OPENRCT2_CLASSIC_MAC
            // Native full-screen ownership hides the OS menu bar and its clock.
            // Preserve the previous repair for the fallback display path.
            if (++_clockFrames >= 10)
            {
                _clockFrames = 0;
#ifdef OPENRCT2_PERF_METRICS
                ShimHost::ClockDamaged(); // Measurement-only overdraw observer.
                Perf::SourceScope clockScope(Perf::clock);
#endif
                if (!ShimHost::SystemMenuBarHidden())
                    ::GfxSetDirtyBlocks({ { static_cast<int32_t>(_width) - 144, 0 }, { static_cast<int32_t>(_width), 24 } });
            }
#endif
            if (gDrawingRefreshRequest)
            {
                // Somebody needs the whole true picture, in memory and on the screen (a
                // screenshot, coming back from another application, a new colour table).
                gDrawingRefreshRequest = false;
                _refreshPending = true;
                GfxInvalidateScreen();
            }
            const bool repaintStaleMemory = _memoryStale;
            _lazyThisFrame = LazyScroll() && !_refreshPending;
            if (!_lazyThisFrame)
            {
                // The engine's own order of work needs memory to be the true picture: if it is
                // not, paint everything first.
                if (_memoryStale && !gPaintForceRedraw)
                {
                    // Hardware scrolling left RAM behind. Recover all pixels
                    // from the current cached geometry, not a new map build.
                    if (gCanvasReuseColumns) GfxRedrawScreenFromColumns();
                    else GfxInvalidateScreen();
                }
                _memoryStale = false;
                _readbackActive = false;
                _refreshPending = false;
            }
            // Remember what is dirty now, in screen positions from before any scroll.
            uint32_t columns = _invalidationGrid.getColumnCount(), rows = _invalidationGrid.getRowCount();
            const int32_t cellWidth = static_cast<int32_t>(_invalidationGrid.getBlockWidth());
            const int32_t cellHeight = static_cast<int32_t>(_invalidationGrid.getBlockHeight());
            int32_t originX = _invalidationGrid.originX();
            int32_t originY = _invalidationGrid.originY();
            const auto damageStart = ShimHost::Microseconds();
            const auto* damageMain = WindowGetMain();
            const bool damageBounds = gPaintColumnCache && gPaintDamageEntityRows && columns <= gPaintDamageTop.size()
                && damageMain != nullptr && damageMain->viewport != nullptr
                && damageMain->viewport->zoom <= ZoomLevel{2};
            _pendingDamage.clear();
            _scrollDamage.clear();
            const int movesBefore = _moveCount;
            const auto viewportStart = ShimHost::Microseconds();
            gPaintDeferScroll = true;
            gPaintScrollDamageHook = RecordScrollDamage;
            WindowUpdateAllViewports();
            gPaintDeferScroll = false;
            gPaintScrollDamageHook = nullptr;
            const auto viewportCost = ShimHost::Microseconds() - viewportStart;
            if (_moveCount > movesBefore)
            {
                _invalidationGrid.traverseDirtyCells([&](int32_t l, int32_t t, int32_t r, int32_t b) {
                    _pendingDamage.push_back({l, t, r, b, _invalidationGrid.currentKind()});
                });
                TranslateDamage(movesBefore);
            }
            else
                // Preserve the original idle row phase. Translating damage
                // uses both world axes during a scroll, but imposing an
                // arbitrary Y phase at rest can expand crowded guest damage.
                AlignDamageGrid(false);
            for (const auto& area : _scrollDamage)
                _invalidationGrid.invalidate(area.left, area.top, area.right, area.bottom, InvalidationGrid::kWorldOnly);
            if (Config::Get().interface.seeThroughLens || _lensWasOn)
            {
                ViewportUpdateSeeThroughLens(ContextGetCursorPositionScaled());
                _lensWasOn = Config::Get().interface.seeThroughLens;
            }
            columns = _invalidationGrid.getColumnCount();
            rows = _invalidationGrid.getRowCount();
            originX = _invalidationGrid.originX();
            originY = _invalidationGrid.originY();
            if (damageBounds)
            {
                // Include viewport strips and translated guest damage before collecting entities.
                std::fill_n(gPaintDamageTop.begin(), columns, std::numeric_limits<int32_t>::max());
                std::fill_n(gPaintDamageBottom.begin(), columns, std::numeric_limits<int32_t>::min());
                gPaintDamageOriginX = originX;
                gPaintDamageOriginY = originY;
                gPaintDamageCellWidth = cellWidth;
                gPaintDamageCellHeight = cellHeight;
                gPaintDamageColumns = columns;
                gPaintDamageRows = rows;
                const bool masks = gPaintDamageEntityMask && rows <= 128;
                if (masks)
                    for (uint32_t column = 0; column < columns; column++) gPaintDamageMasks[column].fill(0);
                _invalidationGrid.forEachDirtyCell([&](uint32_t column, uint32_t row) {
                    gPaintDamageTop[column] = std::min(gPaintDamageTop[column], static_cast<int32_t>(row) * cellHeight + originY);
                    gPaintDamageBottom[column] = std::max(gPaintDamageBottom[column], static_cast<int32_t>(row + 1) * cellHeight + originY);
                    if (masks)
                        gPaintDamageMasks[column][row / 32] |= uint32_t{1} << (row & 31);
                });
                gPaintDamageViewport = damageMain->viewport;
            }
            ShimHost::gDamageMicros += ShimHost::Microseconds() - damageStart - viewportCost;
            // Scrolling may have left the backing canvas at an earlier camera.
            // Secondary viewport damage skips must use its state before drawing
            // any rectangle that presentation will copy from that canvas.
            // A deferred full recovery also needs this guard until its dirty
            // blocks have actually filled RAM, even if no further scroll occurs.
            gDrawingMemoryStale = repaintStaleMemory || _memoryStale;
            ViewportDamageBeginPaint();
            DrawAllDirtyBlocks();
            ViewportDamageEndPaint();
        }


        // The engine still chooses patterns, phases and visible viewport pieces.
        // Collect rows, not dots; clipping and scaling happen once per row/run.
        struct WeatherCollector final : IWeatherDrawer
        {
            ShimDrawingEngine& engine;
            explicit WeatherCollector(ShimDrawingEngine& owner) : engine(owner) {}
            void Span(int x, int y, int count, int step, uint8_t colour, int width)
            {
                if (width == 0 || y >= engine._physicalHeight || x >= engine._physicalWidth) return;
                const int last = x + (count - 1) * step;
                const int tail = std::min(width, engine._physicalWidth - last);
                if (tail < width)
                {
                    if (count > 1) engine._weatherNew.push_back({uint16_t(x), uint16_t(y), uint16_t(count - 1),
                        uint16_t(step), colour, uint8_t(width)});
                    if (tail > 0) engine._weatherNew.push_back({uint16_t(last), uint16_t(y), 1,
                        uint16_t(step), colour, uint8_t(tail)});
                }
                else engine._weatherNew.push_back({uint16_t(x), uint16_t(y), uint16_t(count),
                    uint16_t(step), colour, uint8_t(width)});
            }
            void Draw(RenderTarget& rt, int32_t x, int32_t y, int32_t width, int32_t height,
                      int32_t xStart, int32_t yStart, const uint8_t* pattern) override
            {
                const int xSpace = *pattern++, ySpace = *pattern++;
                // Both current patterns repeat on a 32x32 grid. Visit only rows
                // which contain pixels, retaining pass order and the engine's phases.
                if (engine._scale == 1 && xSpace == 32 && ySpace == 32 && x >= 0 && y >= 0)
                {
                    const int right = std::min(x + width, rt.width), bottom = std::min(y + height, rt.height);
                    const int phaseX = xStart & 31, phaseY = yStart & 31;
                    for (int py = 0; py < 32; py++)
                    {
                        const int px = pattern[py * 2];
                        if (px == 255) continue;
                        const int first = x + ((px - phaseX) & 31);
                        if (first >= right) continue;
                        const int count = (right - first + 31) >> 5;
                        const uint8_t colour = pattern[py * 2 + 1];
                        for (int row = y + ((py - phaseY) & 31); row < bottom; row += 32)
                            engine._weatherNew.push_back({uint16_t(first), uint16_t(row), uint16_t(count),
                                32, colour, 1});
                    }
                    return;
                }
                // Quarter-step interface scales preserve each 32-pixel horizontal
                // period exactly. Precompute the physical run once for each pattern row.
                const float regularStep = 32 * engine._scale;
                if (xSpace == 32 && ySpace == 32 && x >= 0 && y >= 0 && regularStep == std::floor(regularStep))
                {
                    const int right = std::min(x + width, rt.width), bottom = std::min(y + height, rt.height);
                    const int phaseX = xStart & 31, phaseY = yStart & 31;
                    for (int py = 0; py < 32; py++)
                    {
                        const int px = pattern[py * 2];
                        if (px == 255) continue;
                        const int first = x + ((px - phaseX) & 31);
                        if (first >= right) continue;
                        int count = (right - first + 31) >> 5;
                        const int left = engine.Edge(first), pixelWidth = engine.Edge(first + 1) - left;
                        if (pixelWidth == 0 || left >= engine._physicalWidth) continue;
                        const int last = left + (count - 1) * int(regularStep);
                        const int tail = std::min(pixelWidth, engine._physicalWidth - last);
                        const bool clipped = tail < pixelWidth;
                        if (clipped) count--;
                        const uint8_t colour = pattern[py * 2 + 1];
                        for (int row = y + ((py - phaseY) & 31); row < bottom; row += 32)
                        {
                            const int physicalBottom = std::min(engine.Edge(row + 1), engine._physicalHeight);
                            for (int dy = engine.Edge(row); dy < physicalBottom; dy++)
                            {
                                if (count > 0) engine._weatherNew.push_back({uint16_t(left), uint16_t(dy), uint16_t(count),
                                    uint16_t(regularStep), colour, uint8_t(pixelWidth)});
                                if (clipped && tail > 0) engine._weatherNew.push_back({uint16_t(last), uint16_t(dy), 1,
                                    uint16_t(regularStep), colour, uint8_t(tail)});
                            }
                        }
                    }
                    return;
                }
                // Both engine patterns are 32x32. Retain the generic phase for future patterns.
                const bool masks = (xSpace & (xSpace - 1)) == 0 && (ySpace & (ySpace - 1)) == 0;
                const int startX = masks ? xStart & (xSpace - 1) : uint8_t(xStart % xSpace);
                int py = masks ? yStart & (ySpace - 1) : uint8_t(yStart % ySpace) % ySpace;
                const int bottom = std::min(y + height, rt.height), right = std::min(x + width, rt.width);
                const float scaledStep = xSpace * engine._scale;
                for (int row = y; row < bottom; row++, py = masks ? (py + 1) & (ySpace - 1) : (py + 1) % ySpace)
                {
                    const int px = pattern[py * 2];
                    if (row < 0 || px == 255) continue;
                    int first = x + (masks ? (px - startX) & (xSpace - 1) : uint8_t(px - startX) % xSpace);
                    if (first < 0) first += (-first + xSpace - 1) / xSpace * xSpace;
                    if (first >= right) continue;
                    const int count = (right - 1 - first) / xSpace + 1;
                    const uint8_t colour = pattern[py * 2 + 1];
                    if (engine._scale == 1) Span(first, row, count, xSpace, colour, 1);
                    else
                    {
                        const int top = engine.Edge(row), bottom = std::min(engine.Edge(row + 1), engine._physicalHeight);
                        if (scaledStep == std::floor(scaledStep))
                        {
                            const int left = engine.Edge(first), width = engine.Edge(first + 1) - left;
                            for (int dy = top; dy < bottom; dy++) Span(left, dy, count, int(scaledStep), colour, width);
                        }
                        else // Arbitrary saved scale: exact same ceil-edge mapping as presentation.
                            for (int n = 0, column = first; n < count; n++, column += xSpace)
                            {
                                const int left = engine.Edge(column), width = engine.Edge(column + 1) - left;
                                for (int dy = top; dy < bottom; dy++) Span(left, dy, 1, 1, colour, width);
                            }
                    }
                }
            }
        };

        void PaintWeather() override
        {
            OG_SCOPE_NUMBER(11);
            WeatherCollector collector(*this);
            _weatherNew.clear();
            DrawWeather(_mainRT, &collector);
        }

        void EndDraw() override
        {
            X8DrawingEngine::EndDraw();
            // Charge staged writes and audio refills to the picture they delay.
#ifdef OPENRCT2_PAK_VFS
            ClassicDiskIo::Frame(getGameState().currentTicks);
            if (ClassicAutosave::Pending())
            {
                FrameTiming::Scope timing(FrameTiming::autosave);
                ClassicAutosave::Pump();
            }
            {
                ShimHost::PhaseTimer audioTimer{ ShimHost::Phase::pump };
                const auto audioStart = ShimHost::gTracePump ? ShimHost::Microseconds() : 0;
                OpenRCT2::Audio::ClassicAudioTick();
                if (ShimHost::gTracePump)
                    ShimHost::gAudioPumpMicros += ShimHost::Microseconds() - audioStart;
            }
#endif
            const bool moved = _moveCount != 0;
            {
            ShimHost::PhaseTimer timer{ ShimHost::Phase::present };
            // Intermediate palettes during catch-up ticks and palette effects are never shown.
            FlushPalette();
            if (PointerNeedsRestore())
            {
                ShimHost::Present(_scaledBits.data(), _physicalWidth, _pointerRect.left, _pointerRect.top,
                                  _pointerRect.right, _pointerRect.bottom);
                _pointerDrawn = false;
            }
            const auto* bits = reinterpret_cast<const uint8_t*>(_bits);
            if (gPaintForceRedraw)
                _all = true;
            else if (_all && _memoryStale)
            {
                // The whole frame was asked for, but memory does not hold it: paint it all next
                // frame instead (the screen itself is still right apart from this frame's changes).
                _all = false;
                _rectCount = 0;
                _moveCount = 0;
                _refreshPending = true;
            }
            if (!_all)
                for (int i = 0; i < _moveCount; i++)
                {
                    const auto& move = _moves[i];
                    // On the screen only pixels from inside the block may move. The engine's move
                    // also drags in whatever lies beside the block (the toolbar above a view
                    // scrolling down) and paints over it afterwards; on a live screen that showed
                    // for a moment, every frame.
                    int32_t x = move.x, y = move.y, width = move.width, height = move.height;
                    if (move.dx > 0 && x - move.dx < move.blockLeft)
                    {
                        const int32_t cut = move.blockLeft - (x - move.dx);
                        x += cut;
                        width -= cut;
                    }
                    if (move.dx < 0 && x - move.dx + width > move.blockRight)
                        width = move.blockRight - (x - move.dx);
                    if (move.dy > 0 && y - move.dy < move.blockTop)
                    {
                        const int32_t cut = move.blockTop - (y - move.dy);
                        y += cut;
                        height -= cut;
                    }
                    if (move.dy < 0 && y - move.dy + height > move.blockBottom)
                        height = move.blockBottom - (y - move.dy);
                    if (width <= 0 || height <= 0)
                        continue; // all of it is repainted
                    if (!_hardwareScroll || !ScrollDisplay(x, y, width, height, move.dx, move.dy))
                    {
                        if (_memoryStale)
                            _refreshPending = true; // memory cannot supply the moved area: repaint next frame
                        else
                            Add(move.x, move.y, move.x + move.width, move.y + move.height);
                    }
                }
            // For the remote "rects" command: what this frame sent to the screen.
            gLastFrame.all = _all;
            gLastFrame.lazy = _lazyThisFrame;
            gLastFrame.stale = _memoryStale;
            gLastFrame.recovering = _readbackActive;
            gDrawingMemoryStale = _memoryStale;
            gLastFrame.rectCount = _all ? 0 : _rectCount;
            gLastFrame.moveCount = _all ? 0 : _moveCount;
            std::memcpy(gLastFrame.rects, _rects, sizeof(Rect) * static_cast<size_t>(gLastFrame.rectCount));
            std::memcpy(gLastFrame.moves, _moves, sizeof(Move) * static_cast<size_t>(gLastFrame.moveCount));
            // A software pointer does not disturb this: it is restored from memory before any
            // rectangle under it is presented, and drawn again afterwards.
            const bool shownUsable = gPresentChangedOnly && _scale == 1 && !_memoryStale && !moved
                && !Config::Get().general.useVSync;
            gPresentChangedState = (_scale == 1 ? 1 : 0) | (_memoryStale ? 0 : 2) | (moved ? 0 : 4) | (_pointerDrawn ? 0 : 8)
                | (Config::Get().general.useVSync ? 0 : 16) | (_shownValid ? 32 : 0) | (_all ? 64 : 0);
            if (!shownUsable)
                _shownValid = false;
            if (shownUsable && _shownValid && !_all)
            {
                for (int i = 0; i < _rectCount; i++)
                {
                    const auto& r = _rects[i];
                    const int32_t left = _wordRects ? r.left & ~7 : r.left;
                    const int32_t right = _wordRects ? std::min<int32_t>((r.right + 7) & ~7, _width) : r.right;
                    ShimHost::PresentChanged(bits, _shown.data(), _pitch, left, r.top, right, r.bottom);
                }
            }
            else if (_all)
                PresentRect(bits, 0, 0, _width, _height);
            else
                for (int i = 0; i < _rectCount; i++)
                {
                    const auto& r = _rects[i];
                    // At rest all pixels in memory are current. Send full words around an
                    // unaligned dirty edge so the G3 avoids slow byte stores to video memory.
                    // A hardware scroll can leave memory stale, so keep its exact rectangles.
                    const int32_t left = _wordRects && !_memoryStale ? r.left & ~7 : r.left;
                    const int32_t right = _wordRects && !_memoryStale ? std::min<int32_t>((r.right + 7) & ~7, _width) : r.right;
                    PresentRect(bits, left, r.top, right, r.bottom);
                }
            if (shownUsable && !_shownValid)
            {
                // Everything presented so far has made the display equal to memory: copy it once.
                _shown.assign(bits, bits + static_cast<size_t>(_pitch) * _height);
                _shownValid = true;
            }
            _rectCount = 0;
            _moveCount = 0;
            _all = false;
            if (!_weatherNew.empty())
            {
                const auto* clean = _scale == 1 ? (_memoryStale ? nullptr : bits) : _scaledBits.data();
                ShimHost::ShowWeather(_weatherNew.data(), _weatherNew.size(), _weatherSaved,
                                      clean, _scale == 1 ? _pitch : _physicalWidth);
                if (_pointerDrawn && ShimHost::WeatherTouches(_weatherNew.data(), _weatherNew.size(),
                        _pointerRect.left, _pointerRect.top, _pointerRect.right, _pointerRect.bottom, &_weatherSaved))
                    _pointerDrawn = false;
                _weatherShown.swap(_weatherNew);
                _weatherNew.clear();
            }
            // Lazy hardware pans leave RAM stale. Recover once the view has settled so
            // subsequent small repaints can use aligned transfers around their edges.
            if (_idleRefresh && _memoryStale && !moved)
            {
                if (_readbackActive || ++_idleStaleFrames >= 8)
                {
                    _readbackPending = true;
                    _idleStaleFrames = 0;
                }
            }
            else
                _idleStaleFrames = 0;
            DrawPointer();
            ShimHost::EndFrame();
            OpenRCT2::FrameTiming::current.presentedAtUs = ShimHost::Microseconds();
            }

            ShimHost::FrameDone(getGameState().currentTicks);
        }

        std::string Screenshot(const std::string& path) override
        {
            // Capture display dimensions and weather, without drawing or including the pointer.
            std::vector<uint8_t> capture(static_cast<size_t>(_physicalWidth) * _physicalHeight);
            const auto* bits = reinterpret_cast<const uint8_t*>(_bits);
            for (int y = 0; y < _physicalHeight; y++)
            {
                auto* to = capture.data() + y * _physicalWidth;
                const auto* from = bits + (_scale == 1 ? y : _sourceY[y]) * _pitch;
                if (_scale == 1) std::memcpy(to, from, _physicalWidth);
                else for (int x = 0; x < _physicalWidth; x++) to[x] = from[_sourceX[x]];
            }
            ShimHost::OverlayWeather(_weatherShown.data(), _weatherShown.size(), _weatherSaved,
                                     capture.data(), _physicalWidth);
            RenderTarget rt{};
            rt.bits = reinterpret_cast<PaletteIndex*>(capture.data());
            rt.width = _physicalWidth; rt.height = _physicalHeight;
            return ScreenshotDumpPNG(rt, path);
        }

        static bool FullComparison(std::vector<uint8_t>& output, long frame, const std::string& path)
        {
            if (_instance == nullptr) return false;
            auto& self = *_instance;
            std::vector<uint8_t> canvas(static_cast<size_t>(self._pitch) * self._height, 0);
            // Screenshots are requested after EndDraw, when the drawing context
            // has closed. Give the diagnostic its own context and make its
            // canvas the screen target so it exercises the cached painter too.
            auto* savedBits = self._mainRT.bits;
            self._mainRT.bits = reinterpret_cast<PaletteIndex*>(canvas.data());
#ifdef OPENRCT2_REFERENCE_BLITTERS
            _pixelTrace.clear();
            if (const char* targets = std::getenv("RCT_PAINT_TARGETS"))
                if (FILE* file = std::fopen(targets, "r"))
                {
                    long targetFrame;
                    int x, y;
                    while (std::fscanf(file, "%ld %d %d", &targetFrame, &x, &y) == 3)
                        if (targetFrame == frame && x >= 0 && y >= 0 && x < self._physicalWidth && y < self._physicalHeight)
                        {
                            const int sourceX = self._scale == 1 ? x : self._sourceX[x];
                            const int sourceY = self._scale == 1 ? y : self._sourceY[y];
                            _pixelTrace.push_back({ x, y, canvas.data() + sourceY * self._pitch + sourceX });
                        }
                    std::fclose(file);
                }
            if (!_pixelTrace.empty())
            {
                gPaintPixelTraceHook = TracePaintPixel;
                gPaintPaletteFilterTraceHook = TracePaletteFilter;
                gPaintSpriteBlendTraceHook = TraceSpriteBlend;
                gPaintLensTraceHook = TraceLens;
            }
#else
            (void)frame;
            (void)path;
#endif
            self.X8DrawingEngine::BeginDraw();
            ViewportPaintFullComparison(self._mainRT);
            self.X8DrawingEngine::EndDraw();
#ifdef OPENRCT2_REFERENCE_BLITTERS
            gPaintPixelTraceHook = nullptr;
            gPaintPaletteFilterTraceHook = nullptr;
            gPaintSpriteBlendTraceHook = nullptr;
            gPaintLensTraceHook = nullptr;
            if (!_pixelTrace.empty())
                if (FILE* identities = std::fopen((path + ".objects").c_str(), "w"))
                {
                    for (const auto& point : _pixelTrace)
                        for (const auto& item : point.objects)
                            std::fprintf(identities, "%d %d %lld %lld %lld %lld %lld %lld %lld\n", point.x, point.y,
                                static_cast<long long>(item[0]), static_cast<long long>(item[1]), static_cast<long long>(item[2]),
                                static_cast<long long>(item[3]), static_cast<long long>(item[4]), static_cast<long long>(item[5]),
                                static_cast<long long>(item[6]));
                    std::fclose(identities);
                }
            if (!_pixelTrace.empty())
                if (FILE* file = std::fopen((path + ".colours").c_str(), "wb"))
                {
                    // Text coordinates and hex masks keep this endian independent.
                    for (auto& point : _pixelTrace)
                    {
                        // With no sprite contribution, the cleared canvas is
                        // the reference background. Record it only there; it
                        // must never excuse a missing foreground sprite.
                        if (*point.pixel == 0 && std::all_of(point.colours.begin(), point.colours.end(),
                            [](uint8_t value) { return value == 0; }))
                            point.colours[0] = 1;
                        std::fprintf(file, "%d %d ", point.x, point.y);
                        for (const auto byte : point.colours) std::fprintf(file, "%02x", byte);
                        std::fputc('\n', file);
                    }
                    std::fclose(file);
                }
#endif
            self._mainRT.bits = savedBits;
            output.resize(static_cast<size_t>(self._physicalWidth) * self._physicalHeight);
            for (int y = 0; y < self._physicalHeight; y++)
            {
                const auto* from = canvas.data() + (self._scale == 1 ? y : self._sourceY[y]) * self._pitch;
                auto* to = output.data() + y * self._physicalWidth;
                if (self._scale == 1) std::memcpy(to, from, self._physicalWidth);
                else for (int x = 0; x < self._physicalWidth; x++) to[x] = from[self._sourceX[x]];
            }
            ShimHost::OverlayWeather(self._weatherShown.data(), self._weatherShown.size(), self._weatherSaved,
                                     output.data(), self._physicalWidth);
            return true;
        }

        void CopyRect(int32_t x, int32_t y, int32_t width, int32_t height, int32_t dx, int32_t dy) override
        {
            if (dx == 0 && dy == 0)
                return;
            static int traced = 0;
            if (traced < 12 && std::getenv("RCT_TRACE_RECTS") != nullptr)
            {
                traced++;
                std::fprintf(stderr, "copyrect %d,%d %dx%d by %d,%d pitch %u\n", x, y, width, height, dx, dy, static_cast<unsigned>(_pitch));
            }
            const int32_t blockLeft = x, blockTop = y, blockRight = x + width, blockBottom = y + height;
            const bool fullWidth = x == 0 && width == static_cast<int32_t>(_pitch) && width == static_cast<int32_t>(_width);
            // Same clipping as the engine: the source must be on screen.
            const int32_t lmargin = std::min(x - dx, 0);
            const int32_t rmargin = std::min(static_cast<int32_t>(_width) - (x - dx + width), 0);
            const int32_t tmargin = std::min(y - dy, 0);
            const int32_t bmargin = std::min(static_cast<int32_t>(_height) - (y - dy + height), 0);
            x -= lmargin;
            y -= tmargin;
            width += lmargin + rmargin;
            height += tmargin + bmargin;
            if (width <= 0 || height <= 0)
                return;
            // The engine's version, with a row move that works a word at a time whatever the
            // distance (the C library falls back to single bytes when a row moves by a few pixels).
            {
                const uint64_t shiftStart = ShimHost::Microseconds();
                const int32_t stride = static_cast<int32_t>(_pitch);
                auto* bits = reinterpret_cast<uint8_t*>(_bits);
                if (_lazyThisFrame)
                {
                    // Recover only scrolled regions later. RAM outside them is already valid,
                    // so reading a stationary toolbar pointer would only hide it unnecessarily.
                    const Rect stale{ std::max(0, blockLeft), std::max(0, blockTop),
                        std::min<int>(_width, blockRight), std::min<int>(_height, blockBottom) };
                    if (!_memoryStale) _staleRect = stale;
                    else
                    {
                        _staleRect.left = std::min(_staleRect.left, stale.left);
                        _staleRect.top = std::min(_staleRect.top, stale.top);
                        _staleRect.right = std::max(_staleRect.right, stale.right);
                        _staleRect.bottom = std::max(_staleRect.bottom, stale.bottom);
                    }
                    _memoryStale = true; // the display moves its pixels; memory is left as it is
                    _readbackActive = false;
                    _readbackPending = false;
                }
                else if (fullWidth && !_rowShiftOnly)
                {
                    // Full-width rows are one block of memory: a single move by the host's best
                    // routine. Pixels that wrap from one row into the next land in the columns the
                    // scroll exposes, which are repainted anyway.
                    const int32_t offset = dy * stride + dx;
                    uint8_t* const begin = bits;
                    uint8_t* const end = bits + static_cast<int32_t>(_height) * stride;
                    uint8_t* to = bits + y * stride;
                    uint8_t* from = to - offset;
                    int32_t count = height * stride;
                    if (from < begin)
                    {
                        const int32_t skip = static_cast<int32_t>(begin - from);
                        from += skip;
                        to += skip;
                        count -= skip;
                    }
                    if (from + count > end)
                        count = static_cast<int32_t>(end - from);
                    if (count > 0)
                        ShimHost::MoveMemory(to, from, static_cast<size_t>(count));
                }
                else
                {
                    for (int32_t i = 0; i < height; i++)
                    {
                        const int32_t row = dy > 0 ? height - 1 - i : i;
                        // (The OS block move per row measured slower than this on the G3: 3.5 against 3.0 ms.)
                        MoveRow(bits + (y + row) * stride + x, bits + (y + row - dy) * stride + x - dx, width);
                    }
                }
                ShimHost::CountShift(ShimHost::Microseconds() - shiftStart);
            }
            if (_all)
                return;
            if (_moveCount == kMaxMoves)
            {
                _all = true;
                return;
            }
            _moves[_moveCount++] = { x, y, width, height, dx, dy, blockLeft, blockTop, blockRight, blockBottom };
            // Anything already repainted inside the source has moved too.
            const int count = _rectCount;
            for (int i = 0; i < count && !_all; i++)
            {
                const Rect rect = _rects[i];
                const int left = std::max(rect.left + dx, x), top = std::max(rect.top + dy, y);
                const int right = std::min(rect.right + dx, x + width), bottom = std::min(rect.bottom + dy, y + height);
                if (left >= right || top >= bottom)
                    continue;
                // With memory moved along, the moved copy is right there and only needs sending.
                // Without (the screen alone scrolls), memory holds something else at that place:
                // it has to be painted again.
                if (_lazyThisFrame)
                    _invalidationGrid.invalidate(left, top, right, bottom, InvalidationGrid::kGeneral);
                else
                    Add(left, top, right, bottom);
            }
        }
    };

    class ShimPlatformUiContext final : public IPlatformUiContext
    {
    public:
        void SetWindowIcon(SDL_Window*) override
        {
        }
        bool IsSteamOverlayAttached() override
        {
            return false;
        }
        void ShowMessageBox(SDL_Window*, const std::string& message) override
        {
            ShimHost::Message(message.c_str());
        }
        bool HasMenuSupport() override
        {
            return false;
        }
        int32_t ShowMenuDialog(const std::vector<std::string>&, const std::string&, const std::string&) override
        {
            return 0;
        }
        void OpenFolder(const std::string&) override
        {
        }
        void OpenURL(const std::string&) override
        {
        }
        std::string ShowFileDialog(SDL_Window*, const FileDialogDesc&) override
        {
            return {};
        }
        std::string ShowDirectoryDialog(SDL_Window*, const std::string&) override
        {
            return {};
        }
        bool HasFilePicker() const override
        {
            return false;
        }
    };
} // namespace

namespace ShimHost
{
    bool PaintFullCopy(std::vector<uint8_t>& output, long frame, const std::string& path)
    { return ShimDrawingEngine::FullComparison(output, frame, path); }
    bool gWidePresent = true, gBatchCursor = true;
    bool gPresentAlignedCopy = false; // hardware A/B first; pixel-copy mechanics only
    unsigned gPresentPrefetchRows = 0;
    uint64_t gIdleReadbackCalls = 0, gIdleReadbackUs = 0;
    uint64_t gDamageMicros = 0, gScreenScrollMicros = 0, gScreenScrollWaitMicros = 0, gPointerPixels = 0;
    std::string LastFrameRects()
    {
        const auto& frame = ShimDrawingEngine::gLastFrame;
        std::string text = "{\"coordinate_space\":\"canvas\",\"all\":" + std::string(frame.all ? "true" : "false") + ",\"lazy\":" + (frame.lazy ? "true" : "false")
            + ",\"stale\":" + (frame.stale ? "true" : "false")
            + ",\"recovering\":" + (frame.recovering ? "true" : "false")
            + ",\"readback\":[" + std::to_string(frame.readback.left) + "," + std::to_string(frame.readback.top) + ","
                + std::to_string(frame.readback.right - frame.readback.left) + ","
                + std::to_string(frame.readback.bottom - frame.readback.top) + "],\"moves\":[";
        for (int i = 0; i < frame.moveCount; i++)
        {
            const auto& m = frame.moves[i];
            text += (i ? "," : "") + std::string("[") + std::to_string(m.x) + "," + std::to_string(m.y) + ","
                + std::to_string(m.width) + "," + std::to_string(m.height) + "," + std::to_string(m.dx) + ","
                + std::to_string(m.dy) + "]";
        }
        text += "],\"rects\":[";
        for (int i = 0; i < frame.rectCount; i++)
        {
            const auto& r = frame.rects[i];
            text += (i ? "," : "") + std::string("[") + std::to_string(r.left) + "," + std::to_string(r.top) + ","
                + std::to_string(r.right - r.left) + "," + std::to_string(r.bottom - r.top) + "]";
        }
        return text + "]}";
    }

    void SetHardwareScroll(bool on)
    {
        ShimDrawingEngine::SetHardwareScroll(on);
    }
    void SetIdleRefresh(bool on)
    {
        ShimDrawingEngine::SetIdleRefresh(on);
    }
    void SetWordRectangles(bool on)
    {
        ShimDrawingEngine::SetWordRectangles(on);
    }
    void SetAlignedCells(bool on)
    {
        ShimDrawingEngine::SetAlignedCells(on);
    }
    void SetDeferredPalette(bool on)
    {
        ShimDrawingEngine::SetDeferredPalette(on);
    }
    void SetRowShiftOnly(bool on)
    {
        ShimDrawingEngine::SetRowShift(on);
    }
    void SetDirtyCells(int width, int height)
    {
        ShimDrawingEngine::SetCells(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
} // namespace ShimHost

namespace OpenRCT2::Ui
{
    std::unique_ptr<IDrawingEngine> CreateHardwareDisplayDrawingEngine(IUiContext& uiContext)
    {
        return std::make_unique<ShimDrawingEngine>(uiContext);
    }

    std::unique_ptr<IPlatformUiContext> CreatePlatformUiContext()
    {
        return std::make_unique<ShimPlatformUiContext>();
    }
} // namespace OpenRCT2::Ui
