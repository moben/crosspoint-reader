#include "GfxRenderer.h"

#include <BidiUtils.h>
#include <FontDecompressor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>

#include "FontCacheManager.h"

#pragma GCC push_options
#pragma GCC optimize ("O3")

// ---------------------------------------------------------------------------
// Byte-aligned stride processor — 1-bit fonts
// ---------------------------------------------------------------------------
// Processes one glyph row by iterating over framebuffer bytes in the physical
// X direction. For each byte: extracts up to 8 bitmap bits into a single mask,
// applies boundary masking derived from pixelOffset/glyphWidth (no head/tail
// mask parameters needed), then performs exactly ONE read-modify-write on the
// framebuffer. Eliminates all per-pixel drawPixel() calls and RMWs.
// ---------------------------------------------------------------------------
template <GfxRenderer::Orientation orientation, GfxRenderer::RenderMode renderMode>
__attribute__((always_inline)) static inline void renderCharStride1Bit(uint8_t* const fb,
                              const uint8_t* const bitmap,
                              int rowOffset, int byteStart, int byteEnd,
                              int glyphWidth, int pixelOffset) {
  // BW only — grayscale passes are no-ops for 1-bit fonts.
  // FB bit: 0 = ink (black), 1 = no-ink (white).
  // mask has 1-bits where pixels should be drawn → clear those bits.
  const int firstBytePixelStart = byteStart * 8;  // physical pixel index of byteStart's LSB
  const int lastBytePixelEnd    = (byteEnd + 1) * 8 - 1;

  for (int b = byteStart; b <= byteEnd; b++) {
    uint8_t mask = 0;
    const int byteGlobalStart = pixelOffset + (b - byteStart) * 8;  // global bitmap pixel index at start of this byte

    for (int p = 0; p < 8; p++) {
      const int globalPixelIdx = byteGlobalStart + p;  // global bitmap pixel index

      // Boundary check: skip if outside glyph width
      if (globalPixelIdx >= glyphWidth) break;

      // Boundary check: skip if this bit position is before the glyph starts in this byte
      // (handles the case where the glyph starts mid-byte at byteStart)
      const int localInByte = p - (byteGlobalStart % 8);
      if (localInByte < 0) continue;

      const uint8_t byte = bitmap[globalPixelIdx >> 3];
      if ((byte >> (7 - (globalPixelIdx & 7))) & 1) {
        mask |= (1 << (7 - p));  // MSB-first, aligns with FB
      }
    }

    fb[rowOffset + b] &= ~mask;  // clear drawn bits (0 = ink)
  }
}

// ---------------------------------------------------------------------------
// Byte-aligned stride processor — 2-bit fonts
// ---------------------------------------------------------------------------
// Extracts 8 pixels per framebuffer byte from the 2-bit bitmap, builds a
// single mask per renderMode pass, applies boundary masking based on
// pixelOffset/glyphWidth, then performs exactly ONE RMW per framebuffer byte.
// ---------------------------------------------------------------------------
template <GfxRenderer::Orientation orientation, GfxRenderer::RenderMode renderMode>
__attribute__((always_inline)) static inline void renderCharStride2Bit(uint8_t* const fb,
                              const uint8_t* const bitmap,
                              int glyphWidth,
                              int glyphHeight,
                              int pGlyphY,
                              int pixelOffset) {
  const auto get_ink = []() __attribute__((always_inline)) {
    
  };
  const auto pMaxX = (orientation == GfxRenderer::Portrait || orientation == GfxRenderer::PortraitInverted)
    ? glyphHeight
    : glyphWidth;
  const auto endByte = ((pixelOffset + pMaxX - 1) / 8);
  
  // FB bit: 0 = ink (BW), 1 = gray (grayscale passes).
  // mask has 1-bits where pixels should be drawn.
  for (int b = 0; b <= endByte; b++) {
    uint8_t mask = 0;

    for (int p = 0; p < 8; p++) {
      if (b==0 && p < pixelOffset) continue; 
      if (b==endByte && p > (pixelOffset + pMaxX - 1) % 8) continue;

      const int globalPixelIdx = byteGlobalStart + p;
      if (globalPixelIdx >= glyphWidth) break;  // past the end of the glyph width

      const uint8_t byte = bitmap[globalPixelIdx >> 2];
      const uint8_t val = ((byte >> ((3 - (globalPixelIdx & 3)) * 2)) & 0x3);
      // val: 0=white, 1=light-gray, 2=dark-gray, 3=black
      // Inverted for display: 0=black, 1=dark-grey, 2=light-grey, 3=white
      const uint8_t bmpVal = 3 - val;

      if constexpr (renderMode == GfxRenderer::BW) {
        // BW pass: draw all non-white pixels (val < 3 → bmpVal > 0)
        if (bmpVal > 0) mask |= (1 << (7 - p));
      } else if constexpr (renderMode == GfxRenderer::GRAYSCALE_LSB) {
        // GRAYSCALE_LSB: dark gray only (bmpVal == 1)
        if (bmpVal == 1) mask |= (1 << (7 - p));
      } else {  // GRAYSCALE_MSB
        // GRAYSCALE_MSB: dark + light gray (bmpVal == 1 || bmpVal == 2)
        if (bmpVal == 1 || bmpVal == 2) mask |= (1 << (7 - p));
      }
    }

    // BW: clear drawn bits (0 = ink). Grayscale: set drawn bits (1 = gray).
    if constexpr (renderMode == GfxRenderer::BW) {
      fb[b] &= ~mask;
    } else {
      fb[b] |= mask;
    }
  }
}


