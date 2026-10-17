
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ppu.h"
#include "snes.h"
#include "statehandler.h"

// array for layer definitions per mode:
//   0-7: mode 0-7; 8: mode 1 + l3prio; 9: mode 7 + extbg

//   0-3; layers 1-4; 4: sprites; 5: nonexistent
static const int layersPerMode[10][12] = {
  {4, 0, 1, 4, 0, 1, 4, 2, 3, 4, 2, 3},
  {4, 0, 1, 4, 0, 1, 4, 2, 4, 2, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5},
  {4, 0, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5},
  {4, 4, 4, 0, 4, 5, 5, 5, 5, 5, 5, 5},
  {2, 4, 0, 1, 4, 0, 1, 4, 4, 2, 5, 5},
  {4, 4, 1, 4, 0, 4, 1, 5, 5, 5, 5, 5}
};

static const int prioritysPerMode[10][12] = {
  {3, 1, 1, 2, 0, 0, 1, 1, 1, 0, 0, 0},
  {3, 1, 1, 2, 0, 0, 1, 1, 0, 0, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5},
  {3, 1, 2, 1, 0, 0, 5, 5, 5, 5, 5, 5},
  {3, 2, 1, 0, 0, 5, 5, 5, 5, 5, 5, 5},
  {1, 3, 1, 1, 2, 0, 0, 1, 0, 0, 5, 5},
  {3, 2, 1, 1, 0, 0, 0, 5, 5, 5, 5, 5}
};

static const int layerCountPerMode[10] = {
  12, 10, 8, 8, 8, 8, 6, 5, 10, 7
};

static const int bitDepthsPerMode[10][4] = {
  {2, 2, 2, 2},
  {4, 4, 2, 5},
  {4, 4, 5, 5},
  {8, 4, 5, 5},
  {8, 2, 5, 5},
  {4, 2, 5, 5},
  {4, 5, 5, 5},
  {8, 5, 5, 5},
  {4, 4, 2, 5},
  {8, 7, 5, 5}
};

static const int spriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

static void ppu_handlePixel(Ppu* ppu, int x, int y);
static int ppu_getPixel(Ppu* ppu, int x, int y, bool sub, int* r, int* g, int* b);
static uint16_t ppu_getOffsetValue(Ppu* ppu, int col, int row);
static int ppu_getPixelForBgLayer(Ppu* ppu, int x, int y, int layer, bool priority);
static void ppu_handleOPT(Ppu* ppu, int layer, int* lx, int* ly);
static void ppu_calculateMode7Starts(Ppu* ppu, int y);
static int ppu_getPixelForMode7(Ppu* ppu, int x, int layer, bool priority);
static bool ppu_getWindowState(Ppu* ppu, int layer, int x);
static void ppu_evaluateSprites(Ppu* ppu, int line);
static uint16_t ppu_getVramRemap(Ppu* ppu);

Ppu* ppu_init(Snes* snes) {
  Ppu* ppu = malloc(sizeof(Ppu));
  ppu->snes = snes;
  ppu_setPixelOutputFormat(ppu, ppu_pixelOutputFormatBGRX);
  ppu->wideExt = 0;
  ppu->wideMode = ppu_wideModeMode7;
  ppu->wideFrame = false;
  ppu->wideSawMode7 = false;
  memset(ppu->widePixelBuffer, 0, sizeof(ppu->widePixelBuffer));
  memset(ppu->m7lines, 0, sizeof(ppu->m7lines));
  ppu->hudEnabled = false;
  ppu->hudMinLine = 0;
  ppu->hudMaxLine = 0;
  memset(ppu->hudObjMask, 0, sizeof(ppu->hudObjMask));
  ppu->hudBgMask = 0;
  ppu->hudWindowMath = false;
  ppu->hudSkip = 0;
  ppu->hudWideMath = false;
  ppu->hudDropWideMath = false;
  memset(ppu->hudBuffer, 0, sizeof(ppu->hudBuffer));
  ppu->aboveLines = 0;
  ppu->aboveFromLine = 0;
  ppu->aboveRendered = 0;
  ppu->evalAbove = false;
  ppu->aboveBgSkip = 0;
  ppu->m7Override = NULL;
  ppu->m7OverrideMaxLine = 0;
  ppu->m7OverrideLine = false;
  ppu->objSepEnabled = false;
  ppu->objSepMinLine = 0;
  ppu->objSepMaxLine = 0;
  memset(ppu->objSepExclude, 0, sizeof(ppu->objSepExclude));
  memset(ppu->objSepBuffer, 0, sizeof(ppu->objSepBuffer));
  memset(ppu->objSepIndex, 0, sizeof(ppu->objSepIndex));
  memset(ppu->objSepFront, 0, sizeof(ppu->objSepFront));
  ppu->objSepFrontPal = 0;
  ppu->objSepFrontMaxLine = 0;
  ppu->anchorEnabled = false;
  ppu->anchorLine = false;
  ppu->anchorMinLine = ppu->anchorMaxLine = 0;
  ppu->bgAnchorLine = false;
  ppu->bgAnchorMinLine = 0;
  memset(ppu->objAnchor, 0, sizeof(ppu->objAnchor));
  memset(ppu->bgAnchor, 0, sizeof(ppu->bgAnchor));
  ppu->colorWindowAnchor = 0;
  memset(ppu->objHide, 0, sizeof(ppu->objHide));
  return ppu;
}

void ppu_free(Ppu* ppu) {
  free(ppu);
}

void ppu_reset(Ppu* ppu) {
  memset(ppu->vram, 0, sizeof(ppu->vram));
  ppu->vramPointer = 0;
  ppu->vramIncrementOnHigh = false;
  ppu->vramIncrement = 1;
  ppu->vramRemapMode = 0;
  ppu->vramReadBuffer = 0;
  memset(ppu->cgram, 0, sizeof(ppu->cgram));
  ppu->cgramPointer = 0;
  ppu->cgramSecondWrite = false;
  ppu->cgramBuffer = 0;
  memset(ppu->oam, 0, sizeof(ppu->oam));
  memset(ppu->highOam, 0, sizeof(ppu->highOam));
  ppu->oamAdr = 0;
  ppu->oamAdrWritten = 0;
  ppu->oamInHigh = false;
  ppu->oamInHighWritten = false;
  ppu->oamSecondWrite = false;
  ppu->oamBuffer = 0;
  ppu->objPriority = false;
  ppu->objTileAdr1 = 0;
  ppu->objTileAdr2 = 0;
  ppu->objSize = 0;
  memset(ppu->objPixelBuffer, 0, sizeof(ppu->objPixelBuffer));
  memset(ppu->objPriorityBuffer, 0, sizeof(ppu->objPriorityBuffer));
  ppu->timeOver = false;
  ppu->rangeOver = false;
  ppu->objInterlace = false;
  for(int i = 0; i < 4; i++) {
    ppu->bgLayer[i].hScroll = 0;
    ppu->bgLayer[i].vScroll = 0;
    ppu->bgLayer[i].tilemapWider = false;
    ppu->bgLayer[i].tilemapHigher = false;
    ppu->bgLayer[i].tilemapAdr = 0;
    ppu->bgLayer[i].tileAdr = 0;
    ppu->bgLayer[i].bigTiles = false;
    ppu->bgLayer[i].mosaicEnabled = false;
  }
  ppu->scrollPrev = 0;
  ppu->scrollPrev2 = 0;
  ppu->mosaicSize = 1;
  ppu->mosaicStartLine = 1;
  for(int i = 0; i < 5; i++) {
    ppu->layer[i].mainScreenEnabled = false;
    ppu->layer[i].subScreenEnabled = false;
    ppu->layer[i].mainScreenWindowed = false;
    ppu->layer[i].subScreenWindowed = false;
  }
  memset(ppu->m7matrix, 0, sizeof(ppu->m7matrix));
  ppu->m7prev = 0;
  ppu->m7largeField = false;
  ppu->m7charFill = false;
  ppu->m7xFlip = false;
  ppu->m7yFlip = false;
  ppu->m7extBg = false;
  ppu->m7startX = 0;
  ppu->m7startY = 0;
  for(int i = 0; i < 6; i++) {
    ppu->windowLayer[i].window1enabled = false;
    ppu->windowLayer[i].window2enabled = false;
    ppu->windowLayer[i].window1inversed = false;
    ppu->windowLayer[i].window2inversed = false;
    ppu->windowLayer[i].maskLogic = 0;
  }
  ppu->window1left = 0;
  ppu->window1right = 0;
  ppu->window2left = 0;
  ppu->window2right = 0;
  ppu->clipMode = 0;
  ppu->preventMathMode = 0;
  ppu->addSubscreen = false;
  ppu->subtractColor = false;
  ppu->halfColor = false;
  memset(ppu->mathEnabled, 0, sizeof(ppu->mathEnabled));
  ppu->fixedColorR = 0;
  ppu->fixedColorG = 0;
  ppu->fixedColorB = 0;
  ppu->forcedBlank = true;
  ppu->brightness = 0;
  ppu->mode = 0;
  ppu->bg3priority = false;
  ppu->evenFrame = false;
  ppu->pseudoHires = false;
  ppu->overscan = false;
  ppu->frameOverscan = false;
  ppu->interlace = false;
  ppu->frameInterlace = false;
  ppu->directColor = false;
  ppu->hCount = 0;
  ppu->vCount = 0;
  ppu->hCountSecond = false;
  ppu->vCountSecond = false;
  ppu->countersLatched = false;
  ppu->ppu1openBus = 0;
  ppu->ppu2openBus = 0;
  memset(ppu->pixelBuffer, 0, sizeof(ppu->pixelBuffer));
  memset(ppu->widePixelBuffer, 0, sizeof(ppu->widePixelBuffer));
  ppu->wideFrame = false;
  ppu->wideSawMode7 = false;
}

