# Optimization Plan: Byte-Aligned `renderCharImpl`

## Current Bottleneck Analysis

For a typical 12×20 glyph, the current code:
1. Iterates **240 pixels** (glyphY × glyphX)
2. For each pixel: computes screenX/screenY, extracts bitmap bit, then calls `renderer.drawPixel()`
3. Each `drawPixel()` call: rotates coordinates, bounds-checks, computes byte/bit index, then does **1 RMW** on framebuffer

**Total per pixel**: 1 function call + 1 RMW = **240 calls + 240 RMWs** for a 12×20 glyph

## Optimization Strategy

Process **whole bytes at a time** (8 pixels for 1-bit, 8 pixels for 2-bit — two bitmap bytes per framebuffer byte):
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
                              uint8_t headMask, uint8_t tailMask,
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
// Extract 8 pixels from bitmap (2 bytes), build dark/light masks per bit position
uint8_t darkMask = 0;   // bit set for black(val=3) or dark gray(val=2)
uint8_t lightMask = 0;  // bit set for dark gray(val=2) or light gray(val=1)

for (int p = 0; p < 8; p++) {          // 8 pixels per framebuffer byte
    if (pixelIdx < glyphWidth) {
        uint8_t byte = bitmap[pixelIdx >> 2];
        uint8_t val = (byte >> ((3 - (pixelIdx & 3)) * 2)) & 0x3;
        // val: 0=white, 1=light gray, 2=dark gray, 3=black

        uint8_t fbBit = 7 - p;         // MSB-first, aligns with FB
        if (val >= 2) darkMask |= (1 << fbBit);           // black or dark gray
        if (val >= 1 && val <= 2) lightMask |= (1 << fbBit);  // dark or light gray

        pixelIdx++;
    }
}

// Apply head/tail masks (per-bit, not per-nibble — see §6 below)
darkMask &= headMask;
darkMask &= tailMask;
lightMask &= headMask;
lightMask &= tailMask;
```

**Grayscale is rendered in three separate passes** over the same glyph row.
`GfxRenderer` has only one `frameBuffer`. Grayscale planes are created by copying the
same buffer to different display RAM (`copyGrayscaleLsbBuffers` → BW RAM,
`copyGrayscaleMsbBuffers` → RED RAM). Each render pass writes its own masks:

- **BW**:   `(fb[rowOffset] & ~darkMask) | (drawMask & darkMask)` — black + dark gray + light gray
- **LSB**:  `(fb[rowOffset] & ~lightMask) | (lsbMask & lightMask)` — dark gray only
- **MSB**:  `(fb[rowOffset] & ~lightMask) | (msbMask & lightMask)` — dark gray + light gray

For `BW`, the mask selects which pixels to set/clear according to `drawMask` (from the
`black` parameter). For grayscale passes, `lsbMask`/`msbMask` are both `0xFF` (always
set bit = ink) because a "gray" pixel always writes ink to its respective plane.

### 3a. Grayscale Pass Architecture

The rendering pipeline issues **three independent render passes** per glyph row.
BW renders first (no clear), then each grayscale pass clears the framebuffer first
(`clearScreen(0x00)`) and overwrites it. The BW content is saved before the sequence
via `storeBwBuffer()` and restored after `displayGrayBuffer()` via `restoreBwBuffer()`,
placing it on top of the grayscale planes for final display.

```cpp
// Pass 1 — BW (black text overgrays): black + dark gray + light gray
renderer.setRenderMode(GfxRenderer::BW);
renderCharRow2Bit<orientation, rotation, BW>(...);             // writes darkMask

// Pass 2 — GRAYSCALE_LSB (BW RAM): dark gray only
renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
renderer.clearScreen(0x00);
renderCharRow2Bit<orientation, rotation, GRAYSCALE_LSB>(...);  // writes lightMask

// Pass 3 — GRAYSCALE_MSB (RED RAM): dark + light gray
renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
renderer.clearScreen(0x00);
renderCharRow2Bit<orientation, rotation, GRAYSCALE_MSB>(...);  // writes lightMask
```

Each pass independently computes masks and does its own RMW on the shared
`frameBuffer`. The display hardware then interprets the two RAM planes together:

| BW RAM | RED RAM | Display color |
|--------|---------|---------------|
| 1      | 1       | white         |
| 1      | 0       | light gray    |
| 0      | 0       | dark gray     |
| 0      | 1       | black         |
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

## Expected Performance Improvement per Render Pass

| Metric                         | Before | After                    | Improvement  |
|--------------------------------|--------|--------------------------|--------------|
| Function calls per 12×20 glyph | 240    | ~0                       | Eliminated   |
| RMW operations per 12×20 glyph | 240    | ~40 (2 bytes × 20 rows)  | **6× fewer** |
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

### Head/Tail Masks — 2-bit Mode (Per-Bit)

**Same approach as 1-bit mode.** The masks operate on individual bit positions within
each framebuffer byte (8 pixels per byte), not nibble boundaries. This is because the
fb layout is still **1-bit-per-pixel**; the 2-bit data from the font bitmap is decoded
into separate `darkMask` and `lightMask` bytes where each bit corresponds to one pixel
column.

```cpp
// Head mask (first byte): clear bits to the left of glyph start
uint8_t headMask = 0xFF >> (startX & 7);  // e.g., startX=5 → 0xE0 (bits 2-0 cleared)

