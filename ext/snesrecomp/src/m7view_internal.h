/* m7view_internal.h — solver entry point used by snesrecomp.c (needs PPU types). */
#ifndef SNESRECOMP_M7VIEW_INTERNAL_H
#define SNESRECOMP_M7VIEW_INTERNAL_H

#include "snesrecomp/m7view.h"
#include "ppu.h"

/* Fit cameras to one frame's mode-7 lines (index = scanline - 1). */
void m7view_solve(M7View *v, const PpuM7Line *lines, int cols, int nlines);

#endif /* SNESRECOMP_M7VIEW_INTERNAL_H */
