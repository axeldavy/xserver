/*
 * Copyright © 2014 Axel Davy
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
#include <xorg-config.h>
#endif

#include "present_priv.h"
#include <gcstruct.h>
#include <misync.h>
#include <misyncstr.h>
#ifdef MONOTONIC_CLOCK
#include <time.h>
#endif

static Bool present_wayland_add_frame_task(WindowPtr window, pending_task_frame tocall, void *arg)
{
    ScreenPtr screen = window->drawable.pScreen;
    WindowPtr vWindow;

    vWindow = screen->XwlGetVisibleParentWindow(window);
    if (!vWindow)
        return FALSE;
    else
        return screen->XwlAddFrameTask(vWindow, tocall, arg);
}

static Bool present_wayland_can_flip(present_vblank_ptr vblank)
{
    WindowPtr window = vblank->window;
    ScreenPtr screen = window->drawable.pScreen;
    WindowPtr vWindow = screen->XwlGetVisibleParentWindow(window);
    PixmapPtr pixmap = vblank->pixmap;
    PixmapPtr wPixmap = screen->GetWindowPixmap(window);
    if (!vWindow
        || wPixmap != screen->GetWindowPixmap(vWindow)
        || wPixmap == pixmap
        || !RegionEqual(&window->clipList, &vWindow->borderClip)
        || (vblank->valid && !RegionEqual(vblank->valid,&window->winSize))
        || vblank->x_off || vblank->y_off
        || window->drawable.width != pixmap->drawable.width
        || window->drawable.height != pixmap->drawable.height)
        return FALSE;
    return TRUE;
}

static void present_wayland_flip_window(WindowPtr window, PixmapPtr new_pixmap)
{
    ScreenPtr screen = window->drawable.pScreen;
    PixmapPtr old_pixmap = screen->GetWindowPixmap(window);

    new_pixmap->refcnt++;
    present_set_tree_pixmap(window, new_pixmap);
    dixDestroyPixmap(old_pixmap, old_pixmap->drawable.id);
}

static void present_wayland_unflip(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    WindowPtr vWindow = screen->XwlGetVisibleParentWindow(window);
    PixmapPtr current_pixmap = screen->GetWindowPixmap(window);
    PixmapPtr visible_pixmap, new_pixmap;
    present_window_priv_ptr priv;

    if (!vWindow)
        return;

    visible_pixmap = screen->GetWindowPixmap(vWindow);
    if (visible_pixmap != current_pixmap)
        return;

    priv = present_window_priv(vWindow);
    if (!priv || !priv->pixmap_is_flip)
        return;

    new_pixmap = screen->CreatePixmap(screen, vWindow->drawable.width, vWindow->drawable.height, vWindow->drawable.depth, 0);

    if (!new_pixmap) {
        ErrorF("Memory Error: cannot allocate pixmap");
        return;
    }
    present_copy_region(&new_pixmap->drawable, visible_pixmap, NULL, 0, 0);
    present_wayland_flip_window(vWindow, new_pixmap);
    priv->pixmap_is_flip = FALSE;
}

static void count_msc(int flags, uint32_t time, void *arg)
{
    WindowPtr window = arg;
    present_window_priv_ptr priv = present_window_priv(window);
    priv->msc++;
    priv->last_msc_update = time;
    if (!flags)
        present_wayland_add_frame_task(window, count_msc, arg);
    else
        priv->msc_counter_on = FALSE;
}

static Bool present_wayland_init_for_window(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    present_window_priv_ptr priv = present_window_priv(window);
    if (priv->msc_counter_on)
        return TRUE;
    else if (screen->XwlGetVisibleParentWindow
             && present_wayland_add_frame_task(window, count_msc, window)) {
        priv->msc_counter_on = TRUE;
        return TRUE;
    }
    return FALSE;
}

static void present_wayland_flip_vblank_destroy(present_vblank_ptr vblank)
{
    if (!vblank->wayland_pending_events) {
        if (vblank->to_free)
            free(vblank);
        else
            present_vblank_destroy(vblank);
    }
}

static void handle_buffer_release(int flags, void *arg)
{
    present_vblank_ptr vblank = arg;

    if (flags & XWL_OBJECT_DESTRUCTION)
        /* not supposed to happen, since the pixmap still has a reference on it
         * in vblank */
        return;

    if (!flags && !vblank->to_free) /* flags can only be XWL_WINDOW_UNREALIZE */
        present_pixmap_idle(vblank->pixmap, vblank->window, vblank->serial, vblank->idle_fence);

    vblank->wayland_pending_events--;
    present_wayland_flip_vblank_destroy(vblank);
}

