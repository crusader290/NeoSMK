
#ifndef PPU_H
#define PPU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Ppu Ppu;

// widescreen: max extra columns per side (64 → 384 wide, past 16:9)
#define PPU_WIDE_MAX 64
// upward extension: max virtual lines rendered above the picture
#define PPU_ABOVE_MAX 64
enum { ppu_wideModeMode7 = 0, ppu_wideModeAlways = 1 };
enum { ppu_hudClassBg = 1, ppu_hudClassObj = 2, ppu_hudClassMath = 3 };

typedef struct PpuM7Line {
  bool valid;        // line rendered in mode 7 (not forced blank)
  bool xFlip;
  int32_t startX;    // m7startX / m7startY (8.8 fixed) for this line
  int32_t startY;
  int16_t a;         // m7 matrix a / c (per-pixel step)
  int16_t c;
  uint8_t brightness;  // screen brightness this line was drawn with (0..15)
} PpuM7Line;

#include "snes.h"
#include "statehandler.h"

typedef struct BgLayer {
  uint16_t hScroll;
  uint16_t vScroll;
  bool tilemapWider;
  bool tilemapHigher;
  uint16_t tilemapAdr;
  uint16_t tileAdr;
  bool bigTiles;
  bool mosaicEnabled;
} BgLayer;

typedef struct Layer {
  bool mainScreenEnabled;
  bool subScreenEnabled;
  bool mainScreenWindowed;
  bool subScreenWindowed;
} Layer;

typedef struct WindowLayer {
  bool window1enabled;
  bool window2enabled;
  bool window1inversed;
  bool window2inversed;
  uint8_t maskLogic;
} WindowLayer;

