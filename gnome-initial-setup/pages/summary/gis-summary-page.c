/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2012 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

/* Summary page {{{1 */

#define PAGE_ID "summary"

#include "config.h"
#include "fbe-remote-generated.h"
#include "summary-resources.h"
#include "gis-summary-page.h"

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <stdlib.h>
#include <errno.h>

#include <act/act-user-manager.h>

#include <gdm/gdm-client.h>

#define SERVICE_NAME "gdm-password"

struct _GisSummaryPagePrivate {
  ActUser *user_account;
  const gchar *user_password;
};
typedef struct _GisSummaryPagePrivate GisSummaryPagePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GisSummaryPage, gis_summary_page, GIS_TYPE_PAGE);

#define OBJ(type,name) ((type)gtk_builder_get_object(GIS_PAGE(page)->builder,(name)))
#define WID(name) OBJ(GtkWidget*,name)

static gboolean
connect_to_gdm (GdmGreeter      **greeter,
                GdmUserVerifier **user_verifier)
{
  GdmClient *client;

  GError *error = NULL;
  gboolean res = FALSE;

  client = gdm_client_new ();

  *greeter = gdm_client_get_greeter_sync (client, NULL, &error);
  if (error != NULL)
    goto out;

  *user_verifier = gdm_client_get_user_verifier_sync (client, NULL, &error);
  if (error != NULL)
    goto out;

  res = TRUE;

 out:
  if (error != NULL) {
    g_warning ("Failed to open connection to GDM: %s", error->message);
    g_error_free (error);
  }

  return res;
}

static void
request_info_query (GisSummaryPage  *page,
                    GdmUserVerifier *user_verifier,
                    const char      *question,
                    gboolean         is_secret)
{
  /* TODO: pop up modal dialog */
  g_debug ("user verifier asks%s question: %s",
           is_secret ? " secret" : "",
           question);
}

static void
on_info (GdmUserVerifier *user_verifier,
         const char      *service_name,
         const char      *info,
         GisSummaryPage  *page)
{
  g_debug ("PAM module info: %s\n", info);
}

static void
on_problem (GdmUserVerifier *user_verifier,
            const char      *service_name,
            const char      *problem,
            GisSummaryPage  *page)
{
  g_warning ("PAM module error: %s\n", problem);
}

static void
on_info_query (GdmUserVerifier *user_verifier,
               const char      *service_name,
               const char      *question,
               GisSummaryPage  *page)
{
  request_info_query (page, user_verifier, question, FALSE);
}

static void
on_secret_info_query (GdmUserVerifier *user_verifier,
                      const char      *service_name,
                      const char      *question,
                      GisSummaryPage  *page)
{
  GisSummaryPagePrivate *priv = gis_summary_page_get_instance_private (page);
  gboolean should_send_password = priv->user_password != NULL;

  g_debug ("PAM module secret info query: %s\n", question);
  if (should_send_password) {
    g_debug ("sending password\n");
    gdm_user_verifier_call_answer_query (user_verifier,
                                         service_name,
                                         priv->user_password,
                                         NULL, NULL, NULL);
    g_clear_pointer (&priv->user_password, (GDestroyNotify) g_free);
  } else {
    request_info_query (page, user_verifier, question, TRUE);
  }
}

static void
on_session_opened (GdmGreeter     *greeter,
                   const char     *service_name,
                   GisSummaryPage *page)
{
  gdm_greeter_call_start_session_when_ready_sync (greeter, service_name,
                                                  TRUE, NULL, NULL);
}

static void
add_uid_file (uid_t uid)
{
  gchar *gis_uid_path;
  gchar *uid_str;
  GError *error = NULL;

  gis_uid_path = g_build_filename (g_get_home_dir (),
                                   "gnome-initial-setup-uid",
                                   NULL);
  uid_str = g_strdup_printf ("%u", uid);

  if (!g_file_set_contents (gis_uid_path, uid_str, -1, &error)) {
      g_warning ("Unable to create %s: %s", gis_uid_path, error->message);
      g_clear_error (&error);
  }

  g_free (uid_str);
  g_free (gis_uid_path);
}

