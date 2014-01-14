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


#include <unistd.h>

#include <wayland-util.h>
#include <wayland-client.h>

#include <xf86Crtc.h>
#include "xwayland.h"
#include "xwayland-private.h"


/*
 * Handling of frame events and buffer events.
 * API to use them
 */

struct _task
{
    union
    {
	pending_task_frame tff;
	pending_task_buffer tfb;
    } tocall;
    void *args;
    struct wl_list link_window;
    struct wl_list link_pixmap;
};

static void
task_list_free_pixmap(struct wl_list *task_list)
{
    struct _task *pos, *tmp;
    wl_list_for_each_safe(pos, tmp, task_list, link_pixmap) {
	wl_list_remove(&pos->link_window);
	wl_list_remove(&pos->link_pixmap);
	free(pos);
    }
}

static void
task_list_free_window(struct wl_list *task_list)
{
    struct _task *pos, *tmp;
    wl_list_for_each_safe(pos, tmp, task_list, link_window) {
	wl_list_remove(&pos->link_window);
	free(pos);
    }
}

static const struct wl_callback_listener frame_listener;

static void
frame_listener_callback(void *data,
			struct wl_callback *callback, uint32_t time)
{
    struct xwl_window *xwl_window = data;
    struct wl_list task_list;
    struct _task *task;

    wl_callback_destroy(xwl_window->frame_callback);
    xwl_window->frame_callback = wl_surface_frame(xwl_window->surface);
    wl_callback_add_listener(xwl_window->frame_callback,
			     &frame_listener, xwl_window);

    if (wl_list_empty(&xwl_window->frame_tasks))
	return;

    /* task funcs are able to ask to be recalled */

    wl_list_init(&task_list);
    wl_list_insert_list(&task_list, &xwl_window->frame_tasks);
    wl_list_init(&xwl_window->frame_tasks);
    wl_list_for_each(task, &task_list, link_window)
	task->tocall.tff(0, time, task->args);
    task_list_free_window(&task_list);

    /* We need to commit to let the compositor know the new frame callback */
    if (!wl_list_empty(&xwl_window->frame_tasks))
	wl_surface_commit(xwl_window->surface);
}



static const struct wl_callback_listener frame_listener = {
    frame_listener_callback
};


static void
wl_buffer_release_event(void *data, struct wl_buffer *buffer)
{
    struct xwl_pixmap *xwl_pixmap = data;
    struct wl_list task_list;
    struct _task *task;

    if (wl_list_empty(&xwl_pixmap->buffer_tasks))
	return;
    wl_list_init(&task_list);
    wl_list_insert_list(&task_list, &xwl_pixmap->buffer_tasks);
    wl_list_init(&xwl_pixmap->buffer_tasks);
    wl_list_for_each(task, &task_list, link_pixmap)
	task->tocall.tfb(0, task->args);
    task_list_free_pixmap(&task_list);
}

static struct wl_buffer_listener wl_buffer_listener = {
    wl_buffer_release_event
};

void
create_frame_listener(struct xwl_window *xwl_window)
{
    wl_list_init(&xwl_window->frame_tasks);
    wl_list_init(&xwl_window->buffers_tasks);
    xwl_window->frame_callback = wl_surface_frame(xwl_window->surface);
    wl_callback_add_listener(xwl_window->frame_callback,
			     &frame_listener, xwl_window);
    wl_surface_commit(xwl_window->surface);
}

void
destroy_frame_listener(struct xwl_window *xwl_window)
{
    struct _task *pos, *tmp;
    if (!xwl_window->frame_callback)
	return;
    wl_callback_destroy(xwl_window->frame_callback);
    xwl_window->frame_callback = NULL;

    wl_list_for_each_safe(pos, tmp, &xwl_window->frame_tasks, link_window) {
	pos->tocall.tff(XWL_OBJECT_DESTRUCTION
			| XWL_WINDOW_UNREALIZE, 0, pos->args);
	wl_list_remove(&pos->link_window);
	free(pos);
    }
     wl_list_for_each_safe(pos, tmp, &xwl_window->buffers_tasks,
			   link_window) {
	pos->tocall.tfb(XWL_WINDOW_UNREALIZE, pos->args);
	wl_list_remove(&pos->link_window);
	wl_list_remove(&pos->link_pixmap);
	free(pos);
    }
}

void
create_buffer_listener(struct xwl_pixmap *xwl_pixmap)
{
    wl_list_init(&xwl_pixmap->buffer_tasks);
    wl_buffer_add_listener(xwl_pixmap->buffer, &wl_buffer_listener,
			   xwl_pixmap);
}

void
destroy_buffer_listener(struct xwl_pixmap *xwl_pixmap)
{
    struct _task *pos, *tmp;
    wl_list_for_each_safe(pos, tmp, &xwl_pixmap->buffer_tasks, link_pixmap) {
	pos->tocall.tfb(XWL_OBJECT_DESTRUCTION, pos->args);
	wl_list_remove(&pos->link_window);
	wl_list_remove(&pos->link_pixmap);
	free(pos);
    }
}

Bool
xwl_add_frame_task(WindowPtr window, pending_task_frame tocall, void *arg)
{
    struct xwl_window *xwl_window = get_xwl_window(window);
    struct _task *task;

    if (!xwl_window)
	return FALSE;
    if (!xwl_window->frame_callback)
	create_frame_listener(xwl_window);
    if (!xwl_window->frame_callback)
	return FALSE;
    if (wl_list_empty(&xwl_window->frame_tasks))
	wl_surface_commit(xwl_window->surface);
    task = calloc(sizeof(struct _task), 1);
    if (!task)
	return FALSE;
    task->tocall.tff = tocall;
    task->args = arg;
    wl_list_insert(xwl_window->frame_tasks.prev, &task->link_window);
    return TRUE;
}

Bool
xwl_add_buffer_release_task(WindowPtr window,
			    pending_task_buffer tocall, void *arg)
{
    struct xwl_window *xwl_window = get_xwl_window(window);
    struct xwl_pixmap *xwl_pixmap;
    struct _task *task;
    if (!xwl_window)
	return FALSE;
    xwl_pixmap = xwl_window_get_buffer(xwl_window);
    task = calloc(sizeof(struct _task), 1);
    if (!task)
	return FALSE;
    task->tocall.tfb = tocall;
    task->args = arg;
    wl_list_insert(xwl_pixmap->buffer_tasks.prev, &task->link_pixmap);
    wl_list_insert(xwl_window->buffers_tasks.prev, &task->link_window);
    return TRUE;
}
