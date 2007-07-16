/*******************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
\********************************************************************/
/** @file gnc-csv-import.c
    @brief CSV Import GUI code
    @author Copyright (c) 2007 Benny Sperisen <lasindi@gmail.com>
*/
#include "config.h"

#include <gtk/gtk.h>
#include <glade/glade.h>
#include <glib/gi18n.h>
#include <goffice/gtk/go-charmap-sel.h>

#include "import-account-matcher.h"
#include "import-main-matcher.h"

#include "gnc-file.h"
#include "gnc-book.h"
#include "gnc-ui-util.h"
#include "gnc-glib-utils.h"
#include "gnc-gui-query.h"
#include "dialog-utils.h"

#include "gnc-csv-import.h"
#include "gnc-csv-model.h"

#define GCONF_SECTION "dialogs/import/csv"

static QofLogModule log_module = GNC_MOD_IMPORT;

/** Enumeration for separator checkbutton types. These are the
 * different types of checkbuttons that the user can click to
 * configure separators in a delimited file. */
enum SEP_BUTTON_TYPES {SEP_SPACE, SEP_TAB, SEP_COMMA, SEP_COLON, SEP_SEMICOLON, SEP_HYPHEN,
                       SEP_NUM_OF_TYPES};

/** Data for the preview dialog. This struct contains all of the data
 * relevant to the preview dialog that lets the user configure an
 * import. */
typedef struct
{
  GncCsvParseData* parse_data; /**< The actual data we are previewing */
  GtkDialog* dialog;
  GOCharmapSel* encselector; /**< The widget for selecting the encoding */
  GtkComboBox* date_format_combo; /**< The widget for selecting the date format */
  GladeXML* xml; /**< The Glade file that contains the dialog. */
  GtkTreeView* treeview; /**< The treeview containing the data */
  GtkTreeView* ctreeview; /**< The treeview containing the column types */
  GtkCheckButton* sep_buttons[SEP_NUM_OF_TYPES]; /**< Checkbuttons for common separators */
  GtkCheckButton* custom_cbutton; /**< The checkbutton for a custom separator */
  GtkEntry* custom_entry; /**< The entry for custom separators */
  gboolean encoding_selected_called; /**< Before encoding_selected is first called, this is FALSE.
                                      * (See description of encoding_selected.) */
  /* TODO We might not need previewing_errors. */
  gboolean previewing_errors; /**< TRUE if the dialog is displaying
                               * error lines, instead of all the file
                               * data. */
  int code_encoding_calls; /**< Normally this is 0. If the computer
                            * changes encselector, this is set to
                            * 2. encoding_selected is called twice,
                            * each time decrementing this by 1. */
  gboolean approved; /**< This is FALSE until the user clicks "OK". */
} GncCsvPreview;

static void gnc_csv_preview_treeview(GncCsvPreview* preview, gboolean notEmpty);

/** Event handler for separator changes. This function is called
 * whenever one of the widgets for configuring the separators (the
 * separator checkbuttons or the custom separator entry) is
 * changed.
 * @param widget The widget that was changed
 * @param preview The data that is being configured
 */