static void
log_user_in (GisSummaryPage *page)
{
  GisSummaryPagePrivate *priv = gis_summary_page_get_instance_private (page);
  GError *error = NULL;
  GdmGreeter *greeter;
  GdmUserVerifier *user_verifier;

  if (!connect_to_gdm (&greeter, &user_verifier)) {
    g_warning ("No GDM connection; not initiating login");
    return;
  }

  if (error != NULL) {
    g_warning ("Could not set PAM_AUTHTOK: %s", error->message);
    return;
  }

  g_signal_connect (user_verifier, "info",
                    G_CALLBACK (on_info), page);
  g_signal_connect (user_verifier, "problem",
                    G_CALLBACK (on_problem), page);
  g_signal_connect (user_verifier, "info-query",
                    G_CALLBACK (on_info_query), page);
  g_signal_connect (user_verifier, "secret-info-query",
                    G_CALLBACK (on_secret_info_query), page);

  g_signal_connect (greeter, "session-opened",
                    G_CALLBACK (on_session_opened), page);

  /* We are in NEW_USER mode and we want to make it possible for third
   * parties to find out which user ID we created.
   */
  add_uid_file (act_user_get_uid (priv->user_account));

  gdm_user_verifier_call_begin_verification_for_user_sync (user_verifier,
                                                           SERVICE_NAME,
                                                           act_user_get_user_name (priv->user_account),
                                                           NULL, &error);

  if (error != NULL) {
    g_warning ("Could not begin verification: %s", error->message);
    return;
  }
}

static void
play_tutorial_ready_callback (GObject *fbe_remote,
                              GAsyncResult *res,
                              gpointer user_data)
{
  GError *error = NULL;
  GisSummaryPage *summary = user_data;

  fberemote_call_play_tutorial_finish (FBEREMOTE (fbe_remote), res, &error);
  if (error != NULL)
    {
      g_critical ("Can't play tutorial from FBE remote: %s", error->message);
      g_error_free (error);
    }

  /* Either log user in or quit the app depending on if the user is new */
  switch (gis_driver_get_mode (GIS_PAGE (summary)->driver))
    {
    case GIS_DRIVER_MODE_NEW_USER:
      log_user_in (summary);
      break;
    case GIS_DRIVER_MODE_EXISTING_USER:
      g_application_quit (G_APPLICATION (GIS_PAGE (summary)->driver));
      break;
    }
}

static void
launch_tutorial (GisSummaryPage *summary)
{
  FBERemote *fbe_remote;
  GError *error = NULL;

  gboolean is_new_user = (gis_driver_get_mode (GIS_PAGE (summary)->driver) ==
                                               GIS_DRIVER_MODE_NEW_USER);

  fbe_remote = fberemote_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 "com.endlessm.Tutorial",
                                                 "/com/endlessm/Tutorial/FBERemote",
                                                 NULL, &error);

  if (error != NULL)
    {
      g_critical ("Could not get DBus proxy for tutorial FBE remote: %s", error->message);
      g_error_free (error);

      if (is_new_user)
        {
          log_user_in (summary);
        }

      return;
    }

  /* use g_dbus_proxy_call() to specify a custom timeout */
  g_dbus_proxy_call (G_DBUS_PROXY (fbe_remote),
                     "PlayTutorial",
                     g_variant_new ("(bs)",
                                    is_new_user,
                                    gis_driver_get_user_language (GIS_PAGE (summary)->driver)),
                     G_DBUS_CALL_FLAGS_NONE,
                     G_MAXINT,
                     NULL, /* cancellable */
                     play_tutorial_ready_callback, summary);
  g_object_unref (fbe_remote);
}

static void
done_cb (GtkButton *button, GisSummaryPage *page)
{
  switch (gis_driver_get_mode (GIS_PAGE (page)->driver))
    {
    case GIS_DRIVER_MODE_NEW_USER:
      gis_driver_hide_window (GIS_PAGE (page)->driver);
      break;
    case GIS_DRIVER_MODE_EXISTING_USER:
      gis_add_setup_done_file ();
      break;
    }

    launch_tutorial (page);
}

