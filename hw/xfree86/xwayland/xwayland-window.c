/*
 * Copyright © 2011 Intel Corporation
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

#ifdef HAVE_XORG_CONFIG_H
#include "xorg-config.h"
#endif

#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <wayland-client.h>

#include <xf86Crtc.h>
#include <selection.h>
#include <exevents.h>

#include "xwayland.h"
#include "xwayland-private.h"
#include "xserver-client-protocol.h"

static DevPrivateKeyRec xwl_window_private_key;
static DevPrivateKeyRec xwl_pixmap_private_key;

struct xwl_window *
get_xwl_window(WindowPtr window)
{
    return dixLookupPrivate(&window->devPrivates, &xwl_window_private_key);
}

static void
free_pixmap(void *data, struct wl_callback *callback, uint32_t time)
{
    PixmapPtr pixmap = data;
    ScreenPtr screen = pixmap->drawable.pScreen;

    (*screen->DestroyPixmap)(pixmap);
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener free_pixmap_listener = {
	free_pixmap,
};

void
xwl_pixmap_attach_buffer(PixmapPtr pixmap, struct wl_buffer *buffer)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    struct xwl_screen *xwl_screen = xwl_screen_get(screen);
    struct xwl_pixmap *xwl_pixmap = calloc(sizeof *xwl_pixmap, 1);
    if (!xwl_pixmap) {
	wl_buffer_destroy(buffer);
	return;
    }
    xwl_pixmap->pixmap = pixmap;
    xwl_pixmap->buffer = buffer;
    create_buffer_listener(xwl_pixmap);
    dixSetPrivate(&pixmap->devPrivates, &xwl_pixmap_private_key, xwl_pixmap);
    xorg_list_add(&xwl_pixmap->link, &xwl_screen->buffer_list);
}

static void
xwl_window_attach(struct xwl_window *xwl_window, PixmapPtr pixmap)
{
    struct xwl_screen *xwl_screen = xwl_window->xwl_screen;
    struct wl_callback *callback;
    struct xwl_pixmap *xwl_pixmap =
	dixLookupPrivate(&pixmap->devPrivates, &xwl_pixmap_private_key);

    if (!xwl_pixmap) {
	xwl_screen->driver->create_window_buffer(xwl_window, pixmap);
	xwl_pixmap =
	    dixLookupPrivate(&pixmap->devPrivates, &xwl_pixmap_private_key);

	if (!xwl_pixmap) {
	    ErrorF("failed to create buffer\n");
	    return;
	}
    }

    wl_surface_attach(xwl_window->surface, xwl_pixmap->buffer, 0, 0);
    wl_surface_damage(xwl_window->surface, 0, 0,
		      pixmap->drawable.width,
		      pixmap->drawable.height);

    callback = wl_display_sync(xwl_screen->display);
    wl_callback_add_listener(callback, &free_pixmap_listener, pixmap);
    pixmap->refcnt++;
}

struct xwl_pixmap *
pixmap_get_buffer(PixmapPtr pixmap)
{
    struct xwl_pixmap *xwl_pixmap =
	dixLookupPrivate(&pixmap->devPrivates, &xwl_pixmap_private_key);

    return xwl_pixmap;
}

struct xwl_pixmap *
xwl_window_get_buffer(struct xwl_window *xwl_window)
{
    PixmapPtr pixmap = (*xwl_window->xwl_screen->screen->GetWindowPixmap)
	(xwl_window->window);
    struct xwl_pixmap *xwl_pixmap =
	dixLookupPrivate(&pixmap->devPrivates, &xwl_pixmap_private_key);

    if (!xwl_pixmap) {
	xwl_window_attach(xwl_window, pixmap);
	return dixLookupPrivate(&pixmap->devPrivates,
				&xwl_pixmap_private_key);
    } else {
	return xwl_pixmap;
    }
}

static void
damage_report(DamagePtr pDamage, RegionPtr pRegion, void *data)
{
    struct xwl_window *xwl_window = data;
    struct xwl_screen *xwl_screen = xwl_window->xwl_screen;

    xorg_list_add(&xwl_window->link_damage, &xwl_screen->damage_window_list);
}

static void
damage_destroy(DamagePtr pDamage, void *data)
{
}

static Bool
xwl_realize_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xwl_screen *xwl_screen;
    struct xwl_window *xwl_window;
    Bool ret;

    xwl_screen = xwl_screen_get(screen);

    screen->RealizeWindow = xwl_screen->RealizeWindow;
    ret = (*screen->RealizeWindow)(window);
    xwl_screen->RealizeWindow = screen->RealizeWindow;
    screen->RealizeWindow = xwl_realize_window;

    if (xwl_screen->flags & XWL_FLAGS_ROOTLESS) {
	if (window->redirectDraw != RedirectDrawManual)
	    return ret;
    } else {
	if (window->parent)
	    return ret;
    }

    xwl_window = calloc(sizeof *xwl_window, 1);
    xwl_window->xwl_screen = xwl_screen;
    xwl_window->window = window;
    xwl_window->surface =
	wl_compositor_create_surface(xwl_screen->compositor);
    if (xwl_window->surface == NULL) {
	ErrorF("wl_display_create_surface failed\n");
	return FALSE;
    }

    if (xwl_screen->xorg_server)
	xserver_set_window_id(xwl_screen->xorg_server,
			      xwl_window->surface, window->drawable.id);

    wl_surface_set_user_data(xwl_window->surface, xwl_window);
    xwl_window_attach(xwl_window, (*screen->GetWindowPixmap)(window));

    dixSetPrivate(&window->devPrivates,
		  &xwl_window_private_key, xwl_window);

    xwl_window->damage =
	DamageCreate(damage_report, damage_destroy, DamageReportNonEmpty,
		     FALSE, screen, xwl_window);
    DamageRegister(&window->drawable, xwl_window->damage);
    DamageSetReportAfterOp(xwl_window->damage, TRUE);

    xorg_list_add(&xwl_window->link, &xwl_screen->window_list);
    xorg_list_init(&xwl_window->link_damage);

    return ret;
}

static Bool
xwl_unrealize_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    PixmapPtr pixmap;
    struct xwl_screen *xwl_screen;
    struct xwl_window *xwl_window;
    struct xwl_seat *xwl_seat;
    Bool ret;

    xwl_screen = xwl_screen_get(screen);

    xorg_list_for_each_entry(xwl_seat,
			     &xwl_screen->seat_list, link) {
	if (!xwl_seat->focus_window)
	    continue ;
	if (xwl_seat->focus_window->window == window) {
	    xwl_seat->focus_window = NULL;
	    SetDeviceRedirectWindow(xwl_seat->pointer, PointerRootWin);
	}
    }

    xwl_window =
	dixLookupPrivate(&window->devPrivates, &xwl_window_private_key);
    pixmap = (*screen->GetWindowPixmap)(window);

    if (xwl_window && pixmap) {
	pixmap->refcnt++;
	wait_release_to_destroy(pixmap);
    }

    screen->UnrealizeWindow = xwl_screen->UnrealizeWindow;
    ret = (*screen->UnrealizeWindow)(window);
    xwl_screen->UnrealizeWindow = screen->UnrealizeWindow;
    screen->UnrealizeWindow = xwl_unrealize_window;

    if (!xwl_window)
	return ret;

    /* The frame listener is created automatically when needed.
     * Clean it if needed. */
    destroy_frame_listener(xwl_window);

    wl_surface_destroy(xwl_window->surface);
    xorg_list_del(&xwl_window->link);
    if (RegionNotEmpty(DamageRegion(xwl_window->damage)))
	xorg_list_del(&xwl_window->link_damage);
    DamageUnregister(xwl_window->damage);
    DamageDestroy(xwl_window->damage);
    free(xwl_window);
    dixSetPrivate(&window->devPrivates, &xwl_window_private_key, NULL);

    return ret;
}

