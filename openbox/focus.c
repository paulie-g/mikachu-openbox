/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   focus.c for the Openbox window manager
   Copyright (c) 2003        Ben Jansens

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
*/

#include "debug.h"
#include "event.h"
#include "openbox.h"
#include "grab.h"
#include "framerender.h"
#include "client.h"
#include "config.h"
#include "frame.h"
#include "screen.h"
#include "group.h"
#include "prop.h"
#include "focus.h"
#include "stacking.h"
#include "popup.h"

#include <X11/Xlib.h>
#include <glib.h>
#include <assert.h>

ObClient *focus_client;
GList **focus_order; /* these lists are created when screen_startup
                        sets the number of desktops */
ObClient *focus_cycle_target;

static ObIconPopup *focus_cycle_popup;

static void focus_cycle_destructor(ObClient *client, gpointer data)
{
    /* end cycling if the target disappears */
    if (focus_cycle_target == client)
        focus_cycle(TRUE, TRUE, TRUE, TRUE, TRUE);
}

void focus_startup(gboolean reconfig)
{
    focus_cycle_popup = icon_popup_new(TRUE);

    if (!reconfig) {
        client_add_destructor(focus_cycle_destructor, NULL);

        /* start with nothing focused */
        focus_set_client(NULL);
    }
}

void focus_shutdown(gboolean reconfig)
{
    guint i;

    icon_popup_free(focus_cycle_popup);

    if (!reconfig) {
        client_remove_destructor(focus_cycle_destructor);

        for (i = 0; i < screen_num_desktops; ++i)
            g_list_free(focus_order[i]);
        g_free(focus_order);

        /* reset focus to root */
        XSetInputFocus(ob_display, PointerRoot, RevertToPointerRoot,
                       event_lasttime);
    }
}

static void push_to_top(ObClient *client)
{
    guint desktop;

    desktop = client->desktop;
    if (desktop == DESKTOP_ALL) desktop = screen_desktop;
    focus_order[desktop] = g_list_remove(focus_order[desktop], client);
    focus_order[desktop] = g_list_prepend(focus_order[desktop], client);
}

void focus_set_client(ObClient *client)
{
    Window active;
    ObClient *old;

#ifdef DEBUG_FOCUS
    ob_debug("focus_set_client 0x%lx\n", client ? client->window : 0);
#endif

    /* uninstall the old colormap, and install the new one */
    screen_install_colormap(focus_client, FALSE);
    screen_install_colormap(client, TRUE);

    if (client == NULL) {
	/* when nothing will be focused, send focus to the backup target */
	XSetInputFocus(ob_display, screen_support_win, RevertToPointerRoot,
                       event_lasttime);
        XSync(ob_display, FALSE);
    }

    /* in the middle of cycling..? kill it. */
    if (focus_cycle_target)
        focus_cycle(TRUE, TRUE, TRUE, TRUE, TRUE);

    old = focus_client;
    focus_client = client;

    /* move to the top of the list */
    if (client != NULL)
        push_to_top(client);

    /* set the NET_ACTIVE_WINDOW hint, but preserve it on shutdown */
    if (ob_state() != OB_STATE_EXITING) {
        active = client ? client->window : None;
        PROP_SET32(RootWindow(ob_display, ob_screen),
                   net_active_window, window, active);
    }
}

static gboolean focus_under_pointer()
{
    ObClient *c;

    if ((c = client_under_pointer()))
        return client_normal(c) && client_focus(c);
    return FALSE;
}

/* finds the first transient that isn't 'skip' and ensure's that client_normal
 is true for it */
static ObClient *find_transient_recursive(ObClient *c, ObClient *top, ObClient *skip)
{
    GSList *it;
    ObClient *ret;

    for (it = c->transients; it; it = it->next) {
        if (it->data == top) return NULL;
        ret = find_transient_recursive(it->data, top, skip);
        if (ret && ret != skip && client_normal(ret)) return ret;
        if (it->data != skip && client_normal(it->data)) return it->data;
    }
    return NULL;
}

static gboolean focus_fallback_transient(ObClient *top, ObClient *old)
{
    ObClient *target = find_transient_recursive(top, top, old);
    if (!target) {
        /* make sure client_normal is true always */
        if (!client_normal(top))
            return FALSE;
        target = top; /* no transient, keep the top */
    }
    return client_focus(target);
}

