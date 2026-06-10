#pragma GCC push_options
#pragma GCC optimize ("O3")

// ---------------------------------------------------------------------------
// Byte-aligned row processor — 1-bit fonts
// ---------------------------------------------------------------------------
// Processes one glyph row by iterating over framebuffer bytes instead of
// pixels. For each byte: extracts up to 8 bitmap bits into a single mask,
// applies head/tail masks, then performs exactly ONE read-modify-write on
// the framebuffer.  Eliminates all per-pixel drawPixel() calls and RMWs.
// ---------------------------------------------------------------------------
template <GfxRenderer::Orientation orientation, GfxRenderer::RenderMode renderMode>
__attribute__((always_inline)) static inline void renderCharRow1Bit(uint8_t* const fb,
                              const uint8_t* const bitmap,
                              int rowOffset, int byteStart, int byteEnd,
                              uint8_t headMask, uint8_t tailMask,
                              int glyphWidth, int pixelOffset) {
  // BW only — grayscale passes are no-ops for 1-bit fonts.
  // FB bit: 0 = ink (black), 1 = no-ink (white).
  // mask has 1-bits where pixels should be drawn → clear those bits.
  for (int b = byteStart; b <= byteEnd; b++) {
    uint8_t mask = 0;
    for (int p = 0; p < 8; p++) {
      const int pixelIdx = pixelOffset + b * 8 + p;
      if (pixelIdx < glyphWidth) {
        const uint8_t byte = bitmap[pixelIdx >> 3];
        if ((byte >> (7 - (pixelIdx & 7))) & 1) {
          mask |= (1 << (7 - p));  // MSB-first, aligns with FB
        }
      }
    }
    if (b == byteStart && b == byteEnd) {
      mask &= headMask;  // combined head+tail for single-byte glyphs
    } else {
      if (b == byteStart) mask &= headMask;
      if (b == byteEnd)   mask &= tailMask;
    }
    fb[rowOffset + b] &= ~mask;  // clear drawn bits (0 = ink)
  }
}

