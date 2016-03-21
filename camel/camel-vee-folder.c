/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Michael Zucchi <notzed@ximian.com>
 *          Jeffrey Stedfast <fejj@ximian.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <glib/gi18n-lib.h>

#include "camel-db.h"
#include "camel-debug.h"
#include "camel-folder-search.h"
#include "camel-mime-message.h"
#include "camel-session.h"
#include "camel-store.h"
#include "camel-vee-folder.h"
#include "camel-vee-store.h"	/* for open flags */
#include "camel-vee-summary.h"
#include "camel-string-utils.h"
#include "camel-vtrash-folder.h"

#define d(x)
#define dd(x) (camel_debug ("vfolder")?(x):0)

typedef struct _FolderChangedData FolderChangedData;

#define CAMEL_VEE_FOLDER_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), CAMEL_TYPE_VEE_FOLDER, CamelVeeFolderPrivate))

struct _CamelVeeFolderPrivate {
	gboolean destroyed;
	GList *subfolders;		/* lock using subfolder_lock before changing/accessing */
	GHashTable *ignore_changed;	/* hash of subfolder pointers to ignore the next folder's 'changed' signal */
	GHashTable *skipped_changes;	/* CamelFolder -> CamelFolderChangeInfo accumulating ignored changes */
	GHashTable *unmatched_add_changed; /* CamelVeeMessageInfoData -> 1, for unmatched folder, postponed additions from camel_vee_folder_add_vuid () */
	GHashTable *unmatched_remove_changed; /* CamelVeeMessageInfoData -> 1, for unmatched folder, postponed removal from camel_vee_folder_remove_vuid () */
	gboolean auto_update;

	/* Processing queue for folder changes. */
	GAsyncQueue *change_queue;
	gboolean change_queue_busy;

	GRecMutex subfolder_lock;	/* for locking the subfolder list */
	GRecMutex changed_lock;	/* for locking the folders-changed list */

	gchar *expression;	/* query expression */

	/* only set-up if our parent is a vee-store, used also as a flag to
	 * say that this folder is part of the unmatched folder */
	CamelVeeStore *parent_vee_store;

	CamelVeeDataCache *vee_data_cache;
};

/* The custom property ID is a CamelArg artifact.
 * It still identifies the property in state files. */
enum {
	PROP_0,
	PROP_AUTO_UPDATE = 0x2401
};

G_DEFINE_TYPE (CamelVeeFolder, camel_vee_folder, CAMEL_TYPE_FOLDER)

struct _FolderChangedData {
	CamelFolderChangeInfo *changes;
	CamelFolder *subfolder;
};

static FolderChangedData *
vee_folder_changed_data_new (CamelFolder *subfolder,
                             CamelFolderChangeInfo *changes)
{
	FolderChangedData *data;

	data = g_slice_new0 (FolderChangedData);
	data->changes = camel_folder_change_info_new ();
	camel_folder_change_info_cat (data->changes, changes);
	data->subfolder = g_object_ref (subfolder);

	return data;
}

static void
vee_folder_changed_data_free (FolderChangedData *data)
{
	g_object_unref (data->changes);
	g_object_unref (data->subfolder);

	g_slice_free (FolderChangedData, data);
}

static CamelVeeDataCache *
vee_folder_get_data_cache (CamelVeeFolder *vfolder)
{
	g_return_val_if_fail (CAMEL_IS_VEE_FOLDER (vfolder), NULL);

	if (vfolder->priv->parent_vee_store)
		return camel_vee_store_get_vee_data_cache (vfolder->priv->parent_vee_store);

	return vfolder->priv->vee_data_cache;
}

static gboolean
vee_folder_is_unmatched (CamelVeeFolder *vfolder)
{
	g_return_val_if_fail (vfolder != NULL, FALSE);

	return vfolder->priv->parent_vee_store &&
		vfolder == camel_vee_store_get_unmatched_folder (vfolder->priv->parent_vee_store);
}

static void
vee_folder_note_added_uid (CamelVeeFolder *vfolder,
                           CamelVeeSummary *vsummary,
                           CamelVeeMessageInfoData *added_mi_data,
                           CamelFolderChangeInfo *changes,
                           gboolean included_as_changed)
{
	const gchar *vuid;

	vuid = camel_vee_message_info_data_get_vee_message_uid (added_mi_data);
	if (!camel_folder_summary_check_uid (&vsummary->summary, vuid)) {
		/* add it only if it wasn't in yet */
		CamelVeeMessageInfo *vmi;

		vmi = camel_vee_summary_add (vsummary, added_mi_data);
		if (vmi) {
			if (changes)
				camel_folder_change_info_add_uid (changes, vuid);
			g_object_unref (vmi);

			if (vfolder->priv->parent_vee_store)
				camel_vee_store_note_vuid_used (vfolder->priv->parent_vee_store, added_mi_data, vfolder);
		}
	} else {
		camel_vee_summary_replace_flags (vsummary, vuid);
		if (included_as_changed && changes)
			camel_folder_change_info_change_uid (changes, vuid);
	}
}

static void
vee_folder_note_unmatch_uid (CamelVeeFolder *vfolder,
                             CamelVeeSummary *vsummary,
                             CamelFolder *subfolder,
                             CamelVeeDataCache *data_cache,
                             CamelVeeMessageInfoData *unmatched_mi_data,
                             CamelFolderChangeInfo *changes)
{
	const gchar *vuid;

	vuid = camel_vee_message_info_data_get_vee_message_uid (unmatched_mi_data);
	if (camel_folder_summary_check_uid (&vsummary->summary, vuid)) {
		g_object_ref (unmatched_mi_data);

		/* this one doesn't belong to us anymore */
		if (changes)
			camel_folder_change_info_remove_uid (changes, vuid);
		camel_vee_summary_remove (vsummary, vuid, subfolder);

		if (vfolder->priv->parent_vee_store)
			camel_vee_store_note_vuid_unused (vfolder->priv->parent_vee_store, unmatched_mi_data, vfolder);
		else
			camel_vee_data_cache_remove_message_info_data (data_cache, unmatched_mi_data);

		g_object_unref (unmatched_mi_data);
	}
}

static void
vee_folder_remove_unmatched (CamelVeeFolder *vfolder,
                             CamelVeeSummary *vsummary,
                             CamelVeeDataCache *data_cache,
                             CamelFolderChangeInfo *changes,
                             CamelFolder *subfolder,
                             const gchar *orig_message_uid,
                             gboolean is_orig_message_uid) /* if not,
                             then it's 'vee_message_uid' */
{
	CamelVeeMessageInfoData *mi_data;

	if (is_orig_message_uid) {
		/* camel_vee_data_cache_get_message_info_data() auto-adds items if not there,
		 * thus check whether the cache has it already, and if not, then skip the action.
		 * This can happen for virtual Junk/Trash folders.
		*/
		if (!camel_vee_data_cache_contains_message_info_data (data_cache, subfolder, orig_message_uid))
			return;

		mi_data = camel_vee_data_cache_get_message_info_data (data_cache, subfolder, orig_message_uid);
	} else
		mi_data = camel_vee_data_cache_get_message_info_data_by_vuid (data_cache, orig_message_uid);

	if (!mi_data)
		return;

	vee_folder_note_unmatch_uid (vfolder, vsummary, subfolder, data_cache, mi_data, changes);

	g_object_unref (mi_data);
}

