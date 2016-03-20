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
 * Authors: Jeffrey Stedfast <fejj@novell.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib/gi18n-lib.h>

#include "camel-debug.h"
#include "camel-offline-folder.h"
#include "camel-offline-settings.h"
#include "camel-offline-store.h"
#include "camel-operation.h"
#include "camel-session.h"

#define CAMEL_OFFLINE_FOLDER_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), CAMEL_TYPE_OFFLINE_FOLDER, CamelOfflineFolderPrivate))

typedef struct _AsyncContext AsyncContext;
typedef struct _OfflineDownsyncData OfflineDownsyncData;

struct _CamelOfflineFolderPrivate {
	gboolean offline_sync;

	GMutex store_changes_lock;
	guint store_changes_id;
	gboolean store_changes_after_frozen;
};

struct _AsyncContext {
	gchar *expression;
};

struct _OfflineDownsyncData {
	CamelFolder *folder;
	CamelFolderChangeInfo *changes;
};

/* The custom property ID is a CamelArg artifact.
 * It still identifies the property in state files. */
enum {
	PROP_0,
	PROP_OFFLINE_SYNC = 0x2400
};

G_DEFINE_TYPE (CamelOfflineFolder, camel_offline_folder, CAMEL_TYPE_FOLDER)

static void
async_context_free (AsyncContext *async_context)
{
	g_free (async_context->expression);

	g_slice_free (AsyncContext, async_context);
}

static void
offline_downsync_data_free (OfflineDownsyncData *data)
{
	if (data->changes != NULL)
		g_object_unref (data->changes);

	g_object_unref (data->folder);

	g_slice_free (OfflineDownsyncData, data);
}

static void
offline_folder_downsync_background (CamelSession *session,
                                    GCancellable *cancellable,
                                    OfflineDownsyncData *data,
                                    GError **error)
{
	camel_operation_push_message (
		cancellable,
		_("Downloading new messages for offline mode in '%s'"),
		camel_folder_get_full_name (data->folder));

	if (data->changes) {
		GPtrArray *uid_added;
		gboolean success = TRUE;
		gint ii;

		uid_added = data->changes->uid_added;

		for (ii = 0; success && ii < uid_added->len; ii++) {
			const gchar *uid;
			gint percent;

			percent = ii * 100 / uid_added->len;
			uid = g_ptr_array_index (uid_added, ii);

			camel_operation_progress (cancellable, percent);

			success = camel_folder_synchronize_message_sync (
				data->folder, uid, cancellable, error);
		}
	} else {
		camel_offline_folder_downsync_sync (
			CAMEL_OFFLINE_FOLDER (data->folder),
			"(match-all)", cancellable, error);
	}

	camel_operation_pop_message (cancellable);
}

static void
offline_folder_store_changes_job_cb (CamelSession *session,
				     GCancellable *cancellable,
				     gpointer user_data,
				     GError **error)
{
	CamelFolder *folder = user_data;

	g_return_if_fail (CAMEL_IS_OFFLINE_FOLDER (folder));

	camel_folder_synchronize_sync (folder, FALSE, cancellable, error);
}

static gboolean
offline_folder_schedule_store_changes_job (gpointer user_data)
{
	CamelOfflineFolder *offline_folder = user_data;
	GSource *source;

	source = g_main_current_source ();

	if (g_source_is_destroyed (source))
		return FALSE;

	g_return_val_if_fail (CAMEL_IS_OFFLINE_FOLDER (offline_folder), FALSE);

	g_mutex_lock (&offline_folder->priv->store_changes_lock);
	if (offline_folder->priv->store_changes_id == g_source_get_id (source)) {
		CamelSession *session;

		offline_folder->priv->store_changes_id = 0;

		session = camel_service_ref_session (CAMEL_SERVICE (camel_folder_get_parent_store (CAMEL_FOLDER (offline_folder))));
		if (session) {
			gchar *description;

			description = g_strdup_printf (_("Storing changes in folder '%s'"),
				camel_folder_get_full_name (CAMEL_FOLDER (offline_folder)));

			camel_session_submit_job (session, description,
				offline_folder_store_changes_job_cb,
				g_object_ref (offline_folder), g_object_unref);

			g_free (description);
		}

		g_clear_object (&session);
	}
	g_mutex_unlock (&offline_folder->priv->store_changes_lock);

	return FALSE;
}

