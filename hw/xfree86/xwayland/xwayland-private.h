/*
 * Copyright © 2010 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of the
 * copyright holders not be used in advertising or publicity
 * pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#ifndef _XWAYLAND_PRIVATE_H_
#define _XWAYLAND_PRIVATE_H_

#include <xf86Crtc.h>

struct xwl_window {
    struct xwl_screen		*xwl_screen;
    struct wl_surface		*surface;
    WindowPtr			 window;
    DamagePtr			 damage;
    struct xorg_list		 link;
    struct xorg_list		 link_damage;
    struct wl_callback    	*frame_callback;
    struct wl_list		 frame_tasks;
    struct wl_list		 buffers_tasks;
};

struct xwl_pixmap {
    PixmapPtr			 pixmap;
    struct xorg_list		 link;
    struct wl_buffer		*buffer;
    struct wl_list		 buffer_tasks;
    uint32_t			 pending_destroy;
};

struct xwl_output;

struct xwl_screen {
    struct xwl_driver		*driver;
    ScreenPtr			 screen;
    ScrnInfoPtr			 scrninfo;
    int				 drm_fd;
    uint32_t			 drm_capabilities;
    int				 wayland_fd;
    struct wl_display		*display;
    struct wl_registry          *registry;
    struct wl_registry          *drm_registry;
    struct wl_registry          *input_registry;
    struct wl_registry          *output_registry;
    struct wl_compositor	*compositor;
    struct wl_drm		*drm;
    struct wl_shm		*shm;
    struct xserver		*xorg_server;
    uint32_t			 mask;
    uint32_t			 flags;
    char			*device_name;
    uint32_t			 authenticated;
    struct xorg_list		 output_list;
    struct xorg_list		 seat_list;
    struct xorg_list		 damage_window_list;
    struct xorg_list		 window_list;
    struct xorg_list		 buffer_list;
    struct xorg_list		 authenticate_client_list;
    uint32_t			 serial;
    Bool                         outputs_initialized;

    DevPrivateKeyRec             cursor_private_key;

    CreateWindowProcPtr		 CreateWindow;
    DestroyWindowProcPtr	 DestroyWindow;
    RealizeWindowProcPtr	 RealizeWindow;
    UnrealizeWindowProcPtr	 UnrealizeWindow;
    SetWindowPixmapProcPtr	 SetWindowPixmap;
    DestroyPixmapProcPtr	 DestroyPixmap;
    MoveWindowProcPtr		 MoveWindow;
    miPointerSpriteFuncPtr	 sprite_funcs;
};

struct xwl_output {
    struct xorg_list             link;
    struct wl_output		*output;
    struct xwl_screen		*xwl_screen;
    int32_t			 x, y, width, height;
    xf86Monitor			 xf86monitor;
    xf86OutputPtr		 xf86output;
    xf86CrtcPtr			 xf86crtc;
    int32_t                      name;
    Rotation                     rotation;
};


#define MODIFIER_META 0x01

struct xwl_seat {
    DeviceIntPtr		 pointer;
    char                         pointer_name[32];
    DeviceIntPtr		 keyboard;
    char                         keyboard_name[32];
    struct xwl_screen		*xwl_screen;
    struct wl_seat		*seat;
    struct wl_pointer		*wl_pointer;
    struct wl_keyboard		*wl_keyboard;
    struct wl_array		 keys;
    struct wl_surface		*cursor;
    struct xwl_window		*focus_window;
    uint32_t			 id;
    uint32_t			 pointer_enter_serial;
    struct xorg_list		 link;
    CursorPtr                    x_cursor;

    wl_fixed_t			 horizontal_scroll;
    wl_fixed_t			 vertical_scroll;
    uint32_t			 scroll_time;

    size_t			 keymap_size;
    char			*keymap;
    struct wl_surface           *keyboard_focus;
};


struct xwl_screen *xwl_screen_get(ScreenPtr screen);

void xwayland_screen_preinit_output(struct xwl_screen *xwl_screen, ScrnInfoPtr scrninfo);

int xwl_screen_init_cursor(struct xwl_screen *xwl_screen, ScreenPtr screen);
int xwl_screen_init_window(struct xwl_screen *xwl_screen, ScreenPtr screen);

struct xwl_output *xwl_output_create(struct xwl_screen *xwl_screen);

void xwl_input_init(struct xwl_screen *screen);

Bool xwl_drm_initialised(struct xwl_screen *screen);

void xwl_seat_set_cursor(struct xwl_seat *xwl_seat);

void xwl_output_remove(struct xwl_output *output);

void xwl_pixmap_attach_buffer(PixmapPtr pixmap, struct wl_buffer *buffer);
struct xwl_pixmap *pixmap_get_buffer(PixmapPtr pixmap);
struct xwl_pixmap *xwl_window_get_buffer(struct xwl_window *xwl_window);
struct xwl_window *get_xwl_window(WindowPtr window);

void create_frame_listener(struct xwl_window *xwl_window);
void destroy_frame_listener(struct xwl_window *xwl_window);
void create_buffer_listener(struct xwl_pixmap *xwl_pixmap);
void destroy_buffer_listener(struct xwl_pixmap *xwl_pixmap);
void wait_release_to_destroy(PixmapPtr pixmap);

extern const struct xserver_listener xwl_server_listener;

#endif /* _XWAYLAND_PRIVATE_H_ */