void ppu_handleState(Ppu* ppu, StateHandler* sh) {
  sh_handleBools(sh,
    &ppu->vramIncrementOnHigh, &ppu->cgramSecondWrite, &ppu->oamInHigh, &ppu->oamInHighWritten, &ppu->oamSecondWrite,
    &ppu->objPriority, &ppu->timeOver, &ppu->rangeOver, &ppu->objInterlace, &ppu->m7largeField, &ppu->m7charFill,
    &ppu->m7xFlip, &ppu->m7yFlip, &ppu->m7extBg, &ppu->addSubscreen, &ppu->subtractColor, &ppu->halfColor,
    &ppu->mathEnabled[0], &ppu->mathEnabled[1], &ppu->mathEnabled[2], &ppu->mathEnabled[3], &ppu->mathEnabled[4],
    &ppu->mathEnabled[5], &ppu->forcedBlank, &ppu->bg3priority, &ppu->evenFrame, &ppu->pseudoHires, &ppu->overscan,
    &ppu->frameOverscan, &ppu->interlace, &ppu->frameInterlace, &ppu->directColor, &ppu->hCountSecond, &ppu->vCountSecond,
    &ppu->countersLatched, NULL
  );
  sh_handleBytes(sh,
    &ppu->vramRemapMode, &ppu->cgramPointer, &ppu->cgramBuffer, &ppu->oamAdr, &ppu->oamAdrWritten, &ppu->oamBuffer,
    &ppu->objSize, &ppu->scrollPrev, &ppu->scrollPrev2, &ppu->mosaicSize, &ppu->mosaicStartLine, &ppu->m7prev,
    &ppu->window1left, &ppu->window1right, &ppu->window2left, &ppu->window2right, &ppu->clipMode, &ppu->preventMathMode,
    &ppu->fixedColorR, &ppu->fixedColorG, &ppu->fixedColorB, &ppu->brightness, &ppu->mode,
    &ppu->ppu1openBus, &ppu->ppu2openBus, NULL
  );
  sh_handleWords(sh,
    &ppu->vramPointer, &ppu->vramIncrement, &ppu->vramReadBuffer, &ppu->objTileAdr1, &ppu->objTileAdr2,
    &ppu->hCount, &ppu->vCount, NULL
  );
  sh_handleWordsS(sh,
    &ppu->m7matrix[0], &ppu->m7matrix[1], &ppu->m7matrix[2], &ppu->m7matrix[3], &ppu->m7matrix[4], &ppu->m7matrix[5],
    &ppu->m7matrix[6], &ppu->m7matrix[7], NULL
  );
  sh_handleIntsS(sh, &ppu->m7startX, &ppu->m7startY, NULL);
  for(int i = 0; i < 4; i++) {
    sh_handleBools(sh,
      &ppu->bgLayer[i].tilemapWider, &ppu->bgLayer[i].tilemapHigher, &ppu->bgLayer[i].bigTiles,
      &ppu->bgLayer[i].mosaicEnabled, NULL
    );
    sh_handleWords(sh,
      &ppu->bgLayer[i].hScroll, &ppu->bgLayer[i].vScroll, &ppu->bgLayer[i].tilemapAdr, &ppu->bgLayer[i].tileAdr, NULL
    );
  }
  for(int i = 0; i < 5; i++) {
    sh_handleBools(sh,
      &ppu->layer[i].mainScreenEnabled, &ppu->layer[i].subScreenEnabled, &ppu->layer[i].mainScreenWindowed,
      &ppu->layer[i].subScreenWindowed, NULL
    );
  }
  for(int i = 0; i < 6; i++) {
    sh_handleBools(sh,
      &ppu->windowLayer[i].window1enabled, &ppu->windowLayer[i].window1inversed, &ppu->windowLayer[i].window2enabled,
      &ppu->windowLayer[i].window2inversed, NULL
    );
    sh_handleBytes(sh, &ppu->windowLayer[i].maskLogic, NULL);
  }
  sh_handleWordArray(sh, ppu->vram, 0x8000);
  sh_handleWordArray(sh, ppu->cgram, 0x100);
  sh_handleWordArray(sh, ppu->oam, 0x100);
  sh_handleByteArray(sh, ppu->highOam, 0x20);
  sh_handleByteArray(sh, ppu->objPixelBuffer, 256);
  sh_handleByteArray(sh, ppu->objPriorityBuffer, 256);
}

bool ppu_checkOverscan(Ppu* ppu) {
  // called at (0,225)
  ppu->frameOverscan = ppu->overscan; // set if we have a overscan-frame
  return ppu->frameOverscan;
}

void ppu_handleVblank(Ppu* ppu) {
  // called either right after ppu_checkOverscan at (0,225), or at (0,240)
  if(!ppu->forcedBlank) {
    ppu->oamAdr = ppu->oamAdrWritten;
    ppu->oamInHigh = ppu->oamInHighWritten;
    ppu->oamSecondWrite = false;
  }
  ppu->frameInterlace = ppu->interlace; // set if we have a interlaced frame
}

void ppu_handleFrameStart(Ppu* ppu) {
  // called at (0, 0)
  ppu->mosaicStartLine = 1;
  ppu->rangeOver = false;
  ppu->timeOver = false;
  ppu->evenFrame = !ppu->evenFrame;
  // widescreen: decide from the previous frame whether this one is widened
  ppu->wideFrame = ppu->wideExt > 0 &&
    (ppu->wideMode == ppu_wideModeAlways || ppu->wideSawMode7);
  ppu->wideSawMode7 = false;
  for(int i = 0; i < 240; i++) ppu->m7lines[i].valid = false;
  ppu->aboveRendered = 0;
}

typedef struct PpuPixInfo {
  int mainLayer;        // layer that won the main screen (5 = backdrop)
  int baseR, baseG, baseB;   // its colour before clipping / math
  bool mathByWindow;    // colour math applied here because of the colour window
} PpuPixInfo;

static void ppu_composePixel(Ppu* ppu, int x, int y, int* pr, int* pg, int* pb,
                             int* pr2, int* pg2, int* pb2, PpuPixInfo* info);
static void ppu_renderAbove(Ppu* ppu, int fromLine);

void ppu_runLine(Ppu* ppu, int line) {
  // called for lines 1-224/239
  if(ppu->aboveLines > 0 && line == ppu->aboveFromLine && !ppu->forcedBlank && ppu->mode != 7) {
    ppu_renderAbove(ppu, line);
  }
  ppu->m7OverrideLine = ppu->m7Override != NULL && line <= ppu->m7OverrideMaxLine;
  ppu->anchorLine = ppu->anchorEnabled && line >= ppu->anchorMinLine && line <= ppu->anchorMaxLine;
  ppu->bgAnchorLine = ppu->anchorLine && line >= ppu->bgAnchorMinLine;
  // evaluate sprites
  memset(ppu->objPixelBuffer, 0, sizeof(ppu->objPixelBuffer));
  memset(ppu->objWidePixel, 0, sizeof(ppu->objWidePixel));
  if(!ppu->forcedBlank) ppu_evaluateSprites(ppu, line - 1);
  // actual line
  PpuM7Line* m7 = &ppu->m7lines[(line - 1) % 240];
  m7->valid = false;
  if(ppu->mode == 7) {
    ppu_calculateMode7Starts(ppu, line);
    if(!ppu->forcedBlank) {
      ppu->wideSawMode7 = true;
      m7->valid = true;
      m7->xFlip = ppu->m7xFlip;
      m7->startX = ppu->m7startX;
      m7->startY = ppu->m7startY;
      m7->a = ppu->m7matrix[0];
      m7->c = ppu->m7matrix[2];
      m7->brightness = ppu->brightness;
    }
  }
  // window-shaped colour math covering most of the line is a screen effect
  // (e.g. dimming everything but a cut-out), not a HUD element
  ppu->hudWideMath = false;
  if(ppu->hudEnabled && ppu->hudWindowMath && (ppu->preventMathMode == 1 || ppu->preventMathMode == 2)) {
    int n = 0;
    for(int x = 0; x < 256; x++) {
      bool ws = ppu_getWindowState(ppu, 5, x);
      n += ppu->preventMathMode == 1 ? ws : !ws;
    }
    ppu->hudWideMath = n > 160;
  }
  int ext = ppu->wideFrame ? ppu->wideExt : 0;
  for(int x = -ext; x < 256 + ext; x++) {
    ppu_handlePixel(ppu, x, line);
  }
  if(ppu->wideExt > 0 && !ppu->wideFrame) {
    // widescreen on but this frame isn't widened: black extension columns
    int stride = (256 + 2 * ppu->wideExt) * 8;
    uint8_t* row = &ppu->widePixelBuffer[((line - 1) + (ppu->evenFrame ? 0 : 239)) * stride];
    memset(row, 0, ppu->wideExt * 8);
    memset(row + (256 + ppu->wideExt) * 8, 0, ppu->wideExt * 8);
  }
}

void ppu_setAbove(Ppu* ppu, int lines, int fromLine) {
  if(lines < 0) lines = 0;
  if(lines > PPU_ABOVE_MAX) lines = PPU_ABOVE_MAX;
  ppu->aboveLines = lines;
  ppu->aboveFromLine = fromLine;
}

int ppu_putAbove(Ppu* ppu, uint8_t* out) {
  int stride = (256 + 2 * ppu->wideExt) * 8;
  memcpy(out, ppu->aboveBuffer, (size_t)stride * ppu->aboveRendered);
  return ppu->aboveRendered;
}

static bool ppu_bgLayerExtends(Ppu* ppu, int layer) {
  if(ppu->mode != 0 && ppu->mode != 1 && ppu->mode != 3) return false;
  if(bitDepthsPerMode[ppu->mode][layer] > 4 && ppu->mode != 3) return false;   // layer absent in this mode
  if(ppu->mode == 3 && layer > 1) return false;
  return true;
}

