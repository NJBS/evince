/* this file is part of evince, a gnome document viewer
 *
 *  Copyright (C) 2004 Red Hat, Inc.
 *  Copyright (C) 2004, 2005 Anders Carlsson <andersca@gnome.org>
 *
 *  Authors:
 *    Jonathan Blandford <jrb@alum.mit.edu>
 *    Anders Carlsson <andersca@gnome.org>
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
#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include "ev-sidebar-page.h"
#include "ev-sidebar-thumbnails.h"
#include "ev-document-thumbnails.h"
#include "ev-document-misc.h"
#include "ev-job-queue.h"
#include "ev-window.h"
#include "ev-utils.h"

#define THUMBNAIL_WIDTH 100

/* Amount of time we devote to each iteration of the idle, in microseconds */
#define IDLE_WORK_LENGTH 5000

struct _EvSidebarThumbnailsPrivate {
	GtkWidget *tree_view;
	GtkAdjustment *vadjustment;
	GtkListStore *list_store;
	GdkPixbuf *loading_icon;
	EvDocument *document;

	gint n_pages, pages_done;

	/* Visible pages */
	gint start_page, end_page;
};

enum {
	COLUMN_PAGE_STRING,
	COLUMN_PIXBUF,
	COLUMN_THUMBNAIL_SET,
	COLUMN_JOB,
	NUM_COLUMNS
};

static void         ev_sidebar_thumbnails_clear_model      (EvSidebarThumbnails *sidebar);
static gboolean     ev_sidebar_thumbnails_support_document (EvSidebarPage       *sidebar_page,
							    EvDocument          *document);
static void         ev_sidebar_thumbnails_page_iface_init  (EvSidebarPageIface  *iface);
static void         ev_sidebar_thumbnails_set_document     (EvSidebarPage       *sidebar_page,
							    EvDocument          *document);
static const gchar* ev_sidebar_thumbnails_get_label        (EvSidebarPage       *sidebar_page);
static void         thumbnail_job_completed_callback       (EvJobThumbnail      *job,
							    EvSidebarThumbnails *sidebar_thumbnails);

G_DEFINE_TYPE_EXTENDED (EvSidebarThumbnails, 
                        ev_sidebar_thumbnails, 
                        GTK_TYPE_VBOX,
                        0, 
                        G_IMPLEMENT_INTERFACE (EV_TYPE_SIDEBAR_PAGE, 
					       ev_sidebar_thumbnails_page_iface_init))

#define EV_SIDEBAR_THUMBNAILS_GET_PRIVATE(object) \
	(G_TYPE_INSTANCE_GET_PRIVATE ((object), EV_TYPE_SIDEBAR_THUMBNAILS, EvSidebarThumbnailsPrivate));


static void
ev_sidebar_thumbnails_dispose (GObject *object)
{
	EvSidebarThumbnails *sidebar_thumbnails = EV_SIDEBAR_THUMBNAILS (object);
	
	ev_sidebar_thumbnails_clear_model (sidebar_thumbnails);
	g_object_unref (sidebar_thumbnails->priv->loading_icon);

	G_OBJECT_CLASS (ev_sidebar_thumbnails_parent_class)->dispose (object);
}

static void
ev_sidebar_thumbnails_class_init (EvSidebarThumbnailsClass *ev_sidebar_thumbnails_class)
{
	GObjectClass *g_object_class;
	GtkObjectClass *gtk_object_class;

	g_object_class = G_OBJECT_CLASS (ev_sidebar_thumbnails_class);
	gtk_object_class = GTK_OBJECT_CLASS (ev_sidebar_thumbnails_class);

	g_object_class->dispose = ev_sidebar_thumbnails_dispose;

	g_type_class_add_private (g_object_class, sizeof (EvSidebarThumbnailsPrivate));
}

GtkWidget *
ev_sidebar_thumbnails_new (void)
{
	GtkWidget *ev_sidebar_thumbnails;

	ev_sidebar_thumbnails = g_object_new (EV_TYPE_SIDEBAR_THUMBNAILS, NULL);

	return ev_sidebar_thumbnails;
}

