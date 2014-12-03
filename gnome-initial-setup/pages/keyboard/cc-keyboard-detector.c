/*
 * Copyright (C) 2014 Endless Mobile, Inc.
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
 */

#include <config.h>
#include <locale.h>
#include <glib/gi18n.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-languages.h>

#include <egg-list-box.h>

#include <../language/cc-common-language.h>
#include <../language/cc-util.h>
#include "cc-keyboard-detector.h"

#ifdef HAVE_IBUS
#include <ibus.h>
#include "cc-ibus-utils.h"
#endif  /* HAVE_IBUS */

#define INPUT_SOURCE_TYPE_XKB "xkb"
#define INPUT_SOURCE_TYPE_IBUS "ibus"

#define ARROW_NEXT "go-next-symbolic"
#define ARROW_PREV "go-previous-symbolic"

#define MAIN_WINDOW_WIDTH_RATIO 0.60

typedef enum {
  ROW_TRAVEL_DIRECTION_NONE,
  ROW_TRAVEL_DIRECTION_FORWARD,
  ROW_TRAVEL_DIRECTION_BACKWARD
} RowTravelDirection;

typedef enum {
  ROW_LABEL_POSITION_START,
  ROW_LABEL_POSITION_CENTER,
  ROW_LABEL_POSITION_END
} RowLabelPosition;

typedef struct {
  /* Not owned */
  GtkWidget *add_button;
  GtkWidget *filter_entry;
  GtkWidget *list;
  GtkWidget *scrolledwindow;
  GtkAdjustment *adjustment;
  GnomeXkbInfo *xkb_info;
  GHashTable *ibus_engines;

  /* Owned */
  GtkWidget *more_item;
  GtkWidget *no_results;
  GHashTable *locales;
  GHashTable *locales_by_language;
  gboolean showing_extra;
  gchar **filter_words;
} CcKeyboardDetectorPrivate;

#define GET_PRIVATE(detector) ((CcKeyboardDetectorPrivate *) g_object_get_data (G_OBJECT (detector), "private"))
#define WID(name) ((GtkWidget *) gtk_builder_get_object (builder, name))

typedef struct {
  gchar *id;
  gchar *name;
  gchar *unaccented_name;
  gchar *untranslated_name;
  GtkWidget *default_input_source_widget;
  GtkWidget *locale_widget;
  GtkWidget *back_widget;
  GHashTable *layout_widgets_by_id;
  GHashTable *engine_widgets_by_id;
} LocaleInfo;

static void
locale_info_free (gpointer data)
{
  LocaleInfo *info = data;

  g_free (info->id);
  g_free (info->name);
  g_free (info->unaccented_name);
  g_free (info->untranslated_name);
  g_object_unref (info->default_input_source_widget);
  g_object_unref (info->locale_widget);
  g_object_unref (info->back_widget);
  g_hash_table_destroy (info->layout_widgets_by_id);
  g_hash_table_destroy (info->engine_widgets_by_id);
  g_free (info);
}

static void
set_row_widget_margins (GtkWidget *widget)
{
  gtk_widget_set_margin_left (widget, 20);
  gtk_widget_set_margin_right (widget, 20);
  gtk_widget_set_margin_top (widget, 6);
  gtk_widget_set_margin_bottom (widget, 6);
}

static GtkWidget *
padded_label_new (const gchar        *text,
                  RowLabelPosition    position,
                  RowTravelDirection  direction,
                  gboolean            dim_label)
{
  GtkWidget *widget;
  GtkWidget *label;
  GtkWidget *arrow;
  gdouble alignment;
  gboolean rtl;

  rtl = (gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL);

  if (position == ROW_LABEL_POSITION_START)
    alignment = 0.0;
  else if (position == ROW_LABEL_POSITION_CENTER)
    alignment = 0.5;
  else
    alignment = 1.0;

  widget = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  if (direction == ROW_TRAVEL_DIRECTION_BACKWARD)
    {
      arrow = gtk_image_new_from_icon_name (rtl ? ARROW_NEXT : ARROW_PREV,
                                            GTK_ICON_SIZE_MENU);
      gtk_box_pack_start (GTK_BOX (widget), arrow, FALSE, TRUE, 0);
    }

  label = gtk_label_new (text);
  gtk_misc_set_alignment (GTK_MISC (label), alignment, 0.5);
  set_row_widget_margins (label);
  gtk_box_pack_start (GTK_BOX (widget), label, TRUE, TRUE, 0);
  if (dim_label)
    gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");

  if (direction == ROW_TRAVEL_DIRECTION_FORWARD)
    {
      arrow = gtk_image_new_from_icon_name (rtl ? ARROW_PREV : ARROW_NEXT,
                                            GTK_ICON_SIZE_MENU);
      gtk_box_pack_start (GTK_BOX (widget), arrow, FALSE, TRUE, 0);
    }

  return widget;
}

