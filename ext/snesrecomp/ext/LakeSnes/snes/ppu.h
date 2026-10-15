
#ifndef PPU_H
#define PPU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Ppu Ppu;

// widescreen: max extra columns per side (64 → 384 wide, past 16:9)
#define PPU_WIDE_MAX 64
enum { ppu_wideModeMode7 = 0, ppu_wideModeAlways = 1 };

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

#endif
