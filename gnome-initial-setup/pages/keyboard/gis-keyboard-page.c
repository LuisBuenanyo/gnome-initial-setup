/*
 * Copyright (C) 2010 Intel, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Sergey Udaltsov <svu@gnome.org>
 *         Michael Wood <michael.g.wood@intel.com>
 *
 * Based on gnome-control-center cc-region-panel.c
 */

#define PAGE_ID "keyboard"

#include "config.h"

#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#include <polkit/polkit.h>

#include "gis-keyboard-page.h"
#include "keyboard-resources.h"
#include "cc-input-chooser.h"

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-xkb-info.h>
#include <libgnome-desktop/gnome-languages.h>

#ifdef HAVE_IBUS
#include <ibus.h>
#include "cc-ibus-utils.h"
#endif

#include <act/act.h>
#include <unistd.h>
#include <egg-list-box.h>

#define GNOME_DESKTOP_INPUT_SOURCES_DIR "org.gnome.desktop.input-sources"
#define KEY_CURRENT_INPUT_SOURCE "current"
#define KEY_INPUT_SOURCES        "sources"

#define INPUT_SOURCE_TYPE_XKB "xkb"
#define INPUT_SOURCE_TYPE_IBUS "ibus"

#define MAX_INPUT_ROWS_VISIBLE 5

struct _GisKeyboardPagePrivate {
        GDBusProxy  *localed;
        GCancellable *cancellable;

        GtkWidget *input_section;
        GtkWidget *input_list;
        GtkWidget *add_input;
        GtkWidget *remove_input;
        GtkWidget *show_config;
        GtkWidget *show_layout;
        GtkWidget *auto_detect;
        GtkWidget *input_scrolledwindow;
        GList *selected_input_sorted;
        guint n_input_rows;
        GPid gkbd_pid;
        GPermission *permission;


        GSettings *input_settings;
        GnomeXkbInfo *xkb_info;
#ifdef HAVE_IBUS
        IBusBus *ibus;
        GHashTable *ibus_engines;
        GCancellable *ibus_cancellable;
#endif

        guint next_page_id;
};
typedef struct _GisKeyboardPagePrivate GisKeyboardPagePrivate;

#define OBJ(type,name) ((type)gtk_builder_get_object(GIS_PAGE (self)->builder,(name)))
#define WID(name) OBJ(GtkWidget*,name)

G_DEFINE_TYPE_WITH_PRIVATE (GisKeyboardPage, gis_keyboard_page, GIS_TYPE_PAGE);

static void
gis_keyboard_page_dispose (GObject *gobject)
{
  GisKeyboardPage *page = GIS_KEYBOARD_PAGE (gobject);
  GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (page);

  if (priv->cancellable)
    {
      g_cancellable_cancel (priv->cancellable);
      g_clear_object (&priv->cancellable);
    }

  if (priv->input_settings)
    {
      g_signal_handlers_disconnect_by_data (priv->input_settings, page);
      g_clear_object (&priv->input_settings);
    }

  if (priv->next_page_id != 0)
    {
      g_signal_handler_disconnect (gis_driver_get_assistant (GIS_PAGE (page)->driver),
                                   priv->next_page_id);
      priv->next_page_id = 0;
    }

  g_clear_object (&priv->permission);
  g_clear_object (&priv->localed);
  g_clear_object (&priv->xkb_info);

#ifdef HAVE_IBUS
  g_clear_object (&priv->ibus);
  if (priv->ibus_cancellable)
    g_cancellable_cancel (priv->ibus_cancellable);
  g_clear_object (&priv->ibus_cancellable);
#endif

  G_OBJECT_CLASS (gis_keyboard_page_parent_class)->dispose (gobject);
}

static void
gis_keyboard_page_finalize (GObject *object)
{
	GisKeyboardPage *self = GIS_KEYBOARD_PAGE (object);
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);

#ifdef HAVE_IBUS
        g_clear_pointer (&priv->ibus_engines, g_hash_table_destroy);
        g_clear_pointer (&priv->selected_input_sorted, g_list_free);
#endif

	G_OBJECT_CLASS (gis_keyboard_page_parent_class)->finalize (object);
}

static void localed_proxy_ready (GObject *source, GAsyncResult *res, gpointer data);
static void setup_input_section (GisKeyboardPage *self);
static void update_input (GisKeyboardPage *self);