static void
clear_range (EvSidebarThumbnails *sidebar_thumbnails,
	     gint                 start_page,
	     gint                 end_page)
{
	EvSidebarThumbnailsPrivate *priv;
	GtkTreePath *path;
	GtkTreeIter iter;
	gboolean result;

	priv = sidebar_thumbnails->priv = EV_SIDEBAR_THUMBNAILS_GET_PRIVATE (sidebar_thumbnails);

	g_assert (start_page <= end_page);

	path = gtk_tree_path_new_from_indices (start_page, -1);
	for (result = gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->list_store), &iter, path);
	     result && start_page <= end_page;
	     result = gtk_tree_model_iter_next (GTK_TREE_MODEL (priv->list_store), &iter), start_page ++) {
		EvJobThumbnail *job;

		gtk_tree_model_get (GTK_TREE_MODEL (priv->list_store),
				    &iter,
				    COLUMN_JOB, &job,
				    -1);

		if (job) {
			g_signal_handlers_disconnect_by_func (job, thumbnail_job_completed_callback, sidebar_thumbnails);
			ev_job_queue_remove_job (EV_JOB (job));
			g_object_unref (job);
		}

		gtk_list_store_set (priv->list_store, &iter,
				    COLUMN_JOB, NULL,
				    COLUMN_THUMBNAIL_SET, FALSE,
				    COLUMN_PIXBUF, priv->loading_icon,
				    -1);
	}
	gtk_tree_path_free (path);
}

static void
add_range (EvSidebarThumbnails *sidebar_thumbnails,
	   gint                 start_page,
	   gint                 end_page)
{
	EvSidebarThumbnailsPrivate *priv;
	GtkTreePath *path;
	GtkTreeIter iter;
	gboolean result;
	gint page = start_page;

	priv = sidebar_thumbnails->priv = EV_SIDEBAR_THUMBNAILS_GET_PRIVATE (sidebar_thumbnails);

	g_assert (start_page <= end_page);

	path = gtk_tree_path_new_from_indices (start_page, -1);
	for (result = gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->list_store), &iter, path);
	     result && page <= end_page;
	     result = gtk_tree_model_iter_next (GTK_TREE_MODEL (priv->list_store), &iter), page ++) {
		EvJobThumbnail *job;
		gboolean thumbnail_set;

		gtk_tree_model_get (GTK_TREE_MODEL (priv->list_store), &iter,
				    COLUMN_JOB, &job,
				    COLUMN_THUMBNAIL_SET, &thumbnail_set,
				    -1);

		if (job == NULL && !thumbnail_set) {
			job = (EvJobThumbnail *)ev_job_thumbnail_new (priv->document, page, THUMBNAIL_WIDTH);
			ev_job_queue_add_job (EV_JOB (job), EV_JOB_PRIORITY_HIGH);
			g_object_set_data_full (G_OBJECT (job), "tree_iter",
						gtk_tree_iter_copy (&iter),
						(GDestroyNotify) gtk_tree_iter_free);
			g_signal_connect (job, "finished",
					  G_CALLBACK (thumbnail_job_completed_callback),
					  sidebar_thumbnails);
			gtk_list_store_set (priv->list_store, &iter,
					    COLUMN_JOB, job,
					    -1);
			/* The queue and the list own a ref to the job now */
			g_object_unref (job);
		} else if (job) {
			g_object_unref (job);
		}
	}
	gtk_tree_path_free (path);
}

/* This modifies start */
static void
update_visible_range (EvSidebarThumbnails *sidebar_thumbnails,
		      gint                 start_page,
		      gint                 end_page)
{
	EvSidebarThumbnailsPrivate *priv;
	int old_start_page, old_end_page;

	priv = sidebar_thumbnails->priv = EV_SIDEBAR_THUMBNAILS_GET_PRIVATE (sidebar_thumbnails);

	old_start_page = priv->start_page;
	old_end_page = priv->end_page;

	if (start_page == old_start_page &&
	    end_page == old_end_page)
		return;

	/* Clear the areas we no longer display */
	if (old_start_page < start_page)
		clear_range (sidebar_thumbnails, old_start_page, MIN (start_page - 1, old_end_page));

	if (old_end_page > end_page)
		clear_range (sidebar_thumbnails, MAX (end_page + 1, old_start_page), old_end_page);

	add_range (sidebar_thumbnails, start_page, end_page);
	
	priv->start_page = start_page;
	priv->end_page = end_page;
}