static void sep_button_clicked(GtkWidget* widget, GncCsvPreview* preview)
{
  int i;
  char* stock_separator_characters[] = {" ", "\t", ",", ":", ";", "-"};
  GSList* checked_separators = NULL;
  GError* error;

  /* Add the corresponding characters to checked_separators for each
   * button that is checked. */
  for(i = 0; i < SEP_NUM_OF_TYPES; i++)
  {
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(preview->sep_buttons[i])))
      checked_separators = g_slist_append(checked_separators, stock_separator_characters[i]);
  }

  /* Add the custom separator if the user checked its button. */
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(preview->custom_cbutton)))
  {
    char* custom_sep = (char*)gtk_entry_get_text(preview->custom_entry);
    if(custom_sep[0] != '\0') /* Don't add a blank separator (bad things will happen!). */
      checked_separators = g_slist_append(checked_separators, custom_sep);
  }

  /* Set the parse options using the checked_separators list. */
  stf_parse_options_csv_set_separators(preview->parse_data->options, NULL, checked_separators);
  g_slist_free(checked_separators);

  /* Parse the data using the new options. We don't want to reguess
   * the column types because we want to leave the user's
   * configurations in tact. */
  if(gnc_csv_parse(preview->parse_data, FALSE, &error))
  {
    /* Warn the user there was a problem and try to undo what caused
     * the error. (This will cause a reparsing and ideally a usable
     * configuration.) */
    gnc_error_dialog(NULL, "Error in parsing");
    /* If the user changed the custom separator, erase that custom separator. */
    if(widget == GTK_WIDGET(preview->custom_entry))
    {
      gtk_entry_set_text(GTK_ENTRY(widget), "");
    }
    /* If the user checked a checkbutton, toggle that checkbutton back. */
    else
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),
                                   !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
    }
    return;
  }

  /* If we parsed successfully, redisplay the data. */
  gnc_csv_preview_treeview(preview, TRUE);
}

/** Event handler for a new encoding. This is called when the user
 * selects a new encoding; the data is reparsed and shown to the
 * user.
 * @param selector The widget the user uses to select a new encoding
 * @param encoding The encoding that the user selected
 * @param preview The data that is being configured
 */
static void encoding_selected(GOCharmapSel* selector, const char* encoding,
                              GncCsvPreview* preview)
{
  /* This gets called twice everytime a new encoding is selected. The
   * second call actually passes the correct data; thus, we only do
   * something the second time this is called. */

  /* Prevent code-caused calls of this function from having an impact. */
  if(preview->code_encoding_calls > 0)
  {
    preview->code_encoding_calls--;
    return;
  }

  /* If this is the second time the function is called ... */
  if(preview->encoding_selected_called)
  {
    const char* previous_encoding = preview->parse_data->encoding;
    GError* error = NULL;
    /* Try converting the new encoding and reparsing. */
    if(gnc_csv_convert_encoding(preview->parse_data, encoding, &error) ||
       gnc_csv_parse(preview->parse_data, FALSE, &error))
    {
      /* If it fails, change back to the old encoding. */
      gnc_error_dialog(NULL, _("Invalid encoding selected"));
      preview->encoding_selected_called = FALSE;
      go_charmap_sel_set_encoding(selector, previous_encoding);
      return;
    }

    gnc_csv_preview_treeview(preview, TRUE);
    preview->encoding_selected_called = FALSE;
  }
  else /* If this is the first call of the function ... */
  {
    preview->encoding_selected_called = TRUE; /* ... set the flag and wait for the next call. */
  }
}

/* This array contains all of the different strings for different column types. */
static char* column_type_strs[GNC_CSV_NUM_COL_TYPES] = {N_("None"),
                                                        N_("Date"),
                                                        N_("Description"),
                                                        N_("Amount")};

/** Event handler for selecting a new date format.
 * @param format_selector The combo box for selecting date formats
 * @param preview The data that is being configured
 */
static void date_format_selected(GtkComboBox* format_selector, GncCsvPreview* preview)
{
  preview->parse_data->date_format = gtk_combo_box_get_active(format_selector);
}

/** Event handler for the "OK" button. When "OK" is clicked, this
 * function updates the parse data with the user's column type
 * configuration and closes the preview dialog.
 * @param widget The "OK" button
 * @param preview The data that is being configured
 */