struct RemoveUnmatchedData
{
	CamelVeeFolder *vfolder;
	CamelVeeSummary *vsummary;
	CamelFolder *subfolder;
	CamelVeeDataCache *data_cache;
	CamelFolderChangeInfo *changes;
	gboolean is_orig_message_uid;
};

static void
vee_folder_remove_unmatched_cb (gpointer key,
                                gpointer value,
                                gpointer user_data)
{
	struct RemoveUnmatchedData *rud = user_data;
	const gchar *uid = key;

	g_return_if_fail (rud != NULL);

	vee_folder_remove_unmatched (rud->vfolder, rud->vsummary, rud->data_cache, rud->changes, rud->subfolder, uid, rud->is_orig_message_uid);
}

static void
vee_folder_merge_matching (CamelVeeFolder *vfolder,
                           CamelFolder *subfolder,
                           GHashTable *all_uids,
                           GPtrArray *match,
                           CamelFolderChangeInfo *changes,
                           gboolean included_as_changed)
{
	CamelVeeDataCache *data_cache;
	CamelVeeMessageInfoData *mi_data;
	CamelFolder *folder;
	CamelVeeSummary *vsummary;
	struct RemoveUnmatchedData rud;
	gint ii;

	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));
	g_return_if_fail (CAMEL_IS_FOLDER (subfolder));
	g_return_if_fail (all_uids != NULL);
	g_return_if_fail (match != NULL);

	folder = CAMEL_FOLDER (vfolder);
	g_return_if_fail (folder != NULL);

	vsummary = CAMEL_VEE_SUMMARY (folder->summary);
	g_return_if_fail (vsummary != NULL);

	data_cache = vee_folder_get_data_cache (vfolder);
	for (ii = 0; ii < match->len; ii++) {
		const gchar *uid = match->pdata[ii];

		mi_data = camel_vee_data_cache_get_message_info_data (data_cache, subfolder, uid);
		if (!mi_data)
			continue;

		g_hash_table_remove (all_uids, uid);

		vee_folder_note_added_uid (vfolder, vsummary, mi_data, changes, included_as_changed);

		g_object_unref (mi_data);
	}

	rud.vfolder = vfolder;
	rud.vsummary = vsummary;
	rud.subfolder = subfolder;
	rud.data_cache = data_cache;
	rud.changes = changes;
	rud.is_orig_message_uid = TRUE;

	/* in 'all_uids' left only those which are not part of the folder anymore */
	g_hash_table_foreach (all_uids, vee_folder_remove_unmatched_cb, &rud);
}

static void
vee_folder_rebuild_folder_with_changes (CamelVeeFolder *vfolder,
                                        CamelFolder *subfolder,
                                        CamelFolderChangeInfo *changes,
                                        GCancellable *cancellable)
{
	GPtrArray *match = NULL;

	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));
	g_return_if_fail (CAMEL_IS_FOLDER (subfolder));

	/* Unmatched folder cannot be rebuilt */
	if (vee_folder_is_unmatched (vfolder))
		return;

	/* if we have no expression, or its been cleared, then act as if no matches */
	if (vfolder->priv->expression == NULL) {
		match = g_ptr_array_new ();
	} else {
		match = camel_folder_search_by_expression (subfolder, vfolder->priv->expression, cancellable, NULL);
		if (!match)
			return;
	}

	if (!g_cancellable_is_cancelled (cancellable)) {
		GHashTable *all_uids;

		all_uids = camel_folder_summary_get_hash (subfolder->summary);
		vee_folder_merge_matching (vfolder, subfolder, all_uids, match, changes, FALSE);
		g_hash_table_destroy (all_uids);
	}

	camel_folder_search_free (subfolder, match);
}

static void
vee_folder_rebuild_all (CamelVeeFolder *vfolder,
                        GCancellable *cancellable)
{
	CamelFolderChangeInfo *changes;
	GList *iter;

	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));

	/* Unmatched folder cannot be rebuilt */
	if (vee_folder_is_unmatched (vfolder))
		return;

	changes = camel_folder_change_info_new ();

	g_rec_mutex_lock (&vfolder->priv->subfolder_lock);

	for (iter = vfolder->priv->subfolders;
	     iter && !g_cancellable_is_cancelled (cancellable);
	     iter = iter->next) {
		CamelFolder *subfolder = iter->data;

		vee_folder_rebuild_folder_with_changes (vfolder, subfolder, changes, cancellable);
	}

	g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);

	if (camel_folder_change_info_changed (changes))
		camel_folder_changed (CAMEL_FOLDER (vfolder), changes);
	g_object_unref (changes);
}

static void
vee_folder_subfolder_changed (CamelVeeFolder *vfolder,
                              CamelFolder *subfolder,
                              CamelFolderChangeInfo *subfolder_changes,
                              GCancellable *cancellable,
                              GError **error)
{
	CamelVeeDataCache *data_cache;
	CamelFolderChangeInfo *changes;
	CamelFolder *v_folder;
	CamelVeeSummary *vsummary;
	gint ii;

	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));
	g_return_if_fail (CAMEL_IS_FOLDER (subfolder));
	g_return_if_fail (subfolder_changes != NULL);

	g_rec_mutex_lock (&vfolder->priv->subfolder_lock);
	if (!g_list_find (vfolder->priv->subfolders, subfolder)) {
		g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);
		return;
	}
	g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);

	changes = camel_folder_change_info_new ();
	data_cache = vee_folder_get_data_cache (vfolder);
	v_folder = CAMEL_FOLDER (vfolder);
	vsummary = CAMEL_VEE_SUMMARY (v_folder->summary);

	camel_folder_freeze (v_folder);

	for (ii = 0; ii < subfolder_changes->uid_removed->len; ii++) {
		const gchar *orig_message_uid = subfolder_changes->uid_removed->pdata[ii];

		vee_folder_remove_unmatched (vfolder, vsummary, data_cache, changes, subfolder, orig_message_uid, TRUE);
	}

	if (subfolder_changes->uid_added->len + subfolder_changes->uid_changed->len > 0) {
		GPtrArray *test_uids, *match;
		gboolean my_match = FALSE;

		test_uids = g_ptr_array_sized_new (subfolder_changes->uid_added->len + subfolder_changes->uid_changed->len);

		for (ii = 0; ii < subfolder_changes->uid_added->len; ii++) {
			g_ptr_array_add (test_uids, subfolder_changes->uid_added->pdata[ii]);
		}

		for (ii = 0; ii < subfolder_changes->uid_changed->len; ii++) {
			g_ptr_array_add (test_uids, subfolder_changes->uid_changed->pdata[ii]);
		}

		if (!vfolder->priv->expression) {
			my_match = TRUE;
			match = g_ptr_array_new ();

			if (vee_folder_is_unmatched (vfolder)) {
				CamelVeeMessageInfoData *mi_data;
				const gchar *vuid;

				/* all common from test_uids and stored uids
				 * in the unmatched folder should be updated */
				for (ii = 0; ii < test_uids->len; ii++) {
					mi_data = camel_vee_data_cache_get_message_info_data (data_cache, subfolder, test_uids->pdata[ii]);
					if (!mi_data)
						continue;

					vuid = camel_vee_message_info_data_get_vee_message_uid (mi_data);
					if (camel_folder_summary_check_uid (v_folder->summary, vuid))
						g_ptr_array_add (match, (gpointer) camel_pstring_strdup (test_uids->pdata[ii]));
					g_object_unref (mi_data);
				}
			}
		} else {
			/* sadly, if there are threads involved, then searching by uids doesn't work,
			 * because just changed uids can be brought in by the thread condition */
			if (strstr (vfolder->priv->expression, "match-threads") != NULL)
				match = camel_folder_search_by_expression (subfolder, vfolder->priv->expression, cancellable, NULL);
			else
				match = camel_folder_search_by_uids (subfolder, vfolder->priv->expression, test_uids, cancellable, NULL);
		}

		if (match) {
			GHashTable *with_uids;

			/* uids are taken from the string pool, thus use direct hashes */
			with_uids = g_hash_table_new_full (g_direct_hash, g_direct_equal, (GDestroyNotify) camel_pstring_free, NULL);
			for (ii = 0; ii < test_uids->len; ii++) {
				g_hash_table_insert (with_uids, (gpointer) camel_pstring_strdup (test_uids->pdata[ii]), GINT_TO_POINTER (1));
			}

			vee_folder_merge_matching (vfolder, subfolder, with_uids, match, changes, TRUE);

			g_hash_table_destroy (with_uids);
			if (my_match) {
				g_ptr_array_foreach (match, (GFunc) camel_pstring_free, NULL);
				g_ptr_array_free (match, TRUE);
			} else {
				camel_folder_search_free (subfolder, match);
			}
		}

		g_ptr_array_free (test_uids, TRUE);
	}

	camel_folder_thaw (v_folder);

	if (camel_folder_change_info_changed (changes))
		camel_folder_changed (v_folder, changes);
	g_object_unref (changes);
}