// Colours (CGRAM indices) BG `layer` uses on lines y0..y1 with the current scroll.
static void ppu_bgUsedColours(Ppu* ppu, int layer, int y0, int y1, int ext, uint8_t used[256]) {
  memset(used, 0, 256);
  for(int y = y0; y <= y1; y++) {
    int ly = (y + ppu->bgLayer[layer].vScroll) & 0x3ff;
    for(int x = -ext; x < 256 + ext; x += 2) {
      int lx = (x + ppu->bgLayer[layer].hScroll) & 0x3ff;
      int a = ppu_getPixelForBgLayer(ppu, lx, ly, layer, false);
      int b = ppu_getPixelForBgLayer(ppu, lx, ly, layer, true);
      used[a & 0xff] = used[b & 0xff] = 1;
    }
  }
  used[0] = 0;
}

// Does BG `layer` continue on (virtual) line y? False once a row is fully
// transparent (the layer's top edge) or uses colours the real picture's top
// lines don't (other tilemap content stored above the scenery).
static bool ppu_bgRowContinues(Ppu* ppu, int layer, int y, int ext, const uint8_t used[256]) {
  int ly = (y + ppu->bgLayer[layer].vScroll) & 0x3ff;
  int opaque = 0, foreign = 0;
  for(int x = -ext; x < 256 + ext; x++) {
    int lx = (x + ppu->bgLayer[layer].hScroll) & 0x3ff;
    int p = ppu_getPixelForBgLayer(ppu, lx, ly, layer, false);
    if(!p) p = ppu_getPixelForBgLayer(ppu, lx, ly, layer, true);
    if(!p) continue;
    opaque++;
    if(!used[p & 0xff]) foreign++;
  }
  return opaque > 0 && foreign * 50 <= 256 + 2 * ext;   // up to 2% stray pixels
}

// Render ppu->aboveLines virtual lines above `fromLine` with the current
// registers, bottom-up. A BG layer is extended only while it continues the
// picture: once a row is fully transparent (its top edge) or uses colours the
// real top lines don't (other tilemap content), it and everything above are
// left out. Sprites are only
// those that wrap past line 255. Sprite evaluation is redone per virtual
// line, so the CPU-visible range/time-over flags and the line buffers are
// saved and put back.
static void ppu_renderAbove(Ppu* ppu, int fromLine) {
  bool rangeOver = ppu->rangeOver, timeOver = ppu->timeOver;
  uint8_t objPix[256], objPri[256];
  memcpy(objPix, ppu->objPixelBuffer, 256);
  memcpy(objPri, ppu->objPriorityBuffer, 256);
  int n = ppu->aboveLines;
  int ext = ppu->wideFrame ? ppu->wideExt : 0;
  int stride = (256 + 2 * ppu->wideExt) * 8;
  uint8_t stopped = 0;
  static uint8_t used[4][256];
  for(int l = 0; l < 4; l++) {
    if(!ppu_bgLayerExtends(ppu, l)) stopped |= 1 << l;
    else ppu_bgUsedColours(ppu, l, fromLine, fromLine + 15, ext, used[l]);
  }
  for(int i = n - 1; i >= 0; i--) {
    int v = fromLine - n + i;        // virtual line (may be <= 0)
    for(int l = 0; l < 4; l++) {
      if(!(stopped & (1 << l)) && !ppu_bgRowContinues(ppu, l, v, ext, used[l])) stopped |= 1 << l;
    }
    memset(ppu->objPixelBuffer, 0, sizeof(ppu->objPixelBuffer));
    memset(ppu->objWidePixel, 0, sizeof(ppu->objWidePixel));
    ppu->evalAbove = true;
    ppu_evaluateSprites(ppu, v - 1);
    ppu->evalAbove = false;
    uint8_t* row = &ppu->aboveBuffer[i * stride];
    memset(row, 0, stride);
    ppu->aboveBgSkip = stopped;
    for(int x = -ext; x < 256 + ext; x++) {
      int r, g, b, r2, g2, b2;
      PpuPixInfo info;
      ppu->hudSkip = 1 | 2 | 4;      // HUD sprites / BG and window math left out
      ppu_composePixel(ppu, x, v, &r, &g, &b, &r2, &g2, &b2, &info);
      ppu->hudSkip = 0;
      uint8_t* dst = &row[(x + ppu->wideExt) * 8 + ppu->pixelOutputFormat];
      dst[0] = ((b2 << 3) | (b2 >> 2)) * ppu->brightness / 15;
      dst[1] = ((g2 << 3) | (g2 >> 2)) * ppu->brightness / 15;
      dst[2] = ((r2 << 3) | (r2 >> 2)) * ppu->brightness / 15;
      dst[4] = ((b << 3) | (b >> 2)) * ppu->brightness / 15;
      dst[5] = ((g << 3) | (g >> 2)) * ppu->brightness / 15;
      dst[6] = ((r << 3) | (r >> 2)) * ppu->brightness / 15;
    }
    ppu->aboveBgSkip = 0;
  }
  ppu->aboveRendered = n;
  ppu->rangeOver = rangeOver;
  ppu->timeOver = timeOver;
  memcpy(ppu->objPixelBuffer, objPix, 256);
  memcpy(ppu->objPriorityBuffer, objPri, 256);
}

void ppu_setHud(Ppu* ppu, bool enabled, int minLine, int maxLine, const uint8_t objMask[16],
                uint8_t bgMask, bool windowMath) {
  ppu->hudEnabled = enabled;
  ppu->hudMinLine = minLine;
  ppu->hudMaxLine = maxLine;
  if(objMask) memcpy(ppu->hudObjMask, objMask, 16);
  ppu->hudBgMask = bgMask;
  ppu->hudWindowMath = windowMath;
}

int ppu_putHud(Ppu* ppu, uint8_t* out) {
  int stride = (256 + 2 * ppu->wideExt) * 4;
  int rows = ppu->frameOverscan ? 239 : 224;
  for(int y = 0; y < rows; y++) {
    memcpy(out + y * stride, &ppu->hudBuffer[(y + (ppu->evenFrame ? 0 : 239)) * stride], stride);
  }
  return rows;
}

void ppu_setM7Override(Ppu* ppu, const uint16_t* cells, int maxLine) {
  ppu->m7Override = cells;
  ppu->m7OverrideMaxLine = maxLine;
}

void ppu_setObjSep(Ppu* ppu, bool enabled, int minLine, int maxLine, const uint8_t exclude[16]) {
  ppu->objSepEnabled = enabled;
  ppu->objSepMinLine = minLine;
  ppu->objSepMaxLine = maxLine;
  if(exclude) memcpy(ppu->objSepExclude, exclude, 16);
}

void ppu_setObjHide(Ppu* ppu, const uint8_t hide[16]) {
  if(hide) memcpy(ppu->objHide, hide, 16);
  else memset(ppu->objHide, 0, 16);
}

void ppu_setObjSepFrontPalettes(Ppu* ppu, uint8_t mask) {
  ppu->objSepFrontPal = mask;
}

void ppu_setObjSepFrontMaxLine(Ppu* ppu, int maxLine) {
  ppu->objSepFrontMaxLine = maxLine;
}

void ppu_setObjSepFront(Ppu* ppu, const uint8_t front[16]) {
  if(front) memcpy(ppu->objSepFront, front, 16);
  else memset(ppu->objSepFront, 0, 16);
}

void ppu_setAnchors(Ppu* ppu, bool enabled, int minLine, int maxLine, const int8_t objAnchor[128],
                    const int8_t bgAnchor[4], int bgMinLine, int colorWindowAnchor) {
  ppu->anchorEnabled = enabled;
  ppu->bgAnchorMinLine = bgMinLine;
  ppu->anchorMinLine = minLine;
  ppu->anchorMaxLine = maxLine;
  if(objAnchor) memcpy(ppu->objAnchor, objAnchor, 128);
  if(bgAnchor) memcpy(ppu->bgAnchor, bgAnchor, 4);
  ppu->colorWindowAnchor = colorWindowAnchor;
}

int ppu_putObjSep(Ppu* ppu, uint8_t* out) {
  int stride = (256 + 2 * ppu->wideExt) * 4;
  int rows = ppu->frameOverscan ? 239 : 224;
  for(int y = 0; y < rows; y++) {
    memcpy(out + y * stride, &ppu->objSepBuffer[(y + (ppu->evenFrame ? 0 : 239)) * stride], stride);
  }
  return rows;
}

int ppu_putObjSepIndex(Ppu* ppu, uint8_t* out) {
  int stride = 256 + 2 * ppu->wideExt;
  int rows = ppu->frameOverscan ? 239 : 224;
  for(int y = 0; y < rows; y++) {
    memcpy(out + y * stride, &ppu->objSepIndex[(y + (ppu->evenFrame ? 0 : 239)) * stride], stride);
  }
  return rows;
}

const PpuM7Line* ppu_getM7Lines(Ppu* ppu) {
  return ppu->m7lines;
}

void ppu_setWidescreen(Ppu* ppu, int ext, int mode) {
  if(ext < 0) ext = 0;
  if(ext > PPU_WIDE_MAX) ext = PPU_WIDE_MAX;
  if(ext != ppu->wideExt) {
    memset(ppu->widePixelBuffer, 0, sizeof(ppu->widePixelBuffer));
    ppu->wideFrame = false;
  }
  ppu->wideExt = ext;
  ppu->wideMode = mode;
}

