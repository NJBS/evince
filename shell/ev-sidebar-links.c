/* this file is part of evince, a gnome document viewer
 *
 *  Copyright (C) 2004 Red Hat, Inc.
 *
 *  Author:
 *    Jonathan Blandford <jrb@alum.mit.edu>
 *
 * Evince is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Evince is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "ev-sidebar-page.h"
#include "ev-sidebar-links.h"
#include "ev-job-queue.h"
#include "ev-document-links.h"
#include "ev-window.h"
#include "ev-gui.h"

struct _EvSidebarLinksPrivate {
	GtkWidget *tree_view;

	/* Keep these ids around for blocking */
	guint selection_id;
	guint page_changed_id;
	guint row_activated_id;

	EvJob *job;
	GtkTreeModel *model;
	EvDocument *document;
	EvPageCache *page_cache;
};

enum {
	PROP_0,
	PROP_MODEL,
	PROP_WIDGET,
};


static void links_page_num_func				(GtkTreeViewColumn *tree_column,
							 GtkCellRenderer   *cell,
							 GtkTreeModel      *tree_model,
							 GtkTreeIter       *iter,
							 EvSidebarLinks    *sidebar_links);
static void update_page_callback 			(EvPageCache       *page_cache,
							 gint               current_page,
						         EvSidebarLinks    *sidebar_links);
static void row_activated_callback 			(GtkTreeView *treeview,
		                                         GtkTreePath *arg1,
	                                                 GtkTreeViewColumn *arg2,
		                                         gpointer user_data);
static void job_finished_callback 			(EvJobLinks     *job,
				    		         EvSidebarLinks *sidebar_links);
static void ev_sidebar_links_page_iface_init 		(EvSidebarPageIface *iface);
static void ev_sidebar_links_set_document      	 	(EvSidebarPage  *sidebar_page,
		    			        	 EvDocument     *document);
static gboolean ev_sidebar_links_support_document	(EvSidebarPage  *sidebar_page,
						         EvDocument     *document);
static const gchar* ev_sidebar_links_get_label 		(EvSidebarPage *sidebar_page);


G_DEFINE_TYPE_EXTENDED (EvSidebarLinks, 
                        ev_sidebar_links, 
                        GTK_TYPE_VBOX,
                        0, 
                        G_IMPLEMENT_INTERFACE (EV_TYPE_SIDEBAR_PAGE, 
					       ev_sidebar_links_page_iface_init))


#define EV_SIDEBAR_LINKS_GET_PRIVATE(object) \
	(G_TYPE_INSTANCE_GET_PRIVATE ((object), EV_TYPE_SIDEBAR_LINKS, EvSidebarLinksPrivate))