static void ok_button_clicked(GtkWidget* widget, GncCsvPreview* preview)
{
  /* Shorten the column_types identifier. */
  GArray* column_types = preview->parse_data->column_types;
  int i, ncols = column_types->len; /* ncols is the number of columns in the data. */
  /* store contains the actual strings appearing in the column types treeview. */
  GtkTreeModel* store = gtk_tree_view_get_model(preview->ctreeview);
  GtkTreeIter iter;
  /* Get an iterator for the first (and only) row. */
  gtk_tree_model_get_iter_first(store, &iter);

  /* Go through each of the columns. */
  for(i = 0; i < ncols; i++)
  {
    int type; /* The column type contained in this column. */
    gchar* contents; /* The column type string in this column. */
    /* Get the type string first. (store is arranged so that every two
     * columns is a pair of the model used for the combobox and the
     * string that appears, so that store looks like:
     * model 0, string 0, model 1, string 1, ..., model ncols, string ncols. */
    gtk_tree_model_get(store, &iter, 2*i+1, &contents, -1);

    /* Go through each column type until ... */
    for(type = 0; type < GNC_CSV_NUM_COL_TYPES; type++)
    {
      /* ... we find one that matches with what's in the column. */
      if(!strcmp(contents, _(column_type_strs[type])))
      {
        /* Set the column_types array appropriately and quit. */
        column_types->data[i] = type;
        break;
      }
    }
  }

  /* Close the dialog. */
  gtk_widget_hide((GtkWidget*)(preview->dialog));
  preview->approved = TRUE; /* The user has wants to do the import. */
}

/** Event handler for the "Cancel" button. When the user clicks
 * "Cancel", the dialog is simply closed.
 * @param widget The "Cancel" button
 * @param preview The data that is being configured
 */
static void cancel_button_clicked(GtkWidget* widget, GncCsvPreview* preview)
{
  gtk_widget_hide((GtkWidget*)(preview->dialog));
}

/** Event handler for the data treeview being resized. When the data
 * treeview is resized, the column types treeview's columns are also resized to
 * match.
 * @param widget The data treeview
 * @param allocation The size of the data treeview
 * @param preview The data that is being configured
 */
static void treeview_resized(GtkWidget* widget, GtkAllocation* allocation, GncCsvPreview* preview)
{
  /* ncols is the number of columns in the data. */
  int i, ncols = preview->parse_data->column_types->len;

  /* Go through each column except for the last. (We don't want to set
   * the width of the last column because the user won't be able to
   * shrink the dialog back if it's expanded.) */
  for(i = 0; i < ncols - 1; i++)
  {
    gint col_width; /* The width of the column in preview->treeview. */
    GtkTreeViewColumn* ccol; /* The corresponding column in preview->ctreeview. */

    /* Get the width. */
    col_width = gtk_tree_view_column_get_width(gtk_tree_view_get_column(preview->treeview, i));

    /* Set ccol's width the same. */
    ccol = gtk_tree_view_get_column(preview->ctreeview, i);
    gtk_tree_view_column_set_min_width(ccol, col_width);
    gtk_tree_view_column_set_max_width(ccol, col_width);
  }
}

/** Event handler for the user selecting a new column type. When the
 * user selects a new column type, that column's text must be changed
 * to that selection, and any other columns containing that selection
 * must be changed to "None" because we don't allow duplicates.
 * @param renderer The renderer of the column the user changed
 * @param path There is only 1 row in preview->ctreeview, so this is always 0.
 * @param new_text The text the user selected
 * @param preview The data that is being configured
 */
static void column_type_edited(GtkCellRenderer* renderer, gchar* path,
                               gchar* new_text, GncCsvPreview* preview)
{
  /* ncols is the number of columns in the data. */
  int i, ncols = preview->parse_data->column_types->len;
  /* store has the actual strings that appear in preview->ctreeview. */
  GtkTreeModel* store = gtk_tree_view_get_model(preview->ctreeview);
  GtkTreeIter iter;
  /* Get an iterator for the first (and only) row. */
  gtk_tree_model_get_iter_first(store, &iter);

  /* Go through each column. */
  for(i = 0; i < ncols; i++)
  {
    /* We need all this stuff so that we can find out whether or not
     * this was the column that was edited. */
    GtkCellRenderer* col_renderer; /* The renderer for this column. */
    /* The column in the treeview we are looking at */
    GtkTreeViewColumn* col = gtk_tree_view_get_column(preview->ctreeview, i);
    /* The list of renderers for col */
    GList* rend_list = gtk_tree_view_column_get_cell_renderers(col);
    /* rend_list has only one entry, which we put in col_renderer. */
    col_renderer = rend_list->data;
    g_list_free(rend_list);

    /* If this is not the column that was edited ... */
    if(col_renderer != renderer)
    {
      /* The string that appears in the column */
      gchar* contents;
      /* Get the type string. (store is arranged so that every two
       * columns is a pair of the model used for the combobox and the
       * string that appears, so that store looks like:
       * model 0, string 0, model 1, string 1, ..., model ncols, string ncols. */
      gtk_tree_model_get(store, &iter, 2*i+1, &contents, -1);
      /* If this column has the same string that the user selected ... */
      if(!strcmp(contents, new_text))
      {
        /* ... set this column to the "None" type. (We can't allow duplicate types.) */
        gtk_list_store_set(GTK_LIST_STORE(store), &iter, 2*i+1,
                           _(column_type_strs[GNC_CSV_NONE]), -1);
      }
    }
    else /* If this is the column that was edited ... */
    {
      /* Set the text for this column to what the user selected. (See
       * comment above "Get the type string. ..." for why we set
       * column 2*i+1 in store.) */
      gtk_list_store_set(GTK_LIST_STORE(store), &iter, 2*i+1, new_text, -1);
    }
  }
}

