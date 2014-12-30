/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Red Hat
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
 *     Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"
#include "cc-language-chooser.h"

#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <gtk/gtk.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-languages.h>

#include "cc-common-language.h"
#include "cc-util.h"

#include <glib-object.h>

#include <egg-list-box.h>

G_DEFINE_TYPE (CcLanguageChooser, cc_language_chooser, GTK_TYPE_BIN);

#define GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CC_TYPE_LANGUAGE_CHOOSER, CcLanguageChooserPrivate))

enum {
        PROP_0,
        PROP_LANGUAGE,
        PROP_SHOWING_EXTRA,
        PROP_LAST,
};

static GParamSpec *obj_props[PROP_LAST];

enum {
        LANGUAGE_ACTIVATED,
        LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _CcLanguageChooserPrivate
{
        GtkWidget *no_results;
        GtkWidget *more_item;
        GtkWidget *filter_entry;
        GtkWidget *language_list;
        gboolean showing_extra;
        gchar **filter_words;
        gchar *language;
};

typedef struct {
        GtkWidget *box;
        GtkWidget *checkmark;

        gchar *locale_id;
        gchar *locale_name;
        gchar *locale_current_name;
        gchar *locale_untranslated_name;
        gboolean is_extra;
} LanguageWidget;

static LanguageWidget *
get_language_widget (GtkWidget *widget)
{
        return g_object_get_data (G_OBJECT (widget), "language-widget");
}

static GtkWidget *
padded_label_new (char *text)
{
        GtkWidget *widget;
        widget = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_halign (widget, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top (widget, 10);
        gtk_widget_set_margin_bottom (widget, 10);
        gtk_box_pack_start (GTK_BOX (widget), gtk_label_new (text), FALSE, FALSE, 0);
        return widget;
}

static void
language_widget_free (gpointer data)
{
        LanguageWidget *widget = data;

        /* This is called when the box is destroyed,
         * so don't bother destroying the widget and
         * children again. */
        g_free (widget->locale_id);
        g_free (widget->locale_name);
        g_free (widget->locale_current_name);
        g_free (widget->locale_untranslated_name);
        g_free (widget);
}

static GtkWidget *
language_widget_new (const char *locale_id,
                     gboolean    is_extra)
{
        gchar *locale_name, *locale_current_name, *locale_untranslated_name;
        GtkWidget *checkmark;
        LanguageWidget *widget = g_new0 (LanguageWidget, 1);

        locale_name = gnome_get_language_from_locale (locale_id, locale_id);
        locale_current_name = gnome_get_language_from_locale (locale_id, NULL);
        locale_untranslated_name = gnome_get_language_from_locale (locale_id, "C");

        widget->box = padded_label_new (locale_name);
        widget->locale_id = g_strdup (locale_id);
        widget->locale_name = locale_name;
        widget->locale_current_name = locale_current_name;
        widget->locale_untranslated_name = locale_untranslated_name;
        widget->is_extra = is_extra;

        /* We add a check on each side of the label to keep it centered. */
        checkmark = gtk_image_new_from_icon_name ("object-select-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_show (checkmark);
        gtk_widget_set_opacity (checkmark, 0.0);
        gtk_box_pack_start (GTK_BOX (widget->box), checkmark, FALSE, FALSE, 0);
        gtk_box_reorder_child (GTK_BOX (widget->box), checkmark, 0);

        widget->checkmark = gtk_image_new_from_icon_name ("object-select-symbolic", GTK_ICON_SIZE_MENU);
        gtk_box_pack_start (GTK_BOX (widget->box), widget->checkmark,
                            FALSE, FALSE, 0);
        gtk_widget_show (widget->checkmark);

        g_object_set_data_full (G_OBJECT (widget->box), "language-widget", widget,
                                language_widget_free);

        return widget->box;
}

static void
sync_checkmark (GtkWidget *child,
                gpointer   user_data)
{
        LanguageWidget *widget = get_language_widget (child);
        gchar *locale_id;
        gboolean should_be_visible;

        if (widget == NULL)
                return;

        locale_id = user_data;
        should_be_visible = g_str_equal (widget->locale_id, locale_id);
        gtk_widget_set_opacity (widget->checkmark, should_be_visible ? 1.0 : 0.0);
}

static void
sync_all_checkmarks (CcLanguageChooser *chooser)
{
        CcLanguageChooserPrivate *priv = chooser->priv;

        gtk_container_foreach (GTK_CONTAINER (priv->language_list),
                               sync_checkmark, priv->language);
}

static GtkWidget *
more_widget_new (void)
{
        GtkWidget *widget;
        GtkWidget *arrow;

        widget = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_tooltip_text (widget, _("More…"));

        arrow = gtk_image_new_from_icon_name ("view-more-symbolic", GTK_ICON_SIZE_MENU);
        gtk_style_context_add_class (gtk_widget_get_style_context (arrow), "dim-label");
        gtk_widget_set_margin_top (widget, 10);
        gtk_widget_set_margin_bottom (widget, 10);
        gtk_misc_set_alignment (GTK_MISC (arrow), 0.5, 0.5);
        gtk_box_pack_start (GTK_BOX (widget), arrow, TRUE, TRUE, 0);

        return widget;
}

static GtkWidget *
no_results_widget_new (void)
{
        GtkWidget *widget;

        widget = padded_label_new (_("No languages found"));
        gtk_widget_set_sensitive (widget, FALSE);
        return widget;
}

static void
add_languages (CcLanguageChooser  *chooser,
               char               **locale_ids,
               GHashTable          *initial)
{
        CcLanguageChooserPrivate *priv = chooser->priv;

        while (*locale_ids) {
                const gchar *locale_id;
                gboolean is_initial;
                GtkWidget *widget;

                locale_id = *locale_ids;
                locale_ids ++;

                if (!cc_common_language_has_font (locale_id))
                        continue;

                is_initial = (g_hash_table_lookup (initial, locale_id) != NULL);
                widget = language_widget_new (locale_id, !is_initial);

                gtk_container_add (GTK_CONTAINER (priv->language_list), widget);
        }

        gtk_container_add (GTK_CONTAINER (priv->language_list), priv->more_item);
        gtk_container_add (GTK_CONTAINER (priv->language_list), priv->no_results);

        gtk_widget_show_all (priv->language_list);
}

static void
add_all_languages (CcLanguageChooser *chooser)
{
        char **locale_ids;
        GHashTable *initial;

        locale_ids = gnome_get_all_locales ();
        initial = cc_common_language_get_initial_languages ();
        add_languages (chooser, locale_ids, initial);
        g_hash_table_destroy (initial);
        g_strfreev (locale_ids);
}

static gboolean
match_all (gchar       **words,
           const gchar  *str)
{
        gchar **w;

        for (w = words; *w; ++w)
                if (!strstr (str, *w))
                        return FALSE;

        return TRUE;
}

static gboolean
language_visible (GtkWidget *child,
                  gpointer   user_data)
{
        CcLanguageChooser *chooser = user_data;
        CcLanguageChooserPrivate *priv = chooser->priv;
        gchar *locale_name = NULL;
        gchar *locale_current_name = NULL;
        gchar *locale_untranslated_name = NULL;
        LanguageWidget *widget;
        gboolean visible;

        if (child == priv->more_item)
                return !priv->showing_extra;

        /* We hide this in the after-refilter handler below. */
        if (child == priv->no_results)
                return TRUE;

        widget = get_language_widget (child);

        if (!priv->showing_extra && widget->is_extra)
                return FALSE;

        if (!priv->filter_words)
                return TRUE;

        visible = FALSE;

        locale_name = cc_util_normalize_casefold_and_unaccent (widget->locale_name);
        visible = match_all (priv->filter_words, locale_name);
        if (visible)
                goto out;

        locale_current_name = cc_util_normalize_casefold_and_unaccent (widget->locale_current_name);
        visible = match_all (priv->filter_words, locale_current_name);
        if (visible)
                goto out;

        locale_untranslated_name = cc_util_normalize_casefold_and_unaccent (widget->locale_untranslated_name);
        visible = match_all (priv->filter_words, locale_untranslated_name);
        if (visible)
                goto out;

 out:
        g_free (locale_untranslated_name);
        g_free (locale_current_name);
        g_free (locale_name);
        return visible;
}

static gint
sort_languages (GtkWidget *a,
                GtkWidget *b,
                gpointer   data)
{
        LanguageWidget *la, *lb;

        la = get_language_widget (a);
        lb = get_language_widget (b);

        if (la == NULL)
                return 1;

        if (lb == NULL)
                return -1;

        return strcmp (la->locale_name, lb->locale_name);
}

static void
filter_changed (GtkEntry        *entry,
                CcLanguageChooser *chooser)
{
        CcLanguageChooserPrivate *priv = chooser->priv;
        gchar *filter_contents = NULL;

        g_clear_pointer (&priv->filter_words, g_strfreev);

        filter_contents =
                cc_util_normalize_casefold_and_unaccent (gtk_entry_get_text (GTK_ENTRY (priv->filter_entry)));
        if (!filter_contents) {
                egg_list_box_refilter (EGG_LIST_BOX (priv->language_list));
                return;
        }
        priv->filter_words = g_strsplit_set (g_strstrip (filter_contents), " ", 0);
        g_free (filter_contents);
        egg_list_box_refilter (EGG_LIST_BOX (priv->language_list));
}

static void
show_more (CcLanguageChooser *chooser)
{
        CcLanguageChooserPrivate *priv = chooser->priv;

        gtk_widget_show (priv->filter_entry);
        gtk_widget_grab_focus (priv->filter_entry);

        priv->showing_extra = TRUE;
        egg_list_box_refilter (EGG_LIST_BOX (priv->language_list));
        g_object_notify_by_pspec (G_OBJECT (chooser), obj_props[PROP_SHOWING_EXTRA]);
}

static void
set_locale_id (CcLanguageChooser *chooser,
               const gchar       *new_locale_id)
{
        CcLanguageChooserPrivate *priv = chooser->priv;

        if (g_strcmp0 (priv->language, new_locale_id) == 0)
                return;

        g_free (priv->language);
        priv->language = g_strdup (new_locale_id);

        sync_all_checkmarks (chooser);

        g_object_notify_by_pspec (G_OBJECT (chooser), obj_props[PROP_LANGUAGE]);
}

static void
child_activated (EggListBox        *box,
                 GtkWidget         *child,
                 CcLanguageChooser *chooser)
{
        CcLanguageChooserPrivate *priv = chooser->priv;
        LanguageWidget *widget;

        if (child == NULL)
                return;

        if (child == priv->no_results)
                return;

        if (child == priv->more_item) {
                show_more (chooser);
                return;
        }

        widget = get_language_widget (child);
        set_locale_id (chooser, widget->locale_id);

        g_signal_emit (chooser, signals[LANGUAGE_ACTIVATED], 0, widget->locale_id);
}

typedef struct {
        gint count;
        GtkWidget *ignore;
} CountChildrenData;

static void
count_visible_children (GtkWidget *widget,
                        gpointer   user_data)
{
        CountChildrenData *data = user_data;
        if (widget != data->ignore &&
            gtk_widget_get_child_visible (widget) &&
            gtk_widget_get_visible (widget))
                data->count++;
}

static void
end_refilter (EggListBox *list_box,
              gpointer    user_data)
{
        CcLanguageChooser *chooser = CC_LANGUAGE_CHOOSER (user_data);
        CcLanguageChooserPrivate *priv = chooser->priv;

        CountChildrenData data = { 0 };

        data.ignore = priv->no_results;

        gtk_container_foreach (GTK_CONTAINER (list_box),
                               count_visible_children, &data);

        gtk_widget_set_visible (priv->no_results, (data.count == 0));
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

#define WID(name) ((GtkWidget *) gtk_builder_get_object (builder, name))

static void
cc_language_chooser_constructed (GObject *object)
{
        GtkBuilder *builder;
        CcLanguageChooser *chooser = CC_LANGUAGE_CHOOSER (object);
        CcLanguageChooserPrivate *priv = chooser->priv;
        GError *error = NULL;

        G_OBJECT_CLASS (cc_language_chooser_parent_class)->constructed (object);

        builder = gtk_builder_new ();
        if (gtk_builder_add_from_resource (builder, "/org/gnome/control-center/language-chooser.ui", &error) == 0) {
                g_object_unref (builder);
                g_warning ("failed to load language chooser: %s", error->message);
                g_error_free (error);
                return;
        }

        gtk_container_add (GTK_CONTAINER (chooser), WID ("language-chooser"));

        priv->filter_entry = WID ("language-filter-entry");
        priv->language_list = WID ("language-list");
        priv->more_item = more_widget_new ();
        priv->no_results = no_results_widget_new ();

        egg_list_box_set_adjustment (EGG_LIST_BOX (priv->language_list),
                                     gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (WID ("language-scrolledwindow"))));

        egg_list_box_set_sort_func (EGG_LIST_BOX (priv->language_list),
                                    sort_languages, chooser, NULL);
        egg_list_box_set_filter_func (EGG_LIST_BOX (priv->language_list),
                                      language_visible, chooser, NULL);
        egg_list_box_set_separator_funcs (EGG_LIST_BOX (priv->language_list),
                                          update_separator_func, chooser, NULL);
        egg_list_box_set_selection_mode (EGG_LIST_BOX (priv->language_list),
                                         GTK_SELECTION_NONE);
        add_all_languages (chooser);

        g_signal_connect (priv->filter_entry, "changed",
                          G_CALLBACK (filter_changed),
                          chooser);

        g_signal_connect (priv->language_list, "child-activated",
                          G_CALLBACK (child_activated), chooser);

        g_signal_connect_after (priv->language_list, "refilter",
                                G_CALLBACK (end_refilter), chooser);

        egg_list_box_refilter (EGG_LIST_BOX (priv->language_list));

        if (priv->language == NULL)
                priv->language = cc_common_language_get_current_language ();

        sync_all_checkmarks (chooser);

        g_object_unref (builder);
}

static void
cc_language_chooser_get_property (GObject      *object,
                                  guint         prop_id,
                                  GValue       *value,
                                  GParamSpec   *pspec)
{
        CcLanguageChooser *chooser = CC_LANGUAGE_CHOOSER (object);
        switch (prop_id) {
        case PROP_LANGUAGE:
                g_value_set_string (value, cc_language_chooser_get_language (chooser));
                break;
        case PROP_SHOWING_EXTRA:
                g_value_set_boolean (value, cc_language_chooser_get_showing_extra (chooser));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
cc_language_chooser_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
        CcLanguageChooser *chooser = CC_LANGUAGE_CHOOSER (object);
        switch (prop_id) {
        case PROP_LANGUAGE:
                cc_language_chooser_set_language (chooser, g_value_get_string (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
cc_language_chooser_finalize (GObject *object)
{
        CcLanguageChooser *chooser = CC_LANGUAGE_CHOOSER (object);
        CcLanguageChooserPrivate *priv = chooser->priv;

        g_strfreev (priv->filter_words);
}

static void
cc_language_chooser_class_init (CcLanguageChooserClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        g_type_class_add_private (object_class, sizeof(CcLanguageChooserPrivate));

        object_class->get_property = cc_language_chooser_get_property;
        object_class->set_property = cc_language_chooser_set_property;
        object_class->finalize = cc_language_chooser_finalize;
        object_class->constructed = cc_language_chooser_constructed;

        obj_props[PROP_LANGUAGE] =
                g_param_spec_string ("language", "", "", "",
                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

        obj_props[PROP_SHOWING_EXTRA] =
                g_param_spec_string ("showing-extra", "", "", "",
                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

        signals[LANGUAGE_ACTIVATED] =
                g_signal_new ("language-activated",
                              CC_TYPE_LANGUAGE_CHOOSER,
                              G_SIGNAL_RUN_LAST,
                              0, NULL, NULL,
                              NULL,
                              G_TYPE_NONE, 1,
                              G_TYPE_STRING);

        g_object_class_install_properties (object_class, PROP_LAST, obj_props);
}

void
cc_language_chooser_init (CcLanguageChooser *chooser)
{
        chooser->priv = GET_PRIVATE (chooser);
}

void
cc_language_chooser_clear_filter (CcLanguageChooser *chooser)
{
        CcLanguageChooserPrivate *priv = chooser->priv;
        gtk_entry_set_text (GTK_ENTRY (priv->filter_entry), "");
}

const gchar *
cc_language_chooser_get_language (CcLanguageChooser *chooser)
{
        CcLanguageChooserPrivate *priv = chooser->priv;
        return priv->language;
}

void
cc_language_chooser_set_language (CcLanguageChooser *chooser,
                                  const gchar        *language)
{
        set_locale_id (chooser, language);
}

gboolean
cc_language_chooser_get_showing_extra (CcLanguageChooser *chooser)
{
        CcLanguageChooserPrivate *priv = chooser->priv;
        return priv->showing_extra;
}
