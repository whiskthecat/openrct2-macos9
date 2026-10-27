#pragma once
#include "utypes.h"

#include <cstring>
#include <string>

namespace icu
{
    // Code points are held as UTF-32. Only what String.cpp calls is provided.
    class UnicodeString
    {
    public:
        UnicodeString() = default;
        explicit UnicodeString(const wchar_t* text)
        {
            // 16-bit wchar_t (Classic Mac toolchain): UTF-16 units.
            while (text != nullptr && *text != 0)
            {
                char32_t unit = static_cast<char16_t>(*text++);
                if (unit >= 0xD800 && unit < 0xDC00 && *text != 0)
                {
                    const char32_t low = static_cast<char16_t>(*text++);
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                }
                _text.push_back(unit);
            }
        }
        // Bytes in a named code page. Windows-1252 and UTF-8 are converted;
        // other pages keep their ASCII range and replace the rest.
        UnicodeString(const char* data, const char* codePage)
        {
            if (std::strcmp(codePage, "utf-8") == 0)
            {
                *this = fromUTF8(std::string(data));
                return;
            }
            static const char16_t high1252[32] = {
                0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                0x2039, 0x0152, 0x8D, 0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178,
            };
            const bool latin = std::strcmp(codePage, "windows-1252") == 0;
            for (const char* p = data; *p != 0; ++p)
            {
                const auto byte = static_cast<unsigned char>(*p);
                if (byte < 0x80)
                    _text.push_back(byte);
                else if (!latin)
                    _text.push_back(U'?');
                else if (byte < 0xA0)
                    _text.push_back(high1252[byte - 0x80]);
                else
                    _text.push_back(byte);
            }
        }
        static UnicodeString fromUTF8(const std::string& utf8)
        {
            UnicodeString result;
            for (size_t i = 0; i < utf8.size();)
            {
                const auto lead = static_cast<unsigned char>(utf8[i]);
                int extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
                char32_t value = extra == 3 ? lead & 0x07 : extra == 2 ? lead & 0x0F : extra == 1 ? lead & 0x1F : lead;
                ++i;
                for (; extra > 0 && i < utf8.size(); --extra, ++i)
                    value = (value << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
                result._text.push_back(value);
            }
            return result;
        }
        static UnicodeString fromUTF32(const UChar32* text, int32_t length)
        {
            UnicodeString result;
            result._text.assign(text, text + length);
            return result;
        }
        int32_t length() const
        {
            return static_cast<int32_t>(_text.size());
        }
        int32_t toUTF32(UChar32* destination, int32_t capacity, UErrorCode&) const
        {
            const auto count = length() < capacity ? length() : capacity;
            for (int32_t i = 0; i < count; i++)
                destination[i] = static_cast<UChar32>(_text[static_cast<size_t>(i)]);
            return count;
        }
        const char16_t* getBuffer()
        {
            _utf16.clear();
            for (const auto value : _text)
            {
                if (value >= 0x10000)
                {
                    _utf16.push_back(static_cast<char16_t>(0xD800 + ((value - 0x10000) >> 10)));
                    _utf16.push_back(static_cast<char16_t>(0xDC00 + ((value - 0x10000) & 0x3FF)));
                }
                else
                    _utf16.push_back(static_cast<char16_t>(value));
            }
            return _utf16.c_str();
        }
        std::string& toUTF8String(std::string& out) const
        {
            for (const auto value : _text)
            {
                if (value < 0x80)
                    out.push_back(static_cast<char>(value));
                else if (value < 0x800)
                {
                    out.push_back(static_cast<char>(0xC0 | (value >> 6)));
                    out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                }
                else if (value < 0x10000)
                {
                    out.push_back(static_cast<char>(0xE0 | (value >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xF0 | (value >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                }
            }
            return out;
        }
        // ASCII, Latin-1 and the common Latin Extended-A pairs.
        UnicodeString& toUpper()
        {
            for (auto& value : _text)
            {
                if ((value >= U'a' && value <= U'z') || (value >= 0xE0 && value <= 0xFE && value != 0xF7))
                    value -= 0x20;
                else if (value >= 0x100 && value < 0x180 && (value & 1) && value != 0x131 && value != 0x138)
                    value -= 1;
                else if (value >= 0x430 && value < 0x450)
                    value -= 0x20;
            }
            return *this;
        }

    private:
        std::u32string _text;
        std::u16string _utf16;
    };
} // namespace icu