static GtkWidget *
more_widget_new (void)
{
  GtkWidget *widget;
  GtkWidget *arrow;

  widget = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_tooltip_text (widget, _("More…"));

  arrow = gtk_image_new_from_icon_name ("view-more-symbolic", GTK_ICON_SIZE_MENU);
  gtk_style_context_add_class (gtk_widget_get_style_context (arrow), "dim-label");
  set_row_widget_margins (arrow);
  gtk_misc_set_alignment (GTK_MISC (arrow), 0.5, 0.5);
  gtk_box_pack_start (GTK_BOX (widget), arrow, TRUE, TRUE, 0);

  return widget;
}

static GtkWidget *
no_results_widget_new (void)
{
  return padded_label_new (_("No input sources found"), ROW_LABEL_POSITION_CENTER, ROW_TRAVEL_DIRECTION_NONE, TRUE);
}

static GtkWidget *
back_widget_new (const gchar *text)
{
  return padded_label_new (text, ROW_LABEL_POSITION_CENTER, ROW_TRAVEL_DIRECTION_BACKWARD, TRUE);
}

static GtkWidget *
locale_widget_new (const gchar *text)
{
  return padded_label_new (text, ROW_LABEL_POSITION_CENTER, ROW_TRAVEL_DIRECTION_NONE, FALSE);
}

static GtkWidget *
locale_separator_widget_new (const gchar *text)
{
  GtkWidget *widget;

  widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start (GTK_BOX (widget),
                      gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                      FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (widget),
                      padded_label_new (text, ROW_LABEL_POSITION_CENTER, ROW_TRAVEL_DIRECTION_NONE, TRUE),
                      FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (widget),
                      gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                      FALSE, FALSE, 0);
  return widget;
}

static GtkWidget *
input_source_widget_new (GtkWidget   *detector,
                         const gchar *type,
                         const gchar *id)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  GtkWidget *widget = NULL;

  if (g_str_equal (type, INPUT_SOURCE_TYPE_XKB))
    {
      const gchar *display_name;

      gnome_xkb_info_get_layout_info (priv->xkb_info, id, &display_name, NULL, NULL, NULL);

      widget = padded_label_new (display_name,
                                 ROW_LABEL_POSITION_START,
                                 ROW_TRAVEL_DIRECTION_NONE,
                                 FALSE);
      g_object_set_data (G_OBJECT (widget), "name", (gpointer) display_name);
      g_object_set_data_full (G_OBJECT (widget), "unaccented-name",
                              cc_util_normalize_casefold_and_unaccent (display_name), g_free);
    }
  else if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS))
    {
#ifdef HAVE_IBUS
      gchar *display_name;
      GtkWidget *image;

      display_name = engine_get_display_name (g_hash_table_lookup (priv->ibus_engines, id));

      widget = padded_label_new (display_name,
                                 ROW_LABEL_POSITION_START,
                                 ROW_TRAVEL_DIRECTION_NONE,
                                 FALSE);
      image = gtk_image_new_from_icon_name ("system-run-symbolic", GTK_ICON_SIZE_MENU);
      set_row_widget_margins (image);
      gtk_style_context_add_class (gtk_widget_get_style_context (image), "dim-label");
      gtk_box_pack_start (GTK_BOX (widget), image, FALSE, TRUE, 0);

      g_object_set_data_full (G_OBJECT (widget), "name", display_name, g_free);
      g_object_set_data_full (G_OBJECT (widget), "unaccented-name",
                              cc_util_normalize_casefold_and_unaccent (display_name), g_free);
#else
      widget = NULL;
#endif  /* HAVE_IBUS */
    }

  if (widget)
    {
      g_object_set_data (G_OBJECT (widget), "type", (gpointer) type);
      g_object_set_data (G_OBJECT (widget), "id", (gpointer) id);
    }

  return widget;
}

static void
remove_all_children (GtkContainer *container)
{
  GList *list, *l;

  list = gtk_container_get_children (container);
  for (l = list; l; l = l->next)
    gtk_container_remove (container, (GtkWidget *) l->data);
  g_list_free (list);
}