void focus_fallback(ObFocusFallbackType type)
{
    GList *it;
    ObClient *old = NULL;

    old = focus_client;

    /* unfocus any focused clients.. they can be focused by Pointer events
       and such, and then when I try focus them, I won't get a FocusIn event
       at all for them.
    */
    focus_set_client(NULL);

    if (type == OB_FOCUS_FALLBACK_UNFOCUSING && old) {
        if (old->transient_for) {
            gboolean trans = FALSE;

            if (config_focus_last || !config_focus_follow)
                trans = TRUE;
            else {
                ObClient *c;

                if ((c = client_under_pointer()) &&
                    client_search_transient(client_search_top_transient(c),
                                            old))
                    trans = TRUE;
            }

            g_message("trans %d", trans);

            /* try for transient relations */
            if (trans) {
                if (old->transient_for == OB_TRAN_GROUP) {
                    for (it = focus_order[screen_desktop]; it; it = it->next) {
                        GSList *sit;

                        for (sit = old->group->members; sit; sit = sit->next)
                            if (sit->data == it->data)
                                if (focus_fallback_transient(sit->data, old))
                                    return;
                    }
                } else {
                    if (focus_fallback_transient(old->transient_for, old))
                        return;
                }
            }
        }
    }

    if (!config_focus_last && config_focus_follow)
        if (focus_under_pointer())
            return;

#if 0
        /* try for group relations */
        if (old->group) {
            GSList *sit;

            for (it = focus_order[screen_desktop]; it != NULL; it = it->next)
                for (sit = old->group->members; sit; sit = sit->next)
                    if (sit->data == it->data)
                        if (sit->data != old && client_normal(sit->data))
                            if (client_can_focus(sit->data)) {
                                gboolean r = client_focus(sit->data);
                                assert(r);
                                return;
                            }
        }
#endif

    for (it = focus_order[screen_desktop]; it != NULL; it = it->next)
        if (type != OB_FOCUS_FALLBACK_UNFOCUSING || it->data != old)
            if (client_normal(it->data) && client_can_focus(it->data)) {
                gboolean r = client_focus(it->data);
                assert(r);
                return;
            }

    /* nothing to focus, and already set it to none above */
}

static void popup_cycle(ObClient *c, gboolean show)
{
    if (!show) {
        icon_popup_hide(focus_cycle_popup);
    } else {
        Rect *a;
        ObClient *p = c;
        char *title;

        a = screen_physical_area_monitor(0);
        icon_popup_position(focus_cycle_popup, CenterGravity,
                            a->x + a->width / 2, a->y + a->height / 2);
/*        icon_popup_size(focus_cycle_popup, a->height/2, a->height/16);
        icon_popup_show(focus_cycle_popup, c->title,
                        client_icon(c, a->height/16, a->height/16));
*/
        /* XXX the size and the font extents need to be related on some level
         */
        icon_popup_size(focus_cycle_popup, POPUP_WIDTH, POPUP_HEIGHT);

        /* use the transient's parent's title/icon */
        while (p->transient_for && p->transient_for != OB_TRAN_GROUP)
            p = p->transient_for;

        if (p == c)
            title = NULL;
        else
            title = g_strconcat((c->iconic ? c->icon_title : c->title),
                                " - ",
                                (p->iconic ? p->icon_title : p->title),
                                NULL);

        icon_popup_show(focus_cycle_popup,
                        (title ? title :
                         (c->iconic ? c->icon_title : c->title)),
                        client_icon(p, 48, 48));
        g_free(title);
    }
}

static gboolean valid_focus_target(ObClient *ft)
{
    /* we don't use client_can_focus here, because that doesn't let you
       focus an iconic window, but we want to be able to, so we just check
       if the focus flags on the window allow it, and its on the current
       desktop */
    return (!ft->transients && client_normal(ft) &&
            ((ft->can_focus || ft->focus_notify) &&
             !ft->skip_taskbar &&
             (ft->desktop == screen_desktop || ft->desktop == DESKTOP_ALL)));
}

void focus_cycle(gboolean forward, gboolean linear,
                 gboolean dialog, gboolean done, gboolean cancel)
{
    static ObClient *first = NULL;
    static ObClient *t = NULL;
    static GList *order = NULL;
    GList *it, *start, *list;
    ObClient *ft = NULL;

    if (cancel) {
        if (focus_cycle_target)
            frame_adjust_focus(focus_cycle_target->frame, FALSE);
        if (focus_client)
            frame_adjust_focus(focus_client->frame, TRUE);
        focus_cycle_target = NULL;
        goto done_cycle;
    } else if (done && dialog) {
        goto done_cycle;
    }

    if (!focus_order[screen_desktop])
        goto done_cycle;

    if (!first) first = focus_client;
    if (!focus_cycle_target) focus_cycle_target = focus_client;

    if (linear) list = client_list;
    else        list = focus_order[screen_desktop];

    start = it = g_list_find(list, focus_cycle_target);
    if (!start) /* switched desktops or something? */
        start = it = forward ? g_list_last(list) : g_list_first(list);
    if (!start) goto done_cycle;

    do {
        if (forward) {
            it = it->next;
            if (it == NULL) it = g_list_first(list);
        } else {
            it = it->prev;
            if (it == NULL) it = g_list_last(list);
        }
        ft = it->data;
        if (valid_focus_target(ft)) {
            if (ft != focus_cycle_target) { /* prevents flicker */
                if (focus_cycle_target)
                    frame_adjust_focus(focus_cycle_target->frame, FALSE);
                focus_cycle_target = ft;
                frame_adjust_focus(focus_cycle_target->frame, TRUE);
            }
            popup_cycle(ft, dialog);
            return;
        }
    } while (it != start);

done_cycle:
    if (done && focus_cycle_target)
        client_activate(focus_cycle_target, FALSE);

    t = NULL;
    first = NULL;
    focus_cycle_target = NULL;
    g_list_free(order);
    order = NULL;

    popup_cycle(ft, FALSE);

    return;
}