static void
next_page_cb (GisAssistant *assistant,
              GisPage      *which_page,
              GisPage      *this_page)
{
        if (which_page == this_page)
                update_input (GIS_KEYBOARD_PAGE (this_page));
}

static void
gis_keyboard_page_constructed (GObject *object)
{
        GisKeyboardPage *self = GIS_KEYBOARD_PAGE (object);
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);

        G_OBJECT_CLASS (gis_keyboard_page_parent_class)->constructed (object);

        gtk_container_add (GTK_CONTAINER (self), WID ("keyboard_page"));

        setup_input_section (self);

        priv->cancellable = g_cancellable_new ();

        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                  G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                                  NULL,
                                  "org.freedesktop.locale1",
                                  "/org/freedesktop/locale1",
                                  "org.freedesktop.locale1",
                                  priv->cancellable,
                                  (GAsyncReadyCallback) localed_proxy_ready,
                                  self);

        /* If we're in new user mode then we're manipulating system settings */
        if (gis_driver_get_mode (GIS_PAGE (self)->driver) == GIS_DRIVER_MODE_NEW_USER)
                priv->permission = polkit_permission_new_sync ("org.freedesktop.locale1.set-keyboard", NULL, NULL, NULL);

        priv->next_page_id = g_signal_connect (gis_driver_get_assistant (GIS_PAGE (self)->driver),
                                               "next-page",
                                               G_CALLBACK (next_page_cb),
                                               self);

        gis_page_set_complete (GIS_PAGE (self), TRUE);
        gtk_widget_show (GTK_WIDGET (self));
}

static void
gis_keyboard_page_locale_changed (GisPage *page)
{
        gis_page_set_title (GIS_PAGE (page), _("Keyboard Layouts"));
}

static void
gis_keyboard_page_class_init (GisKeyboardPageClass * klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GisPageClass * page_class = GIS_PAGE_CLASS (klass);

        page_class->page_id = PAGE_ID;
        page_class->locale_changed = gis_keyboard_page_locale_changed;

        object_class->constructed = gis_keyboard_page_constructed;
        object_class->dispose = gis_keyboard_page_dispose;
        object_class->finalize = gis_keyboard_page_finalize;
}

static void
update_separator_func (GtkWidget **separator,
                       GtkWidget  *child,
                       GtkWidget  *before,
                       gpointer    user_data)
{
        if (before == NULL)
                return;

        if (*separator == NULL) {
                *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
                g_object_ref_sink (*separator);
                gtk_widget_show (*separator);
        }
}


static void show_input_chooser (GisKeyboardPage *self);
static void remove_selected_input (GisKeyboardPage *self);

#ifdef HAVE_IBUS
static void
update_ibus_active_sources (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GList *rows, *l;
        GtkWidget *row;
        const gchar *type;
        const gchar *id;
        IBusEngineDesc *engine_desc;
        gchar *display_name;
        GtkWidget *label;

        rows = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (l = rows; l; l = l->next) {
                row = l->data;
                type = g_object_get_data (G_OBJECT (row), "type");
                id = g_object_get_data (G_OBJECT (row), "id");
                if (g_strcmp0 (type, INPUT_SOURCE_TYPE_IBUS) != 0)
                        continue;

                engine_desc = g_hash_table_lookup (priv->ibus_engines, id);
                if (engine_desc) {
                        display_name = engine_get_display_name (engine_desc);
                        label = GTK_WIDGET (g_object_get_data (G_OBJECT (row), "label"));
                        gtk_label_set_text (GTK_LABEL (label), display_name);
                        g_free (display_name);
                }
        }
        g_list_free (rows);
}

static void
update_input_chooser (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GtkWidget *chooser;

        chooser = g_object_get_data (G_OBJECT (self), "input-chooser");
        if (!chooser)
                return;

        cc_input_chooser_set_ibus_engines (chooser, priv->ibus_engines);
}