static void
set_fixed_size (GtkWidget *detector)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  GtkPolicyType policy;
  gint width, height;

  gtk_scrolled_window_get_policy (GTK_SCROLLED_WINDOW (priv->scrolledwindow), &policy, NULL);
  if (policy == GTK_POLICY_AUTOMATIC)
    return;

  /* Don't let it automatically get wider than the main GIS window nor
     get taller than the initial height */
  gtk_window_get_size (gtk_window_get_transient_for (GTK_WINDOW (detector)),
                       &width, NULL);
  gtk_window_get_size (GTK_WINDOW (detector), NULL, &height);
  gtk_widget_set_size_request (detector, width * MAIN_WINDOW_WIDTH_RATIO, height);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (priv->scrolledwindow),
                                  GTK_POLICY_AUTOMATIC,
                                  GTK_POLICY_AUTOMATIC);
}

static void
update_separator (GtkWidget **separator,
                  GtkWidget  *child,
                  GtkWidget  *before,
                  gpointer    user_data)
{
  if (*separator && !GTK_IS_SEPARATOR (*separator))
    {
      gtk_widget_destroy (*separator);
      *separator = NULL;
    }

  if (*separator == NULL)
    {
      *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
      g_object_ref_sink (*separator);
      gtk_widget_show (*separator);
    }
}

static void
add_input_source_widgets_for_locale (GtkWidget  *detector,
                                     LocaleInfo *info)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  GtkWidget *widget;
  GHashTableIter iter;
  const gchar *id;

  if (info->default_input_source_widget)
    gtk_container_add (GTK_CONTAINER (priv->list), info->default_input_source_widget);

  g_hash_table_iter_init (&iter, info->layout_widgets_by_id);
  while (g_hash_table_iter_next (&iter, (gpointer *) &id, (gpointer *) &widget))
    gtk_container_add (GTK_CONTAINER (priv->list), widget);

  g_hash_table_iter_init (&iter, info->engine_widgets_by_id);
  while (g_hash_table_iter_next (&iter, (gpointer *) &id, (gpointer *) &widget))
    gtk_container_add (GTK_CONTAINER (priv->list), widget);
}

static void
show_input_sources_for_locale (GtkWidget   *detector,
                               LocaleInfo  *info)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);

  set_fixed_size (detector);

  remove_all_children (GTK_CONTAINER (priv->list));

  if (!info->back_widget)
    {
      info->back_widget = g_object_ref_sink (back_widget_new (info->name));
      g_object_set_data (G_OBJECT (info->back_widget), "back", GINT_TO_POINTER (TRUE));
      g_object_set_data (G_OBJECT (info->back_widget), "locale-info", info);
    }
  gtk_container_add (GTK_CONTAINER (priv->list), info->back_widget);

  add_input_source_widgets_for_locale (detector, info);

  gtk_widget_show_all (priv->list);

  gtk_adjustment_set_value (priv->adjustment,
                            gtk_adjustment_get_lower (priv->adjustment));
  egg_list_box_set_separator_funcs (EGG_LIST_BOX (priv->list), update_separator, NULL, NULL);
  egg_list_box_refilter (EGG_LIST_BOX (priv->list));
  egg_list_box_set_selection_mode (EGG_LIST_BOX (priv->list), GTK_SELECTION_SINGLE);

  if (gtk_widget_is_visible (priv->filter_entry))
    gtk_widget_grab_focus (priv->filter_entry);
}

static gboolean
is_current_locale (const gchar *locale)
{
  return g_strcmp0 (setlocale (LC_CTYPE, NULL), locale) == 0;
}

static void
show_locale_widgets (GtkWidget *detector)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  GHashTable *initial = NULL;
  LocaleInfo *info;
  GHashTableIter iter;

  remove_all_children (GTK_CONTAINER (priv->list));

  if (!priv->showing_extra)
    initial = cc_common_language_get_initial_languages ();

  g_hash_table_iter_init (&iter, priv->locales);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &info))
    {
      if (!info->default_input_source_widget &&
          !g_hash_table_size (info->layout_widgets_by_id) &&
          !g_hash_table_size (info->engine_widgets_by_id))
        continue;

      if (!info->locale_widget)
        {
          info->locale_widget = g_object_ref_sink (locale_widget_new (info->name));
          g_object_set_data (G_OBJECT (info->locale_widget), "locale-info", info);

          if (!priv->showing_extra &&
              !g_hash_table_contains (initial, info->id) &&
              !is_current_locale (info->id))
            g_object_set_data (G_OBJECT (info->locale_widget), "is-extra", GINT_TO_POINTER (TRUE));
        }
      gtk_container_add (GTK_CONTAINER (priv->list), info->locale_widget);
    }

  gtk_container_add (GTK_CONTAINER (priv->list), priv->more_item);

  gtk_widget_show_all (priv->list);

  gtk_adjustment_set_value (priv->adjustment,
                            gtk_adjustment_get_lower (priv->adjustment));
  egg_list_box_set_separator_funcs (EGG_LIST_BOX (priv->list), update_separator, NULL, NULL);
  egg_list_box_refilter (EGG_LIST_BOX (priv->list));
  egg_list_box_set_selection_mode (EGG_LIST_BOX (priv->list), GTK_SELECTION_NONE);

  if (gtk_widget_is_visible (priv->filter_entry))
    gtk_widget_grab_focus (priv->filter_entry);

  if (!priv->showing_extra)
    g_hash_table_destroy (initial);

  return;
}