static void
ev_sidebar_links_set_property (GObject      *object,
			       guint         prop_id,
			       const GValue *value,
			       GParamSpec   *pspec)
{
	EvSidebarLinks *ev_sidebar_links;
	GtkTreeModel *model;
  
	ev_sidebar_links = EV_SIDEBAR_LINKS (object);

	switch (prop_id)
	{
	case PROP_MODEL:
		model = ev_sidebar_links->priv->model;
		ev_sidebar_links->priv->model = GTK_TREE_MODEL (g_value_dup_object (value));
		if (model)
			g_object_unref (model);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
ev_sidebar_links_get_property (GObject    *object,
			       guint       prop_id,
			       GValue     *value,
			       GParamSpec *pspec)
{
	EvSidebarLinks *ev_sidebar_links;
  
	ev_sidebar_links = EV_SIDEBAR_LINKS (object);

	switch (prop_id)
	{
	case PROP_MODEL:
		g_value_set_object (value, ev_sidebar_links->priv->model);
		break;
	case PROP_WIDGET:
		g_value_set_object (value, ev_sidebar_links->priv->tree_view);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
ev_sidebar_links_dispose (GObject *object)
{
	EvSidebarLinks *sidebar = EV_SIDEBAR_LINKS (object);

	if (sidebar->priv->job) {
		g_signal_handlers_disconnect_by_func (sidebar->priv->job,
						      job_finished_callback, sidebar);
		ev_job_queue_remove_job (sidebar->priv->job);						      
		g_object_unref (sidebar->priv->job);
		sidebar->priv->job = NULL;
	}

	if (sidebar->priv->model) {
		g_object_unref (sidebar->priv->model);
		sidebar->priv->model = NULL;
	}

	if (sidebar->priv->document) {
		g_object_unref (sidebar->priv->document);
		sidebar->priv->document = NULL;
		sidebar->priv->page_cache = NULL;
	}


	G_OBJECT_CLASS (ev_sidebar_links_parent_class)->dispose (object);
}

static void
ev_sidebar_links_class_init (EvSidebarLinksClass *ev_sidebar_links_class)
{
	GObjectClass *g_object_class;

	g_object_class = G_OBJECT_CLASS (ev_sidebar_links_class);

	g_object_class->set_property = ev_sidebar_links_set_property;
	g_object_class->get_property = ev_sidebar_links_get_property;
	g_object_class->dispose = ev_sidebar_links_dispose;

	g_object_class_install_property (g_object_class,
					 PROP_MODEL,
					 g_param_spec_object ("model",
							      "Model",
							      "Current Model",
							      GTK_TYPE_TREE_MODEL,
							      G_PARAM_READWRITE));
	g_object_class_override_property (g_object_class,
					  PROP_WIDGET,
					  "main-widget");

	g_type_class_add_private (g_object_class, sizeof (EvSidebarLinksPrivate));
}

static void
selection_changed_callback (GtkTreeSelection   *selection,
		            EvSidebarLinks     *ev_sidebar_links)
{
	EvDocument *document;
	GtkTreeModel *model;
	GtkTreeIter iter;

	g_return_if_fail (EV_IS_SIDEBAR_LINKS (ev_sidebar_links));

	document = EV_DOCUMENT (ev_sidebar_links->priv->document);
	g_return_if_fail (ev_sidebar_links->priv->document != NULL);

	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
		EvLink *link;

		gtk_tree_model_get (model, &iter,
				    EV_DOCUMENT_LINKS_COLUMN_LINK, &link,
				    -1);
		
		if (link == NULL)
			return;

		g_signal_handler_block (ev_sidebar_links->priv->page_cache,
					ev_sidebar_links->priv->page_changed_id);
		/* FIXME: we should handle this better.  This breaks w/ URLs */
		ev_page_cache_set_link (ev_sidebar_links->priv->page_cache, link);
		g_signal_handler_unblock (ev_sidebar_links->priv->page_cache,
					  ev_sidebar_links->priv->page_changed_id);

		g_object_unref (link);
	}
}

static GtkTreeModel *
create_loading_model (void)
{
	GtkTreeModel *retval;
	GtkTreeIter iter;
	gchar *markup;

	/* Creates a fake model to indicate that we're loading */
	retval = (GtkTreeModel *)gtk_list_store_new (EV_DOCUMENT_LINKS_COLUMN_NUM_COLUMNS,
						     G_TYPE_STRING,
						     G_TYPE_OBJECT,
						     G_TYPE_BOOLEAN);

	gtk_list_store_append (GTK_LIST_STORE (retval), &iter);
	markup = g_strdup_printf ("<span size=\"larger\" style=\"italic\">%s</span>", _("Loading..."));
	gtk_list_store_set (GTK_LIST_STORE (retval), &iter,
			    EV_DOCUMENT_LINKS_COLUMN_MARKUP, markup,
			    EV_DOCUMENT_LINKS_COLUMN_EXPAND, FALSE,
			    EV_DOCUMENT_LINKS_COLUMN_LINK, NULL,
			    -1);
	g_free (markup);

	return retval;
}

static void
print_section_cb (GtkWidget *menuitem, EvSidebarLinks *sidebar)
{
	GtkWidget *window;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GtkTreeIter iter;

	selection = gtk_tree_view_get_selection
		(GTK_TREE_VIEW (sidebar->priv->tree_view));

	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
		EvLink *link;
		int first_page, last_page;

		gtk_tree_model_get (model, &iter,
				    EV_DOCUMENT_LINKS_COLUMN_LINK, &link,
				    -1);
		first_page = ev_link_get_page (link) + 1;

		if (link)
			g_object_unref (link);

		if (gtk_tree_model_iter_next (model, &iter)) {
			gtk_tree_model_get (model, &iter,
					    EV_DOCUMENT_LINKS_COLUMN_LINK, &link,
					    -1);
			last_page = ev_link_get_page (link);

			if (link)
		    		g_object_unref (link);
		} else {
			last_page = -1;
		}
	
		window = gtk_widget_get_toplevel (GTK_WIDGET (sidebar));
		if (EV_IS_WINDOW (window)) {
			ev_window_print_range (EV_WINDOW (window),
					       first_page, last_page);
		}
	}
}

static GtkMenu *
build_popup_menu (EvSidebarLinks *sidebar)
{
	GtkWidget *menu;
	GtkWidget *item;

	menu = gtk_menu_new ();
	item = gtk_image_menu_item_new_from_stock (GTK_STOCK_PRINT, NULL);
	gtk_label_set_label (GTK_LABEL (GTK_BIN (item)->child), _("Print..."));
	gtk_widget_show (item);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (item, "activate",
			  G_CALLBACK (print_section_cb), sidebar);

	return GTK_MENU (menu);
}

static void
popup_menu_cb (GtkWidget *treeview, EvSidebarLinks *sidebar)
{
	GtkMenu *menu = build_popup_menu (sidebar);

	gtk_menu_popup (menu, NULL, NULL,
			ev_gui_menu_position_tree_selection,
			sidebar->priv->tree_view, 0,
			gtk_get_current_event_time ());
	gtk_menu_shell_select_first (GTK_MENU_SHELL (menu), FALSE);
}

static gboolean
button_press_cb (GtkWidget *treeview,
                 GdkEventButton *event,
                 EvSidebarLinks *sidebar)
{
	GtkTreePath *path = NULL;

	if (event->button == 3) {
	        if (gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (treeview),
        	                                   event->x,
                	                           event->y,
	                                           &path,
        	                                   NULL, NULL, NULL)) {
			gtk_tree_view_set_cursor (GTK_TREE_VIEW (treeview),
						  path, NULL, FALSE);
			gtk_menu_popup (build_popup_menu (sidebar), NULL,
					NULL, NULL, NULL, event->button,
					gtk_get_current_event_time ());
			gtk_tree_path_free (path);

			return TRUE;
		}
	}

	return FALSE;
}


static void
ev_sidebar_links_construct (EvSidebarLinks *ev_sidebar_links)
{
	EvSidebarLinksPrivate *priv;
	GtkWidget *swindow;
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	GtkTreeSelection *selection;
	GtkTreeModel *loading_model;

	priv = ev_sidebar_links->priv;

	swindow = gtk_scrolled_window_new (NULL, NULL);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (swindow),
					GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (swindow),
					     GTK_SHADOW_IN);

	/* Create tree view */
	loading_model = create_loading_model ();
	priv->tree_view = gtk_tree_view_new_with_model (loading_model);
	g_object_unref (loading_model);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->tree_view));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_NONE);
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (priv->tree_view), FALSE);
	gtk_container_add (GTK_CONTAINER (swindow), priv->tree_view);

	gtk_box_pack_start (GTK_BOX (ev_sidebar_links), swindow, TRUE, TRUE, 0);
	gtk_widget_show_all (GTK_WIDGET (ev_sidebar_links));

	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_expand (GTK_TREE_VIEW_COLUMN (column), TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (priv->tree_view), column);

	renderer = (GtkCellRenderer*)
		g_object_new (GTK_TYPE_CELL_RENDERER_TEXT,
			      "ellipsize", PANGO_ELLIPSIZE_END,
			      NULL);
	gtk_tree_view_column_pack_start (GTK_TREE_VIEW_COLUMN (column), renderer, TRUE);
	gtk_tree_view_column_set_attributes (GTK_TREE_VIEW_COLUMN (column), renderer,
					     "markup", EV_DOCUMENT_LINKS_COLUMN_MARKUP,
					     NULL);

	
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_end (GTK_TREE_VIEW_COLUMN (column), renderer, FALSE);
	gtk_tree_view_column_set_cell_data_func (GTK_TREE_VIEW_COLUMN (column), renderer,
						 (GtkTreeCellDataFunc) links_page_num_func,
						 ev_sidebar_links, NULL);

	g_signal_connect (GTK_TREE_VIEW (priv->tree_view),
			  "button_press_event",
			  G_CALLBACK (button_press_cb),
			  ev_sidebar_links);
	g_signal_connect (GTK_TREE_VIEW (priv->tree_view),
			  "popup_menu",
			  G_CALLBACK (popup_menu_cb),
			  ev_sidebar_links);
}