static void
vee_folder_process_changes (CamelSession *session,
                            GCancellable *cancellable,
                            CamelVeeFolder *vee_folder,
                            GError **error)
{
	CamelFolder *folder;
	FolderChangedData *data;
	GAsyncQueue *change_queue;
	const gchar *display_name;
	const gchar *message;

	folder = CAMEL_FOLDER (vee_folder);

	change_queue = vee_folder->priv->change_queue;

	message = _("Updating folder '%s'");
	display_name = camel_folder_get_display_name (folder);
	camel_operation_push_message (cancellable, message, display_name);

	while ((data = g_async_queue_try_pop (change_queue)) != NULL) {
		vee_folder_subfolder_changed (vee_folder, data->subfolder, data->changes, cancellable, error);
		vee_folder_changed_data_free (data);

		if (g_cancellable_is_cancelled (cancellable))
			break;
	}

	vee_folder->priv->change_queue_busy = FALSE;

	camel_operation_pop_message (cancellable);
}

static void
subfolder_changed (CamelFolder *subfolder,
                   CamelFolderChangeInfo *changes,
                   CamelVeeFolder *vfolder)
{
	g_return_if_fail (vfolder != NULL);
	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));

	g_rec_mutex_lock (&vfolder->priv->changed_lock);
	if (g_hash_table_lookup (vfolder->priv->ignore_changed, subfolder) ||
	    !camel_vee_folder_get_auto_update (vfolder)) {
		CamelFolderChangeInfo *my_changes;

		g_hash_table_remove (vfolder->priv->ignore_changed, subfolder);

		my_changes = g_hash_table_lookup (vfolder->priv->skipped_changes, subfolder);
		if (!my_changes)
			my_changes = camel_folder_change_info_new ();
		camel_folder_change_info_cat (my_changes, changes);
		g_hash_table_insert (vfolder->priv->skipped_changes, subfolder, my_changes);

		g_rec_mutex_unlock (&vfolder->priv->changed_lock);

		return;
	}
	g_rec_mutex_unlock (&vfolder->priv->changed_lock);

	CAMEL_VEE_FOLDER_GET_CLASS (vfolder)->folder_changed (vfolder, subfolder, changes);
}

/* track vanishing folders */
static void
subfolder_deleted (CamelFolder *subfolder,
                   CamelVeeFolder *vfolder)
{
	camel_vee_folder_remove_folder (vfolder, subfolder, NULL);
}

static void
vee_folder_dispose (GObject *object)
{
	CamelFolder *folder;

	folder = CAMEL_FOLDER (object);

	/* parent's class frees summary on dispose, thus depend on it */
	if (folder->summary) {
		CamelVeeFolder *vfolder;

		vfolder = CAMEL_VEE_FOLDER (object);
		vfolder->priv->destroyed = TRUE;

		camel_folder_freeze ((CamelFolder *) vfolder);
		while (vfolder->priv->subfolders) {
			CamelFolder *subfolder = vfolder->priv->subfolders->data;
			camel_vee_folder_remove_folder (vfolder, subfolder, NULL);
		}
		camel_folder_thaw ((CamelFolder *) vfolder);
	}

	/* Chain up to parent's dispose () method. */
	G_OBJECT_CLASS (camel_vee_folder_parent_class)->dispose (object);
}

static void
free_change_info_cb (gpointer folder,
                     gpointer change_info,
                     gpointer user_data)
{
	g_object_unref (change_info);
}

static void
vee_folder_finalize (GObject *object)
{
	CamelVeeFolder *vf;

	vf = CAMEL_VEE_FOLDER (object);

	g_free (vf->priv->expression);

	g_list_free (vf->priv->subfolders);

	g_hash_table_foreach (vf->priv->skipped_changes, free_change_info_cb, NULL);

	g_rec_mutex_clear (&vf->priv->subfolder_lock);
	g_rec_mutex_clear (&vf->priv->changed_lock);
	g_hash_table_destroy (vf->priv->ignore_changed);
	g_hash_table_destroy (vf->priv->skipped_changes);
	g_hash_table_destroy (vf->priv->unmatched_add_changed);
	g_hash_table_destroy (vf->priv->unmatched_remove_changed);

	g_async_queue_unref (vf->priv->change_queue);

	if (vf->priv->vee_data_cache)
		g_object_unref (vf->priv->vee_data_cache);
	vf->priv->vee_data_cache = NULL;

	/* Chain up to parent's finalize () method. */
	G_OBJECT_CLASS (camel_vee_folder_parent_class)->finalize (object);
}