static void
fetch_ibus_engines_result (GObject       *object,
                           GAsyncResult  *result,
                           GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GList *list, *l;
        GError *error;

        error = NULL;
        list = ibus_bus_list_engines_async_finish (priv->ibus, result, &error);
        g_clear_object (&priv->ibus_cancellable);
        if (!list && error) {
                g_warning ("Couldn't finish IBus request: %s", error->message);
                g_error_free (error);
                return;
        }

        /* Maps engine ids to engine description objects */
        priv->ibus_engines = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);

        for (l = list; l; l = l->next) {
                IBusEngineDesc *engine = l->data;
                const gchar *engine_id = ibus_engine_desc_get_name (engine);

                if (g_str_has_prefix (engine_id, "xkb:"))
                        g_object_unref (engine);
                else
                        g_hash_table_replace (priv->ibus_engines, (gpointer)engine_id, engine);
        }
        g_list_free (list);

        update_ibus_active_sources (self);
        update_input_chooser (self);
}

static void
fetch_ibus_engines (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);

        priv->ibus_cancellable = g_cancellable_new ();

        ibus_bus_list_engines_async (priv->ibus,
                                     -1,
                                     priv->ibus_cancellable,
                                     (GAsyncReadyCallback)fetch_ibus_engines_result,
                                     self);

  /* We've got everything we needed, don't want to be called again. */
  g_signal_handlers_disconnect_by_func (priv->ibus, fetch_ibus_engines, self);
}

static void
maybe_start_ibus (void)
{
        /* IBus doesn't export API in the session bus. The only thing
         * we have there is a well known name which we can use as a
         * sure-fire way to activate it.
         */
        g_bus_unwatch_name (g_bus_watch_name (G_BUS_TYPE_SESSION,
                                              IBUS_SERVICE_IBUS,
                                              G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                              NULL,
                                              NULL,
                                              NULL,
                                              NULL));
}

static GDesktopAppInfo *
setup_app_info_for_id (const gchar *id)
{
  GDesktopAppInfo *app_info;
  gchar *desktop_file_name;
  gchar **strv;

  strv = g_strsplit (id, ":", 2);
  desktop_file_name = g_strdup_printf ("ibus-setup-%s.desktop", strv[0]);
  g_strfreev (strv);

  app_info = g_desktop_app_info_new (desktop_file_name);
  g_free (desktop_file_name);

  return app_info;
}
#endif

static void
adjust_input_list_scrolling (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);

        if (priv->n_input_rows >= MAX_INPUT_ROWS_VISIBLE) {
                GtkWidget *parent;
                gint height;

                parent = gtk_widget_get_parent (priv->input_scrolledwindow);
                gtk_widget_get_preferred_height (parent, NULL, &height);
                gtk_widget_set_size_request (parent, -1, height);

                gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (priv->input_scrolledwindow),
                                                GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        } else {
                gtk_widget_set_size_request (gtk_widget_get_parent (priv->input_scrolledwindow), -1, -1);
                gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (priv->input_scrolledwindow),
                                                GTK_POLICY_NEVER, GTK_POLICY_NEVER);
        }
}

static GtkWidget *
add_input_row (GisKeyboardPage   *self,
               const gchar     *type,
               const gchar     *id,
               const gchar     *name,
               GDesktopAppInfo *app_info)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GtkWidget *row;
        GtkWidget *label;
        GtkWidget *image;

        row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        label = gtk_label_new (name);
        gtk_misc_set_alignment (GTK_MISC (label), 0, 0.5);
        gtk_widget_set_margin_left (label, 20);
        gtk_widget_set_margin_right (label, 20);
        gtk_widget_set_margin_top (label, 6);
        gtk_widget_set_margin_bottom (label, 6);
        gtk_box_pack_start (GTK_BOX (row), label, TRUE, TRUE, 0);

        if (strcmp (type, INPUT_SOURCE_TYPE_IBUS) == 0) {
                image = gtk_image_new_from_icon_name ("system-run-symbolic", GTK_ICON_SIZE_BUTTON);
                gtk_widget_set_margin_left (image, 20);
                gtk_widget_set_margin_right (image, 20);
                gtk_widget_set_margin_top (image, 6);
                gtk_widget_set_margin_bottom (image, 6);
                gtk_style_context_add_class (gtk_widget_get_style_context (image), "dim-label");
                gtk_box_pack_start (GTK_BOX (row), image, FALSE, TRUE, 0);
        }

        gtk_widget_show_all (row);
        gtk_container_add (GTK_CONTAINER (priv->input_list), row);
        priv->selected_input_sorted = g_list_prepend (priv->selected_input_sorted, row);

        g_object_set_data (G_OBJECT (row), "label", label);
        g_object_set_data (G_OBJECT (row), "type", (gpointer)type);
        g_object_set_data_full (G_OBJECT (row), "id", g_strdup (id), g_free);
        if (app_info) {
                g_object_set_data_full (G_OBJECT (row), "app-info", g_object_ref (app_info), g_object_unref);
        }

        priv->n_input_rows += 1;
        adjust_input_list_scrolling (self);

        return row;
}