static void
ev_sidebar_links_init (EvSidebarLinks *ev_sidebar_links)
{
	ev_sidebar_links->priv = EV_SIDEBAR_LINKS_GET_PRIVATE (ev_sidebar_links);

	ev_sidebar_links_construct (ev_sidebar_links);
}

static void
links_page_num_func (GtkTreeViewColumn *tree_column,
		     GtkCellRenderer   *cell,
		     GtkTreeModel      *tree_model,
		     GtkTreeIter       *iter,
		     EvSidebarLinks    *sidebar_links)
{
	EvLink *link;

	gtk_tree_model_get (tree_model, iter,
			    EV_DOCUMENT_LINKS_COLUMN_LINK, &link,
			    -1);
	
	if (link != NULL &&
	    ev_link_get_link_type (link) == EV_LINK_TYPE_PAGE) {
		gchar *page_label;
		gchar *page_string;

		page_label = ev_page_cache_get_page_label (sidebar_links->priv->page_cache, ev_link_get_page (link));
		page_string = g_markup_printf_escaped ("<i>%s</i>", page_label);

		g_object_set (cell,
 			      "markup", page_string,
			      "visible", TRUE,
			      NULL);

		g_free (page_label);
		g_free (page_string);
	} else {
		g_object_set (cell,
			      "visible", FALSE,
			      NULL);
	}

	if (link)
		g_object_unref (link);
}