int ppu_putPixelsWide(Ppu* ppu, uint8_t* pixels) {
  int stride = (256 + 2 * ppu->wideExt) * 8;
  int rows = ppu->frameOverscan ? 239 : 224;
  for(int y = 0; y < rows; y++) {
    int src = y + (ppu->evenFrame ? 0 : 239);
    memcpy(pixels + y * stride, &ppu->widePixelBuffer[src * stride], stride);
  }
  return rows;
}

void ppu_setPixelOutputFormat(Ppu* ppu, int pixelOutputFormat) {
  ppu->pixelOutputFormat = pixelOutputFormat;
}

static bool ppu_objIsHud(Ppu* ppu, int x) {
  uint8_t i = ppu->objWideIndex[x + PPU_WIDE_MAX];
  return (ppu->hudObjMask[i >> 3] >> (i & 7)) & 1;
}

// sprite pixel at x goes to the separated sprite layer
static bool ppu_objIsSep(Ppu* ppu, int x) {
  uint8_t i = ppu->objWideIndex[x + PPU_WIDE_MAX];
  if(ppu->hudEnabled && ((ppu->hudObjMask[i >> 3] >> (i & 7)) & 1)) return false;
  return !((ppu->objSepExclude[i >> 3] >> (i & 7)) & 1);
}

// screen-space sprite pixel (drawn in front of all 3D)
static bool ppu_objIsFront(Ppu* ppu, int x) {
  uint8_t n = ppu->objWideIndex[x + PPU_WIDE_MAX];
  int pal = (ppu->objWidePixel[x + PPU_WIDE_MAX] >> 4) & 7;
  return ((ppu->objSepFront[n >> 3] >> (n & 7)) & 1) || ((ppu->objSepFrontPal >> pal) & 1);
}

// bottom screen line (1-based) of OAM entry n, wrapped into 1..255
static int ppu_objBottomLine(Ppu* ppu, int n) {
  int index = n * 2;
  int y = ppu->oam[index] >> 8;
  int size = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
  int height = ppu->objInterlace ? size / 2 : size;
  int bottom = (y + height) & 0xff;
  if(bottom == 0) return 1;
  return bottom > 254 ? 254 : bottom;   // 255 marks "in front" sprites
}