struct Ppu {
  Snes* snes;
  // vram access
  uint16_t vram[0x8000];
  uint16_t vramPointer;
  bool vramIncrementOnHigh;
  uint16_t vramIncrement;
  uint8_t vramRemapMode;
  uint16_t vramReadBuffer;
  // cgram access
  uint16_t cgram[0x100];
  uint8_t cgramPointer;
  bool cgramSecondWrite;
  uint8_t cgramBuffer;
  // oam access
  uint16_t oam[0x100];
  uint8_t highOam[0x20];
  uint8_t oamAdr;
  uint8_t oamAdrWritten;
  bool oamInHigh;
  bool oamInHighWritten;
  bool oamSecondWrite;
  uint8_t oamBuffer;
  // object/sprites
  bool objPriority;
  uint16_t objTileAdr1;
  uint16_t objTileAdr2;
  uint8_t objSize;
  uint8_t objPixelBuffer[256]; // line buffers
  uint8_t objPriorityBuffer[256];
  bool timeOver;
  bool rangeOver;
  bool objInterlace;
  // background layers
  BgLayer bgLayer[4];
  uint8_t scrollPrev;
  uint8_t scrollPrev2;
  uint8_t mosaicSize;
  uint8_t mosaicStartLine;
  // layers
  Layer layer[5];
  // mode 7
  int16_t m7matrix[8]; // a, b, c, d, x, y, h, v
  uint8_t m7prev;
  bool m7largeField;
  bool m7charFill;
  bool m7xFlip;
  bool m7yFlip;
  bool m7extBg;
  // mode 7 internal
  int32_t m7startX;
  int32_t m7startY;
  // windows
  WindowLayer windowLayer[6];
  uint8_t window1left;
  uint8_t window1right;
  uint8_t window2left;
  uint8_t window2right;
  // color math
  uint8_t clipMode;
  uint8_t preventMathMode;
  bool addSubscreen;
  bool subtractColor;
  bool halfColor;
  bool mathEnabled[6];
  uint8_t fixedColorR;
  uint8_t fixedColorG;
  uint8_t fixedColorB;
  // settings
  bool forcedBlank;
  uint8_t brightness;
  uint8_t mode;
  bool bg3priority;
  bool evenFrame;
  bool pseudoHires;
  bool overscan;
  bool frameOverscan; // if we are overscanning this frame (determined at 0,225)
  bool interlace;
  bool frameInterlace; // if we are interlacing this frame (determined at start vblank)
  bool directColor;
  // latching
  uint16_t hCount;
  uint16_t vCount;
  bool hCountSecond;
  bool vCountSecond;
  bool countersLatched;
  uint8_t ppu1openBus;
  uint8_t ppu2openBus;
  // pixel buffer (xbgr)
  // times 2 for even and odd frame
  uint8_t pixelBuffer[512 * 4 * 239 * 2];
  uint8_t pixelOutputFormat;
  // widescreen extension (NeoSMK). Presentation only: never serialized and
  // never visible to the emulated CPU (sprite range/time-over flags are
  // still computed over the 256-pixel hardware line).
  int wideExt;          // extra columns rendered left and right (0 = off)
  int wideMode;         // ppu_wideModeMode7 / ppu_wideModeAlways
  bool wideFrame;       // this frame renders the extension columns
  bool wideSawMode7;    // a mode-7 line was rendered this frame
  uint8_t objWidePixel[256 + 2 * PPU_WIDE_MAX];     // line buffers, x + PPU_WIDE_MAX
  uint8_t objWidePriority[256 + 2 * PPU_WIDE_MAX];
  // (256 + 2 * wideExt) pixels per row, 8 bytes each (sub + main, like
  // pixelBuffer), 239 rows per field, both fields
  uint8_t widePixelBuffer[(256 + 2 * PPU_WIDE_MAX) * 8 * 239 * 2];
  // per-line mode-7 capture (NeoSMK, presentation only): ground position of
  // screen column x on line y is ((startX + a * x) / 256, (startY + c * x) / 256)
  PpuM7Line m7lines[240];
  // HUD separation (NeoSMK, presentation only): pixels of HUD sprites, HUD
  // BG layers (on non-mode-7 lines) and window-shaped colour math, on lines
  // <= hudMaxLine, go to hudBuffer instead of the picture
  bool hudEnabled;
  int hudMinLine;
  int hudMaxLine;
  uint8_t hudObjMask[16];  // bit per OAM entry
  uint8_t hudBgMask;       // bit n = BG n+1
  bool hudWindowMath;
  uint8_t hudSkip;         // internal: what the current compose pass leaves out
  bool hudWideMath;        // internal: window math covers most of this line
  bool hudDropWideMath;    // such screen-wide window math is left out (not kept)
  uint8_t objWideIndex[256 + 2 * PPU_WIDE_MAX];   // OAM entry behind each obj pixel
  // (256 + 2 * wideExt) pixels per row: r, g, b, class (ppu_hudClass*, 0 = none;
  // for window math bit 4 = subtract, bit 5 = half), both fields
  uint8_t hudBuffer[(256 + 2 * PPU_WIDE_MAX) * 4 * 239 * 2];
  // Upward extension (NeoSMK, presentation only): when line aboveFromLine is
  // reached (non-mode-7), aboveLines virtual lines above it are rendered with
  // that line's registers into aboveBuffer, row 0 = the topmost virtual line.
  // Each BG layer stops at its first fully transparent row (no wrapped rows),
  // only sprites wrapping past line 255 count, and HUD layers are left out.
  int aboveLines;
  int aboveFromLine;
  int aboveRendered;       // lines actually rendered this frame
  bool evalAbove;          // internal: evaluating sprites for a virtual line
  uint8_t aboveBgSkip;     // internal: BG layers left out of the current virtual line
  uint8_t aboveBuffer[(256 + 2 * PPU_WIDE_MAX) * 8 * PPU_ABOVE_MAX];
  // Mode-7 tile override (NeoSMK, presentation only): on lines 1..
  // m7OverrideMaxLine, map cell i (ty * 128 + tx) shows tile m7Override[i]
  // instead of the tilemap's, unless that is 0xFFFF. VRAM is not changed.
  const uint16_t* m7Override;
  int m7OverrideMaxLine;
  bool m7OverrideLine;     // internal: override active on the current line
  // Sprite separation (NeoSMK, presentation only): main-screen sprite pixels
  // on lines objSepMinLine..objSepMaxLine, except entries in objSepExclude and
  // HUD sprites, go to objSepBuffer and are removed from the picture.
  // Per pixel: r, g, b, bottom line of the sprite (1..255; 0 = no sprite).
  bool objSepEnabled;
  int objSepMinLine;
  int objSepMaxLine;
  uint8_t objSepExclude[16];   // bit per OAM entry
  uint8_t objSepBuffer[(256 + 2 * PPU_WIDE_MAX) * 4 * 239 * 2];
  uint8_t objSepFront[16];     // entries drawn in front of all 3D (bottom line 255)
  uint8_t objSepFrontPal;      // ... and sprites using these palettes (bit per palette)
  // Widescreen UI anchoring (NeoSMK, presentation only): on widened frames,
  // lines anchorMinLine..anchorMaxLine, OAM entry n is drawn objAnchor[n] *
  // wideExt columns to the side (-1 left edge, +1 right edge, 0 stays), BG n
  // content by bgAnchor[n] * wideExt and the colour window by
  // colorWindowAnchor * wideExt. Sprite range/time-over flags are computed
  // from the hardware positions.
  bool anchorEnabled;
  bool anchorLine;         // internal: anchoring active on the current line
  int anchorMinLine;
  int anchorMaxLine;
  int8_t objAnchor[128];
  int8_t bgAnchor[4];
  int bgAnchorMinLine;     // BG anchoring starts here (keeps full-width borders above)
  bool bgAnchorLine;       // internal
  int colorWindowAnchor;
  // OAM entries not drawn (NeoSMK, presentation only: replaced by 3D); they
  // still count toward the hardware range / time-over limits
  uint8_t objHide[16];
};

