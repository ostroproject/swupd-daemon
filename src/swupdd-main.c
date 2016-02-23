/* vi: set et sw=4 ts=4 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *
 * Daemon for controlling Clear Linux Software Update Client
 * Copyright (C) 2016 Intel Corporation
 *
 * Contact: Jussi Laako <jussi.laako@linux.intel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "swupdd-interface.h"


typedef struct _daemon_state
{
    GDBusConnection *conn;
    ClrSoftwareUpdate *upif;
} daemon_state_t;


static const gchar *_empty_strv[] = { NULL, };


static void _destroy_interface (daemon_state_t *dstate)
{
    if (!dstate->upif)
        return;

    g_dbus_interface_skeleton_unexport (
        G_DBUS_INTERFACE_SKELETON (dstate->upif));
    g_object_unref (dstate->upif);
    dstate->upif = NULL;
}


static gboolean _signal_source_cb (gpointer user_data)
{
    GMainLoop *main_loop = (GMainLoop *) user_data;

    g_main_loop_quit (main_loop);

    return G_SOURCE_CONTINUE;
}


static gboolean _on_bundle_add (ClrSoftwareUpdate *object,
                                GDBusMethodInvocation *invocation,
                                GVariant *arg_options,
                                const gchar *const *arg_bundles)
{
    clr_software_update__complete_bundle_add (object, invocation);

    clr_software_update__emit_request_completed (object,
                                                 "bundleAdd",
                                                 0,
                                                 _empty_strv);
}


static gboolean _on_bundle_remove (ClrSoftwareUpdate *object,
                                   GDBusMethodInvocation *invocation,
                                   GVariant *arg_options,
                                   const gchar *arg_bundle)
{
    clr_software_update__complete_bundle_remove (object, invocation);

    clr_software_update__emit_request_completed (object,
                                                 "bundleRemove",
                                                 0,
                                                 _empty_strv);
}


static gboolean _on_cancel (ClrSoftwareUpdate *object,
                            GDBusMethodInvocation *invocation)
{
    clr_software_update__complete_cancel (object, invocation);

    clr_software_update__emit_request_completed (object,
                                                 "cancel",
                                                 0,
                                                 _empty_strv);
}


static gboolean _on_check_update (ClrSoftwareUpdate *object,
                                  GDBusMethodInvocation *invocation,
                                  GVariant *arg_options,
                                  const gchar *arg_bundle)
{
    clr_software_update__complete_check_update (object, invocation);

    clr_software_update__emit_request_completed (object,
                                                 "checkUpdate",
                                                 0,
                                                 _empty_strv);
}


static gboolean _on_hash_dump (ClrSoftwareUpdate *object,
                               GDBusMethodInvocation *invocation,
                               GVariant *arg_options,
                               const gchar *arg_filename)
{
    clr_software_update__complete_hash_dump (object, invocation);

    clr_software_update__emit_request_completed (object,
                                                 "hashDump",
                                                 0,
                                                 _empty_strv);
}


static gboolean _on_update (ClrSoftwareUpdate *object,
                            GDBusMethodInvocation *invocation,
                            GVariant *arg_options)
{
    clr_software_update__complete_update (object, invocation);

    clr_software_update__emit_request_completed (object,
                                                 "update",
                                                 0,
                                                 _empty_strv);
}


static gboolean _on_verify (ClrSoftwareUpdate *object,
                            GDBusMethodInvocation *invocation,
                            GVariant *arg_options)
{
    clr_software_update__complete_verify (object, invocation);

    clr_software_update__emit_request_completed (object,
                                                 "verify",
                                                 0,
                                                 _empty_strv);
}


static void _on_bus_acquired (GDBusConnection *connection,
                              const gchar *name,
                              gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;
    GError *error = NULL;

    g_message ("bus acquired");

    dstate->conn = connection;
    dstate->upif = clr_software_update__skeleton_new ();
    if (!dstate->upif)
        g_error ("failed to create interface object");
    if (!g_dbus_interface_skeleton_export (
        G_DBUS_INTERFACE_SKELETON (dstate->upif),
        connection,
        "/org/O1/swupdd/Client",
        &error))
        g_error ("exporting interface failed: %s", error->message);
}


static void _on_name_acquired (GDBusConnection *connection,
                               const gchar *name,
                               gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;

    g_message ("name acquired");

    g_signal_connect (dstate->upif,
                      "handle-bundle-add",
                      G_CALLBACK (_on_bundle_add),
                      dstate);
    g_signal_connect (dstate->upif,
                      "handle-bundle-remove",
                      G_CALLBACK (_on_bundle_remove),
                      dstate);
    g_signal_connect (dstate->upif,
                      "handle-hash-dump",
                      G_CALLBACK (_on_hash_dump),
                      dstate);
    g_signal_connect (dstate->upif,
                      "handle-update",
                      G_CALLBACK (_on_update),
                      dstate);
    g_signal_connect (dstate->upif,
                      "handle-verify",
                      G_CALLBACK (_on_verify),
                      dstate);
    g_signal_connect (dstate->upif,
                      "handle-check-update",
                      G_CALLBACK (_on_check_update),
                      dstate);
    g_signal_connect (dstate->upif,
                      "handle-cancel",
                      G_CALLBACK (_on_cancel),
                      dstate);
}


static void _on_name_lost (GDBusConnection *connection,
                           const gchar *name,
                           gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;

    g_message ("name lost");

    _destroy_interface (dstate);
}


int main (int argc, char *argv[])
{
    GMainLoop *main_loop;
    guint sigid_int, sigid_term;
    guint busid;
    daemon_state_t dstate;

    signal(SIGPIPE, SIG_IGN);

    main_loop = g_main_loop_new (NULL, FALSE);
    sigid_int = g_unix_signal_add (SIGINT, _signal_source_cb, main_loop);
    sigid_term = g_unix_signal_add (SIGTERM, _signal_source_cb, main_loop);

    busid = g_bus_own_name (G_BUS_TYPE_SESSION,
                            "org.O1.swupdd.Client",
                            G_BUS_NAME_OWNER_FLAGS_NONE,
                            _on_bus_acquired,
                            _on_name_acquired,
                            _on_name_lost,
                            &dstate,
                            NULL);

    g_main_loop_run (main_loop);

    _destroy_interface (&dstate);
    g_bus_unown_name (busid);

    g_source_remove (sigid_int);
    g_source_remove (sigid_term);

    return 0;
}