static void
vee_folder_get_property (GObject *object,
                         guint property_id,
                         GValue *value,
                         GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_AUTO_UPDATE:
			g_value_set_boolean (
				value, camel_vee_folder_get_auto_update (
				CAMEL_VEE_FOLDER (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
vee_folder_set_property (GObject *object,
                         guint property_id,
                         const GValue *value,
                         GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_AUTO_UPDATE:
			camel_vee_folder_set_auto_update (
				CAMEL_VEE_FOLDER (object),
				g_value_get_boolean (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
vee_folder_propagate_skipped_changes (CamelVeeFolder *vf)
{
	CamelVeeFolderClass *class;
	CamelFolderChangeInfo *changes = NULL;
	GHashTableIter iter;
	gpointer psub, pchanges;

	g_return_if_fail (vf != NULL);

	class = CAMEL_VEE_FOLDER_GET_CLASS (vf);

	g_rec_mutex_lock (&vf->priv->changed_lock);

	/* this is for Unmatched folder only, other folders have unmatched_remove_changed always empty */
	if (g_hash_table_size (vf->priv->unmatched_add_changed) +
	    g_hash_table_size (vf->priv->unmatched_remove_changed) > 0) {
		gpointer pkey, pvalue;
		CamelVeeSummary *vsummary;
		CamelFolder *v_folder;
		CamelVeeDataCache *data_cache;

		changes = camel_folder_change_info_new ();
		data_cache = vee_folder_get_data_cache (vf);
		v_folder = CAMEL_FOLDER (vf);
		vsummary = CAMEL_VEE_SUMMARY (v_folder->summary);

		/* first remove ... */
		g_hash_table_iter_init (&iter, vf->priv->unmatched_remove_changed);
		while (g_hash_table_iter_next (&iter, &pkey, &pvalue)) {
			CamelVeeMessageInfoData *mi_data = pkey;
			CamelVeeSubfolderData *sf_data;
			CamelFolder *subfolder;

			sf_data = camel_vee_message_info_data_get_subfolder_data (mi_data);
			subfolder = camel_vee_subfolder_data_get_folder (sf_data);
			vee_folder_note_unmatch_uid (vf, vsummary, subfolder, data_cache, mi_data, changes);
		}
		g_hash_table_remove_all (vf->priv->unmatched_remove_changed);

		/* ... then add */
		g_hash_table_iter_init (&iter, vf->priv->unmatched_add_changed);
		while (g_hash_table_iter_next (&iter, &pkey, &pvalue)) {
			CamelVeeMessageInfoData *mi_data = pkey;

			vee_folder_note_added_uid (vf, vsummary, mi_data, changes, FALSE);
		}
		g_hash_table_remove_all (vf->priv->unmatched_add_changed);
	}

	g_hash_table_iter_init (&iter, vf->priv->skipped_changes);
	while (g_hash_table_iter_next (&iter, &psub, &pchanges)) {
		g_warn_if_fail (pchanges != NULL);
		if (!pchanges)
			continue;

		if (g_list_find (vf->priv->subfolders, psub) != NULL)
			class->folder_changed (vf, psub, pchanges);

		g_object_unref (pchanges);
	}

	g_hash_table_remove_all (vf->priv->skipped_changes);

	g_rec_mutex_unlock (&vf->priv->changed_lock);

	if (changes) {
		if (camel_folder_change_info_changed (changes))
			camel_folder_changed (CAMEL_FOLDER (vf), changes);
		g_object_unref (changes);
	}
}

static GPtrArray *
vee_folder_search_by_expression (CamelFolder *folder,
                                 const gchar *expression,
                                 GCancellable *cancellable,
                                 GError **error)
{
	CamelFolderSearch *search;
	GPtrArray *matches;

	search = camel_folder_search_new ();
	camel_folder_search_set_folder (search, folder);
	matches = camel_folder_search_search (search, expression, NULL, cancellable, error);
	g_object_unref (search);

	return matches;
}

static GPtrArray *
vee_folder_search_by_uids (CamelFolder *folder,
                           const gchar *expression,
                           GPtrArray *uids,
                           GCancellable *cancellable,
                           GError **error)
{
	CamelFolderSearch *search;
	GPtrArray *matches;

	if (!uids || uids->len == 0)
		return g_ptr_array_new ();

	search = camel_folder_search_new ();
	camel_folder_search_set_folder (search, folder);
	matches = camel_folder_search_search (search, expression, uids, cancellable, error);
	g_object_unref (search);

	return matches;
}

static guint32
vee_folder_count_by_expression (CamelFolder *folder,
                                const gchar *expression,
                                GCancellable *cancellable,
                                GError **error)
{
	CamelFolderSearch *search;
	guint32 count;

	search = camel_folder_search_new ();
	camel_folder_search_set_folder (search, folder);
	count = camel_folder_search_count (search, expression, cancellable, error);
	g_object_unref (search);

	return count;
}

static void
vee_folder_search_free (CamelFolder *folder,
                        GPtrArray *result)
{
	camel_folder_search_free_result (NULL, result);
}

static void
vee_folder_delete (CamelFolder *folder)
{
	CamelVeeFolder *vfolder;

	g_return_if_fail (CAMEL_IS_VEE_FOLDER (folder));

	vfolder = CAMEL_VEE_FOLDER (folder);

	g_rec_mutex_lock (&vfolder->priv->subfolder_lock);
	while (vfolder->priv->subfolders) {
		CamelFolder *subfolder = vfolder->priv->subfolders->data;

		g_object_ref (subfolder);
		g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);

		camel_vee_folder_remove_folder (vfolder, subfolder, NULL);
		g_object_unref (subfolder);

		g_rec_mutex_lock (&vfolder->priv->subfolder_lock);
	}
	g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);

	((CamelFolderClass *) camel_vee_folder_parent_class)->delete_ (folder);
}

static void
vee_folder_freeze (CamelFolder *folder)
{
	CamelVeeFolder *vfolder = CAMEL_VEE_FOLDER (folder);
	GList *link;

	if (vfolder->priv->parent_vee_store &&
	    !vee_folder_is_unmatched (vfolder)) {
		CamelVeeFolder *unmatched_folder;

		unmatched_folder = camel_vee_store_get_unmatched_folder (vfolder->priv->parent_vee_store);
		if (unmatched_folder)
			camel_folder_freeze (CAMEL_FOLDER (unmatched_folder));
	}

	g_rec_mutex_lock (&vfolder->priv->subfolder_lock);
	for (link = vfolder->priv->subfolders; link; link = g_list_next (link)) {
		CamelFolder *subfolder = link->data;

		camel_folder_freeze (subfolder);
	}
	g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);

	/* call parent implementation */
	CAMEL_FOLDER_CLASS (camel_vee_folder_parent_class)->freeze (folder);
}

static void
vee_folder_thaw (CamelFolder *folder)
{
	CamelVeeFolder *vfolder = CAMEL_VEE_FOLDER (folder);
	GList *link;

	if (vfolder->priv->parent_vee_store &&
	    !vee_folder_is_unmatched (vfolder)) {
		CamelVeeFolder *unmatched_folder;

		unmatched_folder = camel_vee_store_get_unmatched_folder (vfolder->priv->parent_vee_store);
		if (unmatched_folder)
			camel_folder_thaw (CAMEL_FOLDER (unmatched_folder));
	}

	g_rec_mutex_lock (&vfolder->priv->subfolder_lock);
	for (link = vfolder->priv->subfolders; link; link = g_list_next (link)) {
		CamelFolder *subfolder = link->data;

		camel_folder_thaw (subfolder);
	}
	g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);

	/* call parent implementation */
	CAMEL_FOLDER_CLASS (camel_vee_folder_parent_class)->thaw (folder);
}

static gboolean
vee_folder_append_message_sync (CamelFolder *folder,
                                CamelMimeMessage *message,
                                CamelMessageInfo *info,
                                gchar **appended_uid,
                                GCancellable *cancellable,
                                GError **error)
{
	g_set_error (
		error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		_("Cannot copy or move messages into a Virtual Folder"));

	return FALSE;
}

static gboolean
vee_folder_expunge_sync (CamelFolder *folder,
                         GCancellable *cancellable,
                         GError **error)
{
	return CAMEL_FOLDER_GET_CLASS (folder)->
		synchronize_sync (folder, TRUE, cancellable, error);
}

static CamelMimeMessage *
vee_folder_get_message_sync (CamelFolder *folder,
                             const gchar *uid,
                             GCancellable *cancellable,
                             GError **error)
{
	CamelVeeMessageInfo *mi;
	CamelMimeMessage *msg = NULL;

	mi = (CamelVeeMessageInfo *) camel_folder_summary_get (folder->summary, uid);
	if (mi) {
		msg = camel_folder_get_message_sync (
			camel_folder_summary_get_folder (mi->orig_summary), camel_message_info_get_uid (mi) + 8,
			cancellable, error);
		g_object_unref (mi);
	} else {
		g_set_error (
			error, CAMEL_FOLDER_ERROR,
			CAMEL_FOLDER_ERROR_INVALID_UID,
			_("No such message %s in %s"), uid,
			camel_folder_get_display_name (folder));
	}

	return msg;
}

static gboolean
vee_folder_refresh_info_sync (CamelFolder *folder,
                              GCancellable *cancellable,
                              GError **error)
{
	CamelVeeFolder *vf = (CamelVeeFolder *) folder;

	vee_folder_propagate_skipped_changes (vf);
	vee_folder_rebuild_all (vf, cancellable);

	return TRUE;
}

static gboolean
vee_folder_synchronize_sync (CamelFolder *folder,
                             gboolean expunge,
                             GCancellable *cancellable,
                             GError **error)
{
	CamelVeeFolder *vfolder = (CamelVeeFolder *) folder;
	gboolean res = TRUE;
	GList *iter;

	g_return_val_if_fail (CAMEL_IS_VEE_FOLDER (folder), FALSE);

	vee_folder_propagate_skipped_changes (vfolder);

	/* basically no-op here, especially do not call synchronize on subfolders
	 * if not expunging, they are responsible for themselfs */
	if (!expunge ||
	    vee_folder_is_unmatched (vfolder))
		return TRUE;

	g_rec_mutex_lock (&vfolder->priv->subfolder_lock);

	for (iter = vfolder->priv->subfolders; iter && !g_cancellable_is_cancelled (cancellable); iter = iter->next) {
		GError *local_error = NULL;
		CamelFolder *subfolder = iter->data;

		if (!camel_folder_synchronize_sync (subfolder, expunge, cancellable, &local_error)) {
			if (local_error && strncmp (local_error->message, "no such table", 13) != 0 && error && !*error) {
				const gchar *desc;

				desc = camel_folder_get_description (subfolder);
				g_propagate_prefixed_error (
					error, local_error,
					_("Error storing '%s': "), desc);

				res = FALSE;
			} else
				g_clear_error (&local_error);
		}
	}

	g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);

	return res;
}

static gboolean
vee_folder_transfer_messages_to_sync (CamelFolder *folder,
                                      GPtrArray *uids,
                                      CamelFolder *dest,
                                      gboolean delete_originals,
                                      GPtrArray **transferred_uids,
                                      GCancellable *cancellable,
                                      GError **error)
{
	g_set_error (
		error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		_("Cannot copy or move messages into a Virtual Folder"));

	return FALSE;
}

static void
vee_folder_set_expression (CamelVeeFolder *vee_folder,
                           const gchar *query)
{
	g_rec_mutex_lock (&vee_folder->priv->subfolder_lock);

	/* no change, do nothing */
	if ((vee_folder->priv->expression && query && strcmp (vee_folder->priv->expression, query) == 0)
	    || (vee_folder->priv->expression == NULL && query == NULL)) {
		g_rec_mutex_unlock (&vee_folder->priv->subfolder_lock);
		return;
	}

	g_free (vee_folder->priv->expression);
	if (query)
		vee_folder->priv->expression = g_strdup (query);

	vee_folder_rebuild_all (vee_folder, NULL);

	g_rec_mutex_unlock (&vee_folder->priv->subfolder_lock);
}

static void
vee_folder_rebuild_folder (CamelVeeFolder *vfolder,
                           CamelFolder *subfolder,
                           GCancellable *cancellable)
{
	CamelFolderChangeInfo *changes;
	CamelFolder *v_folder;

	v_folder = CAMEL_FOLDER (vfolder);
	changes = camel_folder_change_info_new ();

	camel_folder_freeze (v_folder);
	vee_folder_rebuild_folder_with_changes (vfolder, subfolder, changes, cancellable);
	camel_folder_thaw (v_folder);

	if (camel_folder_change_info_changed (changes))
		camel_folder_changed (CAMEL_FOLDER (vfolder), changes);
	g_object_unref (changes);
}

static void
vee_folder_add_folder (CamelVeeFolder *vfolder,
                       CamelFolder *subfolder,
                       GCancellable *cancellable)
{
	if (vfolder->priv->parent_vee_store)
		camel_vee_store_note_subfolder_used (vfolder->priv->parent_vee_store, subfolder, vfolder);
	vee_folder_rebuild_folder (vfolder, subfolder, cancellable);
}

static gboolean
vee_folder_remove_from_unmatched_changed_cb (gpointer key,
                                             gpointer value,
                                             gpointer user_data)
{
	CamelVeeMessageInfoData *mi_data = key;
	CamelFolder *subfolder = user_data;
	CamelVeeSubfolderData *sf_data;

	g_return_val_if_fail (mi_data != NULL, TRUE);

	sf_data = camel_vee_message_info_data_get_subfolder_data (mi_data);

	return subfolder == camel_vee_subfolder_data_get_folder (sf_data);
}

static void
vee_folder_remove_folder (CamelVeeFolder *vfolder,
                          CamelFolder *subfolder,
                          GCancellable *cancellable)
{
	CamelFolderChangeInfo *changes;
	CamelFolder *v_folder;
	GHashTable *uids;

	v_folder = CAMEL_FOLDER (vfolder);
	changes = camel_folder_change_info_new ();

	camel_folder_freeze (v_folder);

	uids = camel_vee_summary_get_uids_for_subfolder (CAMEL_VEE_SUMMARY (v_folder->summary), subfolder);
	if (uids) {
		struct RemoveUnmatchedData rud;

		rud.vfolder = vfolder;
		rud.vsummary = CAMEL_VEE_SUMMARY (v_folder->summary);
		rud.subfolder = subfolder;
		rud.data_cache = vee_folder_get_data_cache (vfolder);
		rud.changes = changes;
		rud.is_orig_message_uid = FALSE;

		g_hash_table_foreach (uids, vee_folder_remove_unmatched_cb, &rud);

		if (vee_folder_is_unmatched (vfolder) &&
		    !camel_vee_folder_get_auto_update (vfolder) &&
		    g_hash_table_size (vfolder->priv->unmatched_add_changed) +
		    g_hash_table_size (vfolder->priv->unmatched_remove_changed) > 0) {
			/* forget about these in cached updates */
			g_hash_table_foreach_remove (vfolder->priv->unmatched_add_changed,
				vee_folder_remove_from_unmatched_changed_cb, subfolder);
			g_hash_table_foreach_remove (vfolder->priv->unmatched_remove_changed,
				vee_folder_remove_from_unmatched_changed_cb, subfolder);
		}

		g_hash_table_destroy (uids);
	}

	if (vfolder->priv->parent_vee_store)
		camel_vee_store_note_subfolder_unused (vfolder->priv->parent_vee_store, subfolder, vfolder);

	camel_folder_thaw (v_folder);

	/* do not notify about changes in vfolder which
	 * is removing its subfolders in dispose */
	if (!vfolder->priv->destroyed &&
	    camel_folder_change_info_changed (changes))
		camel_folder_changed (CAMEL_FOLDER (vfolder), changes);
	g_object_unref (changes);
}

static void
vee_folder_folder_changed (CamelVeeFolder *vee_folder,
                           CamelFolder *subfolder,
                           CamelFolderChangeInfo *changes)
{
	CamelVeeFolderPrivate *p = vee_folder->priv;
	FolderChangedData *data;
	CamelFolder *folder;
	CamelStore *parent_store;
	CamelSession *session;

	if (p->destroyed)
		return;

	folder = CAMEL_FOLDER (vee_folder);
	parent_store = camel_folder_get_parent_store (folder);
	session = camel_service_ref_session (CAMEL_SERVICE (parent_store));
	if (!session)
		return;

	g_async_queue_lock (vee_folder->priv->change_queue);

	data = vee_folder_changed_data_new (subfolder, changes);

	g_async_queue_push_unlocked (vee_folder->priv->change_queue, data);

	if (!vee_folder->priv->change_queue_busy) {
		gchar *description;

		description = g_strdup_printf ("Updating search folder '%s'", camel_folder_get_full_name (CAMEL_FOLDER (vee_folder)));

		camel_session_submit_job (
			session, description, (CamelSessionCallback)
			vee_folder_process_changes,
			g_object_ref (vee_folder),
			(GDestroyNotify) g_object_unref);
		vee_folder->priv->change_queue_busy = TRUE;

		g_free (description);
	}

	g_async_queue_unlock (vee_folder->priv->change_queue);

	g_object_unref (session);
}

static void
camel_vee_folder_class_init (CamelVeeFolderClass *class)
{
	GObjectClass *object_class;
	CamelFolderClass *folder_class;

	g_type_class_add_private (class, sizeof (CamelVeeFolderPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = vee_folder_dispose;
	object_class->finalize = vee_folder_finalize;
	object_class->get_property = vee_folder_get_property;
	object_class->set_property = vee_folder_set_property;

	folder_class = CAMEL_FOLDER_CLASS (class);
	folder_class->search_by_expression = vee_folder_search_by_expression;
	folder_class->search_by_uids = vee_folder_search_by_uids;
	folder_class->count_by_expression = vee_folder_count_by_expression;
	folder_class->search_free = vee_folder_search_free;
	folder_class->delete_ = vee_folder_delete;
	folder_class->freeze = vee_folder_freeze;
	folder_class->thaw = vee_folder_thaw;
	folder_class->append_message_sync = vee_folder_append_message_sync;
	folder_class->expunge_sync = vee_folder_expunge_sync;
	folder_class->get_message_sync = vee_folder_get_message_sync;
	folder_class->refresh_info_sync = vee_folder_refresh_info_sync;
	folder_class->synchronize_sync = vee_folder_synchronize_sync;
	folder_class->transfer_messages_to_sync = vee_folder_transfer_messages_to_sync;

	class->set_expression = vee_folder_set_expression;
	class->add_folder = vee_folder_add_folder;
	class->remove_folder = vee_folder_remove_folder;
	class->rebuild_folder = vee_folder_rebuild_folder;
	class->folder_changed = vee_folder_folder_changed;

	g_object_class_install_property (
		object_class,
		PROP_AUTO_UPDATE,
		g_param_spec_boolean (
			"auto-update",
			"Auto Update",
			_("Automatically _update on change in source folders"),
			TRUE,
			G_PARAM_READWRITE |
			CAMEL_PARAM_PERSISTENT));
}

static void
camel_vee_folder_init (CamelVeeFolder *vee_folder)
{
	CamelFolder *folder = CAMEL_FOLDER (vee_folder);

	vee_folder->priv = CAMEL_VEE_FOLDER_GET_PRIVATE (vee_folder);

	folder->folder_flags |= CAMEL_FOLDER_HAS_SUMMARY_CAPABILITY;

	/* FIXME: what to do about user flags if the subfolder doesn't support them? */
	folder->permanent_flags = CAMEL_MESSAGE_ANSWERED |
		CAMEL_MESSAGE_DELETED |
		CAMEL_MESSAGE_DRAFT |
		CAMEL_MESSAGE_FLAGGED |
		CAMEL_MESSAGE_SEEN;

	g_rec_mutex_init (&vee_folder->priv->subfolder_lock);
	g_rec_mutex_init (&vee_folder->priv->changed_lock);

	vee_folder->priv->auto_update = TRUE;
	vee_folder->priv->ignore_changed = g_hash_table_new (g_direct_hash, g_direct_equal);
	vee_folder->priv->skipped_changes = g_hash_table_new (g_direct_hash, g_direct_equal);
	vee_folder->priv->unmatched_add_changed =
		g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);
	vee_folder->priv->unmatched_remove_changed =
		g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);

	vee_folder->priv->change_queue = g_async_queue_new_full (
		(GDestroyNotify) vee_folder_changed_data_free);
}

void
camel_vee_folder_construct (CamelVeeFolder *vf,
                            guint32 flags)
{
	CamelFolder *folder = (CamelFolder *) vf;
	CamelStore *parent_store;

	vf->flags = flags;

	parent_store = camel_folder_get_parent_store (CAMEL_FOLDER (vf));
	if (CAMEL_IS_VEE_STORE (parent_store))
		vf->priv->parent_vee_store = CAMEL_VEE_STORE (parent_store);
	else
		vf->priv->vee_data_cache = camel_vee_data_cache_new ();

	folder->summary = camel_vee_summary_new (folder);

	/* only for subfolders of vee-store */
	if (vf->priv->parent_vee_store) {
		const gchar *user_data_dir;
		gchar *state_file, *folder_name, *filename;

		user_data_dir = camel_service_get_user_data_dir (CAMEL_SERVICE (parent_store));

		folder_name = g_uri_escape_string (camel_folder_get_full_name (folder), NULL, TRUE);
		filename = g_strconcat (folder_name, ".cmeta", NULL);
		state_file = g_build_filename (user_data_dir, filename, NULL);

		camel_object_set_state_filename (CAMEL_OBJECT (vf), state_file);

		g_free (state_file);
		g_free (filename);
		g_free (folder_name);

		/* set/load persistent state */
		camel_object_state_read (CAMEL_OBJECT (vf));
	}
}

/**
 * camel_vee_folder_new:
 * @parent_store: the parent CamelVeeStore
 * @full: the full path to the vfolder.
 * @flags: flags of some kind
 *
 * Create a new CamelVeeFolder object.
 *
 * Returns: A new CamelVeeFolder widget.
 **/
CamelFolder *
camel_vee_folder_new (CamelStore *parent_store,
                      const gchar *full,
                      guint32 flags)
{
	CamelVeeFolder *vf;

	g_return_val_if_fail (CAMEL_IS_STORE (parent_store), NULL);
	g_return_val_if_fail (full != NULL, NULL);

	if (CAMEL_IS_VEE_STORE (parent_store) && strcmp (full, CAMEL_UNMATCHED_NAME) == 0) {
		vf = camel_vee_store_get_unmatched_folder (CAMEL_VEE_STORE (parent_store));
		if (vf)
			g_object_ref (vf);
	} else {
		const gchar *name = strrchr (full, '/');

		if (name == NULL)
			name = full;
		else
			name++;
		vf = g_object_new (
			CAMEL_TYPE_VEE_FOLDER,
			"display-name", name, "full-name", full,
			"parent-store", parent_store, NULL);
		camel_vee_folder_construct (vf, flags);
	}

	d (printf ("returning folder %s %p, count = %d\n", full, vf, camel_folder_get_message_count ((CamelFolder *) vf)));

	return (CamelFolder *) vf;
}

void
camel_vee_folder_set_expression (CamelVeeFolder *vfolder,
                                 const gchar *expr)
{
	CAMEL_VEE_FOLDER_GET_CLASS (vfolder)->set_expression (vfolder, expr);
}

/**
 * camel_vee_folder_get_expression:
 *
 * FIXME Document me!
 *
 * Since: 3.6
 **/
const gchar *
camel_vee_folder_get_expression (CamelVeeFolder *vfolder)
{
	g_return_val_if_fail (CAMEL_IS_VEE_FOLDER (vfolder), NULL);

	return vfolder->priv->expression;
}

/**
 * camel_vee_folder_add_folder:
 * @vfolder: Virtual Folder object
 * @subfolder: source CamelFolder to add to @vfolder
 *
 * Adds @subfolder as a source folder to @vfolder.
 **/
void
camel_vee_folder_add_folder (CamelVeeFolder *vfolder,
                             CamelFolder *subfolder,
                             GCancellable *cancellable)
{
	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));

	if (vfolder == (CamelVeeFolder *) subfolder) {
		g_warning ("Adding a virtual folder to itself as source, ignored");
		return;
	}

	g_rec_mutex_lock (&vfolder->priv->subfolder_lock);

	if (g_list_find (vfolder->priv->subfolders, subfolder) == NULL) {
		gint freeze_count;

		vfolder->priv->subfolders = g_list_append (vfolder->priv->subfolders, g_object_ref (subfolder));

		freeze_count = camel_folder_get_frozen_count (CAMEL_FOLDER (vfolder));
		while (freeze_count > 0) {
			camel_folder_freeze (subfolder);
			freeze_count--;
		}
	} else {
		/* nothing to do, it's already there */
		g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);
		return;
	}

	g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);

	g_signal_connect (
		subfolder, "changed",
		G_CALLBACK (subfolder_changed), vfolder);

	g_signal_connect (
		subfolder, "deleted",
		G_CALLBACK (subfolder_deleted), vfolder);

	CAMEL_VEE_FOLDER_GET_CLASS (vfolder)->add_folder (vfolder, subfolder, cancellable);
}