static void
gis_summary_page_shown (GisPage *page)
{
  GisSummaryPage *summary = GIS_SUMMARY_PAGE (page);
  GisSummaryPagePrivate *priv = gis_summary_page_get_instance_private (summary);

  gis_driver_save_data (GIS_PAGE (page)->driver);

  gis_driver_get_user_permissions (GIS_PAGE (page)->driver,
                                   &priv->user_account,
                                   &priv->user_password);
}

static GtkBuilder *
gis_summary_page_get_builder (GisPage *page)
{
  GtkBuilder *builder = gtk_builder_new ();

  char *filename = g_build_filename (UIDIR, "summary-distro.ui", NULL);
  GError *error = NULL;

  if (gtk_builder_add_from_file (builder, filename, &error))
    goto out;

  if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    g_warning ("Error while loading summary override: %s", error->message);

  g_clear_error (&error);

  builder = GIS_PAGE_CLASS (gis_summary_page_parent_class)->get_builder (page);

 out:
  g_free (filename);
  return builder;
}

static char *
get_item (const char *buffer, const char *name)
{
  char *label, *start, *end, *result;
  char end_char;

  result = NULL;
  start = NULL;
  end = NULL;
  label = g_strconcat (name, "=", NULL);
  if ((start = strstr (buffer, label)) != NULL)
    {
      start += strlen (label);
      end_char = '\n';
      if (*start == '"')
        {
          start++;
          end_char = '"';
        }

      end = strchr (start, end_char);
    }

    if (start != NULL && end != NULL)
      {
        result = g_strndup (start, end - start);
      }

  g_free (label);

  return result;
}

static void
update_distro_name (GisSummaryPage *page)
{
  char *buffer;
  char *name;

  name = NULL;

  if (g_file_get_contents ("/etc/os-release", &buffer, NULL, NULL))
    {
      name = get_item (buffer, "NAME");
      g_free (buffer);
    }

  if (name)
    {
      gchar *label;
      label = g_strdup_printf (_("_Start Using %s"), name);
      gtk_label_set_label (GTK_LABEL (WID ("summary-start-button-label")), label);
      g_free (label);
      g_free (name);
    }
}

static void
gis_summary_page_constructed (GObject *object)
{
  GisSummaryPage *page = GIS_SUMMARY_PAGE (object);

  G_OBJECT_CLASS (gis_summary_page_parent_class)->constructed (object);

  gtk_container_add (GTK_CONTAINER (page), WID ("summary-page"));
  update_distro_name (page);
  g_signal_connect (WID("summary-start-button"), "clicked", G_CALLBACK (done_cb), page);

  gis_page_set_complete (GIS_PAGE (page), TRUE);

  gtk_widget_set_visible (WID ("warning_icon"),
                          gis_driver_is_live_session (GIS_PAGE (object)->driver));

  if (gis_driver_is_live_session (GIS_PAGE (object)->driver))
    {
      gtk_label_set_label (OBJ (GtkLabel*, "summary-details"),
                           _("You’re ready to try Endless OS"));

      gtk_label_set_markup (OBJ (GtkLabel*, "summary-details2"),
                            _("<b>Any files you download or documents you create will be "
                              "lost forever when you restart or shutdown the computer.</b>"));
    }

  gtk_widget_show (GTK_WIDGET (page));
}

static void
gis_summary_page_locale_changed (GisPage *page)
{
  gis_page_set_title (page, _("Thank You"));
  update_distro_name (GIS_SUMMARY_PAGE (page));
}

static void
gis_summary_page_class_init (GisSummaryPageClass *klass)
{
  GisPageClass *page_class = GIS_PAGE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  page_class->page_id = PAGE_ID;
  page_class->get_builder = gis_summary_page_get_builder;
  page_class->locale_changed = gis_summary_page_locale_changed;
  page_class->shown = gis_summary_page_shown;
  object_class->constructed = gis_summary_page_constructed;
}

static void
gis_summary_page_init (GisSummaryPage *page)
{
  g_resources_register (summary_get_resource ());
}

void
gis_prepare_summary_page (GisDriver *driver)
{
  gis_driver_add_page (driver,
                       g_object_new (GIS_TYPE_SUMMARY_PAGE,
                                     "driver", driver,
                                     NULL));
}