// ---------------------------------------------------------------------------
// Byte-aligned renderCharImpl — orientation-specialized template
// ---------------------------------------------------------------------------
// Adds <orientation, renderMode> as template parameters so that:
//   - Physical coordinate math is fully inlined (no runtime switch)
//     rotateCoordinates() is always_inline; with a constant orientation the
//     compiler DCEs all dead switch arms at compile time.
//   - Dead branches for renderMode are eliminated by constexpr-if
//   - The three-pass grayscale architecture stays at the activity level
// ---------------------------------------------------------------------------

template <GfxRenderer::Orientation orientation, GfxRenderer::RenderMode renderMode>
static void renderCharImpl(const GfxRenderer& renderer,
                           const EpdFontFamily& fontFamily, const uint32_t cp,
                           int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  // Tiled-grayscale band culling: if this glyph's physical y-extent is entirely
  // outside the active strip, skip it before the expensive bitmap decode. This
  // is what makes per-band re-rendering cheap. No-op outside strip mode.
  // 
  // Cursor base positions for glyph rendering (logical coordinates)
  // 
  // TODO what is the Y direction of cursor? up or down?
  // Also, if it's down (as indicated by `-top`), shouldn't it also be `-height`?
  const int cursorBaseX = cursorX + left;             // logical X of glyph's left edge
  const int cursorBaseY = cursorY - top;              // logical Y of glyph's top edge
  const int cursorEndX  = cursorBaseX + width - 1;     // logical X of glyph's right edge
  const int cursorEndY  = cursorBaseY + height - 1;    // logical Y of glyph's bottom edge
  if (!renderer.glyphIntersectsStrip(cursorBaseX, cursorBaseY, cursorEndX, cursorEndY)) {
    return;
  }

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (bitmap == nullptr) return;

  // --- Hoisted: strip target selection ---
  uint8_t* fb = renderer.getWriteTarget();
  int originY = renderer.getWriteOriginY();
  int writeRows = renderer.getWriteRows();
  const uint16_t displayWidth = renderer.getDisplayWidth();
  const uint16_t displayHeight = renderer.getDisplayHeight();

  int pBaseX, pBaseY, pEndX, pEndY;
  rotateCoordinates(orientation, cursorBaseX, cursorBaseY, &pBaseX, &pBaseY, displayWidth, displayHeight);
  rotateCoordinates(orientation, cursorEndX, cursorEndY, &pEndX, &pEndY, displayWidth, displayHeight);

  const auto [pLeftY, pRightY] = std::minmax({pBaseY, pEndY});
  const int pClipLeftY = std::max({0, originY - pLeftY});
  const int pClipRightY = std::max({0, pRightY - (originY+writeRows) + 1});
  
  // TODO can probably be expressed in terms of p*
  const auto pMaxY = (orientation == GfxRenderer::Portrait || orientation == GfxRenderer::PortraitInverted)
    ? width
    : height;
  
  if (is2Bit) {
    // --- 2-bit: byte-aligned row processor ---
    for (int pGlyphY = 0+pClipLeftY; pGlyphY < pMaxY-pClipRightY; pGlyphY++) {
      const int displayPxX = pBaseX;
      const int displayPxY = pBaseY + pGlyphY;

      const auto fbStartPx = (displayPxY + displayPxX);
      const auto fbStartByte = fbStartPx / 8;
      // TODO can be hoisted if display width is a multiple of byte
      const auto fbStartPxOffset = fbStartPx - (fbStartByte * 8);

      renderCharStride2Bit<orientation, renderMode>(
          fb + fbStartByte,
          bitmap,
          width,
          height,
          pGlyphY,
          fbStartPxOffset
      );

      // // Each glyph row spans logical coords [cursorBaseX .. cursorBaseX+width-1] at fixed logical Y.
      // const int logY = cursorBaseY + glyphY;
      // const int logX0 = cursorBaseX;
      // const int logX1 = cursorBaseX + width - 1;

      // int px0, py0, px1, py1;
      // rotateCoordinates(orientation, logX0, logY, &px0, &py0, displayWidth, displayHeight);
      // rotateCoordinates(orientation, logX1, logY, &px1, &py1, displayWidth, displayHeight);

      // const int phyRowMin = (py0 < py1) ? py0 : py1;
      // const int phyRowMax = (py0 > py1) ? py0 : py1;

      // // Clip to strip bounds
      // if (phyRowMax < originY || phyRowMin >= originY + writeRows) continue;

      // const int rowOffsetStart = std::max(phyRowMin - originY, 0);
      // const int rowOffsetEnd = std::min(phyRowMax - originY, writeRows - 1);

      // // Compute physical X range for byte boundary masking
      // const int phyXMin = (px0 < px1) ? px0 : px1;
      // const int phyXMax = (px0 > px1) ? px0 : px1;

      // // Compute byte range within the physical row
      // const int byteStart = phyXMin >> 3;
      // const int byteEnd = phyXMax >> 3;

      // // pixelOffset: total bitmap pixel index at the start of this glyph row.
      // // Always width * glyphY — the flat bitmap is stored row-major with `width` pixels per row,
      // // regardless of bit-depth (2-bit packs 4 pixels/byte but the logical stride is still `width`).
      // const int pixelOffset = glyphY * width;

      // // Dispatch to byte-aligned stride processor — constexpr-if on renderMode eliminates
      // // dead branches at compile time, so a single loop covers all three passes.
      // for (int rowOff = rowOffsetStart; rowOff <= rowOffsetEnd; rowOff++) {
      //   renderCharStride2Bit<orientation, renderMode>(
      //       fb + rowOff * renderer.getDisplayWidthBytes(),
      //       bitmap, rowOff, byteStart, byteEnd, width, pixelOffset);
      // }
    }
  } else {
    // --- 1-bit: byte-aligned row processor ---
    for (int glyphY = 0; glyphY < height; glyphY++) {
      const int logY = cursorBaseY + glyphY;
      const int logX0 = cursorBaseX;
      const int logX1 = cursorBaseX + width - 1;

      int px0, py0, px1, py1;
      rotateCoordinates(orientation, logX0, logY, &px0, &py0, displayWidth, displayHeight);
      rotateCoordinates(orientation, logX1, logY, &px1, &py1, displayWidth, displayHeight);

      const int phyRowMin = (py0 < py1) ? py0 : py1;
      const int phyRowMax = (py0 > py1) ? py0 : py1;

      // Clip to strip bounds
      if (phyRowMax < originY || phyRowMin >= originY + writeRows) continue;

      const int rowOffsetStart = std::max(phyRowMin - originY, 0);
      const int rowOffsetEnd = std::min(phyRowMax - originY, writeRows - 1);

      // Compute physical X range for byte boundary masking
      const int phyXMin = (px0 < px1) ? px0 : px1;
      const int phyXMax = (px0 > px1) ? px0 : px1;

      // Compute byte range within the physical row
      const int byteStart = phyXMin >> 3;
      const int byteEnd = phyXMax >> 3;

      // pixelOffset: total bitmap pixel index at the start of this glyph row.
      // Always width * glyphY — the flat bitmap is stored row-major with `width` pixels per row,
      // regardless of bit-depth (1-bit packs 8 pixels/byte but the logical stride is still `width`).
      const int pixelOffset = glyphY * width;

      // Dispatch to byte-aligned stride processor — BW only, grayscale passes are no-ops.
      for (int rowOff = rowOffsetStart; rowOff <= rowOffsetEnd; rowOff++) {
        renderCharStride1Bit<orientation, renderMode>(
            fb + rowOff * renderer.getDisplayWidthBytes(),
            bitmap, rowOff, byteStart, byteEnd, width, pixelOffset);
      }
    }
  }
}


