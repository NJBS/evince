#include "ev-page-cache.h"
#include "ev-job-queue.h"

typedef struct _EvPageCacheInfo
{
	gint width;
	gint height;
}
EvPageCacheInfo;


struct _EvPageCache
{
	GObject parent;

	gint current_page;
	int n_pages;
	char *title;

	gboolean uniform;
	gint uniform_width;
	gint uniform_height;

	EvPageCacheInfo *size_cache;
};

struct _EvPageCacheClass
{
	GObjectClass parent_class;

	void (* page_changed) (EvPageCache *page_cache, gint page);
};

enum
{
	PAGE_CHANGED,
	N_SIGNALS,
};

static guint signals[N_SIGNALS] = {0, };

static void ev_page_cache_init       (EvPageCache      *page_cache);
static void ev_page_cache_class_init (EvPageCacheClass *page_cache);
static void ev_page_cache_finalize   (GObject *object);

G_DEFINE_TYPE (EvPageCache, ev_page_cache, G_TYPE_OBJECT)

static void
ev_page_cache_init (EvPageCache *page_cache)
{
	page_cache->current_page = 0;
}

static void
ev_page_cache_class_init (EvPageCacheClass *class)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (class);

	object_class->finalize = ev_page_cache_finalize;

	signals [PAGE_CHANGED] =
		g_signal_new ("page-changed",
			      EV_TYPE_PAGE_CACHE,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EvPageCacheClass, page_changed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__INT,
			      G_TYPE_NONE, 1,
			      G_TYPE_INT);

}

static void
ev_page_cache_finalize (GObject *object)
{
	EvPageCache *page_cache;

	page_cache = EV_PAGE_CACHE (object);

	g_free (page_cache->title);
	g_free (page_cache->size_cache);
}

EvPageCache *
_ev_page_cache_new (EvDocument *document)
{
	EvPageCache *page_cache;
	EvPageCacheInfo *info;
	gint i;

	page_cache = (EvPageCache *) g_object_new (EV_TYPE_PAGE_CACHE, NULL);

	g_mutex_lock (EV_DOC_MUTEX);

	/* We read page information out of the document */

	/* Assume all pages are the same size until proven otherwise */
	page_cache->uniform = TRUE;
	page_cache->n_pages = ev_document_get_n_pages (document);
	page_cache->title = ev_document_get_title (document);

	ev_document_set_scale (document, 1.0);
	for (i = 0; i < page_cache->n_pages; i++) {
		gint page_width = 0;
		gint page_height = 0;

		ev_document_get_page_size (document, i, &page_width, &page_height);

		if (i == 0) {
			page_cache->uniform_width = page_width;
			page_cache->uniform_height = page_height;
		} else if (page_cache->uniform &&
			   (page_cache->uniform_width != page_width ||
			    page_cache->uniform_height != page_height)) {
			/* It's a different page size.  Backfill the array. */
			int j;

			page_cache->size_cache = g_new0 (EvPageCacheInfo, page_cache->n_pages);

			for (j = 1; j < i; j++) {
				info = &(page_cache->size_cache [j - 1]);
				info->width = page_width;
				info->height = page_height;
			}
			page_cache->uniform = FALSE;

		}

		if (! page_cache->uniform) {
			info = &(page_cache->size_cache [i - 1]);

			info->width = page_width;
			info->height = page_height;
		}
	}

	/* make some sanity check assertions */
	g_assert (page_cache->n_pages > 0);
	if (! page_cache->uniform)
		g_assert (page_cache->size_cache != NULL);
	if (page_cache->uniform)
		g_assert (page_cache->uniform_width > 0 && page_cache->uniform_height > 0);

	g_mutex_unlock (EV_DOC_MUTEX);

	return page_cache;
}

gint
ev_page_cache_get_n_pages (EvPageCache *page_cache)
{
	g_return_val_if_fail (EV_IS_PAGE_CACHE (page_cache), 0);

	return page_cache->n_pages;
}

gint
ev_page_cache_get_current_page (EvPageCache *page_cache)
{
	g_return_val_if_fail (EV_IS_PAGE_CACHE (page_cache), 0);

	return page_cache->current_page;
}

void
ev_page_cache_set_current_page (EvPageCache *page_cache,
				int          page)
{
	g_return_if_fail (EV_IS_PAGE_CACHE (page_cache));
	g_return_if_fail (page >= 0 || page < page_cache->n_pages);

	if (page == page_cache->current_page)
		return;

	page_cache->current_page = page;
	g_signal_emit (page_cache, signals[PAGE_CHANGED], 0, page);
}

void
ev_page_cache_set_link (EvPageCache *page_cache,
			EvLink      *link)
{
	g_return_if_fail (EV_IS_PAGE_CACHE (page_cache));
	g_return_if_fail (EV_IS_LINK (link));

	ev_page_cache_set_current_page (page_cache, ev_link_get_page (link));
}

char *
ev_page_cache_get_title (EvPageCache *page_cache)
{
	g_return_val_if_fail (EV_IS_PAGE_CACHE (page_cache), NULL);

	return page_cache->title;
}

void
ev_page_cache_get_size (EvPageCache *page_cache,
			gint         page,
			gfloat       scale,
			gint        *width,
			gint        *height)
{
	g_return_if_fail (EV_IS_PAGE_CACHE (page_cache));
	g_return_if_fail (page >= 0 && page < page_cache->n_pages);

	if (page_cache->uniform) {
		if (width)
			*width = page_cache->uniform_width;
		if (height)
			*height = page_cache->uniform_height;
	} else {
		EvPageCacheInfo *info;

		info = &(page_cache->size_cache [page]);
		
		if (width)
			*width = info->width;
		if (height)
			*height = info->height;
	}

	if (width)
		*width = (*width) * scale;
	if (width)
		*height = (*height) * scale;

}

gboolean
ev_page_cache_next_page (EvPageCache *page_cache)
{
	g_return_val_if_fail (EV_IS_PAGE_CACHE (page_cache), FALSE);

	if (page_cache->current_page > page_cache->n_pages)
		return FALSE;

	ev_page_cache_set_current_page (page_cache, page_cache->current_page + 1);
	return TRUE;

}

gboolean
ev_page_cache_prev_page (EvPageCache *page_cache)
{
	g_return_val_if_fail (EV_IS_PAGE_CACHE (page_cache), FALSE);

	if (page_cache->current_page <= 0)
		return FALSE;

	ev_page_cache_set_current_page (page_cache, page_cache->current_page - 1);
	return TRUE;
}