/**
 * camel_vee_folder_remove_folder:
 * @vfolder: Virtual Folder object
 * @subfolder: source CamelFolder to remove from @vfolder
 *
 * Removed the source folder, @subfolder, from the virtual folder, @vfolder.
 **/
void
camel_vee_folder_remove_folder (CamelVeeFolder *vfolder,
                                CamelFolder *subfolder,
                                GCancellable *cancellable)
{
	gint freeze_count;

	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));

	g_rec_mutex_lock (&vfolder->priv->subfolder_lock);

	if (g_list_find (vfolder->priv->subfolders, subfolder) == NULL) {
		g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);
		return;
	}

	g_signal_handlers_disconnect_by_func (subfolder, subfolder_changed, vfolder);
	g_signal_handlers_disconnect_by_func (subfolder, subfolder_deleted, vfolder);

	vfolder->priv->subfolders = g_list_remove (vfolder->priv->subfolders, subfolder);

	freeze_count = camel_folder_get_frozen_count (CAMEL_FOLDER (vfolder));
	while (freeze_count > 0) {
		camel_folder_thaw (subfolder);
		freeze_count--;
	}

	g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);

	CAMEL_VEE_FOLDER_GET_CLASS (vfolder)->remove_folder (vfolder, subfolder, cancellable);

	g_object_unref (subfolder);
}

