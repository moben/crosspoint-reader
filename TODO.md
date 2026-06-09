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
static void renderCharRow1Bit(const uint8_t* fb,
                              const uint8_t* bitmap,
                              int rowOffset, int byteStart, int byteEnd,
                              uint8_t headMask, uint8_t tailMask,
                              int glyphWidth, int pixelOffset);

// For 2-bit mode:
template <GfxRenderer::Orientation orientation, TextRotation rotation,
          GfxRenderer::RenderMode renderMode>
static void renderCharRow2Bit(const uint8_t* fb,
                              const uint8_t* bitmap,
                              int rowOffset, int byteStart, int byteEnd,
                              uint8_t headMask, uint8_t tailMask,
                              int glyphWidth, int pixelOffset);
```

### 2. Per-Byte Processing (1-bit)

1-bit fonts have only black and white glyphs — no gray levels. The grayscale passes (GRAYSCALE_LSB, GRAYSCALE_MSB) are no-ops for 1-bit fonts and are never invoked. Only the BW pass executes.

For each framebuffer byte in `[byteStart, byteEnd]`:

```cpp
// Extract 8 pixels from bitmap, pack into ink mask
uint8_t mask = 0;
for (int p = 0; p < 8; p++) {
    if (pixelIdx < glyphWidth) {
        uint8_t byte = bitmap[pixelIdx >> 3];
        if ((byte >> (7 - (pixelIdx & 7))) & 1)
            mask |= (1 << (7 - p));  // MSB-first, aligns with FB
        pixelIdx++;
    }
}

// Apply head mask (first byte): clear bits left of glyph start
mask &= headMask;

// Apply tail mask (last byte): clear bits right of glyph end
mask &= tailMask;

// Single RMW — BW only (grayscale passes are no-ops for 1-bit)
// FB bit: 0 = ink (black), 1 = no-ink (white)
// mask has 1-bits where pixels should be drawn → clear those bits
fb[rowOffset] &= ~mask;
```

### 3. Per-Byte Processing (2-bit)

2-bit fonts carry 4 gray levels (val 0–3). The function is called **once per render pass**
(BW, then GRAYSCALE_LSB, then GRAYSCALE_MSB) — each pass computes only the mask relevant
to its mode. The `renderMode` template parameter is a compile-time constant, so the compiler
eliminates dead branches.

For each framebuffer byte in `[byteStart, byteEnd]`:

```cpp
// Extract 8 pixels from bitmap (2 bytes), build mask per bit position
uint8_t mask = 0;
for (int p = 0; p < 8; p++) {          // 8 pixels per framebuffer byte
    if (pixelIdx < glyphWidth) {
        uint8_t byte = bitmap[pixelIdx >> 2];
        uint8_t val = (byte >> ((3 - (pixelIdx & 3)) * 2)) & 0x3;
        // val: 0=white, 1=light gray, 2=dark gray, 3=black

        uint8_t fbBit = 7 - p;         // MSB-first, aligns with FB

        // constexpr-if: only compute mask bits relevant to this pass
        if constexpr (renderMode == GfxRenderer::BW) {
            // BW pass: draw all non-white pixels (val < 3)
            if (val < 3) mask |= (1 << fbBit);
        } else {
            // GRAYSCALE_LSB: draw dark gray only (val == 2)
            // GRAYSCALE_MSB: draw dark + light gray (val == 1 || val == 2)
            if constexpr (renderMode == GfxRenderer::GRAYSCALE_LSB) {
                if (val == 2) mask |= (1 << fbBit);
            } else {  // GRAYSCALE_MSB
                if (val == 1 || val == 2) mask |= (1 << fbBit);
            }
        }
        pixelIdx++;
    }
}

// Apply head/tail masks (per-bit, same as 1-bit — see §6 below)
mask &= headMask;
mask &= tailMask;