static gint
list_sort (GtkWidget *a,
           GtkWidget *b,
           gpointer   data)
{
  GtkWidget *detector = data;
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  LocaleInfo *ia;
  LocaleInfo *ib;
  const gchar *la;
  const gchar *lb;
  gint retval;

  /* Always goes at the start */
  if (a == priv->no_results)
    return -1;
  if (b == priv->no_results)
    return 1;

  /* Always goes at the end */
  if (a == priv->more_item)
    return 1;
  if (b == priv->more_item)
    return -1;

  ia = g_object_get_data (G_OBJECT (a), "locale-info");
  ib = g_object_get_data (G_OBJECT (b), "locale-info");

  /* The "Other" locale always goes at the end */
  if (!ia->id[0] && ib->id[0])
    return 1;
  else if (ia->id[0] && !ib->id[0])
    return -1;

  retval = g_strcmp0 (ia->name, ib->name);
  if (retval)
    return retval;

  la = g_object_get_data (G_OBJECT (a), "name");
  lb = g_object_get_data (G_OBJECT (b), "name");

  /* Only input sources have a "name" property and they should always
     go after their respective heading */
  if (la && !lb)
    return 1;
  else if (!la && lb)
    return -1;
  else if (!la && !lb)
    return 0; /* Shouldn't happen */

  /* The default input source always goes first in its group */
  if (g_object_get_data (G_OBJECT (a), "default"))
    return -1;
  if (g_object_get_data (G_OBJECT (b), "default"))
    return 1;

  return g_strcmp0 (la, lb);
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
list_filter (GtkWidget *child,
             gpointer   user_data)
{
  GtkDialog *detector = user_data;
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  LocaleInfo *info;
  gboolean is_extra;
  const gchar *source_name;

  if (child == priv->more_item)
    return !priv->showing_extra;

  /* We hide this in the after-refilter handler below. */
  if (child == priv->no_results)
    return TRUE;

  is_extra = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (child), "is-extra"));

  if (!priv->showing_extra && is_extra)
    return FALSE;

  if (!priv->filter_words)
    return TRUE;

  info = g_object_get_data (G_OBJECT (child), "locale-info");

  if (match_all (priv->filter_words, info->unaccented_name))
    return TRUE;

  if (match_all (priv->filter_words, info->untranslated_name))
    return TRUE;

  source_name = g_object_get_data (G_OBJECT (child), "unaccented-name");
  if (source_name && match_all (priv->filter_words, source_name))
    return TRUE;

  return FALSE;
}

static void
update_separator_filter (GtkWidget **separator,
                         GtkWidget  *child,
                         GtkWidget  *before,
                         gpointer    user_data)
{
  LocaleInfo *child_info = NULL;
  LocaleInfo *before_info = NULL;

  if (child)
    child_info = g_object_get_data (G_OBJECT (child), "locale-info");

  if (before)
    before_info = g_object_get_data (G_OBJECT (before), "locale-info");

  if (!child_info || !before_info)
    return;

  if (child_info == before_info)
    {
      /* Create a regular separator if we don't have one */
      if (*separator && !GTK_IS_SEPARATOR (*separator))
        {
          gtk_widget_destroy (*separator);
          *separator = NULL;
        }

      if (*separator == NULL)
        *separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    }
  else
    {
      /* Create a locale heading separator if we don't have one */
      if (*separator && GTK_IS_SEPARATOR (*separator))
        {
          gtk_widget_destroy (*separator);
          *separator = NULL;
        }

      if (*separator == NULL)
        *separator = locale_separator_widget_new (child_info->name);
    }

  g_object_ref_sink (*separator);
  gtk_widget_show_all (*separator);
}