static void
add_input_source (GisKeyboardPage *self,
                  const gchar     *type,
                  const gchar     *id)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        const gchar *name;
        gchar *display_name;
        GDesktopAppInfo *app_info;

        display_name = NULL;
        app_info = NULL;

        if (g_str_equal (type, INPUT_SOURCE_TYPE_XKB)) {
                gnome_xkb_info_get_layout_info (priv->xkb_info, id, &name, NULL, NULL, NULL);
                if (!name) {
                        g_warning ("Couldn't find XKB input source '%s'", id);
                        return;
                }
                display_name = g_strdup (name);
                type = INPUT_SOURCE_TYPE_XKB;
#ifdef HAVE_IBUS
        } else if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                IBusEngineDesc *engine_desc = NULL;

                if (priv->ibus_engines)
                        engine_desc = g_hash_table_lookup (priv->ibus_engines, id);
                if (engine_desc)
                        display_name = engine_get_display_name (engine_desc);

                app_info = setup_app_info_for_id (id);
                type = INPUT_SOURCE_TYPE_IBUS;
#endif
        } else {
                g_warning ("Unhandled input source type '%s'", type);
                return;
        }

        add_input_row (self, type, id, display_name ? display_name : id, app_info);
        g_free (display_name);
        g_clear_object (&app_info);
}

static void
add_input_sources (GisKeyboardPage *self,
                   GVariant        *sources)
{
        GVariantIter iter;
        const gchar *type;
        const gchar *id;

        g_variant_iter_init (&iter, sources);
        while (g_variant_iter_next (&iter, "(&s&s)", &type, &id))
                add_input_source (self, type, id);
}

static void
add_input_sources_from_settings (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GVariant *sources;
        sources = g_settings_get_value (priv->input_settings, "sources");
        add_input_sources (self, sources);
        g_variant_unref (sources);
}

static void
clear_input_sources (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GList *list, *l;
        list = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (l = list; l; l = l->next) {
                gtk_container_remove (GTK_CONTAINER (priv->input_list), GTK_WIDGET (l->data));
        }
        g_list_free (list);
        g_clear_pointer (&priv->selected_input_sorted, g_list_free);

        priv->n_input_rows = 0;
        adjust_input_list_scrolling (self);
}

static void
select_by_id (GtkWidget   *row,
              gpointer     data)
{
        const gchar *id = data;
        const gchar *row_id;

        row_id = (const gchar *)g_object_get_data (G_OBJECT (row), "id");
        if (g_strcmp0 (row_id, id) == 0)
                egg_list_box_select_child (EGG_LIST_BOX (gtk_widget_get_parent (row)), row);
}

static void
select_input (GisKeyboardPage *self,
              const gchar   *id)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);

        gtk_container_foreach (GTK_CONTAINER (priv->input_list),
                               select_by_id, (gpointer)id);
}

static void
input_sources_changed (GSettings     *settings,
                       const gchar   *key,
                       GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GtkWidget *selected;
        gchar *id = NULL;

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected)
                id = g_strdup (g_object_get_data (G_OBJECT (selected), "id"));
        clear_input_sources (self);
        add_input_sources_from_settings (self);
        if (id) {
                select_input (self, id);
                g_free (id);
        }
}

static void
current_input_source_changed (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GList *all_inputs;
        GtkWidget *current_input;
        guint current_input_index;

        current_input_index = g_settings_get_uint (priv->input_settings, KEY_CURRENT_INPUT_SOURCE);
        all_inputs = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        current_input = g_list_nth_data (all_inputs, current_input_index);
        if (current_input)
                egg_list_box_select_child (EGG_LIST_BOX (priv->input_list), current_input);

        g_list_free (all_inputs);
}