/* Public Functions */

GtkWidget *
ev_sidebar_links_new (void)
{
	GtkWidget *ev_sidebar_links;

	ev_sidebar_links = g_object_new (EV_TYPE_SIDEBAR_LINKS, NULL);

	return ev_sidebar_links;
}

static gboolean
update_page_callback_foreach (GtkTreeModel *model,
			      GtkTreePath  *path,
			      GtkTreeIter  *iter,
			      gpointer      data)
{
	EvSidebarLinks *sidebar_links = (data);
	EvLink *link;

	gtk_tree_model_get (model, iter,
			    EV_DOCUMENT_LINKS_COLUMN_LINK, &link,
			    -1);

	if (link && ev_link_get_link_type (link) == EV_LINK_TYPE_PAGE) {
		int current_page;

		current_page = ev_page_cache_get_current_page (sidebar_links->priv->page_cache);
		if (ev_link_get_page (link) == current_page) {
			GtkTreeSelection *selection;

			selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (sidebar_links->priv->tree_view));

			gtk_tree_selection_select_path (selection, path);
	
			g_object_unref (link);
			return TRUE;
		}
	}

	if (link)
		g_object_unref (link);
	
	return FALSE;
}

static void
update_page_callback (EvPageCache    *page_cache,
		      gint            current_page,
		      EvSidebarLinks *sidebar_links)
{
	GtkTreeSelection *selection;
	/* We go through the tree linearly looking for the first page that
	 * matches.  This is pretty inefficient.  We can do something neat with
	 * a GtkTreeModelSort here to make it faster, if it turns out to be
	 * slow.
	 */

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (sidebar_links->priv->tree_view));

	g_signal_handler_block (selection, sidebar_links->priv->selection_id);
	g_signal_handler_block (sidebar_links->priv->tree_view, sidebar_links->priv->row_activated_id);

	gtk_tree_selection_unselect_all (selection);
	gtk_tree_model_foreach (sidebar_links->priv->model,
				update_page_callback_foreach,
				sidebar_links);

	g_signal_handler_unblock (selection, sidebar_links->priv->selection_id);
	g_signal_handler_unblock (sidebar_links->priv->tree_view, sidebar_links->priv->row_activated_id);
}

static void 
row_activated_callback 			(GtkTreeView *treeview,
                                         GtkTreePath *arg1,
                                         GtkTreeViewColumn *arg2,
                                         gpointer user_data)
{	
	if (gtk_tree_view_row_expanded (GTK_TREE_VIEW (treeview), arg1)) {
		    gtk_tree_view_collapse_row (GTK_TREE_VIEW (treeview), arg1);
	} else {
		    gtk_tree_view_expand_row (GTK_TREE_VIEW (treeview), arg1, FALSE);
	}
	
        return;
}