static void
offline_folder_maybe_schedule_folder_change_store (CamelOfflineFolder *offline_folder)
{
	CamelSession *session;
	CamelStore *store;

	g_return_if_fail (CAMEL_IS_OFFLINE_FOLDER (offline_folder));

	g_mutex_lock (&offline_folder->priv->store_changes_lock);

	if (offline_folder->priv->store_changes_id)
		g_source_remove (offline_folder->priv->store_changes_id);
	offline_folder->priv->store_changes_id = 0;
	offline_folder->priv->store_changes_after_frozen = FALSE;

	if (camel_folder_is_frozen (CAMEL_FOLDER (offline_folder))) {
		offline_folder->priv->store_changes_after_frozen = TRUE;
		g_mutex_unlock (&offline_folder->priv->store_changes_lock);

		return;
	}

	store = camel_folder_get_parent_store (CAMEL_FOLDER (offline_folder));
	session = camel_service_ref_session (CAMEL_SERVICE (store));

	if (session && camel_session_get_online (session) && CAMEL_IS_OFFLINE_STORE (store) &&
	    camel_offline_store_get_online (CAMEL_OFFLINE_STORE (store))) {
		CamelSettings *settings;
		gint interval = -1;

		settings = camel_service_ref_settings (CAMEL_SERVICE (store));
		if (settings && CAMEL_IS_OFFLINE_SETTINGS (settings))
			interval = camel_offline_settings_get_store_changes_interval (CAMEL_OFFLINE_SETTINGS (settings));
		g_clear_object (&settings);

		if (interval == 0)
			offline_folder_schedule_store_changes_job (offline_folder);
		else if (interval > 0)
			offline_folder->priv->store_changes_id = g_timeout_add_seconds (interval,
				offline_folder_schedule_store_changes_job, offline_folder);
	}

	g_clear_object (&session);

	g_mutex_unlock (&offline_folder->priv->store_changes_lock);
}

static void
offline_folder_changed (CamelFolder *folder,
                        CamelFolderChangeInfo *changes)
{
	CamelStore *parent_store;
	CamelService *service;
	CamelSession *session;
	CamelSettings *settings;
	gboolean sync_store;
	gboolean sync_folder;

	parent_store = camel_folder_get_parent_store (folder);

	service = CAMEL_SERVICE (parent_store);
	session = camel_service_ref_session (service);

	if (!session)
		return;

	settings = camel_service_ref_settings (service);

	sync_store = camel_offline_settings_get_stay_synchronized (
		CAMEL_OFFLINE_SETTINGS (settings));

	g_object_unref (settings);

	sync_folder = camel_offline_folder_get_offline_sync (
		CAMEL_OFFLINE_FOLDER (folder));

	if (changes && changes->uid_added->len > 0 && (sync_store || sync_folder)) {
		OfflineDownsyncData *data;
		gchar *description;

		data = g_slice_new0 (OfflineDownsyncData);
		data->changes = camel_folder_change_info_new ();
		camel_folder_change_info_cat (data->changes, changes);
		data->folder = g_object_ref (folder);

		description = g_strdup_printf (_("Checking download of new messages for offline in '%s'"), camel_folder_get_full_name (folder));

		camel_session_submit_job (
			session, description, (CamelSessionCallback)
			offline_folder_downsync_background, data,
			(GDestroyNotify) offline_downsync_data_free);

		g_free (description);
	}

	g_object_unref (session);

	if (changes && changes->uid_changed && changes->uid_changed->len > 0)
		offline_folder_maybe_schedule_folder_change_store (CAMEL_OFFLINE_FOLDER (folder));
}