static void
update_buttons (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GtkWidget *selected;
        GList *children;
        gboolean multiple_sources;

        children = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        multiple_sources = g_list_next (children) != NULL;
        g_list_free (children);

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected == NULL) {
                gtk_widget_set_visible (priv->show_config, FALSE);
                gtk_widget_set_sensitive (priv->remove_input, FALSE);
                gtk_widget_set_sensitive (priv->show_layout, FALSE);
        } else {
                GDesktopAppInfo *app_info;

                app_info = (GDesktopAppInfo *)g_object_get_data (G_OBJECT (selected), "app-info");

                gtk_widget_set_visible (priv->show_config, app_info != NULL);
                gtk_widget_set_sensitive (priv->show_layout, TRUE);
                gtk_widget_set_sensitive (priv->remove_input, multiple_sources);
        }
}

static void
update_current_input (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GtkWidget *selected;
        GList *children;
        guint index;

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected) {
                children = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
                index = g_list_index (children, selected);
                g_settings_set_uint (priv->input_settings, KEY_CURRENT_INPUT_SOURCE, index);
                g_settings_apply (priv->input_settings);
                g_list_free (children);

                /* Put the selected input in the head */
                priv->selected_input_sorted = g_list_remove (priv->selected_input_sorted, selected);
                priv->selected_input_sorted = g_list_prepend (priv->selected_input_sorted, selected);
        }
}

static void
set_input_settings (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        const gchar *type;
        const gchar *id;
        GVariantBuilder builder;
        GVariant *old_sources;
        const gchar *old_current_type;
        const gchar *old_current_id;
        guint old_current;
        guint old_n_sources;
        guint index;
        GList *list, *l;

        old_sources = g_settings_get_value (priv->input_settings, KEY_INPUT_SOURCES);
        old_current = g_settings_get_uint (priv->input_settings, KEY_CURRENT_INPUT_SOURCE);
        old_n_sources = g_variant_n_children (old_sources);

        if (old_n_sources > 0 && old_current < old_n_sources) {
                g_variant_get_child (old_sources, old_current,
                                     "(&s&s)", &old_current_type, &old_current_id);
        } else {
                old_current_type = "";
                old_current_id = "";
        }

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));
        index = 0;
        list = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (l = list; l; l = l->next) {
                type = (const gchar *)g_object_get_data (G_OBJECT (l->data), "type");
                id = (const gchar *)g_object_get_data (G_OBJECT (l->data), "id");
                if (index != old_current &&
                    g_str_equal (type, old_current_type) &&
                    g_str_equal (id, old_current_id)) {
                        g_settings_set_uint (priv->input_settings, KEY_CURRENT_INPUT_SOURCE, index);
                }
                g_variant_builder_add (&builder, "(ss)", type, id);
                index += 1;
        }
        g_list_free (list);

        g_settings_set_value (priv->input_settings, KEY_INPUT_SOURCES, g_variant_builder_end (&builder));
        g_settings_apply (priv->input_settings);

        g_variant_unref (old_sources);
}


static void set_localed_input (GisKeyboardPage *self);

static void
change_locale_permission_acquired (GObject      *source,
                                   GAsyncResult *res,
                                   gpointer      data)
{
  GisKeyboardPage *page = GIS_KEYBOARD_PAGE (data);
  GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (page);
  GError *error = NULL;
  gboolean allowed;

  allowed = g_permission_acquire_finish (priv->permission, res, &error);
  if (error) {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to acquire permission: %s\n", error->message);
      g_error_free (error);
      return;
  }

  if (allowed)
    set_localed_input (GIS_KEYBOARD_PAGE (data));
}


static void
update_input (GisKeyboardPage *self)
{
  GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);

  set_input_settings (self);

  if (gis_driver_get_mode (GIS_PAGE (self)->driver) == GIS_DRIVER_MODE_NEW_USER) {
      if (g_permission_get_allowed (priv->permission)) {
          set_localed_input (self);
      }
      else if (g_permission_get_can_acquire (priv->permission)) {
          g_permission_acquire_async (priv->permission,
                                      NULL,
                                      change_locale_permission_acquired,
                                      self);
      }
  }
}

static gboolean
input_source_already_added (GisKeyboardPage *self,
                            const gchar   *id)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GList *list, *l;
        gboolean retval = FALSE;

        list = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (l = list; l; l = l->next)
                if (g_str_equal (id, (const gchar *) g_object_get_data (G_OBJECT (l->data), "id"))) {
                        retval = TRUE;
                        break;
                }
        g_list_free (list);

        return retval;
}