/**
 * camel_vee_folder_rebuild_folder:
 * @vfolder: Virtual Folder object
 * @subfolder: source CamelFolder to add to @vfolder
 * @cancellable:
 *
 * Rebuild the folder @subfolder, if it should be.
 **/
void
camel_vee_folder_rebuild_folder (CamelVeeFolder *vfolder,
                                 CamelFolder *subfolder,
                                 GCancellable *cancellable)
{
	vee_folder_propagate_skipped_changes (vfolder);

	CAMEL_VEE_FOLDER_GET_CLASS (vfolder)->rebuild_folder (vfolder, subfolder, cancellable);
}

static void
remove_folders (CamelFolder *folder,
                CamelFolder *foldercopy,
                CamelVeeFolder *vf)
{
	camel_vee_folder_remove_folder (vf, folder, NULL);
	g_object_unref (folder);
}

/**
 * camel_vee_folder_set_folders:
 * @vf:
 * @folders: (element-type CamelFolder) (transfer none):
 *
 * Set the whole list of folder sources on a vee folder.
 **/
void
camel_vee_folder_set_folders (CamelVeeFolder *vf,
                              GList *folders,
                              GCancellable *cancellable)
{
	CamelVeeFolderPrivate *p = CAMEL_VEE_FOLDER_GET_PRIVATE (vf);
	GHashTable *remove = g_hash_table_new (NULL, NULL);
	GList *l, *to_add = NULL;
	CamelFolder *folder;

	/* setup a table of all folders we have currently */
	g_rec_mutex_lock (&vf->priv->subfolder_lock);
	l = p->subfolders;
	while (l) {
		g_hash_table_insert (remove, l->data, l->data);
		g_object_ref (l->data);
		l = l->next;
	}
	g_rec_mutex_unlock (&vf->priv->subfolder_lock);

	camel_folder_freeze (CAMEL_FOLDER (vf));

	/* if we already have the folder, ignore it, otherwise mark to add it */
	l = folders;
	while (l) {
		if ((folder = g_hash_table_lookup (remove, l->data))) {
			g_hash_table_remove (remove, folder);
			g_object_unref (folder);
		} else {
			to_add = g_list_prepend (to_add, g_object_ref (l->data));
		}
		l = l->next;
	}

	/* first remove any we still have */
	g_hash_table_foreach (remove, (GHFunc) remove_folders, vf);
	g_hash_table_destroy (remove);

	/* then add those new */
	for (l = to_add; l; l = l->next) {
		camel_vee_folder_add_folder (vf, l->data, cancellable);
	}
	g_list_free_full (to_add, g_object_unref);

	camel_folder_thaw (CAMEL_FOLDER (vf));
}