static void
show_filter_widgets (GtkWidget *detector)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  LocaleInfo *info;
  GHashTableIter iter;

  remove_all_children (GTK_CONTAINER (priv->list));

  gtk_container_add (GTK_CONTAINER (priv->list), priv->no_results);

  g_hash_table_iter_init (&iter, priv->locales);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &info))
    add_input_source_widgets_for_locale (detector, info);

  gtk_widget_show_all (priv->list);

  gtk_adjustment_set_value (priv->adjustment,
                            gtk_adjustment_get_lower (priv->adjustment));
  egg_list_box_set_separator_funcs (EGG_LIST_BOX (priv->list),
                                    update_separator_filter, NULL, NULL);
  egg_list_box_refilter (EGG_LIST_BOX (priv->list));
  egg_list_box_set_selection_mode (EGG_LIST_BOX (priv->list), GTK_SELECTION_SINGLE);

  if (gtk_widget_is_visible (priv->filter_entry))
    gtk_widget_grab_focus (priv->filter_entry);
}

static gboolean
strvs_differ (gchar **av,
              gchar **bv)
{
  gchar **a, **b;

  for (a = av, b = bv; *a && *b; ++a, ++b)
    if (!g_str_equal (*a, *b))
      return TRUE;

  if (*a == NULL && *b == NULL)
    return FALSE;

  return TRUE;
}

static void
filter_changed (GtkWidget *detector)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  gboolean was_filtering;
  gchar **previous_words;
  gchar *filter_contents = NULL;

  previous_words = priv->filter_words;
  was_filtering = previous_words != NULL;

  filter_contents =
    cc_util_normalize_casefold_and_unaccent (gtk_entry_get_text (GTK_ENTRY (priv->filter_entry)));

  if (filter_contents)
    {
      priv->filter_words = g_strsplit_set (g_strstrip (filter_contents), " ", 0);
      g_free (filter_contents);
    }

  if (!priv->filter_words || !priv->filter_words[0])
    {
      g_clear_pointer (&priv->filter_words, g_strfreev);
      if (was_filtering)
        show_locale_widgets (detector);
    }
  else
    {
      if (!was_filtering)
        show_filter_widgets (detector);
      else if (strvs_differ (priv->filter_words, previous_words))
        egg_list_box_refilter (EGG_LIST_BOX (priv->list));
    }

  g_strfreev (previous_words);
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
  GtkDialog *detector = user_data;
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  CountChildrenData data = { 0 };
  gboolean visible;

  data.ignore = priv->no_results;

  gtk_container_foreach (GTK_CONTAINER (list_box),
                         count_visible_children, &data);

  visible = (data.count == 0);

  gtk_widget_set_visible (priv->no_results, visible);
  egg_list_box_set_selection_mode (EGG_LIST_BOX (priv->list),
                                   visible ? GTK_SELECTION_NONE : GTK_SELECTION_SINGLE);
}

static void
show_more (GtkWidget *detector)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);

  set_fixed_size (detector);

  gtk_widget_show (priv->filter_entry);
  gtk_widget_grab_focus (priv->filter_entry);

  priv->showing_extra = TRUE;

  egg_list_box_refilter (EGG_LIST_BOX (priv->list));
}

static void
child_activated (EggListBox *box,
                 GtkWidget  *child,
                 GtkWidget  *detector)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  gpointer data;

  if (!child)
    return;

  if (child == priv->more_item)
    {
      show_more (detector);
      return;
    }

  data = g_object_get_data (G_OBJECT (child), "back");
  if (data)
    {
      show_locale_widgets (detector);
      return;
    }

  data = g_object_get_data (G_OBJECT (child), "name");
  if (data)
    {
      /* It's an input source, we just want to select it */
      return;
    }

  data = g_object_get_data (G_OBJECT (child), "locale-info");
  if (data)
    {
      show_input_sources_for_locale (detector, (LocaleInfo *) data);
      return;
    }
}

static void
child_selected (EggListBox *box,
                GtkWidget  *child,
                GtkWidget  *detector)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);

  gtk_widget_set_sensitive (priv->add_button, child != NULL);
}

static void
add_default_widget (GtkWidget   *detector,
                    LocaleInfo  *info,
                    const gchar *type,
                    const gchar *id)
{
  info->default_input_source_widget = input_source_widget_new (detector, type, id);
  if (info->default_input_source_widget)
    {
      g_object_ref_sink (info->default_input_source_widget);
      g_object_set_data (G_OBJECT (info->default_input_source_widget), "default", GINT_TO_POINTER (TRUE));
      g_object_set_data (G_OBJECT (info->default_input_source_widget), "locale-info", info);
    }
}