void focus_directional_cycle(ObDirection dir,
                             gboolean dialog, gboolean done, gboolean cancel)
{
    static ObClient *first = NULL;
    ObClient *ft = NULL;

    if (cancel) {
        if (focus_cycle_target)
            frame_adjust_focus(focus_cycle_target->frame, FALSE);
        if (focus_client)
            frame_adjust_focus(focus_client->frame, TRUE);
        focus_cycle_target = NULL;
        goto done_cycle;
    } else if (done && dialog) {
        goto done_cycle;
    }

    if (!focus_order[screen_desktop])
        goto done_cycle;

    if (!first) first = focus_client;
    if (!focus_cycle_target) focus_cycle_target = focus_client;

    if (focus_cycle_target)
        ft = client_find_directional(focus_cycle_target, dir);
    else {
        GList *it;

        for (it = focus_order[screen_desktop]; it; it = g_list_next(it))
            if (valid_focus_target(it->data))
                ft = it->data;
    }
        
    if (ft) {
        if (ft != focus_cycle_target) {/* prevents flicker */
            if (focus_cycle_target)
                frame_adjust_focus(focus_cycle_target->frame, FALSE);
            focus_cycle_target = ft;
            frame_adjust_focus(focus_cycle_target->frame, TRUE);
        }
    }
    if (focus_cycle_target) {
        popup_cycle(focus_cycle_target, dialog);
        if (dialog)
            return;
    }


done_cycle:
    if (done && focus_cycle_target)
        client_activate(focus_cycle_target, FALSE);

    first = NULL;
    focus_cycle_target = NULL;

    popup_cycle(ft, FALSE);

    return;
}

void focus_order_add_new(ObClient *c)
{
    guint d, i;

    if (c->iconic)
        focus_order_to_top(c);
    else {
        d = c->desktop;
        if (d == DESKTOP_ALL) {
            for (i = 0; i < screen_num_desktops; ++i) {
                if (focus_order[i] && ((ObClient*)focus_order[i]->data)->iconic)
                    focus_order[i] = g_list_insert(focus_order[i], c, 0);
                else
                    focus_order[i] = g_list_insert(focus_order[i], c, 1);
            }
        } else
             if (focus_order[d] && ((ObClient*)focus_order[d]->data)->iconic)
                focus_order[d] = g_list_insert(focus_order[d], c, 0);
            else
                focus_order[d] = g_list_insert(focus_order[d], c, 1);
    }
}

void focus_order_remove(ObClient *c)
{
    guint d, i;

    d = c->desktop;
    if (d == DESKTOP_ALL) {
        for (i = 0; i < screen_num_desktops; ++i)
            focus_order[i] = g_list_remove(focus_order[i], c);
    } else
        focus_order[d] = g_list_remove(focus_order[d], c);
}

static void to_top(ObClient *c, guint d)
{
    focus_order[d] = g_list_remove(focus_order[d], c);
    if (!c->iconic) {
        focus_order[d] = g_list_prepend(focus_order[d], c);
    } else {
        GList *it;

        /* insert before first iconic window */
        for (it = focus_order[d];
             it && !((ObClient*)it->data)->iconic; it = it->next);
        focus_order[d] = g_list_insert_before(focus_order[d], it, c);
    }
}

void focus_order_to_top(ObClient *c)
{
    guint d, i;

    d = c->desktop;
    if (d == DESKTOP_ALL) {
        for (i = 0; i < screen_num_desktops; ++i)
            to_top(c, i);
    } else
        to_top(c, d);
}

static void to_bottom(ObClient *c, guint d)
{
    focus_order[d] = g_list_remove(focus_order[d], c);
    if (c->iconic) {
        focus_order[d] = g_list_append(focus_order[d], c);
    } else {
        GList *it;

        /* insert before first iconic window */
        for (it = focus_order[d];
             it && !((ObClient*)it->data)->iconic; it = it->next);
        g_list_insert_before(focus_order[d], it, c);
    }
}

void focus_order_to_bottom(ObClient *c)
{
    guint d, i;

    d = c->desktop;
    if (d == DESKTOP_ALL) {
        for (i = 0; i < screen_num_desktops; ++i)
            to_bottom(c, i);
    } else
        to_bottom(c, d);
}