/**
 * camel_vee_folder_add_vuid:
 *
 * FIXME Document me!
 *
 * Since: 3.6
 **/
void
camel_vee_folder_add_vuid (CamelVeeFolder *vfolder,
                           CamelVeeMessageInfoData *mi_data,
                           CamelFolderChangeInfo *changes)
{
	CamelVeeSummary *vsummary;
	CamelVeeSubfolderData *sf_data;
	CamelFolder *subfolder;

	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));
	g_return_if_fail (mi_data != NULL);
	g_return_if_fail (vee_folder_is_unmatched (vfolder));

	sf_data = camel_vee_message_info_data_get_subfolder_data (mi_data);
	subfolder = camel_vee_subfolder_data_get_folder (sf_data);

	g_rec_mutex_lock (&vfolder->priv->changed_lock);
	if (!camel_vee_folder_get_auto_update (vfolder) ||
	    g_hash_table_lookup (vfolder->priv->ignore_changed, subfolder) ||
	    g_hash_table_lookup (vfolder->priv->skipped_changes, subfolder)) {
		g_hash_table_remove (vfolder->priv->unmatched_remove_changed, mi_data);

		g_rec_mutex_lock (&vfolder->priv->subfolder_lock);

		if (g_list_find (vfolder->priv->subfolders, subfolder)) {
			/* postpone addition to the Unmatched folder, if the change was done
			 * in the Unmatched folder itself or auto-update is disabled */
			g_hash_table_insert (
				vfolder->priv->unmatched_add_changed,
				g_object_ref (mi_data), GINT_TO_POINTER (1));
		}

		g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);
		g_rec_mutex_unlock (&vfolder->priv->changed_lock);

		return;
	}

	g_rec_mutex_unlock (&vfolder->priv->changed_lock);

	vsummary = CAMEL_VEE_SUMMARY (CAMEL_FOLDER (vfolder)->summary);
	vee_folder_note_added_uid (vfolder, vsummary, mi_data, changes, FALSE);
}

/**
 * camel_vee_folder_remove_vuid:
 *
 * FIXME Document me!
 *
 * Since: 3.6
 **/