static void
add_widgets_to_table (GtkWidget   *detector,
                      LocaleInfo  *info,
                      GList       *list,
                      const gchar *type,
                      const gchar *default_id)
{
  GHashTable *table;
  GtkWidget *widget;
  const gchar *id;

  if (g_str_equal (type, INPUT_SOURCE_TYPE_XKB))
    table = info->layout_widgets_by_id;
  else if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS))
    table = info->engine_widgets_by_id;
  else
    return;

  while (list)
    {
      id = (const gchar *) list->data;

      /* The widget for the default input source lives elsewhere */
      if (g_strcmp0 (id, default_id))
        {
          widget = input_source_widget_new (detector, type, id);
          if (widget)
            {
              g_object_set_data (G_OBJECT (widget), "locale-info", info);
              g_hash_table_replace (table, (gpointer) id, g_object_ref_sink (widget));
            }
        }
      list = list->next;
    }
}

static void
add_widget (GtkWidget   *detector,
            LocaleInfo  *info,
            const gchar *type,
            const gchar *id)
{
  GList tmp = { 0 };
  tmp.data = (gpointer) id;
  add_widgets_to_table (detector, info, &tmp, type, NULL);
}

static void
add_widget_other (GtkWidget   *detector,
                  const gchar *type,
                  const gchar *id)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  LocaleInfo *info = g_hash_table_lookup (priv->locales, "");
  add_widget (detector, info, type, id);
}

#ifdef HAVE_IBUS
static gboolean
maybe_set_as_default (GtkWidget   *detector,
                      LocaleInfo  *info,
                      const gchar *engine_id)
{
  const gchar *type, *id;

  if (!gnome_get_input_source_from_locale (info->id, &type, &id))
    return FALSE;

  if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS) &&
      g_str_equal (id, engine_id) &&
      info->default_input_source_widget == NULL)
    {
      add_default_widget (detector, info, type, id);
      return TRUE;
    }

  return FALSE;
}

static void
get_ibus_locale_infos (GtkWidget *detector)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  GHashTableIter iter;
  LocaleInfo *info;
  const gchar *engine_id;
  IBusEngineDesc *engine;

  if (!priv->ibus_engines)
    return;

  g_hash_table_iter_init (&iter, priv->ibus_engines);
  while (g_hash_table_iter_next (&iter, (gpointer *) &engine_id, (gpointer *) &engine))
    {
      gchar *lang_code = NULL;
      gchar *country_code = NULL;
      const gchar *ibus_locale = ibus_engine_desc_get_language (engine);

      if (gnome_parse_locale (ibus_locale, &lang_code, &country_code, NULL, NULL) &&
          lang_code != NULL &&
          country_code != NULL)
        {
          gchar *locale = g_strdup_printf ("%s_%s.utf8", lang_code, country_code);

          info = g_hash_table_lookup (priv->locales, locale);
          if (info)
            {
              const gchar *type, *id;

              if (gnome_get_input_source_from_locale (locale, &type, &id) &&
                  g_str_equal (type, INPUT_SOURCE_TYPE_IBUS) &&
                  g_str_equal (id, engine_id))
                {
                  add_default_widget (detector, info, type, id);
                }
              else
                {
                  add_widget (detector, info, INPUT_SOURCE_TYPE_IBUS, engine_id);
                }
            }
          else
            {
              add_widget_other (detector, INPUT_SOURCE_TYPE_IBUS, engine_id);
            }

          g_free (locale);
        }
      else if (lang_code != NULL)
        {
          GHashTableIter iter;
          GHashTable *locales_for_language;
          gchar *language;

          /* Most IBus engines only specify the language so we try to
             add them to all locales for that language. */

          language = gnome_get_language_from_code (lang_code, NULL);
          if (language)
            locales_for_language = g_hash_table_lookup (priv->locales_by_language, language);
          else
            locales_for_language = NULL;
          g_free (language);

          if (locales_for_language)
            {
              g_hash_table_iter_init (&iter, locales_for_language);
              while (g_hash_table_iter_next (&iter, (gpointer *) &info, NULL))
                if (!maybe_set_as_default (detector, info, engine_id))
                  add_widget (detector, info, INPUT_SOURCE_TYPE_IBUS, engine_id);
            }
          else
            {
              add_widget_other (detector, INPUT_SOURCE_TYPE_IBUS, engine_id);
            }
        }
      else
        {
          add_widget_other (detector, INPUT_SOURCE_TYPE_IBUS, engine_id);
        }

      g_free (country_code);
      g_free (lang_code);
    }
}
#endif  /* HAVE_IBUS */

