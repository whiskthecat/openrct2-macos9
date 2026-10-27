#pragma once
#include "ShimHost.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <vector>

namespace ShimHost
{
    // Exact indexed copy of a true-colour display, including scrolls and transient dots/cursors.
    // Palette animation re-expands affected indices without repainting world geometry.
    class IndexedDisplay
    {
        int _width = 0, _height = 0;
        std::vector<uint8_t> _pixels;
        std::array<uint32_t, 256> _rgb{};
        std::array<bool, 256> _dirty{};
        bool _valid = false, _changed = false;
    public:
        void Resize(int width, int height)
        {
            _width = width; _height = height;
            _pixels.assign(static_cast<size_t>(width) * height, 0);
            _valid = false; _changed = false; _dirty.fill(false);
        }
        bool SetPalette(const Colour* colours)
        {
            bool changed = false;
            for (int i = 0; i < 256; i++)
            {
                const uint32_t rgb = (uint32_t(colours[i].red) << 16) | (uint32_t(colours[i].green) << 8) | colours[i].blue;
                if (!_valid || _rgb[i] != rgb) { _dirty[i] = true; changed = true; }
                _rgb[i] = rgb;
            }
            _valid = true; _changed |= changed;
            return changed;
        }
        uint32_t RGB(uint8_t index) const { return _rgb[index]; }
        const uint8_t* Pixels() const { return _pixels.data(); }
        size_t Bytes() const { return _pixels.size(); }
        bool Changed() const { return _changed; }
        void Present(const uint8_t* bits, int pitch, int left, int top, int right, int bottom)
        {
            for (int y = top; y < bottom; y++)
                std::memcpy(_pixels.data() + size_t(y) * _width + left, bits + size_t(y) * pitch + left, right - left);
        }
        bool Scroll(int x, int y, int width, int height, int dx, int dy)
        {
            if (x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > _width || y + height > _height
                || x - dx < 0 || y - dy < 0 || x - dx + width > _width || y - dy + height > _height) return false;
            for (int i = 0; i < height; i++)
            {
                const int row = dy > 0 ? y + height - 1 - i : y + i;
                std::memmove(_pixels.data() + size_t(row) * _width + x,
                    _pixels.data() + size_t(row - dy) * _width + x - dx, width);
            }
            return true;
        }
        void Set(int x, int y, uint8_t index) { _pixels[size_t(y) * _width + x] = index; }
        bool RecolourTouches(int left, int top, int right, int bottom) const
        {
            if (!_changed) return false;
            for (int y = std::max(0, top); y < std::min(_height, bottom); ++y)
                for (int x = std::max(0, left); x < std::min(_width, right); ++x)
                    if (_dirty[_pixels[size_t(y) * _width + x]]) return true;
            return false;
        }
        uint64_t Recolour(uint8_t* screen, int stride)
        {
            if (!_changed) return 0;
            uint64_t count = 0;
            for (int y = 0; y < _height; y++)
            {
                auto* row = reinterpret_cast<uint32_t*>(screen + size_t(y) * stride);
                for (int x = 0; x < _width; x++)
                {
                    const uint8_t index = _pixels[size_t(y) * _width + x];
                    if (_dirty[index] && (row[x] & 0xFFFFFFu) != _rgb[index]) { row[x] = _rgb[index]; count++; }
                }
            }
            _dirty.fill(false); _changed = false;
            return count;
        }
        uint64_t Differences(const uint8_t* screen, int stride) const
        {
            uint64_t count = 0;
            for (int y = 0; y < _height; y++)
            {
                const auto* row = reinterpret_cast<const uint32_t*>(screen + size_t(y) * stride);
                for (int x = 0; x < _width; x++)
                    count += (row[x] & 0xFFFFFFu) != _rgb[_pixels[size_t(y) * _width + x]];
            }
            return count;
        }
    };
}
