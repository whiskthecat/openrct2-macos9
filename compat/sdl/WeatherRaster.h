#pragma once
#include "ShimHost.h"
#include <algorithm>

namespace ShimHost
{
    inline bool WeatherSpanTouches(const WeatherSpan& span, int left, int top, int right, int bottom)
    {
        if (left >= right || top >= bottom || span.y < top || span.y >= bottom || span.count == 0 || span.width == 0)
            return false;
        // First periodic run whose right edge reaches this rectangle; gaps do not count.
        const int distance = left - span.x - span.width + 1;
        const int run = distance > 0 ? (distance + span.step - 1) / span.step : 0;
        return run < span.count && span.x + run * span.step < right;
    }

    inline bool WeatherTouches(const WeatherSpan* spans, size_t count, int left, int top, int right, int bottom,
                               const WeatherPixels* saved = nullptr)
    {
        if (saved != nullptr && saved->count == 0) return false;
        for (size_t i = 0; i < count; i++)
        {
            const auto& span = spans[i];
            if (saved != nullptr && span.y > saved->top && span.y < saved->bottom)
            {
                if (WeatherSpanTouches(span, left, top, std::min(right, saved->left + 1), bottom)
                    || WeatherSpanTouches(span, std::max(left, saved->right), top, right, bottom)) return true;
            }
            else if (WeatherSpanTouches(span, left, top, right, bottom)) return true;
        }
        return false;
    }

    inline void PrepareWeather(const WeatherSpan* spans, size_t count, WeatherPixels& saved)
    {
        size_t pixels = 0;
        for (size_t i = 0; i < count; i++) pixels += size_t(spans[i].count) * spans[i].width;
        if (saved.pixels.size() < pixels) saved.pixels.resize(pixels);
        saved.count = 0;
    }

    // Inputs are clipped once by the collector. Offsets address the host's 8-bit
    // display, whose supported modes fit in 24 bits; colours remain palette indices.
    template<bool ClearPointer>
    inline uint32_t* WeatherRow8(const WeatherSpan& span, uint32_t* saved, uint8_t* screen, int stride,
                                const uint8_t* under, int left, int right)
    {
        const uint32_t base = uint32_t(span.y) * stride;
        const int step = span.step;
        if (span.width == 1)
        {
            uint32_t offset = base + span.x;
            const uint8_t* from = under + span.x;
            for (int i = 0, x = span.x; i < span.count; i++, x += step, offset += step, from += step)
            {
                if constexpr (ClearPointer) if (x > left && x < right) continue;
                *saved++ = (offset << 8) | *from;
                screen[offset] = span.colour;
            }
        }
        else
            for (int i = 0, x = span.x; i < span.count; i++, x += step)
                for (int dx = 0; dx < span.width; dx++)
                {
                    if constexpr (ClearPointer) if (x + dx > left && x + dx < right) continue;
                    *saved++ = ((base + x + dx) << 8) | under[x + dx];
                    screen[base + x + dx] = span.colour;
                }
        return saved;
    }

    inline void RestoreWeather8(const WeatherPixels& saved, uint8_t* screen)
    {
        // Separate passes can hit one pixel. Undo in reverse order when the
        // saved colours came from the display rather than the clean RAM frame.
        for (size_t i = saved.count; i != 0;)
        {
            const uint32_t pixel = saved.pixels[--i];
            screen[pixel >> 8] = static_cast<uint8_t>(pixel);
        }
    }

    inline void OverlayWeather(const WeatherSpan* spans, size_t count, const WeatherPixels& saved,
                               uint8_t* screen, int stride)
    {
        for (size_t i = 0; i < count; i++)
        {
            const auto& span = spans[i];
            auto* row = screen + size_t(span.y) * stride;
            for (int n = 0, x = span.x; n < span.count; n++, x += span.step)
                for (int dx = 0; dx < span.width; dx++)
                    if (!(span.y > saved.top && span.y < saved.bottom
                          && x + dx > saved.left && x + dx < saved.right)) row[x + dx] = span.colour;
        }
    }
}