static void
offline_folder_set_property (GObject *object,
                             guint property_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_OFFLINE_SYNC:
			camel_offline_folder_set_offline_sync (
				CAMEL_OFFLINE_FOLDER (object),
				g_value_get_boolean (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
offline_folder_get_property (GObject *object,
                             guint property_id,
                             GValue *value,
                             GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_OFFLINE_SYNC:
			g_value_set_boolean (
				value, camel_offline_folder_get_offline_sync (
				CAMEL_OFFLINE_FOLDER (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
offline_folder_dispose (GObject *object)
{
	CamelOfflineFolder *offline_folder = CAMEL_OFFLINE_FOLDER (object);

	g_mutex_lock (&offline_folder->priv->store_changes_lock);
	if (offline_folder->priv->store_changes_id)
		g_source_remove (offline_folder->priv->store_changes_id);
	offline_folder->priv->store_changes_id = 0;
	g_mutex_unlock (&offline_folder->priv->store_changes_lock);

	/* Chain up to parent's method. */
	G_OBJECT_CLASS (camel_offline_folder_parent_class)->dispose (object);
}

static void
offline_folder_finalize (GObject *object)
{
	CamelOfflineFolder *offline_folder = CAMEL_OFFLINE_FOLDER (object);

	g_mutex_clear (&offline_folder->priv->store_changes_lock);

	/* Chain up to parent's method. */
	G_OBJECT_CLASS (camel_offline_folder_parent_class)->finalize (object);
}

static void
offline_folder_thaw (CamelFolder *folder)
{
	/* Chain up to parent's method. */
	CAMEL_FOLDER_CLASS (camel_offline_folder_parent_class)->thaw (folder);

	if (!camel_folder_is_frozen (folder)) {
		CamelOfflineFolder *offline_folder;

		g_return_if_fail (CAMEL_IS_OFFLINE_FOLDER (folder));

		offline_folder = CAMEL_OFFLINE_FOLDER (folder);

		g_mutex_lock (&offline_folder->priv->store_changes_lock);
		if (offline_folder->priv->store_changes_after_frozen) {
			offline_folder->priv->store_changes_after_frozen = FALSE;
			g_mutex_unlock (&offline_folder->priv->store_changes_lock);

			offline_folder_maybe_schedule_folder_change_store (offline_folder);
		} else {
			g_mutex_unlock (&offline_folder->priv->store_changes_lock);
		}
	}
}

static gboolean
offline_folder_downsync_sync (CamelOfflineFolder *offline,
                              const gchar *expression,
                              GCancellable *cancellable,
                              GError **error)
{
	CamelFolder *folder = (CamelFolder *) offline;
	GPtrArray *uids, *uncached_uids = NULL;
	const gchar *display_name;
	const gchar *message;
	gint i;

	message = _("Syncing messages in folder '%s' to disk");
	display_name = camel_folder_get_display_name (folder);
	camel_operation_push_message (cancellable, message, display_name);

	if (expression)
		uids = camel_folder_search_by_expression (folder, expression, cancellable, NULL);
	else
		uids = camel_folder_get_uids (folder);

	if (!uids)
		goto done;
	uncached_uids = camel_folder_get_uncached_uids (folder, uids, NULL);
	if (uids) {
		if (expression)
			camel_folder_search_free (folder, uids);
		else
			camel_folder_free_uids (folder, uids);
	}

	if (!uncached_uids)
		goto done;

	for (i = 0; i < uncached_uids->len; i++) {
		camel_folder_synchronize_message_sync (
			folder, uncached_uids->pdata[i], cancellable, NULL);
		camel_operation_progress (
			cancellable, i * 100 / uncached_uids->len);
	}

done:
	if (uncached_uids)
		camel_folder_free_uids (folder, uncached_uids);

	camel_operation_pop_message (cancellable);

	return TRUE;
}

static void
camel_offline_folder_class_init (CamelOfflineFolderClass *class)
{
	GObjectClass *object_class;
	CamelFolderClass *folder_class;

	g_type_class_add_private (class, sizeof (CamelOfflineFolderPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = offline_folder_set_property;
	object_class->get_property = offline_folder_get_property;
	object_class->dispose = offline_folder_dispose;
	object_class->finalize = offline_folder_finalize;

	folder_class = CAMEL_FOLDER_CLASS (class);
	folder_class->thaw = offline_folder_thaw;

	class->downsync_sync = offline_folder_downsync_sync;

	g_object_class_install_property (
		object_class,
		PROP_OFFLINE_SYNC,
		g_param_spec_boolean (
			"offline-sync",
			"Offline Sync",
			_("Copy folder content locally for _offline operation"),
			FALSE,
			G_PARAM_READWRITE |
			CAMEL_PARAM_PERSISTENT));
}

static void
camel_offline_folder_init (CamelOfflineFolder *folder)
{
	folder->priv = CAMEL_OFFLINE_FOLDER_GET_PRIVATE (folder);

	g_mutex_init (&folder->priv->store_changes_lock);
	folder->priv->store_changes_after_frozen = FALSE;

	g_signal_connect (
		folder, "changed",
		G_CALLBACK (offline_folder_changed), NULL);
}

/**
 * camel_offline_folder_get_offline_sync:
 * @folder: a #CamelOfflineFolder
 *
 * Since: 2.32
 **/
gboolean
camel_offline_folder_get_offline_sync (CamelOfflineFolder *folder)
{
	g_return_val_if_fail (CAMEL_IS_OFFLINE_FOLDER (folder), FALSE);

	return folder->priv->offline_sync;
}

/**
 * camel_offline_folder_set_offline_sync:
 * @folder: a #CamelOfflineFolder
 * @offline_sync: whether to synchronize for offline use
 *
 * Since: 2.32
 **/
void
camel_offline_folder_set_offline_sync (CamelOfflineFolder *folder,
                                       gboolean offline_sync)
{
	g_return_if_fail (CAMEL_IS_OFFLINE_FOLDER (folder));

	if (folder->priv->offline_sync == offline_sync)
		return;

	folder->priv->offline_sync = offline_sync;

	g_object_notify (G_OBJECT (folder), "offline-sync");
}

/**
 * camel_offline_folder_downsync_sync:
 * @folder: a #CamelOfflineFolder
 * @expression: search expression describing which set of messages
 *              to downsync (%NULL for all)
 * @cancellable: optional #GCancellable object, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Synchronizes messages in @folder described by the search @expression to
 * the local machine for offline availability.
 *
 * Returns: %TRUE on success, %FALSE on error
 *
 * Since: 3.0
 **/
gboolean
camel_offline_folder_downsync_sync (CamelOfflineFolder *folder,
                                    const gchar *expression,
                                    GCancellable *cancellable,
                                    GError **error)
{
	CamelOfflineFolderClass *class;
	gboolean success;

	g_return_val_if_fail (CAMEL_IS_OFFLINE_FOLDER (folder), FALSE);

	class = CAMEL_OFFLINE_FOLDER_GET_CLASS (folder);
	g_return_val_if_fail (class->downsync_sync != NULL, FALSE);

	success = class->downsync_sync (
		folder, expression, cancellable, error);
	CAMEL_CHECK_GERROR (folder, downsync_sync, success, error);

	return success;
}

/* Helper for camel_offline_folder_downsync() */
static void
offline_folder_downsync_thread (GTask *task,
                                gpointer source_object,
                                gpointer task_data,
                                GCancellable *cancellable)
{
	gboolean success;
	AsyncContext *async_context;
	GError *local_error = NULL;

	async_context = (AsyncContext *) task_data;

	success = camel_offline_folder_downsync_sync (
		CAMEL_OFFLINE_FOLDER (source_object),
		async_context->expression,
		cancellable, &local_error);

	if (local_error != NULL) {
		g_task_return_error (task, local_error);
	} else {
		g_task_return_boolean (task, success);
	}
}

/**
 * camel_offline_folder_downsync:
 * @folder: a #CamelOfflineFolder
 * @expression: search expression describing which set of messages
 *              to downsync (%NULL for all)
 * @io_priority: the I/O priority of the request
 * @cancellable: optional #GCancellable object, or %NULl
 * @callback: a #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: data to pass to the callback function
 *
 * Synchronizes messages in @folder described by the search @expression to
 * the local machine asynchronously for offline availability.
 *
 * When the operation is finished, @callback will be called.  You can then
 * call camel_offline_folder_downsync_finish() to get the result of the
 * operation.
 *
 * Since: 3.0
 **/
void
camel_offline_folder_downsync (CamelOfflineFolder *folder,
                               const gchar *expression,
                               gint io_priority,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
	GTask *task;
	AsyncContext *async_context;

	g_return_if_fail (CAMEL_IS_OFFLINE_FOLDER (folder));

	async_context = g_slice_new0 (AsyncContext);
	async_context->expression = g_strdup (expression);

	task = g_task_new (folder, cancellable, callback, user_data);
	g_task_set_source_tag (task, camel_offline_folder_downsync);
	g_task_set_priority (task, io_priority);

	g_task_set_task_data (
		task, async_context,
		(GDestroyNotify) async_context_free);

	g_task_run_in_thread (task, offline_folder_downsync_thread);

	g_object_unref (task);
}

/**
 * camel_offline_folder_downsync_finish:
 * @folder: a #CamelOfflineFolder
 * @result: a #GAsyncResult
 * @error: return location for a #GError, or %NULL
 *
 * Finishes the operation started with camel_offline_folder_downsync().
 *
 * Returns: %TRUE on success, %FALSE on error
 *
 * Since: 3.0
 **/
gboolean
camel_offline_folder_downsync_finish (CamelOfflineFolder *folder,
                                      GAsyncResult *result,
                                      GError **error)
{
	g_return_val_if_fail (CAMEL_IS_OFFLINE_FOLDER (folder), FALSE);
	g_return_val_if_fail (g_task_is_valid (result, folder), FALSE);

	g_return_val_if_fail (
		g_async_result_is_tagged (
		result, camel_offline_folder_downsync), FALSE);

	return g_task_propagate_boolean (G_TASK (result), error);
}