static void
xwl_set_window_pixmap(WindowPtr window, PixmapPtr pixmap)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xwl_screen *xwl_screen;
    struct xwl_window *xwl_window;
    PixmapPtr old_pixmap;

    xwl_screen = xwl_screen_get(screen);
    xwl_window =
	dixLookupPrivate(&window->devPrivates, &xwl_window_private_key);

    if (xwl_window) {
	old_pixmap = (*screen->GetWindowPixmap)(window);
	if (pixmap != old_pixmap) {
	    old_pixmap->refcnt++;
	    wait_release_to_destroy(old_pixmap);
	}
    }

    screen->SetWindowPixmap = xwl_screen->SetWindowPixmap;
    (*screen->SetWindowPixmap)(window, pixmap);
    xwl_screen->SetWindowPixmap = screen->SetWindowPixmap;
    screen->SetWindowPixmap = xwl_set_window_pixmap;
}

static Bool
xwl_destroy_pixmap(PixmapPtr pixmap)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    struct xwl_screen *xwl_screen = xwl_screen_get(screen);
    Bool ret;

    if (pixmap->refcnt == 1) {
	struct xwl_pixmap *xwl_pixmap =
	    dixLookupPrivate(&pixmap->devPrivates, &xwl_pixmap_private_key);
	if (xwl_pixmap) {
	    destroy_buffer_listener(xwl_pixmap);
	    wl_buffer_destroy(xwl_pixmap->buffer);
	    dixSetPrivate(&pixmap->devPrivates, &xwl_pixmap_private_key,
			  NULL);
	    xorg_list_del(&xwl_pixmap->link);
	    free(xwl_pixmap);
	}
    }

    screen->DestroyPixmap = xwl_screen->DestroyPixmap;
    ret = (*screen->DestroyPixmap)(pixmap);
    xwl_screen->DestroyPixmap = screen->DestroyPixmap;
    screen->DestroyPixmap = xwl_destroy_pixmap;

    return ret;
}

