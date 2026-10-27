// Minimal stand-in for the parts of ICU the engine's String.cpp uses, so the
// game does not need the full ICU library on small targets.
#pragma once
#include <cstdint>
#define U_SIZEOF_WCHAR_T __SIZEOF_WCHAR_T__
typedef int32_t UChar32;
typedef int UErrorCode;
#define U_ZERO_ERROR 0
