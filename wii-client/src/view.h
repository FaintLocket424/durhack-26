//---------------------------------------------------------------------------------
// view.h - Wii only. Shows the tile currently being rendered, scaled to fill
// the whole screen. The network thread feeds it through render's progress hook,
// the UI thread draws it.
//---------------------------------------------------------------------------------
#ifndef VIEW_H
#define VIEW_H

#include <stdint.h>

void view_init(void);						// call once, before starting the node thread
void view_request_quit(void);				// makes an in-progress render abort
int  view_has_tile(void);					// has any tile been rendered yet?
void view_invalidate(void);				// force the next view_present() to redraw

// Draws into the XFB (W pixels wide, H lines) if the preview changed since the
// last call. Returns 1 if it redrew.
int  view_present(uint32_t *xfb, int W, int H);

#endif