enum { ppu_pixelOutputFormatXBGR = 0, ppu_pixelOutputFormatBGRX = 1 };

Ppu* ppu_init(Snes* snes);
void ppu_free(Ppu* ppu);
void ppu_reset(Ppu* ppu);
void ppu_handleState(Ppu* ppu, StateHandler* sh);
bool ppu_checkOverscan(Ppu* ppu);
void ppu_handleVblank(Ppu* ppu);
void ppu_handleFrameStart(Ppu* ppu);
void ppu_runLine(Ppu* ppu, int line);
uint8_t ppu_read(Ppu* ppu, uint8_t adr);
void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val);
void ppu_putPixels(Ppu* ppu, uint8_t* pixels);
void ppu_setPixelOutputFormat(Ppu* ppu, int pixelOutputFormat);
// widescreen: render `ext` extra columns per side (0..PPU_WIDE_MAX, 0 = off).
// ppu_wideModeMode7 widens only frames that use mode 7 (the previous frame's
// result decides), others keep black extension columns.
void ppu_setWidescreen(Ppu* ppu, int ext, int mode);
// copy the last frame as (256 + 2 * ext) * 2 columns x 224/239 rows (no line
// doubling), XBGR/BGRX like ppu_putPixels; returns the number of rows
int ppu_putPixelsWide(Ppu* ppu, uint8_t* pixels);
// HUD separation (see Ppu.hud*): enabled for lines minLine..maxLine
void ppu_setHud(Ppu* ppu, bool enabled, int minLine, int maxLine, const uint8_t objMask[16],
                uint8_t bgMask, bool windowMath);
// copy the last frame's HUD layer like ppu_putPixelsWide (4 bytes per pixel)
int ppu_putHud(Ppu* ppu, uint8_t* out);
// upward extension (see Ppu.above*): `lines` virtual lines above `fromLine`
void ppu_setAbove(Ppu* ppu, int lines, int fromLine);
// copy them (wide layout, 8 bytes per column); returns the number of lines
int ppu_putAbove(Ppu* ppu, uint8_t* out);
// mode-7 tile override (see Ppu.m7Override); NULL disables
void ppu_setM7Override(Ppu* ppu, const uint16_t* cells, int maxLine);
// sprite separation (see Ppu.objSep*)
void ppu_setObjSep(Ppu* ppu, bool enabled, int minLine, int maxLine, const uint8_t exclude[16]);
// OAM entries left out of the picture (see Ppu.objHide); NULL clears
void ppu_setObjHide(Ppu* ppu, const uint8_t hide[16]);
// separated sprites of these OAM entries are marked "in front" (bottom line 255)
void ppu_setObjSepFront(Ppu* ppu, const uint8_t front[16]);
void ppu_setObjSepFrontPalettes(Ppu* ppu, uint8_t mask);
// widescreen UI anchoring (see Ppu.anchor*)
void ppu_setAnchors(Ppu* ppu, bool enabled, int minLine, int maxLine, const int8_t objAnchor[128],
                    const int8_t bgAnchor[4], int bgMinLine, int colorWindowAnchor);
// copy the last frame's sprite layer like ppu_putHud (4 bytes per pixel)
int ppu_putObjSep(Ppu* ppu, uint8_t* out);
// mode-7 line capture of the last frame (lines 1..239 at index line - 1)
const PpuM7Line* ppu_getM7Lines(Ppu* ppu);

#endif