static void
add_locale_to_table (GHashTable  *table,
                     const gchar *lang_code,
                     LocaleInfo  *info)
{
  GHashTable *set;
  gchar *language;

  language = gnome_get_language_from_code (lang_code, NULL);

  set = g_hash_table_lookup (table, language);
  if (!set)
    {
      set = g_hash_table_new (NULL, NULL);
      g_hash_table_replace (table, g_strdup (language), set);
    }
  g_hash_table_add (set, info);

  g_free (language);
}

static void
add_ids_to_set (GHashTable *set,
                GList      *list)
{
  while (list)
    {
      g_hash_table_add (set, list->data);
      list = list->next;
    }
}

static void
get_locale_infos (GtkWidget *detector)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  GHashTable *layouts_with_locale;
  LocaleInfo *info;
  gchar **locale_ids;
  gchar **locale;
  GList *list, *l;

  priv->locales = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         NULL, locale_info_free);
  priv->locales_by_language = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, (GDestroyNotify) g_hash_table_destroy);

  layouts_with_locale = g_hash_table_new (g_str_hash, g_str_equal);

  locale_ids = gnome_get_all_locales ();
  for (locale = locale_ids; *locale; ++locale)
    {
      gchar *lang_code, *country_code;
      gchar *simple_locale;
      gchar *untranslated_locale;
      const gchar *type = NULL;
      const gchar *id = NULL;

      if (!gnome_parse_locale (*locale, &lang_code, &country_code, NULL, NULL))
        continue;

      simple_locale = g_strdup_printf ("%s_%s.utf8", lang_code, country_code);
      if (g_hash_table_contains (priv->locales, simple_locale))
        goto free_and_continue;

      /* We are not interested in locales whose name we can't display */
      untranslated_locale = gnome_get_language_from_locale (simple_locale, "C");
      if (!untranslated_locale)
        goto free_and_continue;

      info = g_new0 (LocaleInfo, 1);
      info->id = g_strdup (simple_locale);
      info->name = gnome_get_language_from_locale (simple_locale, NULL);
      info->unaccented_name = cc_util_normalize_casefold_and_unaccent (info->name);
      info->untranslated_name = cc_util_normalize_casefold_and_unaccent (untranslated_locale);
      g_free (untranslated_locale);

      g_hash_table_replace (priv->locales, simple_locale, info);
      add_locale_to_table (priv->locales_by_language, lang_code, info);

      if (gnome_get_input_source_from_locale (simple_locale, &type, &id) &&
          g_str_equal (type, INPUT_SOURCE_TYPE_XKB))
        {
          add_default_widget (detector, info, type, id);
          g_hash_table_add (layouts_with_locale, (gpointer) id);
        }

      /* We don't own these ids */
      info->layout_widgets_by_id = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                          NULL, g_object_unref);
      info->engine_widgets_by_id = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                          NULL, g_object_unref);

      list = gnome_xkb_info_get_layouts_for_language (priv->xkb_info, lang_code);
      add_widgets_to_table (detector, info, list, INPUT_SOURCE_TYPE_XKB, id);
      add_ids_to_set (layouts_with_locale, list);
      g_list_free (list);

      list = gnome_xkb_info_get_layouts_for_country (priv->xkb_info, country_code);
      add_widgets_to_table (detector, info, list, INPUT_SOURCE_TYPE_XKB, id);
      add_ids_to_set (layouts_with_locale, list);
      g_list_free (list);

    free_and_continue:
      g_free (lang_code);
      g_free (country_code);
      g_free (simple_locale);
    }
  g_strfreev (locale_ids);

  /* Add a "Other" locale to hold the remaining input sources */
  info = g_new0 (LocaleInfo, 1);
  info->id = g_strdup ("");
  info->name = g_strdup (_("Other"));
  info->unaccented_name = g_strdup ("");
  info->untranslated_name = g_strdup ("");
  g_hash_table_replace (priv->locales, info->id, info);

  info->layout_widgets_by_id = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      NULL, g_object_unref);
  info->engine_widgets_by_id = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      NULL, g_object_unref);

  list = gnome_xkb_info_get_all_layouts (priv->xkb_info);
  for (l = list; l; l = l->next)
    if (!g_hash_table_contains (layouts_with_locale, l->data))
      add_widget_other (detector, INPUT_SOURCE_TYPE_XKB, l->data);

  g_list_free (list);

  g_hash_table_destroy (layouts_with_locale);
}

