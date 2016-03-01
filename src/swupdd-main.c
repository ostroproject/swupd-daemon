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
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "swupdd-interface.h"

#define SWUPD_CLIENT    "swupd"


typedef struct _daemon_state
{
    guint busid;
    GDBusConnection *conn;
    ClrSoftwareUpdate *upif;
    const gchar *url;
    const gchar *method;
    GSubprocess *process;
} daemon_state_t;


static const gchar *_empty_strv[] = { NULL, };


static gchar ** _list_to_strv (GList *list)
{
    gchar **strv;
    gchar **temp;

    strv = g_new0 (gchar *, g_list_length (list) + 1);

    temp = strv;
    while (list)
    {
        *temp = list->data;
        temp++;
        list = g_list_next (list);
    }
    return strv;
}


static GList * _set_defaults (GList *head, daemon_state_t *state)
{
    gboolean have_url = FALSE;
    GList *iter = head;

    while (iter)
    {
        if (strncmp ((const char *) iter->data, "--url=", 6) == 0)
            have_url = TRUE;
        iter = g_list_next (iter);
    }

    if (!have_url)
        head = g_list_append (head,
                              g_strdup_printf ("--url=%s", state->url));

    return head;
}


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


static void _on_process_done (GObject *object,
                              GAsyncResult *res,
                              gpointer user_data)
{
    GSubprocess *proc = G_SUBPROCESS(object);
    daemon_state_t *dstate = (daemon_state_t *) user_data;
    GError *error = NULL;
    gint status;
    gchar *outbuf = NULL, *errbuf = NULL;
    gchar **messagev;

    status = g_subprocess_get_exit_status (dstate->process);
    if (g_subprocess_communicate_utf8_finish (proc,
                                              res,
                                              &outbuf,
                                              &errbuf,
                                              &error))
    {
        messagev = g_strsplit_set (outbuf, "\r\n", -1);
        g_free (outbuf);
    }
    else
    {
        gchar *errmsg[2] = { NULL, NULL };
        errmsg[0] = error->message;
        messagev = g_strdupv (errmsg);
        g_error_free (error);
    }

    g_object_unref (dstate->process);
    dstate->process = NULL;

    clr_software_update__emit_request_completed (dstate->upif,
                                                 dstate->method,
                                                 status,
                                                 (const gchar * const *) messagev);
    g_strfreev (messagev);
}


static void _handle_common_options (gchar *key,
                                    GVariant *value,
                                    GList *args)
{
    if (g_strcmp0 (key, "url") == 0)
    {
        args = g_list_append (args,
                              g_strdup_printf ("--url=%s",
                                               g_variant_get_string (value, NULL)));
    }
    else if (g_strcmp0 (key, "port") == 0)
    {
        args = g_list_append (args,
                              g_strdup_printf ("--port=%u",
                                               g_variant_get_uint16 (value)));
    }
    else if (g_strcmp0 (key, "contenturl") == 0 ||
             g_strcmp0 (key, "versionurl") == 0)
    {
        args = g_list_append (args,
                              g_strdup_printf ("--%s=%s",
                                               key,
                                               g_variant_get_string (value, NULL)));
    }
    else if (g_strcmp0 (key, "format") == 0)
    {
        args = g_list_append (args,
                              g_strdup_printf ("--format=%s",
                                               g_variant_get_string (value, NULL)));
    }
    else if (g_strcmp0 (key, "path") == 0)
    {
        args = g_list_append (args,
                              g_strdup_printf ("--path=%s",
                                               g_variant_get_string (value, NULL)));
    }
    else if (g_strcmp0 (key, "force") == 0)
    {
        args = g_list_append (args,
                              g_strdup ("--force"));
    }
}