static void
adjustment_changed_cb (EvSidebarThumbnails *sidebar_thumbnails)
{
	EvSidebarThumbnailsPrivate *priv;
	GtkTreePath *path;
	GtkTreePath *path2;
	gint wy1;
	gint wy2;

	priv = sidebar_thumbnails->priv = EV_SIDEBAR_THUMBNAILS_GET_PRIVATE (sidebar_thumbnails);

	if (! GTK_WIDGET_REALIZED (priv->tree_view))
		return;

	gtk_tree_view_tree_to_widget_coords (GTK_TREE_VIEW (priv->tree_view),
					     0, (int) priv->vadjustment->value,
					     NULL, &wy1);
	gtk_tree_view_tree_to_widget_coords (GTK_TREE_VIEW (priv->tree_view),
					     0, (int) (priv->vadjustment->value + priv->vadjustment->page_size),
					     NULL, &wy2);
	gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (priv->tree_view),
				       1, wy1 + 1, &path,
				       NULL, NULL, NULL);
	gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (priv->tree_view),
				       1, wy2 -1, &path2,
				       NULL, NULL, NULL);
	if (path == NULL)
		path = gtk_tree_path_new_first ();
	if (path2 == NULL)
		path2 = gtk_tree_path_new_from_indices (priv->n_pages,
							-1);
	update_visible_range (sidebar_thumbnails,
			      gtk_tree_path_get_indices (path)[0],
			      gtk_tree_path_get_indices (path2)[0]);

	gtk_tree_path_free (path);
	gtk_tree_path_free (path2);
}

static void
ev_sidebar_tree_selection_changed (GtkTreeSelection *selection,
				   EvSidebarThumbnails *ev_sidebar_thumbnails)
{
	EvSidebarThumbnailsPrivate *priv;
	EvPageCache *page_cache;
	GtkTreePath *path;
	GtkTreeIter iter;
	int page;

	priv = ev_sidebar_thumbnails->priv = EV_SIDEBAR_THUMBNAILS_GET_PRIVATE (ev_sidebar_thumbnails);

	if (!gtk_tree_selection_get_selected (selection, NULL, &iter))
		return;

	path = gtk_tree_model_get_path (GTK_TREE_MODEL (priv->list_store),
					&iter);
	page = gtk_tree_path_get_indices (path)[0];
	gtk_tree_path_free (path);

	page_cache = ev_page_cache_get (priv->document);
	ev_page_cache_set_current_page (page_cache, page);
}

GtkWidget *
ev_sidebar_thumbnails_get_treeview (EvSidebarThumbnails *sidebar)
{
	return sidebar->priv->tree_view;
}

static void
ev_sidebar_thumbnails_init (EvSidebarThumbnails *ev_sidebar_thumbnails)
{
	GtkWidget *swindow;
	EvSidebarThumbnailsPrivate *priv;
	GtkCellRenderer *renderer;
	GtkTreeSelection *selection;

	priv = ev_sidebar_thumbnails->priv = EV_SIDEBAR_THUMBNAILS_GET_PRIVATE (ev_sidebar_thumbnails);

	priv->list_store = gtk_list_store_new (NUM_COLUMNS, G_TYPE_STRING, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, EV_TYPE_JOB_THUMBNAIL);
	priv->tree_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (priv->list_store));

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (priv->tree_view));
	g_signal_connect (selection, "changed",
			  G_CALLBACK (ev_sidebar_tree_selection_changed), ev_sidebar_thumbnails);
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (priv->tree_view), FALSE);
	renderer = g_object_new (GTK_TYPE_CELL_RENDERER_PIXBUF,
				 "xpad", 2,
				 "ypad", 2,
				 NULL);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (priv->tree_view), -1,
						     NULL, renderer,
						     "pixbuf", 1,
						     NULL);
	gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (priv->tree_view), -1,
						     NULL, gtk_cell_renderer_text_new (),
						     "markup", 0, NULL);

	g_object_unref (priv->list_store);

	swindow = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (swindow),
					GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (swindow),
					     GTK_SHADOW_IN);
	priv->vadjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (swindow));
	g_signal_connect_data (G_OBJECT (priv->vadjustment), "value-changed",
			       G_CALLBACK (adjustment_changed_cb),
			       ev_sidebar_thumbnails, NULL,
			       G_CONNECT_SWAPPED | G_CONNECT_AFTER);
	g_signal_connect_swapped (G_OBJECT (swindow), "size-allocate",
				  G_CALLBACK (adjustment_changed_cb),
				  ev_sidebar_thumbnails);
	gtk_container_add (GTK_CONTAINER (swindow), priv->tree_view);
	gtk_box_pack_start (GTK_BOX (ev_sidebar_thumbnails), swindow, TRUE, TRUE, 0);

	gtk_widget_show_all (swindow);
}

static void
page_changed_cb (EvPageCache         *page_cache,
		 int                  page,
		 EvSidebarThumbnails *sidebar)
{
	GtkTreeView *tree_view = GTK_TREE_VIEW (sidebar->priv->tree_view);
	GtkTreePath *path;

	path = gtk_tree_path_new_from_indices (page, -1);
	gtk_tree_view_set_cursor (tree_view, path, NULL, FALSE);
	gtk_tree_view_scroll_to_cell (tree_view, path, NULL, FALSE, 0.0, 0.0);
	gtk_tree_path_free (path);
}