// Runtime dispatch helper — routes to the correct <orientation, renderMode>
// template instantiation. orientation and renderMode are runtime values on GfxRenderer
// but constant for the duration of a single drawText call, so the switch executes once
// per character (not per pixel).
__attribute__((always_inline)) static inline void dispatchRenderCharImpl(const GfxRenderer& renderer,
                                          const EpdFontFamily& fontFamily, uint32_t cp,
                                          int cursorX, int cursorY, bool black,
                                          EpdFontFamily::Style style) {
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      switch (renderer.getRenderMode()) {
        case GfxRenderer::BW:
          renderCharImpl<GfxRenderer::Portrait, GfxRenderer::BW>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
        case GfxRenderer::GRAYSCALE_LSB:
          renderCharImpl<GfxRenderer::Portrait, GfxRenderer::GRAYSCALE_LSB>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
        case GfxRenderer::GRAYSCALE_MSB:
          renderCharImpl<GfxRenderer::Portrait, GfxRenderer::GRAYSCALE_MSB>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
      }
      break;
    case GfxRenderer::LandscapeClockwise:
      switch (renderer.getRenderMode()) {
        case GfxRenderer::BW:
          renderCharImpl<GfxRenderer::LandscapeClockwise, GfxRenderer::BW>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
        case GfxRenderer::GRAYSCALE_LSB:
          renderCharImpl<GfxRenderer::LandscapeClockwise, GfxRenderer::GRAYSCALE_LSB>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
        case GfxRenderer::GRAYSCALE_MSB:
          renderCharImpl<GfxRenderer::LandscapeClockwise, GfxRenderer::GRAYSCALE_MSB>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
      }
      break;
    case GfxRenderer::PortraitInverted:
      switch (renderer.getRenderMode()) {
        case GfxRenderer::BW:
          renderCharImpl<GfxRenderer::PortraitInverted, GfxRenderer::BW>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
        case GfxRenderer::GRAYSCALE_LSB:
          renderCharImpl<GfxRenderer::PortraitInverted, GfxRenderer::GRAYSCALE_LSB>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
        case GfxRenderer::GRAYSCALE_MSB:
          renderCharImpl<GfxRenderer::PortraitInverted, GfxRenderer::GRAYSCALE_MSB>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
      }
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      switch (renderer.getRenderMode()) {
        case GfxRenderer::BW:
          renderCharImpl<GfxRenderer::LandscapeCounterClockwise, GfxRenderer::BW>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
        case GfxRenderer::GRAYSCALE_LSB:
          renderCharImpl<GfxRenderer::LandscapeCounterClockwise, GfxRenderer::GRAYSCALE_LSB>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
        case GfxRenderer::GRAYSCALE_MSB:
          renderCharImpl<GfxRenderer::LandscapeCounterClockwise, GfxRenderer::GRAYSCALE_MSB>(
              renderer, fontFamily, cp, cursorX, cursorY, black, style);
          break;
      }
      break;
  }
}

#pragma GCC pop_options
