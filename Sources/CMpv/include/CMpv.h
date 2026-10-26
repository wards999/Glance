// mpv's client API types and constants for Swift. Only the declarations are used: Glance looks
// the functions up at run time (dlopen), so libmpv is loaded only when a video is opened.
#include "mpv/client.h"
#include "mpv/render.h"
#include "mpv/render_gl.h"