static void
expand_open_links (GtkTreeView *tree_view, GtkTreeModel *model, GtkTreeIter *parent)
{
	GtkTreeIter iter;
	gboolean expand;

	if (gtk_tree_model_iter_children (model, &iter, parent)) {
		do {
			gtk_tree_model_get (model, &iter,
					    EV_DOCUMENT_LINKS_COLUMN_EXPAND, &expand,
					    -1);
			if (expand) {
				GtkTreePath *path;

				path = gtk_tree_model_get_path (model, &iter);
				gtk_tree_view_expand_row (tree_view, path, FALSE);
				gtk_tree_path_free (path);
			}

			expand_open_links (tree_view, model, &iter);
		} while (gtk_tree_model_iter_next (model, &iter));
	}
}
	
static void
job_finished_callback (EvJobLinks     *job,
		       EvSidebarLinks *sidebar_links)
{
	EvSidebarLinksPrivate *priv;
	GtkTreeSelection *selection;

	priv = sidebar_links->priv;
	
	priv->model = job->model;
	g_object_notify (G_OBJECT (sidebar_links), "model");

	gtk_tree_view_set_model (GTK_TREE_VIEW (priv->tree_view), job->model);
	
	g_object_unref (job);
	priv->job = NULL;

	expand_open_links (GTK_TREE_VIEW (priv->tree_view), priv->model, NULL);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->tree_view));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);
	priv->selection_id = g_signal_connect (selection, "changed",
					       G_CALLBACK (selection_changed_callback),
					       sidebar_links);
	priv->page_changed_id = g_signal_connect (priv->page_cache, "page-changed",
						  G_CALLBACK (update_page_callback),
						  sidebar_links);
	priv->row_activated_id = g_signal_connect (G_OBJECT (priv->tree_view), "row-activated",
						    G_CALLBACK (row_activated_callback), sidebar_links);
	update_page_callback (priv->page_cache,
			      ev_page_cache_get_current_page (priv->page_cache),
			      sidebar_links);

}

static void
ev_sidebar_links_set_document (EvSidebarPage  *sidebar_page,
			       EvDocument     *document)
{
	EvSidebarLinks *sidebar_links;
	EvSidebarLinksPrivate *priv;

	g_return_if_fail (EV_IS_SIDEBAR_PAGE (sidebar_page));
	g_return_if_fail (EV_IS_DOCUMENT (document));
	
	sidebar_links = EV_SIDEBAR_LINKS (sidebar_page);

	priv = sidebar_links->priv;

	if (priv->document) {
		gtk_tree_view_set_model (GTK_TREE_VIEW (priv->tree_view), NULL);
		g_object_unref (priv->document);
	}

	priv->document = g_object_ref (document);
	priv->page_cache = ev_page_cache_get (document);

	if (priv->job) {
		g_signal_handlers_disconnect_by_func (priv->job,
						      job_finished_callback,
						      sidebar_links);
		g_object_unref (priv->job);
	}

	priv->job = ev_job_links_new (document);
	g_signal_connect (priv->job,
			  "finished",
			  G_CALLBACK (job_finished_callback),
			  sidebar_links);
	/* The priority doesn't matter for this job */
	ev_job_queue_add_job (priv->job, EV_JOB_PRIORITY_LOW);
}

static gboolean
ev_sidebar_links_support_document (EvSidebarPage  *sidebar_page,
				   EvDocument *document)
{
	return (EV_IS_DOCUMENT_LINKS (document) &&
		    ev_document_links_has_document_links (EV_DOCUMENT_LINKS (document)));
}

static const gchar*
ev_sidebar_links_get_label (EvSidebarPage *sidebar_page)
{
    return _("Index");
}

static void
ev_sidebar_links_page_iface_init (EvSidebarPageIface *iface)
{
	iface->support_document = ev_sidebar_links_support_document;
	iface->set_document = ev_sidebar_links_set_document;
	iface->get_label = ev_sidebar_links_get_label;
}