static void
input_response (GtkWidget *chooser, gint response_id, gpointer data)
{
	GisKeyboardPage *self = data;
        gchar *type;
        gchar *id;
        gchar *name;
        GDesktopAppInfo *app_info = NULL;

        if (response_id == GTK_RESPONSE_OK) {
                if (cc_input_chooser_get_selected (chooser, &type, &id, &name) &&
                    !input_source_already_added (self, id)) {
                        if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                                g_free (type);
                                type = INPUT_SOURCE_TYPE_IBUS;
#ifdef HAVE_IBUS
                                app_info = setup_app_info_for_id (id);
#endif
                        } else {
                                g_free (type);
                                type = INPUT_SOURCE_TYPE_XKB;
                        }

                        add_input_row (self, type, id, name, app_info);
                        update_buttons (self);
                        update_input (self);
                        select_input (self, id);

                        g_free (id);
                        g_free (name);
                        g_clear_object (&app_info);
                }
        }
        gtk_widget_destroy (chooser);
        g_object_set_data (G_OBJECT (self), "input-chooser", NULL);
}

static void
show_input_chooser (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GtkWidget *chooser;
        GtkWidget *toplevel;

        toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
        chooser = cc_input_chooser_new (GTK_WINDOW (toplevel),
                                        priv->xkb_info,
#ifdef HAVE_IBUS
                                        priv->ibus_engines
#else
                                        NULL
#endif
                );
        g_signal_connect (chooser, "response",
                          G_CALLBACK (input_response), self);
        gtk_window_present (GTK_WINDOW (chooser));

        g_object_set_data (G_OBJECT (self), "input-chooser", chooser);
}

static void
add_input (GisKeyboardPage *self)
{
        show_input_chooser (self);
}

static void
do_remove_selected_input (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GtkWidget *selected;

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected == NULL)
                return;

        priv->selected_input_sorted = g_list_delete_link (priv->selected_input_sorted,
                                                          priv->selected_input_sorted);
        gtk_container_remove (GTK_CONTAINER (priv->input_list), selected);
        if (priv->selected_input_sorted)
                egg_list_box_select_child (EGG_LIST_BOX (priv->input_list), priv->selected_input_sorted->data);
        else
                egg_list_box_select_child (EGG_LIST_BOX (priv->input_list), NULL);

        priv->n_input_rows -= 1;
        adjust_input_list_scrolling (self);

        update_buttons (self);
        update_input (self);
}

static void
remove_selected_input (GisKeyboardPage *self)
{
        do_remove_selected_input (self);
}

static void
show_selected_settings (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GtkWidget *selected;
        GdkAppLaunchContext *ctx;
        GDesktopAppInfo *app_info;
        const gchar *id;
        GError *error = NULL;

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected == NULL)
                return;

        app_info = (GDesktopAppInfo *)g_object_get_data (G_OBJECT (selected), "app-info");
        if  (app_info == NULL)
                return;

        ctx = gdk_display_get_app_launch_context (gdk_display_get_default ());
        gdk_app_launch_context_set_timestamp (ctx, gtk_get_current_event_time ());

        id = (const gchar *)g_object_get_data (G_OBJECT (selected), "id");
        g_app_launch_context_setenv (G_APP_LAUNCH_CONTEXT (ctx),
                                     "IBUS_ENGINE_NAME", id);

        if (!g_app_info_launch (G_APP_INFO (app_info), NULL, G_APP_LAUNCH_CONTEXT (ctx), &error)) {
                g_warning ("Failed to launch input source setup: %s", error->message);
                g_error_free (error);
        }

        g_object_unref (ctx);
}