static void
xwl_move_window(WindowPtr window, int x, int y,
		   WindowPtr sibling, VTKind kind)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xwl_screen *xwl_screen;
    struct xwl_window *xwl_window;

    xwl_screen = xwl_screen_get(screen);

    screen->MoveWindow = xwl_screen->MoveWindow;
    (*screen->MoveWindow)(window, x, y, sibling, kind);
    xwl_screen->MoveWindow = screen->MoveWindow;
    screen->MoveWindow = xwl_move_window;

    xwl_window =
	dixLookupPrivate(&window->devPrivates, &xwl_window_private_key);
    if (xwl_window == NULL)
	return;
}

WindowPtr xwl_get_visible_parent_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    struct xwl_screen *xwl_screen;
    struct xwl_window *xwl_window;
    WindowPtr root = screen->root;
    WindowPtr current = window;

    xwl_screen = xwl_screen_get(screen);

    while (current->redirectDraw != RedirectDrawManual
	   && current->parent
	   && current->parent->parent)
	current = current->parent;

    xwl_window =
	dixLookupPrivate(&current->devPrivates, &xwl_window_private_key);

    if (xwl_screen->flags & XWL_FLAGS_ROOTLESS) {
	if (current->parent != root
	    || current == root /* root window isn't visible */
	    || current->redirectDraw != RedirectDrawManual
	    || !xwl_window /* not mapped */)
	    return NULL;
	return current;
    } else {
	if (current != root || !xwl_window)
	    return NULL;
	else
	    return root;
    }
}

int
xwl_screen_init_window(struct xwl_screen *xwl_screen, ScreenPtr screen)
{
    if (!dixRegisterPrivateKey(&xwl_window_private_key, PRIVATE_WINDOW, 0))
	return BadAlloc;

    if (!dixRegisterPrivateKey(&xwl_pixmap_private_key, PRIVATE_PIXMAP, 0))
	return BadAlloc;

    xwl_screen->RealizeWindow = screen->RealizeWindow;
    screen->RealizeWindow = xwl_realize_window;

    xwl_screen->UnrealizeWindow = screen->UnrealizeWindow;
    screen->UnrealizeWindow = xwl_unrealize_window;

    xwl_screen->SetWindowPixmap = screen->SetWindowPixmap;
    screen->SetWindowPixmap = xwl_set_window_pixmap;

    xwl_screen->DestroyPixmap = screen->DestroyPixmap;
    screen->DestroyPixmap = xwl_destroy_pixmap;

    xwl_screen->MoveWindow = screen->MoveWindow;
    screen->MoveWindow = xwl_move_window;

    return Success;
}