void
camel_vee_folder_remove_vuid (CamelVeeFolder *vfolder,
                              CamelVeeMessageInfoData *mi_data,
                              CamelFolderChangeInfo *changes)
{
	CamelVeeSummary *vsummary;
	CamelVeeSubfolderData *sf_data;
	CamelVeeDataCache *data_cache;
	CamelFolder *subfolder;

	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));
	g_return_if_fail (mi_data != NULL);
	g_return_if_fail (vee_folder_is_unmatched (vfolder));

	sf_data = camel_vee_message_info_data_get_subfolder_data (mi_data);
	subfolder = camel_vee_subfolder_data_get_folder (sf_data);

	g_rec_mutex_lock (&vfolder->priv->changed_lock);
	if (!camel_vee_folder_get_auto_update (vfolder) ||
	    g_hash_table_lookup (vfolder->priv->ignore_changed, subfolder) ||
	    g_hash_table_lookup (vfolder->priv->skipped_changes, subfolder)) {
		g_hash_table_remove (vfolder->priv->unmatched_add_changed, mi_data);

		g_rec_mutex_lock (&vfolder->priv->subfolder_lock);

		if (g_list_find (vfolder->priv->subfolders, subfolder)) {
			/* postpone removal from the Unmatched folder, if the change was done
			 * in the Unmatched folder itself or auto-update is disabled */
			g_hash_table_insert (
				vfolder->priv->unmatched_remove_changed,
				g_object_ref (mi_data), GINT_TO_POINTER (1));
		}

		g_rec_mutex_unlock (&vfolder->priv->subfolder_lock);
		g_rec_mutex_unlock (&vfolder->priv->changed_lock);

		return;
	}

	g_rec_mutex_unlock (&vfolder->priv->changed_lock);

	vsummary = CAMEL_VEE_SUMMARY (CAMEL_FOLDER (vfolder)->summary);
	data_cache = vee_folder_get_data_cache (vfolder);
	vee_folder_note_unmatch_uid (vfolder, vsummary, subfolder, data_cache, mi_data, changes);
}

/**
 * camel_vee_folder_get_location:
 * @vf:
 * @vinfo:
 * @realuid: if not NULL, set to the uid of the real message, must be
 * g_free'd by caller.
 *
 * Find the real folder (and uid)
 *
 * Returns: (transfer none):
 **/
CamelFolder *
camel_vee_folder_get_location (CamelVeeFolder *vf,
                               const CamelVeeMessageInfo *vinfo,
                               gchar **realuid)
{
	CamelFolder *folder;

	g_return_val_if_fail (CAMEL_IS_VEE_FOLDER (vf), NULL);
	g_return_val_if_fail (vinfo != NULL, NULL);

	folder = camel_folder_summary_get_folder (vinfo->orig_summary);

	/* locking?  yes?  no?  although the vfolderinfo is valid when obtained
	 * the folder in it might not necessarily be so ...? */
	if (CAMEL_IS_VEE_FOLDER (folder)) {
		CamelFolder *res;
		const CamelVeeMessageInfo *vfinfo;

		vfinfo = (CamelVeeMessageInfo *) camel_folder_get_message_info (folder, camel_message_info_get_uid (vinfo) + 8);
		res = camel_vee_folder_get_location ((CamelVeeFolder *) folder, vfinfo, realuid);
		g_object_unref ((CamelMessageInfo *) vfinfo);
		return res;
	} else {
		if (realuid)
			*realuid = g_strdup (camel_message_info_get_uid (vinfo)+8);

		return folder;
	}
}

/**
 * camel_vee_folder_get_vee_uid_folder:
 *
 * FIXME Document me!
 *
 * Returns: (transfer none):
 *
 * Since: 3.6
 **/
CamelFolder *
camel_vee_folder_get_vee_uid_folder (CamelVeeFolder *vf,
                                     const gchar *vee_message_uid)
{
	CamelFolder *res;
	CamelVeeDataCache *data_cache;
	CamelVeeMessageInfoData *mi_data;
	CamelVeeSubfolderData *sf_data;

	g_return_val_if_fail (CAMEL_IS_VEE_FOLDER (vf), NULL);
	g_return_val_if_fail (vee_message_uid, NULL);

	res = NULL;

	data_cache = vee_folder_get_data_cache (vf);
	g_return_val_if_fail (data_cache != NULL, NULL);

	mi_data = camel_vee_data_cache_get_message_info_data_by_vuid (data_cache, vee_message_uid);
	if (mi_data) {
		sf_data = camel_vee_message_info_data_get_subfolder_data (mi_data);
		res = camel_vee_subfolder_data_get_folder (sf_data);
		g_object_unref (mi_data);
	}

	return res;
}

/**
 * camel_vee_folder_set_auto_update:
 *
 * FIXME Document me!
 *
 * Since: 3.6
 **/
void
camel_vee_folder_set_auto_update (CamelVeeFolder *vfolder,
                                  gboolean auto_update)
{
	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));

	if (vfolder->priv->auto_update == auto_update)
		return;

	vfolder->priv->auto_update = auto_update;

	g_object_notify (G_OBJECT (vfolder), "auto-update");
}

/**
 * camel_vee_folder_get_auto_update:
 *
 * FIXME Document me!
 *
 * Since: 3.6
 **/
gboolean
camel_vee_folder_get_auto_update (CamelVeeFolder *vfolder)
{
	g_return_val_if_fail (CAMEL_IS_VEE_FOLDER (vfolder), FALSE);

	return vfolder->priv->auto_update;
}

/**
 * camel_vee_folder_ignore_next_changed_event:
 * @vfolder: a #CamelVeeFolder
 * @subfolder: a #CamelFolder folder
 *
 * The next @subfolder-'s 'changed' event will be silently ignored. This
 * is usually used in virtual folders when the change was done in them,
 * but it is neither vTrash nor vJunk folder. Doing this avoids unnecessary
 * removals of messages which don't satisfy search criteria anymore,
 * which could be done on asynchronous delivery of folder's 'changed' signal.
 * These ignored changes are accumulated and used on folder refresh.
 *
 * Since: 3.2
 **/
void
camel_vee_folder_ignore_next_changed_event (CamelVeeFolder *vfolder,
                                            CamelFolder *subfolder)
{
	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));
	g_return_if_fail (subfolder != NULL);

	g_rec_mutex_lock (&vfolder->priv->changed_lock);
	g_hash_table_insert (vfolder->priv->ignore_changed, subfolder, GINT_TO_POINTER (1));
	g_rec_mutex_unlock (&vfolder->priv->changed_lock);
}

/**
 * camel_vee_folder_remove_from_ignore_changed_event:
 * @vfolder: a #CamelVeeFolder
 * @subfolder: a #CamelFolder folder
 *
 * Make sure the next @subfolder-'s 'changed' event will not be silently ignored.
 * This is a counter-part function of camel_vee_folder_ignore_next_changed_event(),
 * when there was expected a change, which did not happen, to take back the previous
 * ignore event request.
 *
 * Since: 3.12
 **/
void
camel_vee_folder_remove_from_ignore_changed_event (CamelVeeFolder *vfolder,
                                                   CamelFolder *subfolder)
{
	g_return_if_fail (CAMEL_IS_VEE_FOLDER (vfolder));
	g_return_if_fail (subfolder != NULL);

	g_rec_mutex_lock (&vfolder->priv->changed_lock);
	g_hash_table_remove (vfolder->priv->ignore_changed, subfolder);
	g_rec_mutex_unlock (&vfolder->priv->changed_lock);
}