static void handle_presented(int flags, uint32_t time, void *arg)
{
    present_vblank_ptr vblank = arg;
    present_window_priv_ptr priv;
    uint64_t msc;

    if (!flags && !vblank->to_free) {
        priv = present_window_priv(vblank->window);
        msc = priv->last_msc_update >= time ? priv->msc : priv->msc+1;
        present_vblank_notify(vblank, vblank->kind, PresentCompleteModeFlip, GetTimeInMicros(), msc);
    }
    vblank->wayland_pending_events--;
    present_wayland_flip_vblank_destroy(vblank);
}

static void handle_present(int flags, uint32_t time, void *arg)
{
    present_vblank_ptr vblank = arg;
    WindowPtr window = vblank->window;
    ScreenPtr screen = window->drawable.pScreen;
    WindowPtr vWindow;
    present_window_priv_ptr priv;
    uint64_t msc;
    RegionPtr damage;

    vblank->wayland_pending_events = 0;

    if (vblank->to_free) { /* vblank was canceled */
        free (vblank);
        return;
    }
    if (flags) {
        present_vblank_destroy(vblank);
        return;
    }

    priv = present_window_priv(window);
    msc = priv->last_msc_update >= time ? priv->msc : priv->msc+1;

    if (!vblank->pixmap) {
        if (vblank->target_msc >= msc+1 && present_wayland_add_frame_task(window, handle_present, arg)) {
            vblank->wayland_pending_events = 1;
            return;
        } else {
            vblank->queued = FALSE;
            if (vblank->kind == PresentCompleteKindPixmap)
                present_vblank_notify(vblank, vblank->kind, PresentCompleteModeSkip, GetTimeInMicros(), msc);
            else
                present_vblank_notify(vblank, vblank->kind, PresentCompleteModeCopy, GetTimeInMicros(), msc);
            present_vblank_destroy(vblank);
            return;
        }
    }

    if (vblank->flip_allowed && vblank->target_msc <= msc+1 && present_wayland_can_flip(vblank)) {
        vWindow = screen->XwlGetVisibleParentWindow(window);
        priv = present_get_window_priv(vWindow, TRUE);
        if (!priv) /* memory error */
            goto copy_fallback;
        priv->pixmap_is_flip = TRUE;
        present_wayland_flip_window(vWindow, vblank->pixmap);
        if (vblank->update) {
            damage = vblank->update;
            RegionIntersect(damage, damage, &window->clipList);
        } else
            damage = &window->clipList;
        DamageDamageRegion(&window->drawable, damage);
        vblank->queued = FALSE;
        present_wayland_add_frame_task(vWindow, handle_presented, arg); /* TODO: handle memory error here too*/
        screen->XwlAddBufferTask(vWindow, handle_buffer_release, arg);
        vblank->wayland_pending_events = 2;
        return;
    }

    if (vblank->target_msc > msc && present_wayland_add_frame_task(window, handle_present, arg)){
        vblank->wayland_pending_events = 1;
        return;
    }
copy_fallback:
    present_wayland_unflip(window);
    present_copy_region(&window->drawable, vblank->pixmap, vblank->update, vblank->x_off, vblank->y_off);
    vblank->queued = FALSE;
    vblank->update = NULL;
    present_pixmap_idle(vblank->pixmap, vblank->window, vblank->serial, vblank->idle_fence);
    present_vblank_notify(vblank, vblank->kind, PresentCompleteModeCopy, GetTimeInMicros(), msc);
    present_vblank_destroy(vblank);
}

static void
present_wayland_wait_fence_triggered(void *param)
{
    present_vblank_ptr vblank = param;
    handle_present(0, 0, vblank);
}

void present_wayland_execute(present_vblank_ptr vblank)
{
    if (!present_wayland_init_for_window(vblank->window)) {
        /* The window has not yet been made visible on the screen.
         * Execute the Present operation right away */
        vblank->flip_allowed = FALSE;
        vblank->target_msc = 0;
    }
    if (vblank->wait_fence && !present_fence_check_triggered(vblank->wait_fence))
        present_fence_set_callback(vblank->wait_fence, present_wayland_wait_fence_triggered, vblank);
    else
        handle_present(0, 0, vblank);
}
