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

> **NOTE**: The design below describes the current (partial) byte-aligned implementation.
> A future refactor will replace this "row-oriented" approach with an "orientation-aware stride"
> architecture. See **[Future Refactoring: Orientation-Aware Stride Rendering](#future-refactoring-orientation-aware-stride-rendering)**
> for the complete design specification.

### 1. Static Helper: `renderCharStride` Template

A static template function that processes one glyph row by iterating over framebuffer bytes:

```cpp
// For 1-bit mode:
template <GfxRenderer::Orientation orientation, GfxRenderer::RenderMode renderMode>
static void renderCharStride1Bit(const uint8_t* fb,
                              const uint8_t* bitmap,
                              int rowOffset, int byteStart, int byteEnd,
                              uint8_t headMask, uint8_t tailMask,
                              int glyphWidth, int pixelOffset);

// For 2-bit mode:
template <GfxRenderer::Orientation orientation, GfxRenderer::RenderMode renderMode>
static void renderCharStride2Bit(const uint8_t* fb,
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

// Apply head/tail masks (see §4 below)
mask &= headMask;
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

// Apply head/tail masks (see §4 below)
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
template <GfxRenderer::Orientation orientation, GfxRenderer::RenderMode renderMode>
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

        // Dispatch to byte-aligned stride processor
        if (fontData->is2Bit)
            renderCharStride2Bit<orientation, renderMode>(...);
        else
            renderCharStride1Bit<orientation, renderMode>(...);
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
                case BW:           renderCharImpl<Portrait, BW>(...); break;
                case GRAYSCALE_LSB: renderCharImpl<Portrait, GRAYSCALE_LSB>(...); break;
                case GRAYSCALE_MSB: renderCharImpl<Portrait, GRAYSCALE_MSB>(...); break;
            }
            break;
        case LandscapeClockwise:
            switch (renderMode) {
                case BW:           renderCharImpl<LandscapeClockwise, BW>(...); break;
                case GRAYSCALE_LSB: renderCharImpl<LandscapeClockwise, GRAYSCALE_LSB>(...); break;
                case GRAYSCALE_MSB: renderCharImpl<LandscapeClockwise, GRAYSCALE_MSB>(...); break;
            }
            break;
        case PortraitInverted:
            switch (renderMode) {
                case BW:           renderCharImpl<PortraitInverted, BW>(...); break;
                case GRAYSCALE_LSB: renderCharImpl<PortraitInverted, GRAYSCALE_LSB>(...); break;
                case GRAYSCALE_MSB: renderCharImpl<PortraitInverted, GRAYSCALE_MSB>(...); break;
            }
            break;
        case LandscapeCounterClockwise:
            switch (renderMode) {
                case BW:           renderCharImpl<LandscapeCounterClockwise, BW>(...); break;
                case GRAYSCALE_LSB: renderCharImpl<LandscapeCounterClockwise, GRAYSCALE_LSB>(...); break;
                case GRAYSCALE_MSB: renderCharImpl<LandscapeCounterClockwise, GRAYSCALE_MSB>(...); break;
            }
            break;
    }
}
```

### 5. Compile-Time Orientation Specialization

The template `<orientation, renderMode>` produces exactly **12 instantiations**
(4 orientations × 3 render modes). Each has the coordinate math fully inlined
— no runtime `switch` in the pixel loop.

The compiler will eliminate dead branches (e.g., the `switch` for `orientation`
and `renderMode` become single arithmetic expressions). **All 12 instantiations**
are always emitted because the dispatch `switch` in `drawText` references
all of them. The compiler cannot prune any, even for 1-bit fonts where
GRAYSCALE_LSB/GRAYSCALE_MSB are never called — the switch cases still exist and
the linker sees them as reachable.

This is acceptable: 12 × ~80 bytes ≈ **~1 KB** of code size, which is negligible against
the ESP32-C3's 16 MB flash.

## Expected Performance Improvement per Render Pass

| Metric                         | Before | After                    | Improvement  |
|--------------------------------|--------|--------------------------|--------------|
| Function calls per 12×20 glyph | 240    | ~0                       | Eliminated   |
| RMW operations per 12×20 glyph | 240    | ~40 (2 bytes × 20 rows)  | **6× fewer** |
| Orientation switch hits        | 240    | 0                        | Eliminated   |
| Bounds check hits              | 240    | 0 (hoisted)              | Eliminated   |

## Key Considerations

### Head/Tail Masks

The head and tail masks isolate only the bits that fall within the glyph's physical bounds.
This applies to both 1-bit and 2-bit fonts — the mask operates on individual bit positions
within each framebuffer byte (8 pixels per byte), not nibble boundaries. The fb layout is
still **1-bit-per-pixel**; the 2-bit data from the font bitmap is decoded into a single
mask byte where each bit corresponds to one pixel column.

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

### Grayscale Pass Summary

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
(BW), `drawPixel(x, y, false)` sets bits (grayscale). White pixels (val=3) are never
drawn in any pass — their bits stay at 1 (no ink/gray).

### Strip Mode

Already handled transparently by `getWriteTarget()` and `getWriteOriginY()`:
- When `_stripActive`, these return the scratch buffer pointer and its physical-row origin
- The row offset is subtracted from the physical Y to get the scratch-buffer-relative index
- Pixels outside the band are clipped via the strip bounds check before the byte loop

### Out-of-Scope Functions

The following functions are **out of scope** for this optimization project. They remain on the
old pixel-by-pixel code path (`drawPixel()`-based) and will not be ported to the new
`<orientation, renderMode>` template architecture:

- **`renderCharScaled`** (in `GfxRenderer.cpp`) — used for SUP/SUB text in `drawText()`.
  Called far less frequently than `renderCharImpl`; its pixel-by-pixel rendering is acceptable.
- **`renderCharImplRotated90CW`** (in `GfxRenderer.cpp`) — used for side-button label text
  rendered at 90° clockwise in `drawTextRotated90CW()`. This function uses a different
  coordinate paradigm (vertical text with reversed Y) and is actively called from theme button
  rendering (`BaseTheme.cpp`, `LyraTheme.cpp`).

These functions will be addressed in a separate follow-up PR.

### Memory Safety (ESP32-C3 RISC-V)

- No heap allocation in the hot path — all buffers are on the stack or point to existing framebuffer
- All reads from `bitmap` pointer: glyph bitmaps are in IRAM (flash) or font cache decompressor output, both safe
- `restrict` qualifiers on pointers tell the compiler they don't alias, enabling better optimization

## Implementation Steps

### Completed ✅

1. ✅ **Create `renderCharStride1Bit`** (formerly `renderCharRow1Bit`) — byte-aligned 1-bit stride processor with head/tail masks (§2).
   Template params: `<orientation, renderMode>`.
   **Status**: Defined at `RenderChar.h:~13-43`.

2. ✅ **Create `renderCharStride2Bit`** (formerly `renderCharRow2Bit`) — byte-aligned 2-bit stride processor (§3).
   Uses `constexpr-if` on `renderMode`, applies head/tail masks, performs single RMW.
   **Status**: Defined at `RenderChar.h:~46-85`.

3. ✅ **`renderCharImpl` refactored** — now a template `<orientation, renderMode>`
   with byte-aligned row processing. Strip target and physical
   coordinates are hoisted upfront. Per-row byte range and head/tail masks are computed
   before the row loop.
   **Status**: Defined at `RenderChar.h:~90-277`.

4. ✅ **Runtime dispatch wrapper** — `dispatchRenderCharImpl()` implements the nested `switch` over orientation
   and renderMode (12 total cases).
   **Status**: Defined at `RenderChar.h:~281-346`. Actively used by `drawText()` in `GfxRenderer.cpp`.

### Pending Verification

- [ ] **Correctness** — test all 12 orientation/mode combinations on device (4 orientations × 3 render modes).
- [ ] **2-bit fonts** — verify grayscale rendering of 2-bit fonts in all three passes.
- [ ] **Strip mode** — verify band clipping works correctly with the new byte-aligned code path.
- [ ] **Benchmark** — measure rendering time improvement with profiling.

### Pending Implementation ❌

**P1 — Remove commented-out `TextRotation` remnants in `RenderChar.h:128, 164, 222`**

These three blocks are leftover from the old single-function design where `renderCharImpl`
had a `TextRotation` parameter. They are already commented out but still reference
`TextRotation::Rotated90CW` (defined in `GfxRenderer.cpp:139`). Replace each block with
the corresponding orientation-based logic that is now handled by the `<orientation, renderMode>`
template parameters — the correct orientation for the old `Rotated90CW` path maps to
`GfxRenderer::LandscapeClockwise` in the new coordinate system. The `TextRotation` enum itself can
remain (it is still used by `renderCharImplRotated90CW`, which is out of scope).

## Files to Modify

- `lib/GfxRenderer/RenderChar.h`:
  - New file — contains byte-aligned `renderCharStride1Bit`, `renderCharStride2Bit`, `renderCharImpl`,
    `dispatchRenderCharImpl`.
  - Template params: `<orientation, renderMode>` (12 instantiations total).
- `lib/GfxRenderer/GfxRenderer.cpp`:
  - **Replaced**: `drawText()` now calls `dispatchRenderCharImpl()` instead of
    the old pixel-loop path for normal text rendering. The normal text path is fully
    in scope; `renderCharScaled` and `renderCharImplRotated90CW` remain on the old code path
    (see [Out-of-Scope Functions](#out-of-scope-functions)).
  - **Pending**: Remove commented-out `TextRotation` remnants from `RenderChar.h` (P1).
- `TODO.md`: This file — tracks the optimization plan and implementation status

---

## Future Refactoring: Orientation-Aware Stride Rendering

*This section tracks the move from row-oriented processing to a stride-oriented architecture optimized for the ESP32-C3/E-Ink memory layout. This will replace the current "row" design described above.*

### 1. Rename and Redesign Stride Helpers
- [x] **Rename `renderCharRow1Bit`/`renderCharRow2Bit` to `renderCharStride1Bit`/`renderCharStride2Bit`** — All function definitions, call sites, and header comments in `RenderChar.h` updated.
- [ ] Refactor `renderCharStride*` signature:
    - Remove `headMask` and `tailMask`.
    - Add `pixelOffset` (the starting position of the glyph in the **contiguous** byte direction).
    - Add `glyphWidth` and `glyphHeight` to handle internal boundary masking.
- [ ] Implement Boundary Masking within `renderCharStride*`:
    - The function must ensure that only bits corresponding to the glyph are modified in each framebuffer byte.
    - Use `pixelOffset` and glyph dimensions to calculate a mask for every byte being written.
    - **Crucially**: For the first and last contiguous bytes of a glyph, the mask must exclude any bits that fall outside the glyph's logical boundaries (bits before the glyph starts or after it ends), preserving the existing state of those pixels in the framebuffer.

### 2. Refactor `renderCharImpl` Architecture
The rendering loop must switch from "Row" logic to "Stride" logic based on orientation.

- [ ] **Orientation-Aware Outer Loop**:
    - `renderCharImpl` should iterate through the **non-contiguous** dimension (e.g., in Portrait, this is the logical X-axis).
    - For each step in the outer loop, call `renderCharStride*` to process the **contiguous** direction (e.g., in Portrait, the vertical Y-axis bytes).
- [ ] **Pre-calculation**:
    - Calculate the `pixelOffset` (in the contiguous direction) once per glyph before iteration begins.
- [ ] **Contiguous Stride Logic (`renderCharStride*`)**:
    - Use `constexpr-if` to handle bitmap indexing and stride direction based on:
        1. Current `orientation` (compile-time constant for loop parameters like column vs row access).
        2. Bitmap dimensions (`width`/`height`) — runtime variables, inputs to the inner contiguous stride loop.
        3. The current iteration index (the "row" offset passed from the caller) — runtime variable, input to the inner contiguous stride loop.
- [ ] **Loop Optimization**:
    - Hoist strip clipping and the 2-vs-1 bit condition outside the inner loop to minimize branches.

### 3. Orientation & Coordinate Handling
- [ ] **Coordinate Transformation**:
    - `renderCharImpl` must calculate the rotated glyph coordinates once per glyph.
    - Pass the necessary rotation/offset information to the stride helpers so they can correctly map bitmap bits to physical framebuffer bits.
- [ ] **Portrait Specifics**:
    - In Portrait: Outer loop = Glyph Width; Inner loop = Glyph Height (writing whole framebuffer bytes as vertical columns).
    - In Landscape: Outer loop = Glyph Height; Inner loop = Glyph Width (writing whole framebuffer bytes as horizontal rows).
    - `pixelOffset` is vertical in Portrait and horizontal in Landscape.

### 4. Implementation Safety & Scope
- [ ] **Strict Scope Control**: Do **NOT** refactor `renderCharScaled` or `renderCharImplRotated90CW` during this stride-based optimization. They remain on the old pixel-by-pixel code path and are actively used by live rendering paths (SUP/SUB text and side-button labels, respectively). See [Out-of-Scope Functions](#out-of-scope-functions).