/** Constructor for GncCsvPreview.
 * @return A new GncCsvPreview* ready for use.
 */
static GncCsvPreview* gnc_csv_preview_new()
{
  int i;
  GncCsvPreview* preview = g_new(GncCsvPreview, 1);
  GtkWidget *ok_button, *cancel_button;
  GtkContainer* date_format_container;
  /* The names in the glade file for the sep buttons. */
  char* sep_button_names[] = {"space_cbutton",
                              "tab_cbutton",
                              "comma_cbutton",
                              "colon_cbutton",
                              "semicolon_cbutton",
                              "hyphen_cbutton"};
  /* The table containing preview->encselector and the separator configuration widgets */
  GtkTable* enctable;

  preview->encselector = GO_CHARMAP_SEL(go_charmap_sel_new(GO_CHARMAP_SEL_TO_UTF8));
  /* Connect the selector to the encoding_selected event handler. */
  g_signal_connect(G_OBJECT(preview->encselector), "charmap_changed",
                   G_CALLBACK(encoding_selected), (gpointer)preview);

  /* Load the Glade file. */
  preview->xml = gnc_glade_xml_new("gnc-csv-preview-dialog.glade", "dialog");
  /* Load the dialog. */
  preview->dialog = GTK_DIALOG(glade_xml_get_widget(preview->xml, "dialog"));

  /* Load the separator buttons from the glade file into the
   * preview->sep_buttons array. */
  for(i = 0; i < SEP_NUM_OF_TYPES; i++)
  {
    preview->sep_buttons[i]
      = (GtkCheckButton*)glade_xml_get_widget(preview->xml, sep_button_names[i]);
    /* Connect them to the sep_button_clicked event handler. */
    g_signal_connect(G_OBJECT(preview->sep_buttons[i]), "toggled",
                     G_CALLBACK(sep_button_clicked), (gpointer)preview);
  }

  /* Load and connect the custom separator checkbutton in the same way
   * as the other separator buttons. */
  preview->custom_cbutton
    = (GtkCheckButton*)glade_xml_get_widget(preview->xml, "custom_cbutton");
  g_signal_connect(G_OBJECT(preview->custom_cbutton), "clicked",
                   G_CALLBACK(sep_button_clicked), (gpointer)preview);

  /* Load the entry for the custom separator entry. Connect it to the
   * sep_button_clicked event handler as well. */
  preview->custom_entry = (GtkEntry*)glade_xml_get_widget(preview->xml, "custom_entry");
  g_signal_connect(G_OBJECT(preview->custom_entry), "changed",
                   G_CALLBACK(sep_button_clicked), (gpointer)preview);

  /* Get the table from the Glade file. */
  enctable = GTK_TABLE(glade_xml_get_widget(preview->xml, "enctable"));
  /* Put the selector in at the top. */
  gtk_table_attach_defaults(enctable, GTK_WIDGET(preview->encselector), 1, 2, 0, 1);
  /* Show the table in all its glory. */
  gtk_widget_show_all(GTK_WIDGET(enctable));

  /* Add in the date format combo box and hook it up to an event handler. */
  preview->date_format_combo = GTK_COMBO_BOX(gtk_combo_box_new_text());
  for(i = 0; i < num_date_formats; i++)
  {
    gtk_combo_box_append_text(preview->date_format_combo, _(date_format_user[i]));
  }
  gtk_combo_box_set_active(preview->date_format_combo, 0);
  g_signal_connect(G_OBJECT(preview->date_format_combo), "changed",
                   G_CALLBACK(date_format_selected), (gpointer)preview);

  /* Add it to the dialog. */
  date_format_container = GTK_CONTAINER(glade_xml_get_widget(preview->xml,
                                                             "date_format_container"));
  gtk_container_add(date_format_container, GTK_WIDGET(preview->date_format_combo));
  gtk_widget_show_all(GTK_WIDGET(date_format_container));

  /* Connect the "OK" and "Cancel" buttons to their event handlers. */
  ok_button = glade_xml_get_widget(preview->xml, "ok_button");
  g_signal_connect(G_OBJECT(ok_button), "clicked",
                   G_CALLBACK(ok_button_clicked), (gpointer)preview);

  cancel_button = glade_xml_get_widget(preview->xml, "cancel_button");
  g_signal_connect(G_OBJECT(cancel_button), "clicked",
                   G_CALLBACK(cancel_button_clicked), (gpointer)preview);

  /* Load the data treeview and connect it to its resizing event handler. */
  preview->treeview = (GtkTreeView*)(glade_xml_get_widget(preview->xml, "treeview"));
  g_signal_connect(G_OBJECT(preview->treeview), "size-allocate",
                   G_CALLBACK(treeview_resized), (gpointer)preview);

  /* Load the column type treeview. */
  preview->ctreeview = (GtkTreeView*)(glade_xml_get_widget(preview->xml, "ctreeview"));

  /* This is TRUE only after encoding_selected is called, so we must
   * set it initially to FALSE. */
  preview->encoding_selected_called = FALSE;

  return preview;
}