// Single RMW — operation direction differs by pass
// BW:   FB bit 0 = ink → clear drawn bits: fb &= ~mask
// GRAY: FB bit 1 = gray  → set drawn bits: fb |=  mask
if constexpr (renderMode == GfxRenderer::BW) {
    fb[rowOffset] &= ~mask;
} else {
    fb[rowOffset] |= mask;
}
```

**Why the operation direction flips**: The framebuffer bit meaning is inverted between
BW and grayscale passes. In BW mode, `drawPixel(x, y, true)` clears the bit (0 = ink).
In grayscale passes, `drawPixel(x, y, false)` sets the bit (1 = gray). The optimized
code mirrors this: BW clears bits (`& ~mask`), grayscale sets bits (`| mask`).

After the glyph row finishes, `copyGrayscaleLsbBuffers(frameBuffer)` and
`copyGrayscaleMsbBuffers(frameBuffer)` copy the framebuffer to the two display RAM
planes. The display hardware interprets the BW+RED combination as 4 gray levels
(see §3a table).

### 3a. Grayscale Pass Architecture

The three-pass grayscale sequence is handled **at the activity level**, not inside
`renderCharImpl`. `EpubReaderActivity` calls `page->render()` three times, each with
a different `renderMode`:

```cpp
// Pass 1 — BW (black text overgrays): all non-white pixels
renderer.setRenderMode(GfxRenderer::BW);
renderer.storeBwBuffer();                          // save BW framebuffer
page->render(renderer, ...);                       // renderCharImpl<..., BW>

// Pass 2 — GRAYSCALE_LSB (BW RAM): dark gray only
renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
renderer.clearScreen(0x00);
page->render(renderer, ...);                       // renderCharImpl<..., GRAYSCALE_LSB>
renderer.copyGrayscaleLsbBuffers();

// Pass 3 — GRAYSCALE_MSB (RED RAM): dark + light gray
renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
renderer.clearScreen(0x00);
page->render(renderer, ...);                       // renderCharImpl<..., GRAYSCALE_MSB>
renderer.copyGrayscaleMsbBuffers();

// Display grayscale overlay, then restore BW
renderer.displayGrayBuffer();
renderer.restoreBwBuffer();
```

Each pass independently computes its mask and does its own RMW on the shared
`frameBuffer`. After all passes, `copyGrayscaleLsbBuffers` and
`copyGrayscaleMsbBuffers` copy the framebuffer to the two display RAM planes.
The display hardware then interprets the two RAM planes together:

| BW RAM | RED RAM | Display color |
|--------|---------|---------------|
| 1      | 1       | white         |
| 1      | 0       | light gray    |
| 0      | 0       | dark gray     |
| 0      | 1       | black         |

**Note:** `storeBwBuffer()` / `restoreBwBuffer()` are needed because grayscale passes
overwrite the framebuffer. 1-bit fonts (no grayscale) skip both — they render directly
to the framebuffer in a single BW pass.

### 4. Main `renderCharImpl` — Static Calculations Upfront

Both `orientation` and `renderMode` are template parameters, so the compiler can eliminate
all dead branches — no runtime `switch` on orientation, no runtime comparison on renderMode.

```cpp
template <GfxRenderer::Orientation orientation, TextRotation rotation,
          GfxRenderer::RenderMode renderMode>
