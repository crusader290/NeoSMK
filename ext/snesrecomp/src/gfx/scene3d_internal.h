/* scene3d_internal.h — renderer-side entry points of the 3D layer. */
#ifndef SNESRECOMP_SCENE3D_INTERNAL_H
#define SNESRECOMP_SCENE3D_INTERNAL_H

#include <stdbool.h>

bool scene3d_init(void);       /* GL context current */
void scene3d_shutdown(void);
bool scene3d_has_hook(void);
/* Run the installed hook with the internal target (w x h) bound. */
void scene3d_run_hook(int target_w, int target_h);

#endif /* SNESRECOMP_SCENE3D_INTERNAL_H */