/** Destructor for GncCsvPreview. This does not free
 * preview->parse_data, which must be freed separately.
 * @param preview The preview whose memory is freed.
 */
static void gnc_csv_preview_free(GncCsvPreview* preview)
{
  g_object_unref(preview->xml);
  g_free(preview);
}

/* This function loads the preview's data (preview->parse_data) into
 * its data treeview. notEmpty is TRUE when the data treeview already
 * contains data, FALSE otherwise (e.g. the first time this function
 * is called on a preview). */
/* TODO Look at getting rid of notEmpty */
static void gnc_csv_preview_treeview(GncCsvPreview* preview, gboolean notEmpty)
{
  /* store has the data from the file being imported. cstores is an
   * array of stores that hold the combo box entries for each
   * column. ctstore contains both pointers to models in cstore and
   * the actual text that appears in preview->ctreeview. */
  GtkListStore *store, **cstores, *ctstore;
  GtkTreeIter iter;
  /* ncols is the number of columns in the file data. */
  int i, j, ncols = preview->parse_data->column_types->len;

  /* store contains only strings. */
  GType* types = g_new(GType, 2 * ncols);
  for(i = 0; i < ncols; i++)
    types[i] = G_TYPE_STRING;
  store = gtk_list_store_newv(ncols, types);

  /* ctstore is arranged as follows:
   * model 0, text 0, model 1, text 1, ..., model ncols, text ncols. */
  for(i = 0; i < 2*ncols; i += 2)
  {
    types[i] = GTK_TYPE_TREE_MODEL;
    types[i+1] = G_TYPE_STRING;
  }
  ctstore = gtk_list_store_newv(2*ncols, types);

  g_free(types);

  /* Each element in cstores is a single column model. */
  cstores = g_new(GtkListStore*, ncols);
  for(i = 0; i < ncols; i++)
  {
    cstores[i] = gtk_list_store_new(1, G_TYPE_STRING);
    /* Add all of the possible entries to the combo box. */
    for(j = 0; j < GNC_CSV_NUM_COL_TYPES; j++)
    {
      gtk_list_store_append(cstores[i], &iter);
      gtk_list_store_set(cstores[i], &iter, 0, _(column_type_strs[j]), -1);
    }
  }

  if(notEmpty)
  {
    GList *children, *children_begin;
    GList *tv_columns, *tv_columns_begin, *ctv_columns, *ctv_columns_begin;
    tv_columns = tv_columns_begin = gtk_tree_view_get_columns(preview->treeview);
    ctv_columns = ctv_columns_begin = gtk_tree_view_get_columns(preview->ctreeview);
    /* Clear out exisiting columns in preview->treeview. */
    while(tv_columns != NULL)
    {
      gtk_tree_view_remove_column(preview->treeview, GTK_TREE_VIEW_COLUMN(tv_columns->data));
      tv_columns = g_list_next(tv_columns);
    }
    /* Do the same in preview->ctreeview. */
    while(ctv_columns != NULL)
    {
      gtk_tree_view_remove_column(preview->ctreeview, GTK_TREE_VIEW_COLUMN(ctv_columns->data));
      ctv_columns = g_list_next(ctv_columns);
    }
    g_list_free(tv_columns_begin);
    g_list_free(ctv_columns_begin);
  }
  
  /* Fill the data treeview with data from the file. */
  if(preview->previewing_errors) /* If we are showing only errors ... */
  {
    /* ... only pick rows that are in preview->error_lines. */
    GList* error_lines = preview->parse_data->error_lines;
    while(error_lines != NULL)
    {
      i = GPOINTER_TO_INT(error_lines->data);
      gtk_list_store_append(store, &iter);
      for(j = 0; j < ((GPtrArray*)(preview->parse_data->orig_lines->pdata[i]))->len; j++)
      {
        gtk_list_store_set(store, &iter, j,
                           ((GPtrArray*)(preview->parse_data->orig_lines->pdata[i]))->pdata[j],
                           -1);
      }
      error_lines = g_list_next(error_lines);
    }
  }
  else /* Otherwise, put in all of the data. */
  {
    for(i = 0; i < preview->parse_data->orig_lines->len; i++)
    {
      gtk_list_store_append(store, &iter);
      for(j = 0; j < ((GPtrArray*)(preview->parse_data->orig_lines->pdata[i]))->len; j++)
      {
        gtk_list_store_set(store, &iter, j,
                           ((GPtrArray*)(preview->parse_data->orig_lines->pdata[i]))->pdata[j],
                           -1);
      }
    }
  }
  /* Set all the column types to what's in the parse data. */
  gtk_list_store_append(ctstore, &iter);
  for(i = 0; i < ncols; i++)
  {
    gtk_list_store_set(ctstore, &iter, 2*i, cstores[i], 2*i+1,
                       _(column_type_strs[(int)(preview->parse_data->column_types->data[i])]),
                       -1);
  }

  /* Insert columns into the data and column type treeviews. */
  for(i = 0; i < ncols; i++)
  {
    /* Create renderers for the data treeview (renderer) and the
     * column type treeview (crenderer). */
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new(),
      *crenderer = gtk_cell_renderer_combo_new();
    /* We are using cstores for the combo box entries, and we don't
     * want the user to be able to manually enter their own column
     * types. */
    g_object_set(G_OBJECT(crenderer), "model", cstores[i], "text-column", 0,
                 "editable", TRUE, "has-entry", FALSE, NULL);
    g_signal_connect(G_OBJECT(crenderer), "edited",
                     G_CALLBACK(column_type_edited), (gpointer)preview);

    /* Add a single column for the treeview. */
    gtk_tree_view_insert_column_with_attributes(preview->treeview,
                                                -1, "", renderer, "text", i, NULL);
    /* Use the alternating model and text entries from ctstore in
     * preview->ctreeview. */
    gtk_tree_view_insert_column_with_attributes(preview->ctreeview,
                                                -1, "", crenderer, "model", 2*i,
                                                "text", 2*i+1, NULL);
  }

  /* Set the treeviews to use the models. */
  gtk_tree_view_set_model(preview->treeview, GTK_TREE_MODEL(store));
  gtk_tree_view_set_model(preview->ctreeview, GTK_TREE_MODEL(ctstore));

  /* Free the memory for the stores. */
  g_object_unref(GTK_TREE_MODEL(store));
  g_object_unref(GTK_TREE_MODEL(ctstore));
  for(i = 0; i < ncols; i++)
    g_object_unref(GTK_TREE_MODEL(cstores[i]));

  /* Make the things actually appear. */
  gtk_widget_show_all(GTK_WIDGET(preview->treeview));
  gtk_widget_show_all(GTK_WIDGET(preview->ctreeview));

  /* Set the encoding selector to the right encoding. */
  preview->code_encoding_calls = 2;
  go_charmap_sel_set_encoding(preview->encselector, preview->parse_data->encoding);

  /* Set the date format to what's in the combo box (since we don't
   * necessarily know if this will always be the same). */
  preview->parse_data->date_format = gtk_combo_box_get_active(preview->date_format_combo);
}