static void ppu_composePixel(Ppu* ppu, int x, int y, int* pr, int* pg, int* pb,
                             int* pr2, int* pg2, int* pb2, PpuPixInfo* info) {
  int r = 0, r2 = 0;
  int g = 0, g2 = 0;
  int b = 0, b2 = 0;
  {
    int mainLayer = ppu_getPixel(ppu, x, y, false, &r, &g, &b);
    info->mainLayer = mainLayer;
    info->baseR = r; info->baseG = g; info->baseB = b;
    bool colorWindowState = ppu_getWindowState(ppu, 5, x);
    if(
      ppu->clipMode == 3 ||
      (ppu->clipMode == 2 && colorWindowState) ||
      (ppu->clipMode == 1 && !colorWindowState)
    ) {
      r = 0;
      g = 0;
      b = 0;
    }
    int secondLayer = 5; // backdrop
    bool mathEnabled = mainLayer < 6 && ppu->mathEnabled[mainLayer] && !(
      ppu->preventMathMode == 3 ||
      (ppu->preventMathMode == 2 && colorWindowState) ||
      (ppu->preventMathMode == 1 && !colorWindowState)
    );
    info->mathByWindow = mathEnabled && (ppu->preventMathMode == 1 || ppu->preventMathMode == 2);
    if(info->mathByWindow && (ppu->hudSkip & 4)) mathEnabled = false;
    if((mathEnabled && ppu->addSubscreen) || ppu->pseudoHires || ppu->mode == 5 || ppu->mode == 6) {
      secondLayer = ppu_getPixel(ppu, x, y, true, &r2, &g2, &b2);
    }
    // TODO: subscreen pixels can be clipped to black as well
    // TODO: math for subscreen pixels (add/sub sub to main)
    if(mathEnabled) {
      if(ppu->subtractColor) {
        r -= (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g -= (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b -= (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      } else {
        r += (ppu->addSubscreen && secondLayer != 5) ? r2 : ppu->fixedColorR;
        g += (ppu->addSubscreen && secondLayer != 5) ? g2 : ppu->fixedColorG;
        b += (ppu->addSubscreen && secondLayer != 5) ? b2 : ppu->fixedColorB;
      }
      if(ppu->halfColor && (secondLayer != 5 || !ppu->addSubscreen)) {
        r >>= 1;
        g >>= 1;
        b >>= 1;
      }
      if(r > 31) r = 31;
      if(g > 31) g = 31;
      if(b > 31) b = 31;
      if(r < 0) r = 0;
      if(g < 0) g = 0;
      if(b < 0) b = 0;
    }
    if(!(ppu->pseudoHires || ppu->mode == 5 || ppu->mode == 6)) {
      r2 = r; g2 = g; b2 = b;
    }
  }
  *pr = r; *pg = g; *pb = b;
  *pr2 = r2; *pg2 = g2; *pb2 = b2;
}

static void ppu_handlePixel(Ppu* ppu, int x, int y) {
  int r = 0, r2 = 0;
  int g = 0, g2 = 0;
  int b = 0, b2 = 0;
  int hudR = 0, hudG = 0, hudB = 0, hudClass = 0;
  int sepR = 0, sepG = 0, sepB = 0, sepBottom = 0, sepIndex = 0;
  if(!ppu->forcedBlank) {
    PpuPixInfo info;
    ppu->hudSkip = 0;
    ppu_composePixel(ppu, x, y, &r, &g, &b, &r2, &g2, &b2, &info);
    if(ppu->objSepEnabled && (info.mainLayer == 4 || info.mainLayer == 6) && ppu_objIsSep(ppu, x) &&
       ((y >= ppu->objSepMinLine && y <= ppu->objSepMaxLine) ||
        (y <= ppu->objSepFrontMaxLine && ppu_objIsFront(ppu, x)))) {
      // a track sprite: to the sprite layer, the picture shows what is behind it
      sepR = info.baseR; sepG = info.baseG; sepB = info.baseB;
      {
        uint8_t n = ppu->objWideIndex[x + PPU_WIDE_MAX];
        sepBottom = ppu_objIsFront(ppu, x) ? 255 : ppu_objBottomLine(ppu, n);
        sepIndex = n + 1;
      }
      ppu->hudSkip = 8;
      ppu_composePixel(ppu, x, y, &r, &g, &b, &r2, &g2, &b2, &info);
      ppu->hudSkip = 0;
    }
    if(ppu->hudEnabled && y >= ppu->hudMinLine && y <= ppu->hudMaxLine) {
      int ml = info.mainLayer;
      int skip = 0;
      if((ml == 4 || ml == 6) && ppu_objIsHud(ppu, x)) {
        hudClass = ppu_hudClassObj; skip = 1;
        hudR = info.baseR; hudG = info.baseG; hudB = info.baseB;
      } else if(ml < 4 && ppu->mode != 7 && ((ppu->hudBgMask >> ml) & 1)) {
        hudClass = ppu_hudClassBg; skip = 2;
        hudR = info.baseR; hudG = info.baseG; hudB = info.baseB;
      } else if(ppu->hudWindowMath && info.mathByWindow && !ppu->hudWideMath) {
        hudClass = ppu_hudClassMath | (ppu->subtractColor ? 0x10 : 0) | (ppu->halfColor ? 0x20 : 0);
        skip = 4;
        hudR = ppu->fixedColorR; hudG = ppu->fixedColorG; hudB = ppu->fixedColorB;
      } else if(ppu->hudWindowMath && info.mathByWindow && ppu->hudDropWideMath) {
        skip = 4;   // screen-wide effect the layout can't carry: left out
      }
      if(skip) {   // the picture as it would look without the HUD element
        ppu->hudSkip = (uint8_t)(skip | (sepBottom ? 8 : 0));
        ppu_composePixel(ppu, x, y, &r, &g, &b, &r2, &g2, &b2, &info);
        if(skip != 4 && ppu->hudWindowMath && info.mathByWindow &&
           (!ppu->hudWideMath || ppu->hudDropWideMath)) {
          // window-math HUD (the position numeral) under a HUD sprite / BG:
          // leave it out of the picture too, flagged so it is still drawn;
          // a dropped screen-wide effect goes as well
          if(!ppu->hudWideMath) hudClass |= 0x40;
          ppu->hudSkip |= 4;
          ppu_composePixel(ppu, x, y, &r, &g, &b, &r2, &g2, &b2, &info);
        }
        ppu->hudSkip = 0;
      }
    }
  }
  int row = (y - 1) + (ppu->evenFrame ? 0 : 239);
  uint8_t px[6] = {
    ((b2 << 3) | (b2 >> 2)) * ppu->brightness / 15,
    ((g2 << 3) | (g2 >> 2)) * ppu->brightness / 15,
    ((r2 << 3) | (r2 >> 2)) * ppu->brightness / 15,
    ((b << 3) | (b >> 2)) * ppu->brightness / 15,
    ((g << 3) | (g >> 2)) * ppu->brightness / 15,
    ((r << 3) | (r >> 2)) * ppu->brightness / 15,
  };
  if(x >= 0 && x < 256) {
    uint8_t* dst = &ppu->pixelBuffer[row * 2048 + x * 8 + ppu->pixelOutputFormat];
    dst[0] = px[0]; dst[1] = px[1]; dst[2] = px[2];
    dst[4] = px[3]; dst[5] = px[4]; dst[6] = px[5];
  }
  if(ppu->wideExt > 0) {
    int stride = (256 + 2 * ppu->wideExt) * 8;
    uint8_t* dst = &ppu->widePixelBuffer[row * stride + (x + ppu->wideExt) * 8 + ppu->pixelOutputFormat];
    dst[0] = px[0]; dst[1] = px[1]; dst[2] = px[2];
    dst[4] = px[3]; dst[5] = px[4]; dst[6] = px[5];
  }
  if(ppu->objSepEnabled) {
    int stride = (256 + 2 * ppu->wideExt) * 4;
    uint8_t* o = &ppu->objSepBuffer[row * stride + (x + ppu->wideExt) * 4];
    o[0] = ((sepR << 3) | (sepR >> 2)) * ppu->brightness / 15;
    o[1] = ((sepG << 3) | (sepG >> 2)) * ppu->brightness / 15;
    o[2] = ((sepB << 3) | (sepB >> 2)) * ppu->brightness / 15;
    o[3] = (uint8_t)sepBottom;
    ppu->objSepIndex[row * (256 + 2 * ppu->wideExt) + x + ppu->wideExt] = (uint8_t)sepIndex;
  }
  if(ppu->hudEnabled) {
    int stride = (256 + 2 * ppu->wideExt) * 4;
    uint8_t* h = &ppu->hudBuffer[row * stride + (x + ppu->wideExt) * 4];
    h[0] = ((hudR << 3) | (hudR >> 2)) * ppu->brightness / 15;
    h[1] = ((hudG << 3) | (hudG >> 2)) * ppu->brightness / 15;
    h[2] = ((hudB << 3) | (hudB >> 2)) * ppu->brightness / 15;
    h[3] = (uint8_t)hudClass;
  }
}

static int ppu_getPixel(Ppu* ppu, int x, int y, bool sub, int* r, int* g, int* b) {
  // figure out which color is on this location on main- or subscreen, sets it in r, g, b
  // returns which layer it is: 0-3 for bg layer, 4 or 6 for sprites (depending on palette), 5 for backdrop
  int actMode = ppu->mode == 1 && ppu->bg3priority ? 8 : ppu->mode;
  actMode = ppu->mode == 7 && ppu->m7extBg ? 9 : actMode;
  int layer = 5;
  int pixel = 0;
  for(int i = 0; i < layerCountPerMode[actMode]; i++) {
    int curLayer = layersPerMode[actMode][i];
    int curPriority = prioritysPerMode[actMode][i];
    bool layerActive = false;
    if(!sub) {
      layerActive = ppu->layer[curLayer].mainScreenEnabled && (
        !ppu->layer[curLayer].mainScreenWindowed || !ppu_getWindowState(ppu, curLayer, x)
      );
    } else {
      layerActive = ppu->layer[curLayer].subScreenEnabled && (
        !ppu->layer[curLayer].subScreenWindowed || !ppu_getWindowState(ppu, curLayer, x)
      );
    }
    if(layerActive && curLayer < 4 && (ppu->hudSkip & 2) && ppu->mode != 7 &&
       ((ppu->hudBgMask >> curLayer) & 1)) {
      layerActive = false;   // HUD BG layer left out of this compose pass
    }
    if(layerActive && curLayer < 4 && ppu->mode != 7 && ((ppu->hudBgMask >> curLayer) & 1)) {
      // a HUD BG layer only has content in the 256 hardware columns (where
      // the anchoring put them); its tilemap beyond them is unrelated
      int src = x - (ppu->wideFrame && ppu->bgAnchorLine ? ppu->bgAnchor[curLayer] * ppu->wideExt : 0);
      if(src < 0 || src > 255) layerActive = false;
    }
    if(layerActive && curLayer < 4 && ((ppu->aboveBgSkip >> curLayer) & 1)) {
      layerActive = false;   // virtual line above this layer's content
    }
    if(layerActive) {
      if(curLayer < 4) {
        // bg layer
        int lx = x;
        int ly = y;
        if(ppu->bgLayer[curLayer].mosaicEnabled && ppu->mosaicSize > 1) {
          lx -= ((lx % ppu->mosaicSize) + ppu->mosaicSize) % ppu->mosaicSize; // x < 0 in widescreen
          ly -= (ly - ppu->mosaicStartLine) % ppu->mosaicSize;
        }
        if(ppu->mode == 7) {
          pixel = ppu_getPixelForMode7(ppu, lx, curLayer, curPriority);
        } else {
          lx += ppu->bgLayer[curLayer].hScroll;
          if(ppu->wideFrame && ppu->bgAnchorLine) lx -= ppu->bgAnchor[curLayer] * ppu->wideExt;
          if(ppu->mode == 5 || ppu->mode == 6) {
            lx *= 2;
            lx += (sub || ppu->bgLayer[curLayer].mosaicEnabled) ? 0 : 1;
            if(ppu->interlace) {
              ly *= 2;
              ly += (ppu->evenFrame || ppu->bgLayer[curLayer].mosaicEnabled) ? 0 : 1;
            }
          }
          ly += ppu->bgLayer[curLayer].vScroll;
          if(ppu->mode == 2 || ppu->mode == 4 || ppu->mode == 6) {
            ppu_handleOPT(ppu, curLayer, &lx, &ly);
          }
          pixel = ppu_getPixelForBgLayer(
            ppu, lx & 0x3ff, ly & 0x3ff,
            curLayer, curPriority
          );
        }
      } else {
        // get a pixel from the sprite buffer
        pixel = 0;
        if(ppu->objWidePriority[x + PPU_WIDE_MAX] == curPriority) pixel = ppu->objWidePixel[x + PPU_WIDE_MAX];
        if(pixel && (ppu->hudSkip & 1) && ppu_objIsHud(ppu, x)) pixel = 0;
        if(pixel && (ppu->hudSkip & 8) && ppu_objIsSep(ppu, x)) pixel = 0;
      }
    }
    if(pixel > 0) {
      layer = curLayer;
      break;
    }
  }
  if(ppu->directColor && layer < 4 && bitDepthsPerMode[actMode][layer] == 8) {
    *r = ((pixel & 0x7) << 2) | ((pixel & 0x100) >> 7);
    *g = ((pixel & 0x38) >> 1) | ((pixel & 0x200) >> 8);
    *b = ((pixel & 0xc0) >> 3) | ((pixel & 0x400) >> 8);
  } else {
    uint16_t color = ppu->cgram[pixel & 0xff];
    *r = color & 0x1f;
    *g = (color >> 5) & 0x1f;
    *b = (color >> 10) & 0x1f;
  }
  if(layer == 4 && pixel < 0xc0) layer = 6; // sprites with palette color < 0xc0
  return layer;
}

static void ppu_handleOPT(Ppu* ppu, int layer, int* lx, int* ly) {
  int x = *lx;
  int y = *ly;
  int column = 0;
  if(ppu->mode == 6) {
    column = ((x - (x & 0xf)) - ((ppu->bgLayer[layer].hScroll * 2) & 0xfff0)) >> 4;
  } else {
    column = ((x - (x & 0x7)) - (ppu->bgLayer[layer].hScroll & 0xfff8)) >> 3;
  }
  if(column > 0) {
    // fetch offset values from layer 3 tilemap
    int valid = layer == 0 ? 0x2000 : 0x4000;
    uint16_t hOffset = ppu_getOffsetValue(ppu, column - 1, 0);
    uint16_t vOffset = 0;
    if(ppu->mode == 4) {
      if(hOffset & 0x8000) {
        vOffset = hOffset;
        hOffset = 0;
      }
    } else {
      vOffset = ppu_getOffsetValue(ppu, column - 1, 1);
    }
    if(ppu->mode == 6) {
      // TODO: not sure if correct
      if(hOffset & valid) *lx = (((hOffset & 0x3f8) + (column * 8)) * 2) | (x & 0xf);
    } else {
      if(hOffset & valid) *lx = ((hOffset & 0x3f8) + (column * 8)) | (x & 0x7);
    }
    // TODO: not sure if correct for interlace
    if(vOffset & valid) *ly = (vOffset & 0x3ff) + (y - ppu->bgLayer[layer].vScroll);
  }
}

static uint16_t ppu_getOffsetValue(Ppu* ppu, int col, int row) {
  int x = col * 8 + ppu->bgLayer[2].hScroll;
  int y = row * 8 + ppu->bgLayer[2].vScroll;
  int tileBits = ppu->bgLayer[2].bigTiles ? 4 : 3;
  int tileHighBit = ppu->bgLayer[2].bigTiles ? 0x200 : 0x100;
  uint16_t tilemapAdr = ppu->bgLayer[2].tilemapAdr + (((y >> tileBits) & 0x1f) << 5 | ((x >> tileBits) & 0x1f));
  if((x & tileHighBit) && ppu->bgLayer[2].tilemapWider) tilemapAdr += 0x400;
  if((y & tileHighBit) && ppu->bgLayer[2].tilemapHigher) tilemapAdr += ppu->bgLayer[2].tilemapWider ? 0x800 : 0x400;
  return ppu->vram[tilemapAdr & 0x7fff];
}

static int ppu_getPixelForBgLayer(Ppu* ppu, int x, int y, int layer, bool priority) {
  // figure out address of tilemap word and read it
  bool wideTiles = ppu->bgLayer[layer].bigTiles || ppu->mode == 5 || ppu->mode == 6;
  int tileBitsX = wideTiles ? 4 : 3;
  int tileHighBitX = wideTiles ? 0x200 : 0x100;
  int tileBitsY = ppu->bgLayer[layer].bigTiles ? 4 : 3;
  int tileHighBitY = ppu->bgLayer[layer].bigTiles ? 0x200 : 0x100;
  uint16_t tilemapAdr = ppu->bgLayer[layer].tilemapAdr + (((y >> tileBitsY) & 0x1f) << 5 | ((x >> tileBitsX) & 0x1f));
  if((x & tileHighBitX) && ppu->bgLayer[layer].tilemapWider) tilemapAdr += 0x400;
  if((y & tileHighBitY) && ppu->bgLayer[layer].tilemapHigher) tilemapAdr += ppu->bgLayer[layer].tilemapWider ? 0x800 : 0x400;
  uint16_t tile = ppu->vram[tilemapAdr & 0x7fff];
  // check priority, get palette
  if(((bool) (tile & 0x2000)) != priority) return 0; // wrong priority
  int paletteNum = (tile & 0x1c00) >> 10;
  // figure out position within tile
  int row = (tile & 0x8000) ? 7 - (y & 0x7) : (y & 0x7);
  int col = (tile & 0x4000) ? (x & 0x7) : 7 - (x & 0x7);
  int tileNum = tile & 0x3ff;
  if(wideTiles) {
    // if unflipped right half of tile, or flipped left half of tile
    if(((bool) (x & 8)) ^ ((bool) (tile & 0x4000))) tileNum += 1;
  }
  if(ppu->bgLayer[layer].bigTiles) {
    // if unflipped bottom half of tile, or flipped upper half of tile
    if(((bool) (y & 8)) ^ ((bool) (tile & 0x8000))) tileNum += 0x10;
  }
  // read tiledata, ajust palette for mode 0
  int bitDepth = bitDepthsPerMode[ppu->mode][layer];
  if(ppu->mode == 0) paletteNum += 8 * layer;
  // plane 1 (always)
  int paletteSize = 4;
  uint16_t plane1 = ppu->vram[(ppu->bgLayer[layer].tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + row) & 0x7fff];
  int pixel = (plane1 >> col) & 1;
  pixel |= ((plane1 >> (8 + col)) & 1) << 1;
  // plane 2 (for 4bpp, 8bpp)
  if(bitDepth > 2) {
    paletteSize = 16;
    uint16_t plane2 = ppu->vram[(ppu->bgLayer[layer].tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + 8 + row) & 0x7fff];
    pixel |= ((plane2 >> col) & 1) << 2;
    pixel |= ((plane2 >> (8 + col)) & 1) << 3;
  }
  // plane 3 & 4 (for 8bpp)
  if(bitDepth > 4) {
    paletteSize = 256;
    uint16_t plane3 = ppu->vram[(ppu->bgLayer[layer].tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + 16 + row) & 0x7fff];
    pixel |= ((plane3 >> col) & 1) << 4;
    pixel |= ((plane3 >> (8 + col)) & 1) << 5;
    uint16_t plane4 = ppu->vram[(ppu->bgLayer[layer].tileAdr + ((tileNum & 0x3ff) * 4 * bitDepth) + 24 + row) & 0x7fff];
    pixel |= ((plane4 >> col) & 1) << 6;
    pixel |= ((plane4 >> (8 + col)) & 1) << 7;
  }
  // return cgram index, or 0 if transparent, palette number in bits 10-8 for 8-color layers
  return pixel == 0 ? 0 : paletteSize * paletteNum + pixel;
}

static void ppu_calculateMode7Starts(Ppu* ppu, int y) {
  // expand 13-bit values to signed values
  int hScroll = ((int16_t) (ppu->m7matrix[6] << 3)) >> 3;
  int vScroll = ((int16_t) (ppu->m7matrix[7] << 3)) >> 3;
  int xCenter = ((int16_t) (ppu->m7matrix[4] << 3)) >> 3;
  int yCenter = ((int16_t) (ppu->m7matrix[5] << 3)) >> 3;
  // do calculation
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  if(ppu->bgLayer[0].mosaicEnabled && ppu->mosaicSize > 1) {
    y -= (y - ppu->mosaicStartLine) % ppu->mosaicSize;
  }
  uint8_t ry = ppu->m7yFlip ? 255 - y : y;
  ppu->m7startX = (
    ((ppu->m7matrix[0] * clippedH) & ~63) +
    ((ppu->m7matrix[1] * ry) & ~63) +
    ((ppu->m7matrix[1] * clippedV) & ~63) +
    (xCenter << 8)
  );
  ppu->m7startY = (
    ((ppu->m7matrix[2] * clippedH) & ~63) +
    ((ppu->m7matrix[3] * ry) & ~63) +
    ((ppu->m7matrix[3] * clippedV) & ~63) +
    (yCenter << 8)
  );
}

static int ppu_getPixelForMode7(Ppu* ppu, int x, int layer, bool priority) {
  int rx = ppu->m7xFlip ? 255 - x : x; // int: x runs past 0..255 in widescreen
  int xPos = (ppu->m7startX + ppu->m7matrix[0] * rx) >> 8;
  int yPos = (ppu->m7startY + ppu->m7matrix[2] * rx) >> 8;
  bool outsideMap = xPos < 0 || xPos >= 1024 || yPos < 0 || yPos >= 1024;
  xPos &= 0x3ff;
  yPos &= 0x3ff;
  if(!ppu->m7largeField) outsideMap = false;
  int cell = (yPos >> 3) * 128 + (xPos >> 3);
  uint8_t tile = outsideMap ? 0 : ppu->vram[cell] & 0xff;
  if(ppu->m7OverrideLine && !outsideMap && ppu->m7Override[cell] != 0xffff) tile = (uint8_t)ppu->m7Override[cell];
  uint8_t pixel = outsideMap && !ppu->m7charFill ? 0 : ppu->vram[tile * 64 + (yPos & 7) * 8 + (xPos & 7)] >> 8;
  if(layer == 1) {
    if(((bool) (pixel & 0x80)) != priority) return 0;
    return pixel & 0x7f;
  }
  return pixel;
}

static bool ppu_getWindowState(Ppu* ppu, int layer, int x) {
  if(!ppu->windowLayer[layer].window1enabled && !ppu->windowLayer[layer].window2enabled) {
    return false;
  }
  // widescreen UI anchoring of the colour window (window-shaped colour math)
  if(layer == 5 && ppu->wideFrame && ppu->anchorLine) x -= ppu->colorWindowAnchor * ppu->wideExt;
  // widescreen extension columns take the window state of the nearest edge
  if(x < 0) x = 0;
  if(x > 255) x = 255;
  if(ppu->windowLayer[layer].window1enabled && !ppu->windowLayer[layer].window2enabled) {
    bool test = x >= ppu->window1left && x <= ppu->window1right;
    return ppu->windowLayer[layer].window1inversed ? !test : test;
  }
  if(!ppu->windowLayer[layer].window1enabled && ppu->windowLayer[layer].window2enabled) {
    bool test = x >= ppu->window2left && x <= ppu->window2right;
    return ppu->windowLayer[layer].window2inversed ? !test : test;
  }
  bool test1 = x >= ppu->window1left && x <= ppu->window1right;
  bool test2 = x >= ppu->window2left && x <= ppu->window2right;
  if(ppu->windowLayer[layer].window1inversed) test1 = !test1;
  if(ppu->windowLayer[layer].window2inversed) test2 = !test2;
  switch(ppu->windowLayer[layer].maskLogic) {
    case 0: return test1 || test2;
    case 1: return test1 && test2;
    case 2: return test1 != test2;
    case 3: return test1 == test2;
  }
  return false;
}

static void ppu_evaluateSprites(Ppu* ppu, int line) {
  // TODO: rectangular sprites, wierdness with sprites at -256
  // Widescreen: sprites/slivers visible only in the extension columns are
  // drawn too, but never count toward the 32-sprite / 34-sliver hardware
  // limits, so rangeOver/timeOver (CPU-visible) match the 256-wide hardware.
  int ext = ppu->wideFrame ? ppu->wideExt : 0;
  uint8_t index = ppu->objPriority ? (ppu->oamAdr & 0xfe) : 0;
  int spritesFound = 0; // hardware in-range sprites
  int wideOnly = 0;     // sprites visible only in the left extension
  int listed = 0;
  int tilesFound = 0;
  uint8_t foundSprites[64] = {};
  // iterate over oam to find sprites in range
  for(int i = 0; i < 128; i++) {
    uint8_t y = ppu->oam[index] >> 8;
    // check if the sprite is on this line and get the sprite size
    uint8_t row = line - y;
    int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int spriteHeight = ppu->objInterlace ? spriteSize / 2 : spriteSize;
    // virtual lines above the screen (line < 0): only sprites that wrap past
    // line 255 (others are parked off the bottom or belong to the lower
    // screen), and screen-space ones (Lakitu) flying up out of the picture
    // (y 224 / 240: the usual parking lines for unused sprites, not "above")
    bool frontAbove = ppu->evalAbove && y >= 160 && y != 224 && y != 240 &&
                      ((ppu->objSepFront[index >> 4] >> ((index >> 1) & 7)) & 1);
    if(row < spriteHeight && (!ppu->evalAbove || line >= 0 || y + spriteHeight > 256 || frontAbove)) {
      // in y-range, get the x location, using the high bit as well
      int x = ppu->oam[index] & 0xff;
      x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
      if(x > 255) x -= 512;
      // if in x-range, record
      if(x > -spriteSize) {
        // break if we found 32 sprites already
        spritesFound++;
        if(spritesFound > 32) {
          ppu->rangeOver = true;
          spritesFound = 32;
          break;
        }
        foundSprites[listed++] = index;
      } else if(x > -spriteSize - ext && wideOnly < 32) {
        wideOnly++;
        foundSprites[listed++] = index;
      }
    }
    index += 2;
  }
  // iterate over found sprites backwards to fetch max 34 tile slivers
  for(int i = listed; i > 0; i--) {
    index = foundSprites[i - 1];
    uint8_t y = ppu->oam[index] >> 8;
    uint8_t row = line - y;
    int spriteSize = spriteSizes[ppu->objSize][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int x = ppu->oam[index] & 0xff;
    x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if(x > 255) x -= 512;
    bool hwSprite = x > -spriteSize;
    // widescreen UI anchoring: drawn shifted, counted where the hardware has it
    int drawX = x + (ppu->wideFrame && ppu->anchorLine ? ppu->objAnchor[index >> 1] * ext : 0);
    bool hidden = (ppu->objHide[index >> 4] >> ((index >> 1) & 7)) & 1;
    // update row according to obj-interlace
    if(ppu->objInterlace) row = row * 2 + (ppu->evenFrame ? 0 : 1);
    // get some data for the sprite and y-flip row if needed
    int tile = ppu->oam[index + 1] & 0xff;
    int palette = (ppu->oam[index + 1] & 0xe00) >> 9;
    bool hFlipped = ppu->oam[index + 1] & 0x4000;
    if(ppu->oam[index + 1] & 0x8000) row = spriteSize - 1 - row;
    // fetch all tiles in x-range
    for(int col = 0; col < spriteSize; col += 8) {
      if(hwSprite && col + x > -8 && col + x < 256) {
        // break if we found > 34 8*1 slivers already
        tilesFound++;
        if(tilesFound > 34) {
          ppu->timeOver = true;
          break;
        }
      }
      if(hidden) continue;   // drawn in 3D instead (counted above like hardware)
      if(col + drawX > -8 - ext && col + drawX < 256 + ext) {
        // figure out which tile this uses, looping within 16x16 pages, and get it's data
        int usedCol = hFlipped ? spriteSize - 1 - col : col;
        uint8_t usedTile = (((tile >> 4) + (row / 8)) << 4) | (((tile & 0xf) + (usedCol / 8)) & 0xf);
        uint16_t objAdr = (ppu->oam[index + 1] & 0x100) ? ppu->objTileAdr2 : ppu->objTileAdr1;
        uint16_t plane1 = ppu->vram[(objAdr + usedTile * 16 + (row & 0x7)) & 0x7fff];
        uint16_t plane2 = ppu->vram[(objAdr + usedTile * 16 + 8 + (row & 0x7)) & 0x7fff];
        // go over each pixel
        for(int px = 0; px < 8; px++) {
          int shift = hFlipped ? px : 7 - px;
          int pixel = (plane1 >> shift) & 1;
          pixel |= ((plane1 >> (8 + shift)) & 1) << 1;
          pixel |= ((plane2 >> shift) & 1) << 2;
          pixel |= ((plane2 >> (8 + shift)) & 1) << 3;
          // draw it in the buffer if there is a pixel here
          int screenCol = col + drawX + px;
          if(pixel > 0 && screenCol >= -ext && screenCol < 256 + ext) {
            ppu->objWidePixel[screenCol + PPU_WIDE_MAX] = 0x80 + 16 * palette + pixel;
            ppu->objWidePriority[screenCol + PPU_WIDE_MAX] = (ppu->oam[index + 1] & 0x3000) >> 12;
            ppu->objWideIndex[screenCol + PPU_WIDE_MAX] = index >> 1;
          }
        }
      }
    }
    if(tilesFound > 34) break; // break out of sprite-loop if max tiles found
  }
  // keep the (serialized) 256-wide line buffers identical to upstream
  memcpy(ppu->objPixelBuffer, &ppu->objWidePixel[PPU_WIDE_MAX], 256);
  memcpy(ppu->objPriorityBuffer, &ppu->objWidePriority[PPU_WIDE_MAX], 256);
}

static uint16_t ppu_getVramRemap(Ppu* ppu) {
  uint16_t adr = ppu->vramPointer;
  switch(ppu->vramRemapMode) {
    case 0: return adr;
    case 1: return (adr & 0xff00) | ((adr & 0xe0) >> 5) | ((adr & 0x1f) << 3);
    case 2: return (adr & 0xfe00) | ((adr & 0x1c0) >> 6) | ((adr & 0x3f) << 3);
    case 3: return (adr & 0xfc00) | ((adr & 0x380) >> 7) | ((adr & 0x7f) << 3);
  }
  return adr;
}

uint8_t ppu_read(Ppu* ppu, uint8_t adr) {
  switch(adr) {
    case 0x04: case 0x14: case 0x24:
    case 0x05: case 0x15: case 0x25:
    case 0x06: case 0x16: case 0x26:
    case 0x08: case 0x18: case 0x28:
    case 0x09: case 0x19: case 0x29:
    case 0x0a: case 0x1a: case 0x2a: {
      return ppu->ppu1openBus;
    }
    case 0x34:
    case 0x35:
    case 0x36: {
      int result = ppu->m7matrix[0] * (ppu->m7matrix[1] >> 8);
      ppu->ppu1openBus = (result >> (8 * (adr - 0x34))) & 0xff;
      return ppu->ppu1openBus;
    }
    case 0x37: {
      // TODO: only when ppulatch is set
      ppu->hCount = ppu->snes->hPos / 4;
      ppu->vCount = ppu->snes->vPos;
      ppu->countersLatched = true;
      return ppu->snes->openBus;
    }
    case 0x38: {
      uint8_t ret = 0;
      if(ppu->oamInHigh) {
        ret = ppu->highOam[((ppu->oamAdr & 0xf) << 1) | ppu->oamSecondWrite];
        if(ppu->oamSecondWrite) {
          ppu->oamAdr++;
          if(ppu->oamAdr == 0) ppu->oamInHigh = false;
        }
      } else {
        if(!ppu->oamSecondWrite) {
          ret = ppu->oam[ppu->oamAdr] & 0xff;
        } else {
          ret = ppu->oam[ppu->oamAdr++] >> 8;
          if(ppu->oamAdr == 0) ppu->oamInHigh = true;
        }
      }
      ppu->oamSecondWrite = !ppu->oamSecondWrite;
      ppu->ppu1openBus = ret;
      return ret;
    }
    case 0x39: {
      uint16_t val = ppu->vramReadBuffer;
      if(!ppu->vramIncrementOnHigh) {
        ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
        ppu->vramPointer += ppu->vramIncrement;
      }
      ppu->ppu1openBus = val & 0xff;
      return val & 0xff;
    }
    case 0x3a: {
      uint16_t val = ppu->vramReadBuffer;
      if(ppu->vramIncrementOnHigh) {
        ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
        ppu->vramPointer += ppu->vramIncrement;
      }
      ppu->ppu1openBus = val >> 8;
      return val >> 8;
    }
    case 0x3b: {
      uint8_t ret = 0;
      if(!ppu->cgramSecondWrite) {
        ret = ppu->cgram[ppu->cgramPointer] & 0xff;
      } else {
        ret = ((ppu->cgram[ppu->cgramPointer++] >> 8) & 0x7f) | (ppu->ppu2openBus & 0x80);
      }
      ppu->cgramSecondWrite = !ppu->cgramSecondWrite;
      ppu->ppu2openBus = ret;
      return ret;
    }
    case 0x3c: {
      uint8_t val = 0;
      if(ppu->hCountSecond) {
        val = ((ppu->hCount >> 8) & 1) | (ppu->ppu2openBus & 0xfe);
      } else {
        val = ppu->hCount & 0xff;
      }
      ppu->hCountSecond = !ppu->hCountSecond;
      ppu->ppu2openBus = val;
      return val;
    }
    case 0x3d: {
      uint8_t val = 0;
      if(ppu->vCountSecond) {
        val = ((ppu->vCount >> 8) & 1) | (ppu->ppu2openBus & 0xfe);
      } else {
        val = ppu->vCount & 0xff;
      }
      ppu->vCountSecond = !ppu->vCountSecond;
      ppu->ppu2openBus = val;
      return val;
    }
    case 0x3e: {
      uint8_t val = 0x1; // ppu1 version (4 bit)
      val |= ppu->ppu1openBus & 0x10;
      val |= ppu->rangeOver << 6;
      val |= ppu->timeOver << 7;
      ppu->ppu1openBus = val;
      return val;
    }
    case 0x3f: {
      uint8_t val = 0x3; // ppu2 version (4 bit)
      val |= ppu->snes->palTiming << 4; // ntsc/pal
      val |= ppu->ppu2openBus & 0x20;
      val |= ppu->countersLatched << 6;
      val |= ppu->evenFrame << 7;
      ppu->countersLatched = false; // TODO: only when ppulatch is set
      ppu->hCountSecond = false;
      ppu->vCountSecond = false;
      ppu->ppu2openBus = val;
      return val;
    }
    default: {
      return ppu->snes->openBus;
    }
  }
}

void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val) {
  switch(adr) {
    case 0x00: {
      // TODO: oam address reset when written on first line of vblank, (and when forced blank is disabled?)
      ppu->brightness = val & 0xf;
      ppu->forcedBlank = val & 0x80;
      break;
    }
    case 0x01: {
      ppu->objSize = val >> 5;
      ppu->objTileAdr1 = (val & 7) << 13;
      ppu->objTileAdr2 = ppu->objTileAdr1 + (((val & 0x18) + 8) << 9);
      break;
    }
    case 0x02: {
      ppu->oamAdr = val;
      ppu->oamAdrWritten = ppu->oamAdr;
      ppu->oamInHigh = ppu->oamInHighWritten;
      ppu->oamSecondWrite = false;
      break;
    }
    case 0x03: {
      ppu->objPriority = val & 0x80;
      ppu->oamInHigh = val & 1;
      ppu->oamInHighWritten = ppu->oamInHigh;
      ppu->oamAdr = ppu->oamAdrWritten;
      ppu->oamSecondWrite = false;
      break;
    }
    case 0x04: {
      if(ppu->oamInHigh) {
        ppu->highOam[((ppu->oamAdr & 0xf) << 1) | ppu->oamSecondWrite] = val;
        if(ppu->oamSecondWrite) {
          ppu->oamAdr++;
          if(ppu->oamAdr == 0) ppu->oamInHigh = false;
        }
      } else {
        if(!ppu->oamSecondWrite) {
          ppu->oamBuffer = val;
        } else {
          ppu->oam[ppu->oamAdr++] = (val << 8) | ppu->oamBuffer;
          if(ppu->oamAdr == 0) ppu->oamInHigh = true;
        }
      }
      ppu->oamSecondWrite = !ppu->oamSecondWrite;
      break;
    }
    case 0x05: {
      ppu->mode = val & 0x7;
      ppu->bg3priority = val & 0x8;
      ppu->bgLayer[0].bigTiles = val & 0x10;
      ppu->bgLayer[1].bigTiles = val & 0x20;
      ppu->bgLayer[2].bigTiles = val & 0x40;
      ppu->bgLayer[3].bigTiles = val & 0x80;
      break;
    }
    case 0x06: {
      // TODO: mosaic line reset specifics
      ppu->bgLayer[0].mosaicEnabled = val & 0x1;
      ppu->bgLayer[1].mosaicEnabled = val & 0x2;
      ppu->bgLayer[2].mosaicEnabled = val & 0x4;
      ppu->bgLayer[3].mosaicEnabled = val & 0x8;
      ppu->mosaicSize = (val >> 4) + 1;
      ppu->mosaicStartLine = ppu->snes->vPos;
      break;
    }
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0a: {
      ppu->bgLayer[adr - 7].tilemapWider = val & 0x1;
      ppu->bgLayer[adr - 7].tilemapHigher = val & 0x2;
      ppu->bgLayer[adr - 7].tilemapAdr = (val & 0xfc) << 8;
      break;
    }
    case 0x0b: {
      ppu->bgLayer[0].tileAdr = (val & 0xf) << 12;
      ppu->bgLayer[1].tileAdr = (val & 0xf0) << 8;
      break;
    }
    case 0x0c: {
      ppu->bgLayer[2].tileAdr = (val & 0xf) << 12;
      ppu->bgLayer[3].tileAdr = (val & 0xf0) << 8;
      break;
    }
    case 0x0d: {
      ppu->m7matrix[6] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-HOFS
    }
    case 0x0f:
    case 0x11:
    case 0x13: {
      ppu->bgLayer[(adr - 0xd) / 2].hScroll = ((val << 8) | (ppu->scrollPrev & 0xf8) | (ppu->scrollPrev2 & 0x7)) & 0x3ff;
      ppu->scrollPrev = val;
      ppu->scrollPrev2 = val;
      break;
    }
    case 0x0e: {
      ppu->m7matrix[7] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-VOFS
    }
    case 0x10:
    case 0x12:
    case 0x14: {
      ppu->bgLayer[(adr - 0xe) / 2].vScroll = ((val << 8) | ppu->scrollPrev) & 0x3ff;
      ppu->scrollPrev = val;
      break;
    }
    case 0x15: {
      if((val & 3) == 0) {
        ppu->vramIncrement = 1;
      } else if((val & 3) == 1) {
        ppu->vramIncrement = 32;
      } else {
        ppu->vramIncrement = 128;
      }
      ppu->vramRemapMode = (val & 0xc) >> 2;
      ppu->vramIncrementOnHigh = val & 0x80;
      break;
    }
    case 0x16: {
      ppu->vramPointer = (ppu->vramPointer & 0xff00) | val;
      ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
      break;
    }
    case 0x17: {
      ppu->vramPointer = (ppu->vramPointer & 0x00ff) | (val << 8);
      ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
      break;
    }
    case 0x18: {
      // TODO: vram access during rendering (also cgram and oam)
      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0xff00) | val;
      if(!ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case 0x19: {
      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0x00ff) | (val << 8);
      if(ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case 0x1a: {
      ppu->m7largeField = val & 0x80;
      ppu->m7charFill = val & 0x40;
      ppu->m7yFlip = val & 0x2;
      ppu->m7xFlip = val & 0x1;
      break;
    }
    case 0x1b:
    case 0x1c:
    case 0x1d:
    case 0x1e: {
      ppu->m7matrix[adr - 0x1b] = (val << 8) | ppu->m7prev;
      ppu->m7prev = val;
      break;
    }
    case 0x1f:
    case 0x20: {
      ppu->m7matrix[adr - 0x1b] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      break;
    }
    case 0x21: {
      ppu->cgramPointer = val;
      ppu->cgramSecondWrite = false;
      break;
    }
    case 0x22: {
      if(!ppu->cgramSecondWrite) {
        ppu->cgramBuffer = val;
      } else {
        ppu->cgram[ppu->cgramPointer++] = (val << 8) | ppu->cgramBuffer;
      }
      ppu->cgramSecondWrite = !ppu->cgramSecondWrite;
      break;
    }
    case 0x23:
    case 0x24:
    case 0x25: {
      ppu->windowLayer[(adr - 0x23) * 2].window1inversed = val & 0x1;
      ppu->windowLayer[(adr - 0x23) * 2].window1enabled = val & 0x2;
      ppu->windowLayer[(adr - 0x23) * 2].window2inversed = val & 0x4;
      ppu->windowLayer[(adr - 0x23) * 2].window2enabled = val & 0x8;
      ppu->windowLayer[(adr - 0x23) * 2 + 1].window1inversed = val & 0x10;
      ppu->windowLayer[(adr - 0x23) * 2 + 1].window1enabled = val & 0x20;
      ppu->windowLayer[(adr - 0x23) * 2 + 1].window2inversed = val & 0x40;
      ppu->windowLayer[(adr - 0x23) * 2 + 1].window2enabled = val & 0x80;
      break;
    }
    case 0x26: {
      ppu->window1left = val;
      break;
    }
    case 0x27: {
      ppu->window1right = val;
      break;
    }
    case 0x28: {
      ppu->window2left = val;
      break;
    }
    case 0x29: {
      ppu->window2right = val;
      break;
    }
    case 0x2a: {
      ppu->windowLayer[0].maskLogic = val & 0x3;
      ppu->windowLayer[1].maskLogic = (val >> 2) & 0x3;
      ppu->windowLayer[2].maskLogic = (val >> 4) & 0x3;
      ppu->windowLayer[3].maskLogic = (val >> 6) & 0x3;
      break;
    }
    case 0x2b: {
      ppu->windowLayer[4].maskLogic = val & 0x3;
      ppu->windowLayer[5].maskLogic = (val >> 2) & 0x3;
      break;
    }
    case 0x2c: {
      ppu->layer[0].mainScreenEnabled = val & 0x1;
      ppu->layer[1].mainScreenEnabled = val & 0x2;
      ppu->layer[2].mainScreenEnabled = val & 0x4;
      ppu->layer[3].mainScreenEnabled = val & 0x8;
      ppu->layer[4].mainScreenEnabled = val & 0x10;
      break;
    }
    case 0x2d: {
      ppu->layer[0].subScreenEnabled = val & 0x1;
      ppu->layer[1].subScreenEnabled = val & 0x2;
      ppu->layer[2].subScreenEnabled = val & 0x4;
      ppu->layer[3].subScreenEnabled = val & 0x8;
      ppu->layer[4].subScreenEnabled = val & 0x10;
      break;
    }
    case 0x2e: {
      ppu->layer[0].mainScreenWindowed = val & 0x1;
      ppu->layer[1].mainScreenWindowed = val & 0x2;
      ppu->layer[2].mainScreenWindowed = val & 0x4;
      ppu->layer[3].mainScreenWindowed = val & 0x8;
      ppu->layer[4].mainScreenWindowed = val & 0x10;
      break;
    }
    case 0x2f: {
      ppu->layer[0].subScreenWindowed = val & 0x1;
      ppu->layer[1].subScreenWindowed = val & 0x2;
      ppu->layer[2].subScreenWindowed = val & 0x4;
      ppu->layer[3].subScreenWindowed = val & 0x8;
      ppu->layer[4].subScreenWindowed = val & 0x10;
      break;
    }
    case 0x30: {
      ppu->directColor = val & 0x1;
      ppu->addSubscreen = val & 0x2;
      ppu->preventMathMode = (val & 0x30) >> 4;
      ppu->clipMode = (val & 0xc0) >> 6;
      break;
    }
    case 0x31: {
      ppu->subtractColor = val & 0x80;
      ppu->halfColor = val & 0x40;
      for(int i = 0; i < 6; i++) {
        ppu->mathEnabled[i] = val & (1 << i);
      }
      break;
    }
    case 0x32: {
      if(val & 0x80) ppu->fixedColorB = val & 0x1f;
      if(val & 0x40) ppu->fixedColorG = val & 0x1f;
      if(val & 0x20) ppu->fixedColorR = val & 0x1f;
      break;
    }
    case 0x33: {
      ppu->interlace = val & 0x1;
      ppu->objInterlace = val & 0x2;
      ppu->overscan = val & 0x4;
      ppu->pseudoHires = val & 0x8;
      ppu->m7extBg = val & 0x40;
      break;
    }
    default: {
      break;
    }
  }
}

void ppu_putPixels(Ppu* ppu, uint8_t* pixels) {
  for(int y = 0; y < (ppu->frameOverscan ? 239 : 224); y++) {
    int dest = y * 2 + (ppu->frameOverscan ? 2 : 16);
    int y1 = y, y2 = y + 239;
    if(!ppu->frameInterlace) {
      y1 = y + (ppu->evenFrame ? 0 : 239);
      y2 = y1;
    }
    memcpy(pixels + (dest * 2048), &ppu->pixelBuffer[y1 * 2048], 2048);
    memcpy(pixels + ((dest + 1) * 2048), &ppu->pixelBuffer[y2 * 2048], 2048);
  }
  // clear top 2 lines, and following 14 and last 16 lines if not overscanning
  memset(pixels, 0, 2048 * 2);
  if(!ppu->frameOverscan) {
    memset(pixels + (2 * 2048), 0, 2048 * 14);
    memset(pixels + (464 * 2048), 0, 2048 * 16);
  }
}