static gboolean _on_bundle_add (ClrSoftwareUpdate *object,
                                GDBusMethodInvocation *invocation,
                                GVariant *arg_options,
                                const gchar *const *arg_bundles,
                                gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;
    GError *error = NULL;
    GList *args = NULL;
    GVariantIter iter;
    GVariant *value;
    gchar *key;
    gchar **argv;

    g_message ("bundleAdd");
    clr_software_update__complete_bundle_add (object, invocation,
                                              (dstate->process == NULL));
    if (dstate->process)
        return FALSE;

    args = g_list_append (args, g_strdup (SWUPD_CLIENT));
    args = g_list_append (args, g_strdup ("bundle-add"));

    g_variant_iter_init (&iter, arg_options);
    while (g_variant_iter_next (&iter, "{sv}", &key, &value))
    {
        if (g_strcmp0 (key, "list") == 0)
        {
            args = g_list_append (args,
                                  g_strdup ("--list"));
        }
        else
            _handle_common_options (key, value, args);
        g_free (key);
        g_variant_unref (value);
    }

    while (*arg_bundles)
    {
        args = g_list_append (args,
                              g_strdup (*arg_bundles));
        arg_bundles++;
    }

    args = _set_defaults (args, dstate);
    argv = _list_to_strv (args);
    dstate->method = "bundleAdd";
    dstate->process = g_subprocess_newv ((const gchar * const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         &error);
    if (dstate->process == NULL)
    {
        gchar *errmsg[2] = { NULL, NULL };
        errmsg[0] = error->message;
        clr_software_update__emit_request_completed (object,
                                                     dstate->method,
                                                     -1,
                                                     (const gchar * const *) errmsg);
        g_error_free (error);
    }
    else
    {
        g_subprocess_communicate_utf8_async (dstate->process,
                                             NULL,
                                             NULL,
                                             _on_process_done,
                                             dstate);
    }

    g_free (argv);
    g_list_free_full (args, g_free);

    return TRUE;
}


static gboolean _on_bundle_remove (ClrSoftwareUpdate *object,
                                   GDBusMethodInvocation *invocation,
                                   GVariant *arg_options,
                                   const gchar *arg_bundle,
                                   gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;
    GError *error = NULL;
    GList *args = NULL;
    GVariantIter iter;
    GVariant *value;
    gchar *key;
    gchar **argv;

    g_message ("bundleRemove");
    clr_software_update__complete_bundle_remove (object, invocation,
                                                 (dstate->process == NULL));
    if (dstate->process)
        return FALSE;

    args = g_list_append (args, g_strdup (SWUPD_CLIENT));
    args = g_list_append (args, g_strdup ("bundle-remove"));

    g_variant_iter_init (&iter, arg_options);
    while (g_variant_iter_next (&iter, "{sv}", &key, &value))
    {
        _handle_common_options (key, value, args);
        g_free (key);
        g_variant_unref (value);
    }

    if (arg_bundle)
        args = g_list_append (args,
                              g_strdup (arg_bundle));

    args = _set_defaults (args, dstate);
    argv = _list_to_strv (args);
    dstate->method = "bundleRemove";
    dstate->process = g_subprocess_newv ((const gchar * const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         &error);
    if (dstate->process == NULL)
    {
        gchar *errmsg[2] = { NULL, NULL };
        errmsg[0] = error->message;
        clr_software_update__emit_request_completed (object,
                                                     dstate->method,
                                                     -1,
                                                     (const gchar * const *) errmsg);
        g_error_free (error);
    }
    else
    {
        g_subprocess_communicate_utf8_async (dstate->process,
                                             NULL,
                                             NULL,
                                             _on_process_done,
                                             dstate);
    }

    g_free (argv);
    g_list_free_full (args, g_free);

    return TRUE;
}


static gboolean _on_cancel (ClrSoftwareUpdate *object,
                            GDBusMethodInvocation *invocation,
                            gboolean arg_force,
                            gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;

    g_message ("cancel");
    clr_software_update__complete_cancel (object, invocation,
                                          (dstate->process != NULL));
    if (!dstate->process)
        return FALSE;

    if (arg_force)
        g_subprocess_force_exit (dstate->process);
    else
        g_subprocess_send_signal (dstate->process, SIGINT);

    clr_software_update__emit_request_completed (object,
                                                 "cancel",
                                                 0,
                                                 _empty_strv);
    return TRUE;
}


static gboolean _on_check_update (ClrSoftwareUpdate *object,
                                  GDBusMethodInvocation *invocation,
                                  GVariant *arg_options,
                                  const gchar *arg_bundle,
                                  gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;
    GError *error = NULL;
    GList *args = NULL;
    GVariantIter iter;
    GVariant *value;
    gchar *key;
    gchar **argv;

    g_message ("checkUpdate");
    clr_software_update__complete_check_update (object, invocation,
                                                (dstate->process == NULL));
    if (dstate->process)
        return FALSE;

    args = g_list_append (args, g_strdup (SWUPD_CLIENT));
    args = g_list_append (args, g_strdup ("check-update"));

    g_variant_iter_init (&iter, arg_options);
    while (g_variant_iter_next (&iter, "{sv}", &key, &value))
    {
        _handle_common_options (key, value, args);
        g_free (key);
        g_variant_unref (value);
    }

    args = _set_defaults (args, dstate);
    argv = _list_to_strv (args);
    dstate->method = "checkUpdate";
    dstate->process = g_subprocess_newv ((const gchar * const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         &error);
    if (dstate->process == NULL)
    {
        gchar *errmsg[2] = { NULL, NULL };
        errmsg[0] = error->message;
        clr_software_update__emit_request_completed (object,
                                                     dstate->method,
                                                     -1,
                                                     (const gchar * const *) errmsg);
        g_error_free (error);
    }
    else
    {
        g_subprocess_communicate_utf8_async (dstate->process,
                                             NULL,
                                             NULL,
                                             _on_process_done,
                                             dstate);
    }

    g_free (argv);
    g_list_free_full (args, g_free);

    return TRUE;
}


static gboolean _on_hash_dump (ClrSoftwareUpdate *object,
                               GDBusMethodInvocation *invocation,
                               GVariant *arg_options,
                               const gchar *arg_filename,
                               gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;
    GError *error = NULL;
    GList *args = NULL;
    GVariantIter iter;
    GVariant *value;
    gchar *key;
    gchar **argv;

    g_message ("hashDump");
    clr_software_update__complete_hash_dump (object, invocation,
                                             (dstate->process == NULL));
    if (dstate->process)
        return FALSE;

    args = g_list_append (args, g_strdup (SWUPD_CLIENT));
    args = g_list_append (args, g_strdup ("hashdump"));

    g_variant_iter_init (&iter, arg_options);
    while (g_variant_iter_next (&iter, "{sv}", &key, &value))
    {
        _handle_common_options (key, value, args);
        g_free (key);
        g_variant_unref (value);
    }

    if (arg_filename)
        args = g_list_append (args,
                              g_strdup (arg_filename));

    argv = _list_to_strv (args);
    dstate->method = "hashDump";
    dstate->process = g_subprocess_newv ((const gchar * const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         &error);
    if (dstate->process == NULL)
    {
        gchar *errmsg[2] = { NULL, NULL };
        errmsg[0] = error->message;
        clr_software_update__emit_request_completed (object,
                                                     dstate->method,
                                                     -1,
                                                     (const gchar * const *) errmsg);
        g_error_free (error);
    }
    else
    {
        g_subprocess_communicate_utf8_async (dstate->process,
                                             NULL,
                                             NULL,
                                             _on_process_done,
                                             dstate);
    }

    g_free (argv);
    g_list_free_full (args, g_free);

    return TRUE;
}


static gboolean _on_update (ClrSoftwareUpdate *object,
                            GDBusMethodInvocation *invocation,
                            GVariant *arg_options,
                            gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;
    GError *error = NULL;
    GList *args = NULL;
    GVariantIter iter;
    GVariant *value;
    gchar *key;
    gchar **argv;

    g_message ("update");
    clr_software_update__complete_update (object, invocation,
                                          (dstate->process == NULL));
    if (dstate->process)
        return FALSE;

    args = g_list_append (args, g_strdup (SWUPD_CLIENT));
    args = g_list_append (args, g_strdup ("update"));

    g_variant_iter_init (&iter, arg_options);
    while (g_variant_iter_next (&iter, "{sv}", &key, &value))
    {
        if (g_strcmp0 (key, "download") == 0)
        {
            args = g_list_append (args,
                                  g_strdup ("--download"));
        }
        else if (g_strcmp0 (key, "status") == 0)
        {
            args = g_list_append (args,
                                  g_strdup ("--status"));
        }
        else
            _handle_common_options (key, value, args);
        g_free (key);
        g_variant_unref (value);
    }

    args = _set_defaults (args, dstate);
    argv = _list_to_strv (args);
    dstate->method = "update";
    dstate->process = g_subprocess_newv ((const gchar * const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         &error);
    if (dstate->process == NULL)
    {
        gchar *errmsg[2] = { NULL, NULL };
        errmsg[0] = error->message;
        clr_software_update__emit_request_completed (object,
                                                     dstate->method,
                                                     -1,
                                                     (const gchar * const *) errmsg);
        g_error_free (error);
    }
    else
    {
        g_subprocess_communicate_utf8_async (dstate->process,
                                             NULL,
                                             NULL,
                                             _on_process_done,
                                             dstate);
    }

    g_free (argv);
    g_list_free_full (args, g_free);

    return TRUE;
}


static gboolean _on_verify (ClrSoftwareUpdate *object,
                            GDBusMethodInvocation *invocation,
                            GVariant *arg_options,
                            gpointer user_data)
{
    daemon_state_t *dstate = (daemon_state_t *) user_data;
    GError *error = NULL;
    GList *args = NULL;
    GVariantIter iter;
    GVariant *value;
    gchar *key;
    gchar **argv;

    g_message ("verify");
    clr_software_update__complete_verify (object, invocation,
                                          (dstate->process == NULL));
    if (dstate->process)
        return FALSE;

    args = g_list_append (args, g_strdup (SWUPD_CLIENT));
    args = g_list_append (args, g_strdup ("verify"));

    g_variant_iter_init (&iter, arg_options);
    while (g_variant_iter_next (&iter, "{sv}", &key, &value))
    {
        if (g_strcmp0 (key, "manifest") == 0)
        {
            args = g_list_append (args,
                                  g_strdup_printf ("--manifest=%s",
                                                   g_variant_get_string (value, NULL)));
        }
        else if (g_strcmp0 (key, "fix") == 0)
        {
            args = g_list_append (args,
                                  g_strdup ("--fix"));
        }
        else if (g_strcmp0 (key, "install") == 0)
        {
            args = g_list_append (args,
                                  g_strdup ("--install"));
        }
        else if (g_strcmp0 (key, "quick") == 0)
        {
            args = g_list_append (args,
                                  g_strdup ("--quick"));
        }
        else
            _handle_common_options (key, value, args);
        g_free (key);
        g_variant_unref (value);
    }

    args = _set_defaults (args, dstate);
    argv = _list_to_strv (args);
    dstate->method = "verify";
    dstate->process = g_subprocess_newv ((const gchar * const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         &error);
    if (dstate->process == NULL)
    {
        gchar *errmsg[2] = { NULL, NULL };
        errmsg[0] = error->message;
        clr_software_update__emit_request_completed (object,
                                                     dstate->method,
                                                     -1,
                                                     (const gchar * const *) errmsg);
        g_error_free (error);
    }
    else
    {
        g_subprocess_communicate_utf8_async (dstate->process,
                                             NULL,
                                             NULL,
                                             _on_process_done,
                                             dstate);
    }

    g_free (argv);
    g_list_free_full (args, g_free);

    return TRUE;
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
    {
        g_error ("exporting interface failed: %s", error->message);
        g_error_free (error);
    }
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
    daemon_state_t dstate;

    signal(SIGPIPE, SIG_IGN);
    memset (&dstate, 0x00, sizeof(dstate));

    dstate.url = g_getenv ("SWUPDD_URL");

    main_loop = g_main_loop_new (NULL, FALSE);
    sigid_int = g_unix_signal_add (SIGINT, _signal_source_cb, main_loop);
    sigid_term = g_unix_signal_add (SIGTERM, _signal_source_cb, main_loop);

    dstate.busid = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                                   "org.O1.swupdd.Client",
                                   G_BUS_NAME_OWNER_FLAGS_NONE,
                                   _on_bus_acquired,
                                   _on_name_acquired,
                                   _on_name_lost,
                                   &dstate,
                                   NULL);

    g_main_loop_run (main_loop);

    _destroy_interface (&dstate);
    g_bus_unown_name (dstate.busid);

    g_source_remove (sigid_int);
    g_source_remove (sigid_term);

    return 0;
}