/** A function that lets the user preview a file's data. This function
 * is used to let the user preview and configure the data parsed from
 * the file. It doesn't return until the user clicks "OK" or "Cancel"
 * on the dialog.
 * @param preview The GUI for previewing the data
 * @param parse_data The data we want to preview
 * @return 0 if the user approved the import; 1 if the user didn't.
 */
static int gnc_csv_preview(GncCsvPreview* preview, GncCsvParseData* parse_data)
{
  /* Set the preview's parse_data to the one we're getting passed. */
  preview->parse_data = parse_data;
  preview->previewing_errors = FALSE; /* We're looking at all the data. */
  preview->approved = FALSE; /* This is FALSE until the user clicks "OK". */

  /* Load the data into the treeview. (This is the first time we've
   * called gnc_csv_preview_treeview on this preview, so we use
   * FALSE. */
  gnc_csv_preview_treeview(preview, FALSE);
  /* Wait until the user clicks "OK" or "Cancel". */
  gtk_dialog_run(GTK_DIALOG(preview->dialog));

  if(preview->approved)
    return 0;
  else
    return 1;
}

/** A function that lets the user preview rows with errors. This
 * function must only be called after calling gnc_csv_preview. It is
 * essentially identical in behavior to gnc_csv_preview except that it
 * displays lines with errors instead of all of the data.
 * @param preview The GUI for previewing the data (and the data being previewed)
 * @return 0 if the user approved of importing the lines; 1 if the user didn't.
 */
