# Optimization Plan: Byte-Aligned `renderCharImpl`

## Current Bottleneck Analysis

For a typical 12×20 glyph, the current code:
1. Iterates **240 pixels** (glyphY × glyphX)
2. For each pixel: computes screenX/screenY, extracts bitmap bit, then calls `renderer.drawPixel()`
3. Each `drawPixel()` call: rotates coordinates, bounds-checks, computes byte/bit index, then does **1 RMW** on framebuffer

**Total per pixel**: 1 function call + 1 RMW = **240 calls + 240 RMWs** for a 12×20 glyph

## Optimization Strategy

Process **whole bytes at a time** (8 pixels for 1-bit, 4 pixels for 2-bit):
- Hoist all static calculations (orientation rotation, strip target, physical row offset, write pointer) **once** per glyph row
- Extract all 8 pixels from the bitmap into a single mask value
- Do **1 RMW** per framebuffer byte (not per pixel)

**Total per glyph row**: 1 RMW per framebuffer byte = ~2 RMWs for a 12-pixel row (vs 12 RMWs currently)

## Implementation Design

### 1. Static Helper: `renderCharRow` Template

A static template function that processes one glyph row by iterating over framebuffer bytes:

```cpp
// For 1-bit mode:
template <GfxRenderer::Orientation orientation, TextRotation rotation,
          GfxRenderer::RenderMode renderMode>
static void renderCharRow1Bit(const uint8_t* restrict fb,
                              const uint8_t* restrict bitmap,
                              int rowOffset, int byteStart, int byteEnd,
                              uint8_t headMask, uint8_t tailMask,
                              int glyphWidth, int pixelOffset);

// For 2-bit mode:
template <GfxRenderer::Orientation orientation, TextRotation rotation,
          GfxRenderer::RenderMode renderMode>
static void renderCharRow2Bit(const uint8_t* restrict fb,
                              const uint8_t* restrict bitmap,
                              int rowOffset, int byteStart, int byteEnd,
                              uint32_t headMask, uint32_t tailMask,
                              int glyphWidth, int pixelOffset);
```

### 2. Per-Byte Processing (1-bit)

For each framebuffer byte in `[byteStart, byteEnd]`:

```cpp
// Extract 8 pixels from bitmap, pack into ink mask
uint8_t inkMask = 0;
for (int p = 0; p < 8; p++) {
    if (pixelIdx < glyphWidth) {
        uint8_t byte = bitmap[pixelIdx >> 3];
        if ((byte >> (7 - (pixelIdx & 7))) & 1)
            inkMask |= (1 << (7 - p));  // MSB-first, aligns with FB
        pixelIdx++;
    }
}

// Apply head mask (first byte): clear bits left of glyph start
inkMask &= headMask;

// Apply tail mask (last byte): clear bits right of glyph end
inkMask &= tailMask;

// Single RMW — mode-specific
switch (renderMode) {
    case BW:
        fb[rowOffset] = (fb[rowOffset] & ~inkMask) | (inkMask & drawMask);
        break;
    case GRAY_MSB:
        fb[rowOffset] = (fb[rowOffset] & ~inkMask) | (inkMask & msbMask);
        break;
    case GRAY_LSB:
        fb[rowOffset] = (fb[rowOffset] & ~inkMask) | (inkMask & lsbMask);
        break;
}
```

### 3. Per-Byte Processing (2-bit)

For each framebuffer byte in `[byteStart, byteEnd]`:

```cpp
// Extract 4 pixels, pack dark/gray into 32-bit word
uint32_t packed = 0;  // [dark nibble][light nibble]
for (int p = 0; p < 4; p++) {
    if (pixelIdx < glyphWidth) {
        uint8_t byte = bitmap[pixelIdx >> 2];
        uint8_t val = (byte >> ((3 - (pixelIdx & 3)) * 2)) & 0x3;
        uint8_t bmpVal = 3 - val;  // 0=black, 1=dark, 2=light, 3=white

        if (bmpVal == 0 || bmpVal == 1) packed |= ((uint32_t)val << (p * 2));
        if (bmpVal == 1 || bmpVal == 2) packed |= ((uint32_t)val << (p * 2 + 16));

        pixelIdx++;
    }
}

// Apply head mask (clear unused high nibbles)
packed &= headMask;

// Apply tail mask (clear unused low nibbles)
packed &= tailMask;

// Single RMW per plane
fb[rowOffset] = (fb[rowOffset] & ~darkMask) | (packed >> 16 & darkMask);
fb[grayRowOffset] = (fb[grayRowOffset] & ~lightMask) | (packed & lightMask);
```

### 4. Main `renderCharImpl` — Static Calculations Upfront

```cpp
template <TextRotation rotation>
static void renderCharImpl(...) {
    // --- Glyph lookup (unchanged) ---
    const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
    const EpdFontData* fontData = fontFamily.getData(style);

    // --- Hoisted: orientation-aware physical coordinate mapping ---
    // Compute physical row offset and physical y for row 0 once
    uint8_t headMask, tailMask;  // per-byte masks for head/tail
    int byteStart, byteEnd;      // framebuffer byte range

    // --- Hoisted: strip target selection ---
    const auto& renderer = ...;
    uint8_t* fb = renderer.getWriteTarget();
    int rowOffsetBase = renderer.getWriteOriginY();
    int writeRows = renderer.getWriteRows();

    // --- Row loop ---
    for (int glyphY = 0; glyphY < height; glyphY++) {
        // Physical row offset computed once
        int rowOffset = computePhysicalRowOffset(orientation, rotation, ...);

        // Clip to strip bounds
        if (rowOffset < 0 || rowOffset >= writeRows) continue;

        // Byte range and masks computed once per row
        computeByteRangeAndMasks(..., &byteStart, &byteEnd, &headMask, &tailMask);

        // Dispatch to byte-aligned row processor
        if (is2Bit)
            renderCharRow2Bit<orientation, rotation, renderMode>(...);
        else
            renderCharRow1Bit<orientation, rotation, renderMode>(...);
    }
}
```