static void renderCharImpl(const GfxRenderer& renderer, ...) {
    // --- Glyph lookup (unchanged) ---
    const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
    const EpdFontData* fontData = fontFamily.getData(style);

    // --- Hoisted: orientation-aware physical coordinate mapping ---
    // Compute physical row offset and physical y for row 0 once
    uint8_t headMask, tailMask;  // per-byte masks for head/tail
    int byteStart, byteEnd;      // framebuffer byte range

    // --- Hoisted: strip target selection ---
    uint8_t* fb = renderer.getWriteTarget();
    int originY = renderer.getWriteOriginY();
    int writeRows = renderer.getWriteRows();

    // --- Row loop ---
    for (int glyphY = 0; glyphY < height; glyphY++) {
        // Physical row offset — fully inlined, no runtime switch
        // orientation is a template param → compiler emits one arithmetic expression
        int rowOffset = computePhysicalRowOffset<orientation>(glyphY, ...);

        // Clip to strip bounds
        if (rowOffset < 0 || rowOffset >= writeRows) continue;

        // Byte range and masks computed once per row
        computeByteRangeAndMasks(..., &byteStart, &byteEnd, &headMask, &tailMask);

        // Dispatch to byte-aligned row processor
        if (fontData->is2Bit)
            renderCharRow2Bit<orientation, rotation, renderMode>(...);
        else
            renderCharRow1Bit<orientation, rotation, renderMode>(...);
    }
}
```

### 4a. Runtime Dispatch Wrapper

`orientation` and `renderMode` are runtime values (`GfxRenderer` member variables), but
they are **constant for the duration of a single `drawText` call**. We use a runtime
`switch` to dispatch to the correct template instantiation — the switch executes once
per `drawText` call (not per glyph, not per pixel), so overhead is negligible.

```cpp
// In drawText (called once per line of text):
void drawText(const int fontId, const int x, const int y, const char* text, ...) const {
    // ... font lookup, BiDi resolution, etc. ...

    // Runtime dispatch — executed once per drawText call
    switch (orientation) {
        case Portrait:
            switch (renderMode) {
                case BW:           renderCharImpl<Portrait, TextRotation::None, BW>(...); break;
                case GRAYSCALE_LSB: renderCharImpl<Portrait, TextRotation::None, GRAYSCALE_LSB>(...); break;
                case GRAYSCALE_MSB: renderCharImpl<Portrait, TextRotation::None, GRAYSCALE_MSB>(...); break;
            }
            break;
        case LandscapeClockwise:
            switch (renderMode) {
                case BW:           renderCharImpl<LandscapeClockwise, TextRotation::None, BW>(...); break;
                case GRAYSCALE_LSB: renderCharImpl<LandscapeClockwise, TextRotation::None, GRAYSCALE_LSB>(...); break;
                case GRAYSCALE_MSB: renderCharImpl<LandscapeClockwise, TextRotation::None, GRAYSCALE_MSB>(...); break;
            }
            break;
        case PortraitInverted:
            switch (renderMode) {
                case BW:           renderCharImpl<PortraitInverted, TextRotation::None, BW>(...); break;
                case GRAYSCALE_LSB: renderCharImpl<PortraitInverted, TextRotation::None, GRAYSCALE_LSB>(...); break;
                case GRAYSCALE_MSB: renderCharImpl<PortraitInverted, TextRotation::None, GRAYSCALE_MSB>(...); break;
            }
            break;
        case LandscapeCounterClockwise:
            switch (renderMode) {
                case BW:           renderCharImpl<LandscapeCounterClockwise, TextRotation::None, BW>(...); break;
                case GRAYSCALE_LSB: renderCharImpl<LandscapeCounterClockwise, TextRotation::None, GRAYSCALE_LSB>(...); break;
                case GRAYSCALE_MSB: renderCharImpl<LandscapeCounterClockwise, TextRotation::None, GRAYSCALE_MSB>(...); break;
            }
            break;
    }
}
```

Similarly, `drawTextRotated90CW` dispatches to the `Rotated90CW` variants:
```cpp
// In drawTextRotated90CW:
switch (orientation) {
    case Portrait:
        switch (renderMode) {
            case BW:           renderCharImpl<Portrait, TextRotation::Rotated90CW, BW>(...); break;
            case GRAYSCALE_LSB: renderCharImpl<Portrait, TextRotation::Rotated90CW, GRAYSCALE_LSB>(...); break;
            case GRAYSCALE_MSB: renderCharImpl<Portrait, TextRotation::Rotated90CW, GRAYSCALE_MSB>(...); break;
        }
        break;
    // ... 3 more orientation cases
}
```

### 5. Compile-Time Orientation Specialization

The template `<orientation, rotation, renderMode>` produces exactly **24 instantiations**
(4 orientations × 2 rotations × 3 render modes). Each has the coordinate math fully inlined
— no runtime `switch` in the pixel loop.

The compiler will eliminate dead branches (e.g., the `switch` for `orientation`
and `renderMode` become single arithmetic expressions). **All 24 instantiations
are always emitted** because the dispatch `switch` in `drawText` references
all of them. The compiler cannot prune any, even for 1-bit fonts where
GRAYSCALE_LSB/GRAYSCALE_MSB are never called — the switch cases still exist and
the linker sees them as reachable.

This is acceptable: 24 × ~80 bytes ≈ **~2 KB** of code size, which is negligible against
the ESP32-C3's 16 MB flash.

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

**Same approach as 1-bit mode.** The mask operates on individual bit positions within
each framebuffer byte (8 pixels per byte), not nibble boundaries. This is because the
fb layout is still **1-bit-per-pixel**; the 2-bit data from the font bitmap is decoded
into a single mask byte where each bit corresponds to one pixel column.

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
computing only the mask relevant to its pass and writing to the single `frameBuffer`:

| Pass     | RenderMode       | What gets drawn              | Mask condition           | FB write action |
|----------|------------------|------------------------------|--------------------------|-----------------|
| BW       | BW               | All non-white pixels         | `val < 3`                | `fb &= ~mask`   |
| LSB      | GRAYSCALE_LSB    | Dark gray only               | `val == 2`               | `fb |= mask`    |
| MSB      | GRAYSCALE_MSB    | Dark + light gray            | `val == 1 \|\| val == 2` | `fb |= mask`    |

**Why BW uses `&= ~mask` and grayscale uses `|= mask`**: The framebuffer bit meaning
inverts between BW and grayscale passes:
- **BW**: bit 0 = ink, bit 1 = white → drawn pixels get `&= ~mask` (clear to 0)
- **Grayscale**: bit 0 = no-gray, bit 1 = gray → drawn pixels get `|= mask` (set to 1)

This mirrors the existing `drawPixel()` behavior: `drawPixel(x, y, true)` clears bits
(BW), `drawPixel(x, y, false)` sets bits (grayscale).

White pixels (val=3) are never drawn in any pass — their bits stay at 1 (no ink/gray).

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

24 template instantiations × ~80 bytes each ≈ **~2 KB** of code size. This is acceptable
against the ESP32-C3's 16 MB flash. All 24 are always emitted (the dispatch switch in
`drawText` references all of them, so the linker sees them as reachable even for 1-bit
fonts where grayscale instantiations are never called).

### renderCharScaled — Deferred to v2

`renderCharScaled` (in `GfxRenderer.cpp`, used for SUP/SUB text) still uses pixel-by-pixel
rendering with `drawPixel()` calls. It is called far less frequently than `renderCharImpl` and
is out of scope for this optimization. Will be byte-aligned in a follow-up PR.

### Memory Safety (ESP32-C3 RISC-V)

- No heap allocation in the hot path — all buffers are on the stack or point to existing framebuffer
- All reads from `bitmap` pointer: glyph bitmaps are in IRAM (flash) or font cache decompressor output, both safe
- `restrict` qualifiers on pointers tell the compiler they don't alias, enabling better optimization

## Implementation Steps

1. **Create `renderCharRow1Bit`** template — byte-aligned 1-bit row processor with head/tail masks (§2).
   Template params: `<orientation, rotation, renderMode>`.

2. **Create `renderCharRow2Bit`** template — byte-aligned 2-bit row processor (§3):
   - Extracts 8 pixels per framebuffer byte (2 bitmap bytes), builds a single `mask`
   - Uses `constexpr-if` on `renderMode` to select the mask condition:
     `val < 3` for BW, `val == 2` for GRAYSCALE_LSB, `val == 1 \|\| val == 2` for GRAYSCALE_MSB
   - Applies head/tail per-bit masks
   - Performs a single RMW: `fb &= ~mask` for BW, `fb |= mask` for grayscale passes
   - Template params: `<orientation, rotation, renderMode>`

3. **Refactor `renderCharImpl`** to:
   - Add `orientation` and `renderMode` as template parameters:
     `<orientation, rotation, renderMode>`
   - Hoist orientation rotation, strip target, physical coordinates upfront
   - Compute per-row byte range and head/tail masks before the row loop
   - Dispatch to `renderCharRow1Bit` or `renderCharRow2Bit` with the same template params
   - The **three-pass** architecture stays at the activity level (caller invokes
     `page->render()` three times with different `renderMode` settings)

4. **Add runtime dispatch wrapper** in `drawText` and `drawTextRotated90CW`:
   - Replace the direct `renderCharImpl<TextRotation::None>(...)` call with a nested
     `switch (orientation) { case ...: switch (renderMode) { case ...: renderCharImpl<...>(...) } }`
   - 12 dispatch cases per function, 24 total

5. **Verify correctness**: test all 24 orientation/rotation/mode combinations against existing output
6. **Benchmark**: measure rendering time with profiling (the existing `start_ms` timing in `clearScreen`)

## Files to Modify

- `lib/GfxRenderer/GfxRenderer.cpp`:
  - Replace the pixel-loop `renderCharImpl<TextRotation::None>` and `renderCharImpl<TextRotation::Rotated90CW>` with the byte-aligned `<orientation, rotation, renderMode>` versions
  - Add `renderCharRow1Bit` and `renderCharRow2Bit` helper templates
  - Add nested `switch` dispatch in `drawText()` and `drawTextRotated90CW()` to route to the correct instantiation
- `TODO.md`: This file — tracks the optimization plan and implementation status