/* TODO Let the user manually edit cells' data? */
static int gnc_csv_preview_errors(GncCsvPreview* preview)
{
  GtkLabel* instructions_label = GTK_LABEL(glade_xml_get_widget(preview->xml, "instructions_label"));
  GtkImage* instructions_image = GTK_IMAGE(glade_xml_get_widget(preview->xml, "instructions_image"));
  gchar* name;
  GtkIconSize size;
  gtk_image_get_stock(instructions_image, &name, &size);
  gtk_image_set_from_stock(instructions_image, GTK_STOCK_DIALOG_ERROR, size);
  gtk_label_set_text(instructions_label,
                     _("The rows displayed below had errors. You can attempt to correct these errors by changing the configuration."));
  gtk_widget_show(GTK_WIDGET(instructions_image));
  gtk_widget_show(GTK_WIDGET(instructions_label));

  /* TODO Implement */
  preview->previewing_errors = TRUE;
  preview->approved = FALSE; /* This is FALSE until the user clicks "OK". */

  /* Wait until the user clicks "OK" or "Cancel". */
  gnc_csv_preview_treeview(preview, TRUE);
  gtk_dialog_run(GTK_DIALOG(preview->dialog));
  
  if(preview->approved)
    return 0;
  else
    return 1;
}

/** Lets the user import a CSV/Fixed-Width file. */
void gnc_file_csv_import(void)
{
  /* The name of the file the user selected. */
  char* selected_filename;
   /* The default directory for the user to select files. */
  char* default_dir= gnc_get_default_directory(GCONF_SECTION);
  /* The generic GUI for importing transactions. */
  GNCImportMainMatcher* gnc_csv_importer_gui = NULL;

  /* Let the user select a file. */
  selected_filename = gnc_file_dialog(_("Select an CSV/Fixed-Width file to import"),
				      NULL, default_dir, GNC_FILE_DIALOG_IMPORT);
  g_free(default_dir); /* We don't need default_dir anymore. */

  /* If the user actually selected a file ... */
  if(selected_filename!=NULL)
  {
    int i, user_canceled = 0;
    Account* account; /* The account the user will select */
    GError* error = NULL;
    GList* transactions; /* A list of the transactions we create */
    GncCsvParseData* parse_data;
    GncCsvPreview* preview;
    
    /* Remember the directory of the selected file as the default. */
    default_dir = g_path_get_dirname(selected_filename);
    gnc_set_default_directory(GCONF_SECTION, default_dir);
    g_free(default_dir);

    /* Load the file into parse_data. */
    parse_data = gnc_csv_new_parse_data();
    if(gnc_csv_load_file(parse_data, selected_filename, &error))
    {
      /* If we couldn't load the file ... */
      gnc_error_dialog(NULL, "%s", error->message);
      if(error->code == GNC_CSV_FILE_OPEN_ERR)
      {
        gnc_csv_parse_data_free(parse_data);
        g_free(selected_filename);
        return;
      }
      /* If we couldn't guess the encoding, we are content with just
       * displaying an error message and move on with a blank
       * display. */
    }
    /* Parse the data. */
    if(gnc_csv_parse(parse_data, TRUE, &error))
    {
      /* If we couldn't parse the data ... */
      gnc_error_dialog(NULL, "%s", error->message);
    }

    /* Preview the data. */
    preview = gnc_csv_preview_new();
    if(gnc_csv_preview(preview, parse_data))
    {
      /* If the user clicked "Cancel", free everything and quit. */
      gnc_csv_preview_free(preview);
      gnc_csv_parse_data_free(parse_data);
      g_free(selected_filename);
      return;
    }

    /* Let the user select an account to put the transactions in. */
    account = gnc_import_select_account(NULL, NULL, 1, NULL, NULL, 0, NULL, NULL);
    if(account == NULL) /* Quit if the user canceled. */
    {
      gnc_csv_preview_free(preview);
      gnc_csv_parse_data_free(parse_data);
      g_free(selected_filename);
      return;
    }

    /* Create transactions from the parsed data. */
    gnc_parse_to_trans(parse_data, account, FALSE);

    /* If there are errors, let the user try and eliminate them by
     * previewing them. Repeat until either there are no errors or the
     * user gives up. */
    while(!((parse_data->error_lines == NULL) || user_canceled))
    {
      user_canceled = gnc_csv_preview_errors(preview);
      gnc_parse_to_trans(parse_data, account, TRUE);
    }

    /* Create the genereic transaction importer GUI. */
    gnc_csv_importer_gui = gnc_gen_trans_list_new(NULL, NULL, FALSE, 42);

    /* Get the list of the transactions that were created. */
    transactions = parse_data->transactions;
    /* Copy all of the transactions to the importer GUI. */
    while(transactions != NULL)
    {
      GncCsvTransLine* trans_line = transactions->data;
      gnc_gen_trans_list_add_trans(gnc_csv_importer_gui,
                                   trans_line->trans);
      transactions = g_list_next(transactions);
    }
    /* Let the user load those transactions into the account, so long
     * as there is at least one transaction to be loaded. */
    if(parse_data->transactions != NULL)
      gnc_gen_trans_list_run(gnc_csv_importer_gui);
    else
      gnc_gen_trans_list_delete(gnc_csv_importer_gui);

    /* Free the memory we allocated. */
    gnc_csv_preview_free(preview);
    gnc_csv_parse_data_free(parse_data);
    g_free(selected_filename);
  }
}