### 5. Compile-Time Orientation Specialization

The template `<orientation, rotation, renderMode>` produces **24 instantiations** (4 orientations × 2 rotations × 3 render modes). Each has the coordinate math fully inlined — no runtime switch in the pixel loop.

The compiler will eliminate dead branches (e.g., the `switch` for `orientation` becomes a single arithmetic expression).

## Expected Performance Improvement

| Metric                         | Before | After                    | Improvement  |
|--------------------------------|--------|--------------------------|--------------|
| Function calls per 12×20 glyph | 240    | ~0                       | Eliminated   |
| RMW operations per 12×20 glyph | 240    | ~30 (5 bytes × 2 planes) | **8× fewer** |
| Orientation switch hits        | 240    | 0                        | Eliminated   |
| Bounds check hits              | 240    | 0 (hoisted)              | Eliminated   |

## Key Considerations

### Head/Tail Masks — 1-bit Mode

The head and tail masks isolate only the bits that fall within the glyph's physical bounds:

- **Head mask** (first byte): clears bits to the left of the glyph start position
  ```cpp
  uint8_t headMask = 0xFF >> (startX & 7);  // e.g., startX=5 → 0xE0 (bits 2-0 cleared)
  ```

- **Tail mask** (last byte): clears bits to the right of the glyph end position
  ```cpp
  uint8_t tailMask = 0xFF << (7 - (endX & 7));  // e.g., endX=10 → 0x03 (bits 7-3 cleared)
  ```

- When `byteStart == byteEnd` (glyph fits in one byte), combine: `(headMask & tailMask)`

### Head/Tail Masks — 2-bit Mode

In 2-bit mode, each nibble (4 bits) holds 2 pixels, so masks operate on **nibble boundaries**:

- **First valid nibble index**: `firstNibble = startX / 4`
- **Last valid nibble index**: `lastNibble = endX / 4`

```cpp
// Head mask: clear all nibbles BEFORE firstNibble
uint32_t headMask;
if (startX == 0) headMask = 0x00000000;       // no nibbles to clear
else             headMask = 0xF0F0F0F0 >> (4 * firstNibble);

// Tail mask: clear all nibbles AFTER lastNibble
uint32_t tailMask = (1U << (4 * (lastNibble + 1))) - 1;  // e.g., lastNibble=2 → 0x00000FFF
```

### Nibbles Alignment — 2-bit Mode

The extraction packs pixels sequentially into nibbles 0-3 of the 32-bit word, but the framebuffer may require them at different nibble positions. A left-shift compensates:

```cpp
// Shift amount = starting position within the byte, in nibbles
int shiftAmount = (startX % 4) * 2;  // bits per pixel = 2
packed <<= shiftAmount;
```

### Grayscale Mode Handling

For `GRAY_MSB` and `GRAY_LSB`, dark gray and light gray pixels write to **different framebuffer planes**:

- Precompute `darkMask` (bits where pixel is black or dark gray) and `lightMask` (bits where pixel is white or light gray) per byte
- Do 1 RMW on the BW plane for dark, 1 RMW on the grayscale plane for light
- For `BW` mode, only the dark mask applies — white/light pixels are left untouched

### Strip Mode

Already handled transparently by `getWriteTarget()` and `getWriteOriginY()`:
- When `_stripActive`, these return the scratch buffer pointer and its physical-row origin
- The row offset is subtracted from the physical Y to get the scratch-buffer-relative index
- Pixels outside the band are clipped via the strip bounds check before the byte loop

### Code Size Budget

24 template instantiations × ~80 bytes each ≈ **~2 KB** of code size. This is acceptable against the ESP32-C3's 16 MB flash.

### Memory Safety (ESP32-C3 RISC-V)

- No heap allocation in the hot path — all buffers are on the stack or point to existing framebuffer
- All reads from `bitmap` pointer: glyph bitmaps are in IRAM (flash) or font cache decompressor output, both safe
- `restrict` qualifiers on pointers tell the compiler they don't alias, enabling better optimization

## Implementation Steps

1. **Create `renderCharRow1Bit`** template — byte-aligned 1-bit row processor with head/tail masks
2. **Create `renderCharRow2Bit`** template — byte-aligned 2-bit row processor with nibble-level masks and shift compensation
3. **Refactor `renderCharImpl`** to:
   - Hoist orientation rotation, strip target, physical coordinates upfront
   - Compute per-row byte range and head/tail masks before the pixel loop
   - Dispatch to the appropriate template function
4. **Verify correctness**: test all 24 orientation/rotation/mode combinations against existing output
5. **Benchmark**: measure rendering time with profiling (the existing `start_ms` timing in `clearScreen`)

## Files to Modify

- `lib/GfxRenderer/GfxRenderer.cpp`: Replace the pixel-loop versions of `renderCharImpl<TextRotation::None>` and `renderCharImpl<TextRotation::Rotated90CW>` with the byte-aligned versions
- `TODO.md`: This file — tracks the optimization plan and implementation status