// ---------------------------------------------------------------------------
// Byte-aligned row processor — 2-bit fonts
// ---------------------------------------------------------------------------
// Extracts 8 pixels per framebuffer byte from the 2-bit bitmap, builds a
// single mask per renderMode pass, applies head/tail masks, then performs
// exactly ONE RMW per framebuffer byte.
// ---------------------------------------------------------------------------
template <GfxRenderer::Orientation orientation, GfxRenderer::RenderMode renderMode>
__attribute__((always_inline)) static inline void renderCharRow2Bit(uint8_t* const fb,
                              const uint8_t* const bitmap,
                              int rowOffset, int byteStart, int byteEnd,
                              uint8_t headMask, uint8_t tailMask,
                              int glyphWidth, int pixelOffset) {
  // FB bit: 0 = ink (BW), 1 = gray (grayscale passes).
  // mask has 1-bits where pixels should be drawn.
  for (int b = byteStart; b <= byteEnd; b++) {
    uint8_t mask = 0;
    for (int p = 0; p < 8; p++) {
      const int pixelIdx = pixelOffset + b * 4 + p / 2;  // 4 pixels per bitmap byte
      if (pixelIdx < glyphWidth) {
        const uint8_t byte = bitmap[pixelIdx >> 2];
        const uint8_t val = ((byte >> ((3 - (pixelIdx & 3)) * 2)) & 0x3);
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
    }
    if (b == byteStart && b == byteEnd) {
      mask &= headMask;
    } else {
      if (b == byteStart) mask &= headMask;
      if (b == byteEnd)   mask &= tailMask;
    }
    // BW: clear drawn bits (0 = ink). Grayscale: set drawn bits (1 = gray).
    if constexpr (renderMode == GfxRenderer::BW) {
      fb[rowOffset + b] &= ~mask;
    } else {
      fb[rowOffset + b] |= mask;
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
  // FIXME: should use orientation, not rotation
  if constexpr (rotation == TextRotation::Rotated90CW) {
    const int ob = cursorX + fontData->ascender - top;
    const int ib = cursorY - left;
    if (!renderer.glyphIntersectsStrip(ob, ib - (width - 1), ob + height - 1, ib)) {
      return;
    }
  } else {
    const int gx0 = cursorX + left;
    const int gy0 = cursorY - top;
    if (!renderer.glyphIntersectsStrip(gx0, gy0, gx0 + width - 1, gy0 + height - 1)) {
      return;
    }
  }

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (bitmap == nullptr) return;

  // --- Hoisted: strip target selection ---
  uint8_t* fb = renderer.getWriteTarget();
  int originY = renderer.getWriteOriginY();
  int writeRows = renderer.getWriteRows();

  // For Normal text: outer loop = glyphY (→ screenY), inner = glyphX (→ screenX)
  // For Rotated90CW: outer loop = glyphY (→ screenX), inner = glyphX (→ screenY, reversed)
  const int outerBase = cursorY - top;   // screenY base for Normal text
  const int innerBase = cursorX + left;  // screenX base for Normal text

  if (is2Bit) {
    // --- 2-bit: byte-aligned row processor ---
    for (int glyphY = 0; glyphY < height; glyphY++) {
      // Compute physical Y range for this glyph row.
      // The glyph row spans logical coords:
      //   Normal:     logY = outerBase+glyphY,  logX ∈ [innerBase .. innerBase+width-1]
      //   Rotated90CW: screenX = outerBase+glyphY, screenY ∈ [innerBase-(innerBase), innerBase-(innerBase+width-1)]
      int logY0, logY1, logX0, logX1;
      // FIXME: should use orientation, not rotation
      if constexpr (rotation == TextRotation::Rotated90CW) {
        // outerBase = cursorX + fontData->ascender - top  → this is screenX for rotated
        // innerBase = cursorY - left                       → this is screenY base
        const int screenX = outerBase + glyphY;
        logY0 = innerBase - (width - 1);   // screenY at glyphX=width-1 (reversed)
        logY1 = innerBase;                 // screenY at glyphX=0
        logX0 = screenX;
        logX1 = screenX;
      } else {
        logY0 = outerBase + glyphY;
        logY1 = outerBase + glyphY;
        logX0 = innerBase;
        logX1 = innerBase + width - 1;
      }

      int px0, py0, px1, py1;
      rotateCoordinates(orientation, logX0, logY0, &px0, &py0, renderer.getDisplayWidth(), renderer.getDisplayHeight());
      rotateCoordinates(orientation, logX1, logY1, &px1, &py1, renderer.getDisplayWidth(), renderer.getDisplayHeight());

      const int phyRowMin = (py0 < py1) ? py0 : py1;
      const int phyRowMax = (py0 > py1) ? py0 : py1;

      // Clip to strip bounds
      if (phyRowMax < originY || phyRowMin >= originY + writeRows) continue;

      const int rowOffsetStart = std::max(phyRowMin - originY, 0);
      const int rowOffsetEnd = std::min(phyRowMax - originY, writeRows - 1);

      // Compute physical X range for head/tail masks
      int pxHead, pyHead, pxTail, pyTail;
      rotateCoordinates(orientation, logX0, logY0, &pxHead, &pyHead, renderer.getDisplayWidth(), renderer.getDisplayHeight());
      rotateCoordinates(orientation, logX1, logY0, &pxTail, &pyTail, renderer.getDisplayWidth(), renderer.getDisplayHeight());

      const int phyXMin = (pxHead < pxTail) ? pxHead : pxTail;
      const int phyXMax = (pxHead > pxTail) ? pxHead : pxTail;

      // Compute byte range within the physical row
      const int byteStart = phyXMin >> 3;
      const int byteEnd = phyXMax >> 3;
      const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (phyXMin & 7));
      const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (phyXMax & 7)));

      // Pixel offset into the bitmap for this row (4 pixels per bitmap byte for 2-bit)
      const int pixelOffset = glyphY * ((width + 3) / 4);

      // Dispatch to byte-aligned row processor — constexpr-if on renderMode eliminates
      // dead branches at compile time, so a single loop covers all three passes.
      for (int rowOff = rowOffsetStart; rowOff <= rowOffsetEnd; rowOff++) {
        renderCharRow2Bit<orientation, renderMode>(
            fb + rowOff * renderer.getDisplayWidthBytes(),
            bitmap, rowOff, byteStart, byteEnd, headMask, tailMask, width, pixelOffset);
      }
    }
  } else {
    // --- 1-bit: byte-aligned row processor ---
    for (int glyphY = 0; glyphY < height; glyphY++) {
      int logY0, logY1, logX0, logX1;
      // FIXME: should use orientation, not rotation
      if constexpr (rotation == TextRotation::Rotated90CW) {
        const int screenX = outerBase + glyphY;
        logY0 = innerBase - (width - 1);
        logY1 = innerBase;
        logX0 = screenX;
        logX1 = screenX;
      } else {
        logY0 = outerBase + glyphY;
        logY1 = outerBase + glyphY;
        logX0 = innerBase;
        logX1 = innerBase + width - 1;
      }

      int px0, py0, px1, py1;
      rotateCoordinates(orientation, logX0, logY0, &px0, &py0, renderer.getDisplayWidth(), renderer.getDisplayHeight());
      rotateCoordinates(orientation, logX1, logY1, &px1, &py1, renderer.getDisplayWidth(), renderer.getDisplayHeight());

      const int phyRowMin = (py0 < py1) ? py0 : py1;
      const int phyRowMax = (py0 > py1) ? py0 : py1;

      // Clip to strip bounds
      if (phyRowMax < originY || phyRowMin >= originY + writeRows) continue;

      const int rowOffsetStart = std::max(phyRowMin - originY, 0);
      const int rowOffsetEnd = std::min(phyRowMax - originY, writeRows - 1);

      // Compute physical X range for head/tail masks
      int pxHead, pyHead, pxTail, pyTail;
      rotateCoordinates(orientation, logX0, logY0, &pxHead, &pyHead, renderer.getDisplayWidth(), renderer.getDisplayHeight());
      rotateCoordinates(orientation, logX1, logY0, &pxTail, &pyTail, renderer.getDisplayWidth(), renderer.getDisplayHeight());

      const int phyXMin = (pxHead < pxTail) ? pxHead : pxTail;
      const int phyXMax = (pxHead > pxTail) ? pxHead : pxTail;

      // Compute byte range within the physical row
      const int byteStart = phyXMin >> 3;
      const int byteEnd = phyXMax >> 3;
      const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (phyXMin & 7));
      const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (phyXMax & 7)));

      // Pixel offset into the bitmap for this row (8 pixels per bitmap byte for 1-bit)
      const int pixelOffset = glyphY * ((width + 7) / 8);

      // Dispatch to byte-aligned row processor — BW only, grayscale passes are no-ops.
      for (int rowOff = rowOffsetStart; rowOff <= rowOffsetEnd; rowOff++) {
        renderCharRow1Bit<orientation, renderMode>(
            fb + rowOff * renderer.getDisplayWidthBytes(),
            bitmap, rowOff, byteStart, byteEnd, headMask, tailMask, width, pixelOffset);
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

__attribute__((always_inline)) static inline void dispatchRenderCharImplRotated(const GfxRenderer& renderer,
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