static void
thumbnail_job_completed_callback (EvJobThumbnail      *job,
				  EvSidebarThumbnails *sidebar_thumbnails)
{
	EvSidebarThumbnailsPrivate *priv = sidebar_thumbnails->priv;
	GtkTreeIter *iter;

	iter = (GtkTreeIter *) g_object_get_data (G_OBJECT (job), "tree_iter");
	gtk_list_store_set (priv->list_store,
			    iter,
			    COLUMN_PIXBUF, job->thumbnail,
			    COLUMN_THUMBNAIL_SET, TRUE,
			    COLUMN_JOB, NULL,
			    -1);
}

static void
ev_sidebar_thumbnails_set_document (EvSidebarPage	*sidebar_page,
				    EvDocument          *document)
{
	EvSidebarThumbnails *sidebar_thumbnails = EV_SIDEBAR_THUMBNAILS (sidebar_page);
	gint i, n_pages;
	GtkTreeIter iter;
	gint width = THUMBNAIL_WIDTH;
	gint height = THUMBNAIL_WIDTH;
	EvPageCache *page_cache;

	EvSidebarThumbnailsPrivate *priv = sidebar_thumbnails->priv;

	g_return_if_fail (EV_IS_DOCUMENT_THUMBNAILS (document));

	page_cache = ev_page_cache_get (document);
	n_pages = ev_page_cache_get_n_pages (page_cache);

	priv->document = document;
	priv->n_pages = n_pages;

	/* We get the dimensions of the first doc so that we can make a blank
	 * icon.  */
	ev_document_doc_mutex_lock ();
	ev_document_thumbnails_get_dimensions (EV_DOCUMENT_THUMBNAILS (priv->document),
					       0, THUMBNAIL_WIDTH, &width, &height);
	ev_document_doc_mutex_unlock ();

	if (priv->loading_icon)
		g_object_unref (priv->loading_icon);
	priv->loading_icon = ev_document_misc_get_thumbnail_frame (width, height, NULL);

	ev_sidebar_thumbnails_clear_model (sidebar_thumbnails);
	for (i = 0; i < n_pages; i++) {
		gchar *page_label;
		gchar *page_string;

		page_label = ev_page_cache_get_page_label (page_cache, i);
		page_string = g_markup_printf_escaped ("<i>%s</i>", page_label);

		gtk_list_store_append (priv->list_store, &iter);
		gtk_list_store_set (priv->list_store, &iter,
				    COLUMN_PAGE_STRING, page_string,
				    COLUMN_PIXBUF, priv->loading_icon,
				    COLUMN_THUMBNAIL_SET, FALSE,
				    -1);
		g_free (page_label);
		g_free (page_string);
	}

	/* Connect to the signal and trigger a fake callback */
	g_signal_connect (page_cache, "page-changed", G_CALLBACK (page_changed_cb), sidebar_thumbnails);
	adjustment_changed_cb (sidebar_thumbnails);
}

static gboolean
ev_sidebar_thumbnails_clear_job (GtkTreeModel *model,                                             
			         GtkTreePath *path, 					                                                 
			         GtkTreeIter *iter, 											                                              
				 gpointer data)
{
    EvJob *job;
    
    gtk_tree_model_get (model, iter, COLUMN_JOB, &job, -1);
    
    if (job != NULL) {
        ev_job_queue_remove_job (job);
	g_signal_handlers_disconnect_by_func (job, thumbnail_job_completed_callback, data);
    	g_object_unref (job);
    }

    return FALSE;    
}

static void 
ev_sidebar_thumbnails_clear_model (EvSidebarThumbnails *sidebar_thumbnails)
{
    EvSidebarThumbnailsPrivate *priv = sidebar_thumbnails->priv;
    
    gtk_tree_model_foreach (GTK_TREE_MODEL (priv->list_store), ev_sidebar_thumbnails_clear_job, sidebar_thumbnails);
    gtk_list_store_clear (priv->list_store);
}

static gboolean
ev_sidebar_thumbnails_support_document (EvSidebarPage   *sidebar_page,
				        EvDocument *document)
{
	return (EV_IS_DOCUMENT_THUMBNAILS (document) &&
		    (ev_document_get_n_pages (document) > 1));
}

static const gchar*
ev_sidebar_thumbnails_get_label (EvSidebarPage *sidebar_page)
{
    return _("Thumbnails");
}

static void
ev_sidebar_thumbnails_page_iface_init (EvSidebarPageIface *iface)
{
	iface->support_document = ev_sidebar_thumbnails_support_document;
	iface->set_document = ev_sidebar_thumbnails_set_document;
	iface->get_label = ev_sidebar_thumbnails_get_label;
}