static void
show_selected_layout (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GtkWidget *selected;
        const gchar *type;
        const gchar *id;
        const gchar *layout;
        const gchar *variant;
        gchar *commandline;
        gchar **argv = NULL;

        selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->input_list));
        if (selected == NULL)
                return;

        type = (const gchar *)g_object_get_data (G_OBJECT (selected), "type");
        id = (const gchar *)g_object_get_data (G_OBJECT (selected), "id");

        if (g_str_equal (type, INPUT_SOURCE_TYPE_XKB)) {
                gnome_xkb_info_get_layout_info (priv->xkb_info,
                                                id, NULL, NULL,
                                                &layout, &variant);

                if (!layout || !layout[0]) {
                        g_warning ("Couldn't find XKB input source '%s'", id);
                        return;
                }
#ifdef HAVE_IBUS
        } else if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                IBusEngineDesc *engine_desc = NULL;

                if (priv->ibus_engines)
                        engine_desc = g_hash_table_lookup (priv->ibus_engines, id);

                if (engine_desc) {
                        layout = ibus_engine_desc_get_layout (engine_desc);
                        variant = "";
                } else {
                        g_warning ("Couldn't find IBus input source '%s'", id);
                        return;
                }
#endif
        } else {
                g_warning ("Unhandled input source type '%s'", type);
                return;
        }

        if (variant[0])
                commandline = g_strdup_printf ("gkbd-keyboard-display -l \"%s\t%s\"",
                                               layout, variant);
        else
                commandline = g_strdup_printf ("gkbd-keyboard-display -l %s",
                                               layout);

        if (!g_shell_parse_argv (commandline,
                            NULL,
                            &argv,
                            NULL))
          goto out;

        if (priv->gkbd_pid)
          {
            kill (priv->gkbd_pid, 9);
            priv->gkbd_pid = 0;
          }

        g_spawn_async (NULL,
                       argv,
                       NULL,
                       G_SPAWN_SEARCH_PATH,
                       NULL,
                       NULL,
                       &priv->gkbd_pid,
                       NULL);
        g_strfreev (argv);
  out:
        g_free (commandline);
}

static void
auto_detect (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        /* TODO Incorporate the keyboard detection heuristic */
        printf("auto_detect\n");
}

static void
add_default_input_source_for_locale (GisKeyboardPage *self)
{
        const gchar *locale;
        const gchar *type;
        const gchar *id;

        locale = gis_driver_get_user_language (GIS_PAGE (self)->driver);

        if (!gnome_get_input_source_from_locale (locale, &type, &id))
                return;

        add_input_source (self, type, id);
}

static void
setup_input_section (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);

        priv->input_settings = g_settings_new (GNOME_DESKTOP_INPUT_SOURCES_DIR);
        g_settings_delay (priv->input_settings);

        priv->xkb_info = gnome_xkb_info_new ();

#ifdef HAVE_IBUS
        ibus_init ();
        if (!priv->ibus) {
                priv->ibus = ibus_bus_new_async ();
                if (ibus_bus_is_connected (priv->ibus))
                        fetch_ibus_engines (self);
                else
                        g_signal_connect_swapped (priv->ibus, "connected",
                                                  G_CALLBACK (fetch_ibus_engines), self);
        }
        maybe_start_ibus ();
#endif

        priv->input_section = WID ("input_section");
        priv->input_list = WID ("input_list");
        priv->add_input = WID ("input_source_add");
        priv->remove_input = WID ("input_source_remove");
        priv->show_config = WID ("input_source_config");
        priv->show_layout = WID ("input_source_layout");
        priv->auto_detect = WID ("input_auto_detect");
        priv->input_scrolledwindow = WID ("input_scrolledwindow");

        g_signal_connect_swapped (priv->add_input, "clicked",
                                  G_CALLBACK (add_input), self);
        g_signal_connect_swapped (priv->remove_input, "clicked",
                                  G_CALLBACK (remove_selected_input), self);
        g_signal_connect_swapped (priv->show_config, "clicked",
                                  G_CALLBACK (show_selected_settings), self);
        g_signal_connect_swapped (priv->show_layout, "clicked",
                                  G_CALLBACK (show_selected_layout), self);
        g_signal_connect_swapped (priv->auto_detect, "clicked",
                                  G_CALLBACK (auto_detect), self);

        egg_list_box_set_selection_mode (EGG_LIST_BOX (priv->input_list),
                                         GTK_SELECTION_SINGLE);
        egg_list_box_set_separator_funcs (EGG_LIST_BOX (priv->input_list),
                                          update_separator_func,
                                          NULL, NULL);
        g_signal_connect_swapped (priv->input_list, "child-selected",
                                  G_CALLBACK (update_buttons), self);

        g_signal_connect_swapped (priv->input_list, "child-selected",
                                  G_CALLBACK (update_current_input), self);

        g_signal_connect (priv->input_settings, "changed::" KEY_INPUT_SOURCES,
                          G_CALLBACK (input_sources_changed), self);

        g_signal_connect_swapped (priv->input_settings, "changed::" KEY_CURRENT_INPUT_SOURCE,
                                  G_CALLBACK (current_input_source_changed), self);

        add_default_input_source_for_locale (self);
        set_input_settings (self);
        current_input_source_changed (self);
}