static void
cc_keyboard_detector_private_free (gpointer data)
{
  CcKeyboardDetectorPrivate *priv = data;

  g_object_unref (priv->more_item);
  g_object_unref (priv->no_results);
  g_hash_table_destroy (priv->locales);
  g_hash_table_destroy (priv->locales_by_language);
  g_strfreev (priv->filter_words);
  g_free (priv);
}

GtkWidget *
cc_keyboard_detector_new (GtkWindow    *main_window,
                          GnomeXkbInfo *xkb_info,
                          GHashTable   *ibus_engines)
{
  GtkBuilder *builder;
  GtkWidget *detector;
  CcKeyboardDetectorPrivate *priv;
  gint width;
  GError *error = NULL;

  builder = gtk_builder_new ();
  if (gtk_builder_add_from_resource (builder, "/org/gnome/initial-setup/keyboard-detector.ui", &error) == 0)
    {
      g_object_unref (builder);
      g_warning ("failed to load keyboard detector: %s", error->message);
      g_error_free (error);
      return NULL;
    }
  detector = WID ("input-dialog");
  priv = g_new0 (CcKeyboardDetectorPrivate, 1);
  g_object_set_data_full (G_OBJECT (detector), "private", priv, cc_keyboard_detector_private_free);
  g_object_set_data_full (G_OBJECT (detector), "builder", builder, g_object_unref);

  priv->xkb_info = xkb_info;
  priv->ibus_engines = ibus_engines;

  priv->add_button = WID ("add-button");
  priv->filter_entry = WID ("filter-entry");
  priv->list = WID ("list");
  priv->scrolledwindow = WID ("scrolledwindow");
  priv->adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (priv->scrolledwindow));

  priv->more_item = g_object_ref_sink (more_widget_new ());
  priv->no_results = g_object_ref_sink (no_results_widget_new ());

  egg_list_box_set_adjustment (EGG_LIST_BOX (priv->list), priv->adjustment);
  egg_list_box_set_filter_func (EGG_LIST_BOX (priv->list), list_filter, detector, NULL);
  egg_list_box_set_sort_func (EGG_LIST_BOX (priv->list), list_sort, detector, NULL);
  g_signal_connect (priv->list, "child-activated", G_CALLBACK (child_activated), detector);
  g_signal_connect (priv->list, "child-selected", G_CALLBACK (child_selected), detector);
  g_signal_connect_after (priv->list, "refilter", G_CALLBACK (end_refilter), detector);

  g_signal_connect_swapped (priv->filter_entry, "changed", G_CALLBACK (filter_changed), detector);

  get_locale_infos (detector);
#ifdef HAVE_IBUS
  get_ibus_locale_infos (detector);
#endif  /* HAVE_IBUS */
  show_locale_widgets (detector);

  /* Try to come up with a sensible width */
  gtk_window_get_size (main_window, &width, NULL);
  gtk_widget_set_size_request (detector, width * MAIN_WINDOW_WIDTH_RATIO, -1);
  gtk_window_set_resizable (GTK_WINDOW (detector), TRUE);

  gtk_window_set_transient_for (GTK_WINDOW (detector), main_window);

  return detector;
}

void
cc_keyboard_detector_set_ibus_engines (GtkWidget  *detector,
                                       GHashTable *ibus_engines)
{
#ifdef HAVE_IBUS
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);

  /* This should only be called once when IBus shows up in case it
     wasn't up yet when the user opened the keyboard detector dialog. */
  g_return_if_fail (priv->ibus_engines == NULL);

  priv->ibus_engines = ibus_engines;
  get_ibus_locale_infos (detector);
  show_locale_widgets (detector);
#endif  /* HAVE_IBUS */
}

gboolean
cc_keyboard_detector_get_selected (GtkWidget  *detector,
                                   gchar     **type,
                                   gchar     **id,
                                   gchar     **name)
{
  CcKeyboardDetectorPrivate *priv = GET_PRIVATE (detector);
  GtkWidget *selected;
  const gchar *t, *i, *n;

  selected = egg_list_box_get_selected_child (EGG_LIST_BOX (priv->list));
  if (!selected)
    return FALSE;

  t = g_object_get_data (G_OBJECT (selected), "type");
  i = g_object_get_data (G_OBJECT (selected), "id");
  n = g_object_get_data (G_OBJECT (selected), "name");

  if (!t || !i || !n)
    return FALSE;

  *type = g_strdup (t);
  *id = g_strdup (i);
  *name = g_strdup (n);

  return TRUE;
}