// Tail mask (last byte): clear bits to the right of glyph end
uint8_t tailMask = 0xFF << (7 - (endX & 7));  // e.g., endX=10 → 0x03 (bits 7-3 cleared)

// Mid bytes: no masking needed (all 8 bits are within glyph bounds)
// Combined mask for first-and-last byte case:
if (byteStart == byteEnd) {
    headMask &= tailMask;
    tailMask = 0;  // avoid double-applying
}
```

### Grayscale Mode Handling

Grayscale rendering uses **three separate render passes** over the same glyph row, each
writing masks into the single `frameBuffer`:

| Pass     | RenderMode       | What gets drawn                              | Mask used  | Write action                                  |
|----------|------------------|----------------------------------------------|------------|-----------------------------------------------|
| BW       | BW               | Black + dark gray + light gray (val=0,1,2)   | darkMask   | `fb &= ~darkMask; fb |= drawMask & darkMask`  |
| LSB      | GRAYSCALE_LSB    | Dark gray (val=2) only                       | lightMask  | `fb &= ~lightMask; fb |= lsbMask & lightMask` |
| MSB      | GRAYSCALE_MSB    | Dark gray + light gray (val=1 or 2)          | lightMask  | `fb &= ~lightMask; fb |= msbMask & lightMask` |

- `lsbMask = 0xFF` in LSB pass — dark gray always writes ink to BW RAM
- `msbMask = 0xFF` in MSB pass — dark+light gray always write ink to RED RAM
- `drawMask` is derived from the `black` parameter in BW mode (set bit for black, clear for white)
- White pixels (val=3) are never drawn — their bits stay at 1 (no ink)

This three-pass approach matches the existing rendering pipeline: after all glyphs in a
row are rendered, `copyGrayscaleLsbBuffers(frameBuffer)` and
`copyGrayscaleMsbBuffers(frameBuffer)` copy the same framebuffer to the two display RAM
planes. The hardware interprets the BW+RED combination as 4 gray levels (see §3a table).

### Strip Mode

Already handled transparently by `getWriteTarget()` and `getWriteOriginY()`:
- When `_stripActive`, these return the scratch buffer pointer and its physical-row origin
- The row offset is subtracted from the physical Y to get the scratch-buffer-relative index
- Pixels outside the band are clipped via the strip bounds check before the byte loop

### Code Size Budget

24 template instantiations × ~80 bytes each ≈ **~2 KB** of code size. This is acceptable against the ESP32-C3's 16 MB flash.

### renderCharScaled — Deferred to v2

`renderCharScaled` (in `GfxRenderer.cpp`, used for SUP/SUB text) still uses pixel-by-pixel
rendering with `drawPixel()` calls. It is called far less frequently than `renderCharImpl` and
is out of scope for this optimization. Will be byte-aligned in a follow-up PR.

### Memory Safety (ESP32-C3 RISC-V)

- No heap allocation in the hot path — all buffers are on the stack or point to existing framebuffer
- All reads from `bitmap` pointer: glyph bitmaps are in IRAM (flash) or font cache decompressor output, both safe
- `restrict` qualifiers on pointers tell the compiler they don't alias, enabling better optimization

## Implementation Steps

1. **Create `renderCharRow1Bit`** template — byte-aligned 1-bit row processor with head/tail masks (same as §2)
2. **Create `renderCharRow2Bit`** template — byte-aligned 2-bit row processor that:
   - Extracts 8 pixels per framebuffer byte (2 bitmap bytes), builds `darkMask` + `lightMask`
   - Applies head/tail per-bit masks
   - Performs a single RMW using the precomputed mask and the renderMode-specific write pattern
3. **Refactor `renderCharImpl`** to:
   - Hoist orientation rotation, strip target, physical coordinates upfront (unchanged)
   - Compute per-row byte range and head/tail masks before the row loop (unchanged)
   - For 2-bit fonts: call `renderCharRow2Bit` once — the function internally
     applies the mask pattern for the current `renderMode`, so one call handles all modes
   - The **three-pass** architecture from §3a is handled outside this function: the caller
     invokes `renderCharImpl` with BW, then GRAYSCALE_LSB (after clearScreen), then GRAYSCALE_MSB
     (after clearScreen)
   - **Store/Restore**: before the three-pass sequence, the caller must call
     `renderer.storeBwBuffer()` to save the BW framebuffer. After `displayGrayBuffer()`
     and `restoreBwBuffer()`, the BW content is restored on top of the grayscale planes.
     1-bit fonts (no grayscale) skip this entirely — they render directly to the
     framebuffer in a single pass.
4. **Verify correctness**: test all 24 orientation/rotation/mode combinations against existing output
5. **Benchmark**: measure rendering time with profiling (the existing `start_ms` timing in `clearScreen`)

## Files to Modify

- `lib/GfxRenderer/GfxRenderer.cpp`: Replace the pixel-loop versions of `renderCharImpl<TextRotation::None>` and `renderCharImpl<TextRotation::Rotated90CW>` with the byte-aligned versions
- `TODO.md`: This file — tracks the optimization plan and implementation status