static void
add_input_sources_from_localed (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GVariant *v;
        const gchar *s;
        gchar **layouts = NULL;
        gchar **variants = NULL;
        gint i, n;

        if (!priv->localed)
                return;

        v = g_dbus_proxy_get_cached_property (priv->localed, "X11Layout");
        if (v) {
                s = g_variant_get_string (v, NULL);
                layouts = g_strsplit (s, ",", -1);
                g_variant_unref (v);
        }

        v = g_dbus_proxy_get_cached_property (priv->localed, "X11Variant");
        if (v) {
                s = g_variant_get_string (v, NULL);
                if (s && *s)
                        variants = g_strsplit (s, ",", -1);
                g_variant_unref (v);
        }

        if (variants && variants[0])
                n = MIN (g_strv_length (layouts), g_strv_length (variants));
        else if (layouts && layouts[0])
                n = g_strv_length (layouts);
        else
                n = 0;

        for (i = 0; i < n && layouts[i][0]; i++) {
                const gchar *name;
                gchar *id;

                if (variants && variants[i] && variants[i][0])
                        id = g_strdup_printf ("%s+%s", layouts[i], variants[i]);
                else
                        id = g_strdup (layouts[i]);

                if (!input_source_already_added (self, id)) {
                        gnome_xkb_info_get_layout_info (priv->xkb_info, id, &name, NULL, NULL, NULL);
                        add_input_row (self, INPUT_SOURCE_TYPE_XKB, id, name ? name : id, NULL);
                }

                g_free (id);
        }

        g_strfreev (variants);
        g_strfreev (layouts);
}

static void
set_localed_input (GisKeyboardPage *self)
{
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GString *layouts;
        GString *variants;
        const gchar *type, *id;
        GList *list, *li;
        const gchar *l, *v;

        layouts = g_string_new ("");
        variants = g_string_new ("");

        list = gtk_container_get_children (GTK_CONTAINER (priv->input_list));
        for (li = list; li; li = li->next) {
                type = (const gchar *)g_object_get_data (G_OBJECT (li->data), "type");
                id = (const gchar *)g_object_get_data (G_OBJECT (li->data), "id");
                if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS))
                        continue;

                if (gnome_xkb_info_get_layout_info (priv->xkb_info, id, NULL, NULL, &l, &v)) {
                        if (layouts->str[0]) {
                                g_string_append_c (layouts, ',');
                                g_string_append_c (variants, ',');
                        }
                        g_string_append (layouts, l);
                        g_string_append (variants, v);
                }
        }
        g_list_free (list);

        g_dbus_proxy_call (priv->localed,
                           "SetX11Keyboard",
                           g_variant_new ("(ssssbb)", layouts->str, "", variants->str, "", TRUE, TRUE),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1, NULL, NULL, NULL);

        g_string_free (layouts, TRUE);
        g_string_free (variants, TRUE);
}

static void
localed_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      data)
{
        GisKeyboardPage *self = data;
        GisKeyboardPagePrivate *priv = gis_keyboard_page_get_instance_private (self);
        GDBusProxy *proxy;
        GError *error = NULL;

        proxy = g_dbus_proxy_new_finish (res, &error);

        if (!proxy) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to contact localed: %s\n", error->message);
                g_error_free (error);
                return;
        }

        priv->localed = proxy;

        add_input_sources_from_localed (self);
        update_input (self);
        current_input_source_changed (self);
        update_buttons (self);
}

static void
gis_keyboard_page_init (GisKeyboardPage *self)
{
        g_resources_register (keyboard_get_resource ());
}

void
gis_prepare_keyboard_page (GisDriver *driver)
{
  gis_driver_add_page (driver,
                       g_object_new (GIS_TYPE_KEYBOARD_PAGE,
                                     "driver", driver,
                                     NULL));
}
