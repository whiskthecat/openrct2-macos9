#pragma once
#include <cstdint>

// Indexed, non-overlapping RAM/framebuffer transfers. No Toolbox dependencies;
// the native backend and the NAS copy fixture use this exact implementation.
namespace PixelCopy {
using Copy8 = double __attribute__((may_alias));
using Copy4 = uint32_t __attribute__((may_alias));
inline void Row(uint8_t *out, const uint8_t *in, int count, bool wide) {
  while (count > 0 && (reinterpret_cast<uintptr_t>(out) & 7) != 0) {
    *out++ = *in++;
    count--;
  }
  if ((reinterpret_cast<uintptr_t>(in) & 7) == 0) {
    for (; count >= 32; count -= 32, out += 32, in += 32) {
      const auto *s = reinterpret_cast<const Copy8 *>(in);
      auto *d = reinterpret_cast<Copy8 *>(out);
      const double a = s[0], b = s[1], c = s[2], e = s[3];
      d[0] = a;
      d[1] = b;
      d[2] = c;
      d[3] = e;
    }
    if (wide)
      for (; count >= 8; count -= 8, out += 8, in += 8)
        *reinterpret_cast<Copy8 *>(out) = *reinterpret_cast<const Copy8 *>(in);
  }
  // Arbitrary source pitches need not preserve word alignment. The
  // production 8-bit canvas does, but readbacks/tests may not.
  if ((reinterpret_cast<uintptr_t>(in) & 3) == 0)
    for (; count >= 4; count -= 4, out += 4, in += 4)
      *reinterpret_cast<Copy4 *>(out) = *reinterpret_cast<const Copy4 *>(in);
  for (; count > 0; count--)
    *out++ = *in++;
}

template <unsigned Tail>
inline void AlignedRows(uint8_t *out, int outPitch, const uint8_t *in,
                        int inPitch, int width, int height) {
  const int bulk = width / 32;
  for (int y = 0; y < height; ++y, out += outPitch, in += inPitch) {
    const auto *s = reinterpret_cast<const Copy8 *>(in);
    auto *d = reinterpret_cast<Copy8 *>(out);
    for (int n = 0; n < bulk; ++n, s += 4, d += 4) {
      const double a = s[0], b = s[1], c = s[2], e = s[3];
      d[0] = a;
      d[1] = b;
      d[2] = c;
      d[3] = e;
    }
    if constexpr (Tail >= 1)
      d[0] = s[0];
    if constexpr (Tail >= 2)
      d[1] = s[1];
    if constexpr (Tail >= 3)
      d[2] = s[2];
  }
}

inline void Rect(uint8_t *out, int outPitch, const uint8_t *in, int inPitch,
                 int width, int height, bool aligned, bool wide) {
  if (width <= 0 || height <= 0)
    return;
  // Test once per rectangle, rather than align and dispatch every row.
  if (aligned && wide &&
      ((reinterpret_cast<uintptr_t>(out) | reinterpret_cast<uintptr_t>(in) |
        static_cast<uintptr_t>(outPitch | inPitch | width)) &
       7) == 0) {
    switch ((width / 8) & 3) {
    case 0:
      AlignedRows<0>(out, outPitch, in, inPitch, width, height);
      break;
    case 1:
      AlignedRows<1>(out, outPitch, in, inPitch, width, height);
      break;
    case 2:
      AlignedRows<2>(out, outPitch, in, inPitch, width, height);
      break;
    case 3:
      AlignedRows<3>(out, outPitch, in, inPitch, width, height);
      break;
    }
    return;
  }
  for (int y = 0; y < height; ++y, out += outPitch, in += inPitch)
    Row(out, in, width, wide);
}
} // namespace PixelCopy
