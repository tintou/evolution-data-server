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
 * Authors: Sankar P <psankar@novell.com>
 *          Srinivasa Ragavan <sragavan@novell.com>
 */

#include "camel-db.h"
#include "camel-string-utils.h"

#include <config.h>

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include "camel-debug.h"
#include "camel-object.h"

/* how long to wait before invoking sync on the file */
#define SYNC_TIMEOUT_SECONDS 5

static sqlite3_vfs *old_vfs = NULL;
static GThreadPool *sync_pool = NULL;

typedef struct {
	sqlite3_file parent;
	sqlite3_file *old_vfs_file; /* pointer to old_vfs' file */
	GRecMutex sync_mutex;
	guint timeout_id;
	gint flags;

	/* Do know how many syncs are pending, to not close
	   the file before the last sync is over */
	guint pending_syncs;
	GMutex pending_syncs_lock;
	GCond pending_syncs_cond;
} CamelSqlite3File;

static gint
call_old_file_Sync (CamelSqlite3File *cFile,
                    gint flags)
{
	g_return_val_if_fail (old_vfs != NULL, SQLITE_ERROR);
	g_return_val_if_fail (cFile != NULL, SQLITE_ERROR);

	g_return_val_if_fail (cFile->old_vfs_file->pMethods != NULL, SQLITE_ERROR);
	return cFile->old_vfs_file->pMethods->xSync (cFile->old_vfs_file, flags);
}

typedef struct {
	GCond cond;
	GMutex mutex;
	gboolean is_set;
} SyncDone;

struct SyncRequestData
{
	CamelSqlite3File *cFile;
	guint32 flags;
	SyncDone *done; /* not NULL when waiting for a finish; will be freed by the caller */
};

static void
sync_request_thread_cb (gpointer task_data,
                        gpointer null_data)
{
	struct SyncRequestData *sync_data = task_data;
	SyncDone *done;

	g_return_if_fail (sync_data != NULL);
	g_return_if_fail (sync_data->cFile != NULL);

	call_old_file_Sync (sync_data->cFile, sync_data->flags);

	g_mutex_lock (&sync_data->cFile->pending_syncs_lock);
	g_warn_if_fail (sync_data->cFile->pending_syncs > 0);
	sync_data->cFile->pending_syncs--;
	if (!sync_data->cFile->pending_syncs)
		g_cond_signal (&sync_data->cFile->pending_syncs_cond);
	g_mutex_unlock (&sync_data->cFile->pending_syncs_lock);

	done = sync_data->done;
	g_free (sync_data);

	if (done != NULL) {
		g_mutex_lock (&done->mutex);
		done->is_set = TRUE;
		g_cond_broadcast (&done->cond);
		g_mutex_unlock (&done->mutex);
	}
}

static void
sync_push_request (CamelSqlite3File *cFile,
                   gboolean wait_for_finish)
{
	struct SyncRequestData *data;
	SyncDone *done = NULL;
	GError *error = NULL;

	g_return_if_fail (cFile != NULL);
	g_return_if_fail (sync_pool != NULL);

	g_rec_mutex_lock (&cFile->sync_mutex);

	if (!cFile->flags) {
		/* nothing to sync, might be when xClose is called
		 * without any pending xSync request */
		g_rec_mutex_unlock (&cFile->sync_mutex);
		return;
	}

	if (wait_for_finish) {
		done = g_slice_new (SyncDone);
		g_cond_init (&done->cond);
		g_mutex_init (&done->mutex);
		done->is_set = FALSE;
	}

	data = g_new0 (struct SyncRequestData, 1);
	data->cFile = cFile;
	data->flags = cFile->flags;
	data->done = done;

	cFile->flags = 0;

	g_mutex_lock (&cFile->pending_syncs_lock);
	cFile->pending_syncs++;
	g_mutex_unlock (&cFile->pending_syncs_lock);

	g_rec_mutex_unlock (&cFile->sync_mutex);

	g_thread_pool_push (sync_pool, data, &error);

	if (error) {
		g_warning ("%s: Failed to push to thread pool: %s\n", G_STRFUNC, error->message);
		g_error_free (error);

		if (done != NULL) {
			g_cond_clear (&done->cond);
			g_mutex_clear (&done->mutex);
			g_slice_free (SyncDone, done);
		}

		return;
	}

	if (done != NULL) {
		g_mutex_lock (&done->mutex);
		while (!done->is_set)
			g_cond_wait (&done->cond, &done->mutex);
		g_mutex_unlock (&done->mutex);

		g_cond_clear (&done->cond);
		g_mutex_clear (&done->mutex);
		g_slice_free (SyncDone, done);
	}
}

static gboolean
sync_push_request_timeout (CamelSqlite3File *cFile)
{
	g_rec_mutex_lock (&cFile->sync_mutex);

	if (cFile->timeout_id != 0) {
		sync_push_request (cFile, FALSE);
		cFile->timeout_id = 0;
	}

	g_rec_mutex_unlock (&cFile->sync_mutex);

	return FALSE;
}

#define def_subclassed(_nm, _params, _call) \
static gint \
camel_sqlite3_file_ ## _nm _params \
{ \
	CamelSqlite3File *cFile; \
 \
	g_return_val_if_fail (old_vfs != NULL, SQLITE_ERROR); \
	g_return_val_if_fail (pFile != NULL, SQLITE_ERROR); \
 \
	cFile = (CamelSqlite3File *) pFile; \
	g_return_val_if_fail (cFile->old_vfs_file->pMethods != NULL, SQLITE_ERROR); \
	return cFile->old_vfs_file->pMethods->_nm _call; \
}

#define def_subclassed_void(_nm, _params, _call) \
static void \
camel_sqlite3_file_ ## _nm _params \
{ \
	CamelSqlite3File *cFile; \
 \
	g_return_if_fail (old_vfs != NULL); \
	g_return_if_fail (pFile != NULL); \
 \
	cFile = (CamelSqlite3File *) pFile; \
	g_return_if_fail (cFile->old_vfs_file->pMethods != NULL); \
	cFile->old_vfs_file->pMethods->_nm _call; \
}

def_subclassed (xRead, (sqlite3_file *pFile, gpointer pBuf, gint iAmt, sqlite3_int64 iOfst), (cFile->old_vfs_file, pBuf, iAmt, iOfst))
def_subclassed (xWrite, (sqlite3_file *pFile, gconstpointer pBuf, gint iAmt, sqlite3_int64 iOfst), (cFile->old_vfs_file, pBuf, iAmt, iOfst))
def_subclassed (xTruncate, (sqlite3_file *pFile, sqlite3_int64 size), (cFile->old_vfs_file, size))
def_subclassed (xFileSize, (sqlite3_file *pFile, sqlite3_int64 *pSize), (cFile->old_vfs_file, pSize))
def_subclassed (xLock, (sqlite3_file *pFile, gint lockType), (cFile->old_vfs_file, lockType))
def_subclassed (xUnlock, (sqlite3_file *pFile, gint lockType), (cFile->old_vfs_file, lockType))
def_subclassed (xFileControl, (sqlite3_file *pFile, gint op, gpointer pArg), (cFile->old_vfs_file, op, pArg))
def_subclassed (xSectorSize, (sqlite3_file *pFile), (cFile->old_vfs_file))
def_subclassed (xDeviceCharacteristics, (sqlite3_file *pFile), (cFile->old_vfs_file))
def_subclassed (xShmMap, (sqlite3_file *pFile, gint iPg, gint pgsz, gint n, void volatile **arr), (cFile->old_vfs_file, iPg, pgsz, n, arr))
def_subclassed (xShmLock, (sqlite3_file *pFile, gint offset, gint n, gint flags), (cFile->old_vfs_file, offset, n, flags))
def_subclassed_void (xShmBarrier, (sqlite3_file *pFile), (cFile->old_vfs_file))
def_subclassed (xShmUnmap, (sqlite3_file *pFile, gint deleteFlag), (cFile->old_vfs_file, deleteFlag))
def_subclassed (xFetch, (sqlite3_file *pFile, sqlite3_int64 iOfst, int iAmt, void **pp), (cFile->old_vfs_file, iOfst, iAmt, pp))
def_subclassed (xUnfetch, (sqlite3_file *pFile, sqlite3_int64 iOfst, void *p), (cFile->old_vfs_file, iOfst, p))

#undef def_subclassed

static gint
camel_sqlite3_file_xCheckReservedLock (sqlite3_file *pFile,
                                       gint *pResOut)
{
	CamelSqlite3File *cFile;

	g_return_val_if_fail (old_vfs != NULL, SQLITE_ERROR);
	g_return_val_if_fail (pFile != NULL, SQLITE_ERROR);

	cFile = (CamelSqlite3File *) pFile;
	g_return_val_if_fail (cFile->old_vfs_file->pMethods != NULL, SQLITE_ERROR);

	/* check version in runtime */
	if (sqlite3_libversion_number () < 3006000)
		return ((gint (*)(sqlite3_file *)) (cFile->old_vfs_file->pMethods->xCheckReservedLock)) (cFile->old_vfs_file);
	else
		return ((gint (*)(sqlite3_file *, gint *)) (cFile->old_vfs_file->pMethods->xCheckReservedLock)) (cFile->old_vfs_file, pResOut);
}

static gint
camel_sqlite3_file_xClose (sqlite3_file *pFile)
{
	CamelSqlite3File *cFile;
	gint res;

	g_return_val_if_fail (old_vfs != NULL, SQLITE_ERROR);
	g_return_val_if_fail (pFile != NULL, SQLITE_ERROR);

	cFile = (CamelSqlite3File *) pFile;

	g_rec_mutex_lock (&cFile->sync_mutex);

	/* Cancel any pending sync requests. */
	if (cFile->timeout_id > 0) {
		g_source_remove (cFile->timeout_id);
		cFile->timeout_id = 0;
	}

	g_rec_mutex_unlock (&cFile->sync_mutex);

	/* Make the last sync. */
	sync_push_request (cFile, TRUE);

	g_mutex_lock (&cFile->pending_syncs_lock);
	while (cFile->pending_syncs > 0) {
		g_cond_wait (&cFile->pending_syncs_cond, &cFile->pending_syncs_lock);
	}
	g_mutex_unlock (&cFile->pending_syncs_lock);

	if (cFile->old_vfs_file->pMethods)
		res = cFile->old_vfs_file->pMethods->xClose (cFile->old_vfs_file);
	else
		res = SQLITE_OK;

	g_free (cFile->old_vfs_file);
	cFile->old_vfs_file = NULL;

	g_rec_mutex_clear (&cFile->sync_mutex);
	g_mutex_clear (&cFile->pending_syncs_lock);
	g_cond_clear (&cFile->pending_syncs_cond);

	return res;
}

static gint
camel_sqlite3_file_xSync (sqlite3_file *pFile,
                          gint flags)
{
	CamelSqlite3File *cFile;

	g_return_val_if_fail (old_vfs != NULL, SQLITE_ERROR);
	g_return_val_if_fail (pFile != NULL, SQLITE_ERROR);

	cFile = (CamelSqlite3File *) pFile;

	g_rec_mutex_lock (&cFile->sync_mutex);

	/* If a sync request is already scheduled, accumulate flags. */
	cFile->flags |= flags;

	/* Cancel any pending sync requests. */
	if (cFile->timeout_id > 0)
		g_source_remove (cFile->timeout_id);

	/* Wait SYNC_TIMEOUT_SECONDS before we actually sync. */
	cFile->timeout_id = g_timeout_add_seconds (
		SYNC_TIMEOUT_SECONDS, (GSourceFunc)
		sync_push_request_timeout, cFile);
	g_source_set_name_by_id (
		cFile->timeout_id,
		"[camel] sync_push_request_timeout");

	g_rec_mutex_unlock (&cFile->sync_mutex);

	return SQLITE_OK;
}

static gint
camel_sqlite3_vfs_xOpen (sqlite3_vfs *pVfs,
                         const gchar *zPath,
                         sqlite3_file *pFile,
                         gint flags,
                         gint *pOutFlags)
{
	static GRecMutex only_once_lock;
	static sqlite3_io_methods io_methods = {0};
	CamelSqlite3File *cFile;
	gint res;

	g_return_val_if_fail (old_vfs != NULL, -1);
	g_return_val_if_fail (pFile != NULL, -1);

	cFile = (CamelSqlite3File *) pFile;
	cFile->old_vfs_file = g_malloc0 (old_vfs->szOsFile);

	res = old_vfs->xOpen (old_vfs, zPath, cFile->old_vfs_file, flags, pOutFlags);
	if (res != SQLITE_OK) {
		g_free (cFile->old_vfs_file);
		return res;
	}

	g_rec_mutex_init (&cFile->sync_mutex);
	g_mutex_init (&cFile->pending_syncs_lock);
	g_cond_init (&cFile->pending_syncs_cond);

	cFile->pending_syncs = 0;

	g_rec_mutex_lock (&only_once_lock);

	if (!sync_pool)
		sync_pool = g_thread_pool_new (sync_request_thread_cb, NULL, 2, FALSE, NULL);

	/* cFile->old_vfs_file->pMethods is NULL when open failed for some reason,
	 * thus do not initialize our structure when do not know the version */
	if (io_methods.xClose == NULL && cFile->old_vfs_file->pMethods) {
		/* initialize our subclass function only once */
		io_methods.iVersion = cFile->old_vfs_file->pMethods->iVersion;

		/* check version in compile time */
		#if SQLITE_VERSION_NUMBER < 3006000
		io_methods.xCheckReservedLock = (gint (*)(sqlite3_file *)) camel_sqlite3_file_xCheckReservedLock;
		#else
		io_methods.xCheckReservedLock = camel_sqlite3_file_xCheckReservedLock;
		#endif

		#define use_subclassed(x) io_methods.x = camel_sqlite3_file_ ## x
		use_subclassed (xClose);
		use_subclassed (xRead);
		use_subclassed (xWrite);
		use_subclassed (xTruncate);
		use_subclassed (xSync);
		use_subclassed (xFileSize);
		use_subclassed (xLock);
		use_subclassed (xUnlock);
		use_subclassed (xFileControl);
		use_subclassed (xSectorSize);
		use_subclassed (xDeviceCharacteristics);

		if (io_methods.iVersion > 1) {
			use_subclassed (xShmMap);
			use_subclassed (xShmLock);
			use_subclassed (xShmBarrier);
			use_subclassed (xShmUnmap);
		}

		if (io_methods.iVersion > 2) {
			use_subclassed (xFetch);
			use_subclassed (xUnfetch);
		}

		if (io_methods.iVersion > 3) {
			g_warning ("%s: Unchecked IOMethods version %d, downgrading to version 3", G_STRFUNC, io_methods.iVersion);
			io_methods.iVersion = 3;
		}
		#undef use_subclassed
	}

	g_rec_mutex_unlock (&only_once_lock);

	cFile->parent.pMethods = &io_methods;

	return res;
}

static gpointer
init_sqlite_vfs (void)
{
	static sqlite3_vfs vfs = { 0 };

	old_vfs = sqlite3_vfs_find (NULL);
	g_return_val_if_fail (old_vfs != NULL, NULL);

	memcpy (&vfs, old_vfs, sizeof (sqlite3_vfs));

	vfs.szOsFile = sizeof (CamelSqlite3File);
	vfs.zName = "camel_sqlite3_vfs";
	vfs.xOpen = camel_sqlite3_vfs_xOpen;

	sqlite3_vfs_register (&vfs, 1);

	return NULL;
}

#define d(x) if (camel_debug("sqlite")) x
#define START(stmt) \
	if (camel_debug ("dbtime")) { \
		g_print ( \
			"\n===========\n" \
			"DB SQL operation [%s] started\n", stmt); \
		if (!cdb->priv->timer) { \
			cdb->priv->timer = g_timer_new (); \
		} else { \
			g_timer_reset (cdb->priv->timer); \
		} \
	}
#define END \
	if (camel_debug ("dbtime")) { \
		g_timer_stop (cdb->priv->timer); \
		g_print ( \
			"DB Operation ended. " \
			"Time Taken : %f\n###########\n", \
			g_timer_elapsed (cdb->priv->timer, NULL)); \
	}
#define STARTTS(stmt) \
	if (camel_debug ("dbtimets")) { \
		g_print ( \
			"\n===========\n" \
			"DB SQL operation [%s] started\n", stmt); \
		if (!cdb->priv->timer) { \
			cdb->priv->timer = g_timer_new (); \
		} else { \
			g_timer_reset (cdb->priv->timer); \
		} \
	}
#define ENDTS \
	if (camel_debug ("dbtimets")) { \
		g_timer_stop (cdb->priv->timer); \
		g_print ( \
			"DB Operation ended. " \
			"Time Taken : %f\n###########\n", \
			g_timer_elapsed (cdb->priv->timer, NULL)); \
	}

struct _CamelDBPrivate {
	GTimer *timer;
	GRWLock rwlock;
	gchar *file_name;
	GMutex transaction_lock;
	GThread *transaction_thread;
	guint32 transaction_level;
};

/**
 * cdb_sql_exec 
 * @db: 
 * @stmt: 
 * @error: 
 * 
 * Callers should hold the lock
 **/
static gint
cdb_sql_exec (sqlite3 *db,
              const gchar *stmt,
              gint (*callback)(gpointer ,gint,gchar **,gchar **),
              gpointer data,
	      gint *out_sqlite_error_code,
              GError **error)
{
	gchar *errmsg = NULL;
	gint   ret = -1, retries = 0;

	d (g_print ("Camel SQL Exec:\n%s\n", stmt));

	ret = sqlite3_exec (db, stmt, callback, data, &errmsg);
	while (ret == SQLITE_BUSY || ret == SQLITE_LOCKED || ret == -1) {
		/* try for ~15 seconds, then give up */
		if (retries > 150)
			break;
		retries++;

		if (errmsg) {
			sqlite3_free (errmsg);
			errmsg = NULL;
		}
		g_thread_yield ();
		g_usleep (100 * 1000); /* Sleep for 100 ms */

		ret = sqlite3_exec (db, stmt, NULL, NULL, &errmsg);
	}

	if (out_sqlite_error_code)
		*out_sqlite_error_code = ret;

	if (ret != SQLITE_OK) {
		d (g_print ("Error in SQL EXEC statement: %s [%s].\n", stmt, errmsg));
		g_set_error (
			error, CAMEL_ERROR,
			CAMEL_ERROR_GENERIC, "%s", errmsg);
		sqlite3_free (errmsg);
		errmsg = NULL;
		return -1;
	}

	if (errmsg) {
		sqlite3_free (errmsg);
		errmsg = NULL;
	}

	return 0;
}

/* checks whether string 'where' contains whole word 'what',
 * case insensitively (ascii, not utf8, same as 'LIKE' in SQLite3)
*/
static void
cdb_match_func (sqlite3_context *ctx,
                gint nArgs,
                sqlite3_value **values)
{
	gboolean matches = FALSE;
	const gchar *what, *where;

	g_return_if_fail (ctx != NULL);
	g_return_if_fail (nArgs == 2);
	g_return_if_fail (values != NULL);

	what = (const gchar *) sqlite3_value_text (values[0]);
	where = (const gchar *) sqlite3_value_text (values[1]);

	if (what && where && !*what) {
		matches = TRUE;
	} else if (what && where) {
		gboolean word = TRUE;
		gint i, j;

		for (i = 0, j = 0; where[i] && !matches; i++) {
			gchar c = where[i];

			if (c == ' ') {
				word = TRUE;
				j = 0;
			} else if (word && tolower (c) == tolower (what[j])) {
				j++;
				if (what[j] == 0 && (where[i + 1] == 0 || isspace (where[i + 1])))
					matches = TRUE;
			} else {
				word = FALSE;
			}
		}
	}

	sqlite3_result_int (ctx, matches ? 1 : 0);
}

static void
cdb_writer_lock (CamelDB *cdb)
{
	g_return_if_fail (cdb != NULL);

	g_mutex_lock (&cdb->priv->transaction_lock);
	if (cdb->priv->transaction_thread != g_thread_self ()) {
		g_mutex_unlock (&cdb->priv->transaction_lock);

		g_rw_lock_writer_lock (&cdb->priv->rwlock);

		g_mutex_lock (&cdb->priv->transaction_lock);

		g_warn_if_fail (cdb->priv->transaction_thread == NULL);
		g_warn_if_fail (cdb->priv->transaction_level == 0);

		cdb->priv->transaction_thread = g_thread_self ();
	}

	cdb->priv->transaction_level++;

	g_mutex_unlock (&cdb->priv->transaction_lock);
}

static void
cdb_writer_unlock (CamelDB *cdb)
{
	g_return_if_fail (cdb != NULL);

	g_mutex_lock (&cdb->priv->transaction_lock);

	g_warn_if_fail (cdb->priv->transaction_thread == g_thread_self ());
	g_warn_if_fail (cdb->priv->transaction_level > 0);

	cdb->priv->transaction_level--;

	if (!cdb->priv->transaction_level) {
		cdb->priv->transaction_thread = NULL;
		g_mutex_unlock (&cdb->priv->transaction_lock);

		g_rw_lock_writer_unlock (&cdb->priv->rwlock);
	} else {
		g_mutex_unlock (&cdb->priv->transaction_lock);
	}
}

static void
cdb_reader_lock (CamelDB *cdb)
{
	g_return_if_fail (cdb != NULL);

	g_mutex_lock (&cdb->priv->transaction_lock);
	if (cdb->priv->transaction_thread == g_thread_self ()) {
		/* already holding write lock */
		g_mutex_unlock (&cdb->priv->transaction_lock);
	} else {
		g_mutex_unlock (&cdb->priv->transaction_lock);

		g_rw_lock_reader_lock (&cdb->priv->rwlock);
	}
}

static void
cdb_reader_unlock (CamelDB *cdb)
{
	g_return_if_fail (cdb != NULL);

	g_mutex_lock (&cdb->priv->transaction_lock);
	if (cdb->priv->transaction_thread == g_thread_self ()) {
		/* already holding write lock */
		g_mutex_unlock (&cdb->priv->transaction_lock);
	} else {
		g_mutex_unlock (&cdb->priv->transaction_lock);

		g_rw_lock_reader_unlock (&cdb->priv->rwlock);
	}
}

static gboolean
cdb_is_in_transaction (CamelDB *cdb)
{
	gboolean res;

	g_return_val_if_fail (cdb != NULL, FALSE);

	g_mutex_lock (&cdb->priv->transaction_lock);
	res = cdb->priv->transaction_level > 0 && cdb->priv->transaction_thread == g_thread_self ();
	g_mutex_unlock (&cdb->priv->transaction_lock);

	return res;
}

static gchar *
cdb_construct_transaction_stmt (CamelDB *cdb,
				const gchar *prefix)
{
	gchar *name;

	g_return_val_if_fail (cdb != NULL, NULL);

	g_mutex_lock (&cdb->priv->transaction_lock);
	g_warn_if_fail (cdb->priv->transaction_thread == g_thread_self ());
	name = g_strdup_printf ("%sTN%d", prefix ? prefix : "", cdb->priv->transaction_level);
	g_mutex_unlock (&cdb->priv->transaction_lock);

	return name;
}

static gint
camel_db_command_internal (CamelDB *cdb,
			   const gchar *stmt,
			   gint *out_sqlite_error_code,
			   GError **error)
{
	gint ret;

	if (!cdb)
		return TRUE;

	cdb_writer_lock (cdb);

	START (stmt);
	ret = cdb_sql_exec (cdb->db, stmt, NULL, NULL, out_sqlite_error_code, error);
	END;

	cdb_writer_unlock (cdb);

	return ret;
}

/**
 * camel_db_open:
 *
 * Since: 2.24
 **/
CamelDB *
camel_db_open (const gchar *path,
               GError **error)
{
	static GOnce vfs_once = G_ONCE_INIT;
	CamelDB *cdb;
	sqlite3 *db;
	gint ret, cdb_sqlite_error_code = SQLITE_OK;
	gboolean reopening = FALSE;
	GError *local_error = NULL;

	g_once (&vfs_once, (GThreadFunc) init_sqlite_vfs, NULL);

	CAMEL_DB_USE_SHARED_CACHE;

 reopen:
	ret = sqlite3_open (path, &db);
	if (ret) {
		if (!db) {
			g_set_error (
				error, CAMEL_ERROR,
				CAMEL_ERROR_GENERIC,
				_("Insufficient memory"));
		} else {
			const gchar *errmsg;
			errmsg = sqlite3_errmsg (db);
			d (g_print ("Can't open database %s: %s\n", path, errmsg));
			g_set_error (
				error, CAMEL_ERROR,
				CAMEL_ERROR_GENERIC, "%s", errmsg);
			sqlite3_close (db);
		}
		return NULL;
	}

	cdb = g_new (CamelDB, 1);
	cdb->db = db;
	cdb->priv = g_new0 (CamelDBPrivate, 1);
	cdb->priv->file_name = g_strdup (path);
	g_rw_lock_init (&cdb->priv->rwlock);
	g_mutex_init (&cdb->priv->transaction_lock);
	cdb->priv->transaction_thread = NULL;
	cdb->priv->transaction_level = 0;
	cdb->priv->timer = NULL;
	d (g_print ("\nDatabase succesfully opened  \n"));

	sqlite3_create_function (db, "MATCH", 2, SQLITE_UTF8, NULL, cdb_match_func, NULL, NULL);

	/* Which is big / costlier ? A Stack frame or a pointer */
	if (g_getenv ("CAMEL_SQLITE_DEFAULT_CACHE_SIZE") != NULL) {
		gchar *cache = NULL;

		cache = g_strdup_printf ("PRAGMA cache_size=%s", g_getenv ("CAMEL_SQLITE_DEFAULT_CACHE_SIZE"));
		camel_db_command_internal (cdb, cache, &cdb_sqlite_error_code, &local_error);
		g_free (cache);
	}

	if (cdb_sqlite_error_code == SQLITE_OK)
		camel_db_command_internal (cdb, "ATTACH DATABASE ':memory:' AS mem", &cdb_sqlite_error_code, &local_error);

	if (cdb_sqlite_error_code == SQLITE_OK && g_getenv ("CAMEL_SQLITE_IN_MEMORY") != NULL) {
		/* Optionally turn off Journaling, this gets over fsync issues, but could be risky */
		camel_db_command_internal (cdb, "PRAGMA main.journal_mode = off", &cdb_sqlite_error_code, &local_error);
		if (cdb_sqlite_error_code == SQLITE_OK)
			camel_db_command_internal (cdb, "PRAGMA temp_store = memory", &cdb_sqlite_error_code, &local_error);
	}

	if (!reopening && (
	    cdb_sqlite_error_code == SQLITE_CANTOPEN ||
	    cdb_sqlite_error_code == SQLITE_CORRUPT ||
	    cdb_sqlite_error_code == SQLITE_NOTADB)) {
		gchar *second_filename;

		camel_db_close (cdb);

		reopening = TRUE;

		second_filename = g_strconcat (path, ".corrupt", NULL);
		if (g_rename (path, second_filename) == -1) {
			if (!local_error) {
				g_set_error (&local_error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
					_("Could not rename '%s' to %s: %s"),
					path, second_filename, g_strerror (errno));
			}

			g_propagate_error (error, local_error);

			g_free (second_filename);

			return NULL;
		}

		g_free (second_filename);

		g_warning ("%s: Failed to open '%s', renamed old file to .corrupt; code:%s (%d) error:%s", G_STRFUNC, path,
			cdb_sqlite_error_code == SQLITE_CANTOPEN ? "SQLITE_CANTOPEN" :
			cdb_sqlite_error_code == SQLITE_CORRUPT ? "SQLITE_CORRUPT" :
			cdb_sqlite_error_code == SQLITE_NOTADB ? "SQLITE_NOTADB" : "???",
			cdb_sqlite_error_code, local_error ? local_error->message : "Unknown error");

		g_clear_error (&local_error);

		goto reopen;
	}

	if (local_error) {
		g_propagate_error (error, local_error);
		camel_db_close (cdb);
		return NULL;
	}

	sqlite3_busy_timeout (cdb->db, CAMEL_DB_SLEEP_INTERVAL);

	return cdb;
}

/**
 * camel_db_clone:
 *
 * Since: 2.26
 **/
CamelDB *
camel_db_clone (CamelDB *cdb,
                GError **error)
{
	return camel_db_open (cdb->priv->file_name, error);
}

/**
 * camel_db_close:
 *
 * Since: 2.24
 **/
void
camel_db_close (CamelDB *cdb)
{
	if (cdb) {
		sqlite3_close (cdb->db);
		g_rw_lock_clear (&cdb->priv->rwlock);
		g_mutex_clear (&cdb->priv->transaction_lock);
		g_free (cdb->priv->file_name);
		g_free (cdb->priv);
		g_free (cdb);
		d (g_print ("\nDatabase succesfully closed \n"));
	}
}

/**
 * camel_db_set_collate:
 * @func: (scope call):
 *
 * Since: 2.24
 **/
gint
camel_db_set_collate (CamelDB *cdb,
                      const gchar *col,
                      const gchar *collate,
                      CamelDBCollate func)
{
		gint ret = 0;

		if (!cdb)
			return 0;

		cdb_writer_lock (cdb);
		d (g_print ("Creating Collation %s on %s with %p\n", collate, col, (gpointer) func));
		if (collate && func)
			ret = sqlite3_create_collation (cdb->db, collate, SQLITE_UTF8,  NULL, func);
		cdb_writer_unlock (cdb);

		return ret;
}

/**
 * camel_db_command:
 *
 * Since: 2.24
 **/
gint
camel_db_command (CamelDB *cdb,
                  const gchar *stmt,
                  GError **error)
{
	return camel_db_command_internal (cdb, stmt, NULL, error);
}

/**
 * camel_db_begin_transaction:
 *
 * Since: 2.24
 **/
gint
camel_db_begin_transaction (CamelDB *cdb,
                            GError **error)
{
	gchar *stmt;
	gint res;

	if (!cdb)
		return -1;

	cdb_writer_lock (cdb);

	stmt = cdb_construct_transaction_stmt (cdb, "SAVEPOINT ");

	STARTTS (stmt);
	res = cdb_sql_exec (cdb->db, stmt, NULL, NULL, NULL, error);
	g_free (stmt);

	return res;
}

/**
 * camel_db_end_transaction:
 *
 * Since: 2.24
 **/
gint
camel_db_end_transaction (CamelDB *cdb,
                          GError **error)
{
	gchar *stmt;
	gint ret;

	if (!cdb)
		return -1;

	stmt = cdb_construct_transaction_stmt (cdb, "RELEASE SAVEPOINT ");
	ret = cdb_sql_exec (cdb->db, stmt, NULL, NULL, NULL, error);
	g_free (stmt);

	ENDTS;
	cdb_writer_unlock (cdb);
	CAMEL_DB_RELEASE_SQLITE_MEMORY;

	return ret;
}

/**
 * camel_db_abort_transaction:
 *
 * Since: 2.24
 **/
gint
camel_db_abort_transaction (CamelDB *cdb,
                            GError **error)
{
	gchar *stmt;
	gint ret;

	stmt = cdb_construct_transaction_stmt (cdb, "ROLLBACK TO SAVEPOINT ");
	ret = cdb_sql_exec (cdb->db, stmt, NULL, NULL, NULL, error);
	g_free (stmt);

	cdb_writer_unlock (cdb);
	CAMEL_DB_RELEASE_SQLITE_MEMORY;

	return ret;
}

/**
 * camel_db_add_to_transaction:
 *
 * Since: 2.24
 **/
gint
camel_db_add_to_transaction (CamelDB *cdb,
                             const gchar *stmt,
                             GError **error)
{
	if (!cdb)
		return -1;

	g_return_val_if_fail (cdb_is_in_transaction (cdb), -1);

	return (cdb_sql_exec (cdb->db, stmt, NULL, NULL, NULL, error));
}

/**
 * camel_db_transaction_command:
 * @cdb: a #CamelDB
 * @qry_list: (element-type utf8) (transfer none): A #GList of querries
 *
 * Since: 2.24
 **/
gint
camel_db_transaction_command (CamelDB *cdb,
                              GList *qry_list,
                              GError **error)
{
	gboolean in_transaction = FALSE;
	gint ret;
	const gchar *query;

	if (!cdb)
		return -1;

	ret = camel_db_begin_transaction (cdb, error);
	if (ret)
		goto end;

	in_transaction = TRUE;

	while (qry_list) {
		query = qry_list->data;
		ret = cdb_sql_exec (cdb->db, query, NULL, NULL, NULL, error);
		if (ret)
			goto end;
		qry_list = g_list_next (qry_list);
	}

	ret = camel_db_end_transaction (cdb, error);
	in_transaction = FALSE;
end:
	if (in_transaction)
		ret = camel_db_abort_transaction (cdb, error);

	return ret;
}

static gint
count_cb (gpointer data,
          gint argc,
          gchar **argv,
          gchar **azColName)
{
	gint i;

	for (i = 0; i < argc; i++) {
		if (strstr (azColName[i], "COUNT")) {
			*(guint32 *)data = argv [i] ? strtoul (argv [i], NULL, 10) : 0;
		}
	}

	return 0;
}

/**
 * camel_db_count_message_info:
 *
 * Since: 2.26
 **/
gint
camel_db_count_message_info (CamelDB *cdb,
                             const gchar *query,
                             guint32 *count,
                             GError **error)
{
	gint ret = -1;

	cdb_reader_lock (cdb);

	START (query);
	ret = cdb_sql_exec (cdb->db, query, count_cb, count, NULL, error);
	END;

	cdb_reader_unlock (cdb);

	CAMEL_DB_RELEASE_SQLITE_MEMORY;

	return ret;
}

/**
 * camel_db_count_junk_message_info:
 *
 * Since: 2.24
 **/
gint
camel_db_count_junk_message_info (CamelDB *cdb,
                                  const gchar *table_name,
                                  guint32 *count,
                                  GError **error)
{
	gint ret;
	gchar *query;

	if (!cdb)
		return -1;

	query = sqlite3_mprintf ("SELECT COUNT (*) FROM %Q WHERE junk = 1", table_name);

	ret = camel_db_count_message_info (cdb, query, count, error);
	sqlite3_free (query);

	return ret;
}

/**
 * camel_db_count_unread_message_info:
 *
 * Since: 2.24
 **/
gint
camel_db_count_unread_message_info (CamelDB *cdb,
                                    const gchar *table_name,
                                    guint32 *count,
                                    GError **error)
{
	gint ret;
	gchar *query;

	if (!cdb)
		return -1;

	query = sqlite3_mprintf ("SELECT COUNT (*) FROM %Q WHERE read = 0", table_name);

	ret = camel_db_count_message_info (cdb, query, count, error);
	sqlite3_free (query);

	return ret;
}

/**
 * camel_db_count_visible_unread_message_info:
 *
 * Since: 2.24
 **/
gint
camel_db_count_visible_unread_message_info (CamelDB *cdb,
                                            const gchar *table_name,
                                            guint32 *count,
                                            GError **error)
{
	gint ret;
	gchar *query;

	if (!cdb)
		return -1;

	query = sqlite3_mprintf ("SELECT COUNT (*) FROM %Q WHERE read = 0 AND junk = 0 AND deleted = 0", table_name);

	ret = camel_db_count_message_info (cdb, query, count, error);
	sqlite3_free (query);

	return ret;
}

/**
 * camel_db_count_visible_message_info:
 *
 * Since: 2.24
 **/
gint
camel_db_count_visible_message_info (CamelDB *cdb,
                                     const gchar *table_name,
                                     guint32 *count,
                                     GError **error)
{
	gint ret;
	gchar *query;

	if (!cdb)
		return -1;

	query = sqlite3_mprintf ("SELECT COUNT (*) FROM %Q WHERE junk = 0 AND deleted = 0", table_name);

	ret = camel_db_count_message_info (cdb, query, count, error);
	sqlite3_free (query);

	return ret;
}

/**
 * camel_db_count_junk_not-deleted_message_info:
 *
 * Since: 2.24
 **/
gint
camel_db_count_junk_not_deleted_message_info (CamelDB *cdb,
                                              const gchar *table_name,
                                              guint32 *count,
                                              GError **error)
{
	gint ret;
	gchar *query;

	if (!cdb)
		return -1;

	query = sqlite3_mprintf ("SELECT COUNT (*) FROM %Q WHERE junk = 1 AND deleted = 0", table_name);

	ret = camel_db_count_message_info (cdb, query, count, error);
	sqlite3_free (query);

	return ret;
}

/**
 * camel_db_count_deleted_message_info:
 *
 * Since: 2.24
 **/
gint
camel_db_count_deleted_message_info (CamelDB *cdb,
                                     const gchar *table_name,
                                     guint32 *count,
                                     GError **error)
{
	gint ret;
	gchar *query;

	if (!cdb)
		return -1;

	query = sqlite3_mprintf ("SELECT COUNT (*) FROM %Q WHERE deleted = 1", table_name);

	ret = camel_db_count_message_info (cdb, query, count, error);
	sqlite3_free (query);

	return ret;
}

/**
 * camel_db_count_total_message_info:
 *
 * Since: 2.24
 **/
gint
camel_db_count_total_message_info (CamelDB *cdb,
                                   const gchar *table_name,
                                   guint32 *count,
                                   GError **error)
{

	gint ret;
	gchar *query;

	if (!cdb)
		return -1;

	query = sqlite3_mprintf ("SELECT COUNT (*) FROM %Q where read=0 or read=1", table_name);

	ret = camel_db_count_message_info (cdb, query, count, error);
	sqlite3_free (query);

	return ret;
}

/**
 * camel_db_select:
 * @callback: (scope async):
 *
 * Since: 2.24
 **/
gint
camel_db_select (CamelDB *cdb,
                 const gchar *stmt,
                 CamelDBSelectCB callback,
                 gpointer user_data,
                 GError **error)
{
	gint ret = -1;

	if (!cdb)
		return ret;

	d (g_print ("\n%s:\n%s \n", G_STRFUNC, stmt));
	cdb_reader_lock (cdb);

	START (stmt);
	ret = cdb_sql_exec (cdb->db, stmt, callback, user_data, NULL, error);
	END;

	cdb_reader_unlock (cdb);
	CAMEL_DB_RELEASE_SQLITE_MEMORY;

	return ret;
}

static gint
read_uids_callback (gpointer ref_array,
                    gint ncol,
                    gchar **cols,
                    gchar **name)
{
	GPtrArray *array = ref_array;

	g_return_val_if_fail (ncol == 1, 0);

	if (cols[0])
		g_ptr_array_add (array, (gchar *) (camel_pstring_strdup (cols[0])));

	return 0;
}

static gint
read_uids_to_hash_callback (gpointer ref_hash,
                            gint ncol,
                            gchar **cols,
                            gchar **name)
{
	GHashTable *hash = ref_hash;

	g_return_val_if_fail (ncol == 2, 0);

	if (cols[0])
		g_hash_table_insert (hash, (gchar *) camel_pstring_strdup (cols[0]), GUINT_TO_POINTER (cols[1] ? strtoul (cols[1], NULL, 10) : 0));

	return 0;
}

/**
 * camel_db_get_folder_uids:
 * @db: 
 * @folder_name: 
 * @sort_by: 
 * @collate: 
 * @hash: (element-type utf8 uint):
 * @error: 
 *
 * Fills hash with uid->GUINT_TO_POINTER (flag)
 *
 * Since: 2.24
 **/
gint
camel_db_get_folder_uids (CamelDB *db,
                          const gchar *folder_name,
                          const gchar *sort_by,
                          const gchar *collate,
                          GHashTable *hash,
                          GError **error)
{
	 gchar *sel_query;
	 gint ret;

		sel_query = sqlite3_mprintf (
		"SELECT uid,flags FROM %Q%s%s%s%s",
		folder_name,
		sort_by ? " order by " : "",
		sort_by ? sort_by : "",
		(sort_by && collate) ? " collate " : "",
		(sort_by && collate) ? collate : "");

	 ret = camel_db_select (db, sel_query, read_uids_to_hash_callback, hash, error);
	 sqlite3_free (sel_query);

	 return ret;
}

/**
 * camel_db_get_folder_junk_uids:
 *
 * Returns: (element-type utf8) (transfer full):
 *
 * Since: 2.24
 **/
GPtrArray *
camel_db_get_folder_junk_uids (CamelDB *db,
                               gchar *folder_name,
                               GError **error)
{
	 gchar *sel_query;
	 gint ret;
	 GPtrArray *array = g_ptr_array_new ();

	 sel_query = sqlite3_mprintf ("SELECT uid FROM %Q where junk=1", folder_name);

	 ret = camel_db_select (db, sel_query, read_uids_callback, array, error);

	 sqlite3_free (sel_query);

	 if (!array->len || ret != 0) {
		 g_ptr_array_free (array, TRUE);
		 array = NULL;
	 }
	 return array;
}

/**
 * camel_db_get_folder_deleted_uids:
 *
 * Returns: (element-type utf8) (transfer full):
 *
 * Since: 2.24
 **/
GPtrArray *
camel_db_get_folder_deleted_uids (CamelDB *db,
                                  const gchar *folder_name,
                                  GError **error)
{
	 gchar *sel_query;
	 gint ret;
	 GPtrArray *array = g_ptr_array_new ();

	 sel_query = sqlite3_mprintf ("SELECT uid FROM %Q where deleted=1", folder_name);

	 ret = camel_db_select (db, sel_query, read_uids_callback, array, error);
	 sqlite3_free (sel_query);

	 if (!array->len || ret != 0) {
		 g_ptr_array_free (array, TRUE);
		 array = NULL;
	 }

	 return array;
}

struct ReadPreviewData
{
	GHashTable *columns_hash;
	GHashTable *hash;
};

static gint
read_preview_callback (gpointer ref,
                       gint ncol,
                       gchar **cols,
                       gchar **name)
{
	struct ReadPreviewData *rpd = ref;
	const gchar *uid = NULL;
	gchar *msg = NULL;
	gint i;

	for (i = 0; i < ncol; ++i) {
		if (!name[i] || !cols[i])
			continue;

		switch (camel_db_get_column_ident (&rpd->columns_hash, i, ncol, name)) {
			case CAMEL_DB_COLUMN_UID:
				uid = camel_pstring_strdup (cols[i]);
				break;
			case CAMEL_DB_COLUMN_PREVIEW:
				msg = g_strdup (cols[i]);
				break;
			default:
				g_warn_if_reached ();
				break;
		}
	}

	g_hash_table_insert (rpd->hash, (gchar *) uid, msg);

	return 0;
}

/**
 * camel_db_get_folder_preview:
 *
 * Returns: (element-type utf8 utf8) (transfer full):
 *
 * Since: 2.28
 **/
GHashTable *
camel_db_get_folder_preview (CamelDB *db,
                             const gchar *folder_name,
                             GError **error)
{
	gchar *sel_query;
	gint ret;
	struct ReadPreviewData rpd;
	GHashTable *hash = g_hash_table_new (g_str_hash, g_str_equal);

	sel_query = sqlite3_mprintf ("SELECT uid, preview FROM '%q_preview'", folder_name);

	rpd.columns_hash = NULL;
	rpd.hash = hash;

	ret = camel_db_select (db, sel_query, read_preview_callback, &rpd, error);
	sqlite3_free (sel_query);

	if (rpd.columns_hash)
		g_hash_table_destroy (rpd.columns_hash);

	if (!g_hash_table_size (hash) || ret != 0) {
		g_hash_table_destroy (hash);
		hash = NULL;
	}

	return hash;
}

/**
 * camel_db_write_preview_record:
 *
 * Since: 2.28
 **/
gint
camel_db_write_preview_record (CamelDB *db,
                               const gchar *folder_name,
                               const gchar *uid,
                               const gchar *msg,
                               GError **error)
{
	gchar *query;
	gint ret;

	query = sqlite3_mprintf ("INSERT OR REPLACE INTO '%q_preview' VALUES(%Q,%Q)", folder_name, uid, msg);

	ret = camel_db_add_to_transaction (db, query, error);
	sqlite3_free (query);

	return ret;
}

/**
 * camel_db_create_folders_table:
 *
 * Since: 2.24
 **/
gint
camel_db_create_folders_table (CamelDB *cdb,
                               GError **error)
{
	const gchar *query = "CREATE TABLE IF NOT EXISTS folders ( "
		"folder_name TEXT PRIMARY KEY, "
		"version REAL, "
		"flags INTEGER, "
		"nextuid INTEGER, "
		"time NUMERIC, "
		"saved_count INTEGER, "
		"unread_count INTEGER, "
		"deleted_count INTEGER, "
		"junk_count INTEGER, "
		"visible_count INTEGER, "
		"jnd_count INTEGER, "
		"bdata TEXT )";
	CAMEL_DB_RELEASE_SQLITE_MEMORY;
	return ((camel_db_command (cdb, query, error)));
}

static gint
camel_db_create_message_info_table (CamelDB *cdb,
                                    const gchar *folder_name,
                                    GError **error)
{
	gint ret;
	gchar *table_creation_query, *safe_index;

	/* README: It is possible to compress all system flags into a single
	 * column and use just as userflags but that makes querying for other
	 * applications difficult and bloats the parsing code. Instead, it is
	 * better to bloat the tables. Sqlite should have some optimizations
	 * for sparse columns etc. */
	table_creation_query = sqlite3_mprintf (
		"CREATE TABLE IF NOT EXISTS %Q ( "
			"uid TEXT PRIMARY KEY , "
			"flags INTEGER , "
			"msg_type INTEGER , "
			"read INTEGER , "
			"deleted INTEGER , "
			"replied INTEGER , "
			"important INTEGER , "
			"junk INTEGER , "
			"attachment INTEGER , "
			"dirty INTEGER , "
			"size INTEGER , "
			"dsent NUMERIC , "
			"dreceived NUMERIC , "
			"subject TEXT , "
			"mail_from TEXT , "
			"mail_to TEXT , "
			"mail_cc TEXT , "
			"mlist TEXT , "
			"followup_flag TEXT , "
			"followup_completed_on TEXT , "
			"followup_due_by TEXT , "
			"part TEXT , "
			"labels TEXT , "
			"usertags TEXT , "
			"cinfo TEXT , "
			"bdata TEXT, "
			"created TEXT, "
			"modified TEXT)",
			folder_name);
	ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
	sqlite3_free (table_creation_query);

	table_creation_query = sqlite3_mprintf (
		"CREATE TABLE IF NOT EXISTS '%q_bodystructure' ( "
			"uid TEXT PRIMARY KEY , "
			"bodystructure TEXT )",
			folder_name);
	ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
	sqlite3_free (table_creation_query);

	/* Create message preview table. */
	table_creation_query = sqlite3_mprintf ("CREATE TABLE IF NOT EXISTS '%q_preview' (  uid TEXT PRIMARY KEY , preview TEXT)", folder_name);
	ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
	sqlite3_free (table_creation_query);

	/* FIXME: sqlize folder_name before you create the index */
	safe_index = g_strdup_printf ("SINDEX-%s", folder_name);
	table_creation_query = sqlite3_mprintf ("DROP INDEX IF EXISTS %Q", safe_index);
	ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
	g_free (safe_index);
	sqlite3_free (table_creation_query);

	/* INDEX on preview */
	safe_index = g_strdup_printf ("SINDEX-%s-preview", folder_name);
	table_creation_query = sqlite3_mprintf ("CREATE INDEX IF NOT EXISTS %Q ON '%q_preview' (uid, preview)", safe_index, folder_name);
	ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
	g_free (safe_index);
	sqlite3_free (table_creation_query);

	/* Index on deleted*/
	safe_index = g_strdup_printf ("DELINDEX-%s", folder_name);
	table_creation_query = sqlite3_mprintf ("CREATE INDEX IF NOT EXISTS %Q ON %Q (deleted)", safe_index, folder_name);
	ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
	g_free (safe_index);
	sqlite3_free (table_creation_query);

	/* Index on Junk*/
	safe_index = g_strdup_printf ("JUNKINDEX-%s", folder_name);
	table_creation_query = sqlite3_mprintf ("CREATE INDEX IF NOT EXISTS %Q ON %Q (junk)", safe_index, folder_name);
	ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
	g_free (safe_index);
	sqlite3_free (table_creation_query);

	/* Index on unread*/
	safe_index = g_strdup_printf ("READINDEX-%s", folder_name);
	table_creation_query = sqlite3_mprintf ("CREATE INDEX IF NOT EXISTS %Q ON %Q (read)", safe_index, folder_name);
	ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
	g_free (safe_index);
	sqlite3_free (table_creation_query);

	return ret;
}

static gint
camel_db_migrate_folder_prepare (CamelDB *cdb,
                                 const gchar *folder_name,
                                 gint version,
                                 GError **error)
{
	gint ret = 0;
	gchar *table_creation_query;

	/* Migration stage one: storing the old data */

	if (version < 0) {
		ret = camel_db_create_message_info_table (cdb, folder_name, error);
		g_clear_error (error);
	} else if (version < 1) {

		/* Between version 0-1 the following things are changed
		 * ADDED: created: time
		 * ADDED: modified: time
		 * RENAMED: msg_security to dirty
		 * */

		table_creation_query = sqlite3_mprintf ("DROP TABLE IF EXISTS 'mem.%q'", folder_name);
		ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
		sqlite3_free (table_creation_query);

		table_creation_query = sqlite3_mprintf (
			"CREATE TEMP TABLE IF NOT EXISTS 'mem.%q' ( "
				"uid TEXT PRIMARY KEY , "
				"flags INTEGER , "
				"msg_type INTEGER , "
				"read INTEGER , "
				"deleted INTEGER , "
				"replied INTEGER , "
				"important INTEGER , "
				"junk INTEGER , "
				"attachment INTEGER , "
				"dirty INTEGER , "
				"size INTEGER , "
				"dsent NUMERIC , "
				"dreceived NUMERIC , "
				"subject TEXT , "
				"mail_from TEXT , "
				"mail_to TEXT , "
				"mail_cc TEXT , "
				"mlist TEXT , "
				"followup_flag TEXT , "
				"followup_completed_on TEXT , "
				"followup_due_by TEXT , "
				"part TEXT , "
				"labels TEXT , "
				"usertags TEXT , "
				"cinfo TEXT , "
				"bdata TEXT, "
				"created TEXT, "
				"modified TEXT )",
				folder_name);
		ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
		sqlite3_free (table_creation_query);
		g_clear_error (error);

		table_creation_query = sqlite3_mprintf (
			"INSERT INTO 'mem.%q' SELECT "
			"uid , flags , msg_type , read , deleted , "
			"replied , important , junk , attachment , dirty , "
			"size , dsent , dreceived , subject , mail_from , "
			"mail_to , mail_cc , mlist , followup_flag , "
			"followup_completed_on , followup_due_by , "
			"part , labels , usertags , cinfo , bdata , "
			"strftime(\"%%s\", 'now'), "
			"strftime(\"%%s\", 'now') FROM %Q",
			folder_name, folder_name);
		ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
		sqlite3_free (table_creation_query);
		g_clear_error (error);

		table_creation_query = sqlite3_mprintf ("DROP TABLE IF EXISTS %Q", folder_name);
		ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
		sqlite3_free (table_creation_query);
		g_clear_error (error);

		ret = camel_db_create_message_info_table (cdb, folder_name, error);
		g_clear_error (error);
	}

	/* Add later version migrations here */

	return ret;
}

static gint
camel_db_migrate_folder_recreate (CamelDB *cdb,
                                  const gchar *folder_name,
                                  gint version,
                                  GError **error)
{
	gint ret = 0;
	gchar *table_creation_query;

	/* Migration stage two: writing back the old data */

	if (version < 2) {
		GError *local_error = NULL;

		table_creation_query = sqlite3_mprintf (
			"INSERT INTO %Q SELECT uid , flags , msg_type , "
			"read , deleted , replied , important , junk , "
			"attachment , dirty , size , dsent , dreceived , "
			"subject , mail_from , mail_to , mail_cc , mlist , "
			"followup_flag , followup_completed_on , "
			"followup_due_by , part , labels , usertags , "
			"cinfo , bdata, created, modified FROM 'mem.%q'",
			folder_name, folder_name);
		ret = camel_db_add_to_transaction (cdb, table_creation_query, &local_error);
		sqlite3_free (table_creation_query);

		if (!local_error) {
			table_creation_query = sqlite3_mprintf ("DROP TABLE 'mem.%q'", folder_name);
			ret = camel_db_add_to_transaction (cdb, table_creation_query, &local_error);
			sqlite3_free (table_creation_query);
		}

		if (local_error) {
			if (local_error->message && strstr (local_error->message, "no such table") != NULL) {
				/* ignore 'no such table' errors here */
				g_clear_error (&local_error);
				ret = 0;
			} else {
				g_propagate_error (error, local_error);
			}
		}
	}

	/* Add later version migrations here */

	return ret;
}

/**
 * camel_db_reset_folder_version:
 *
 * Since: 2.28
 **/
gint
camel_db_reset_folder_version (CamelDB *cdb,
                               const gchar *folder_name,
                               gint reset_version,
                               GError **error)
{
	gint ret = 0;
	gchar *version_creation_query;
	gchar *version_insert_query;
	gchar *drop_folder_query;

	drop_folder_query = sqlite3_mprintf ("DROP TABLE IF EXISTS '%q_version'", folder_name);
	version_creation_query = sqlite3_mprintf ("CREATE TABLE IF NOT EXISTS '%q_version' ( version TEXT )", folder_name);

	version_insert_query = sqlite3_mprintf ("INSERT INTO '%q_version' VALUES ('%d')", folder_name, reset_version);

	ret = camel_db_add_to_transaction (cdb, drop_folder_query, error);
	ret = camel_db_add_to_transaction (cdb, version_creation_query, error);
	ret = camel_db_add_to_transaction (cdb, version_insert_query, error);

	sqlite3_free (drop_folder_query);
	sqlite3_free (version_creation_query);
	sqlite3_free (version_insert_query);

	return ret;
}

static gint
camel_db_write_folder_version (CamelDB *cdb,
                               const gchar *folder_name,
                               gint old_version,
                               GError **error)
{
	gint ret = 0;
	gchar *version_creation_query;
	gchar *version_insert_query;

	version_creation_query = sqlite3_mprintf ("CREATE TABLE IF NOT EXISTS '%q_version' ( version TEXT )", folder_name);

	if (old_version == -1)
		version_insert_query = sqlite3_mprintf ("INSERT INTO '%q_version' VALUES ('2')", folder_name);
	else
		version_insert_query = sqlite3_mprintf ("UPDATE '%q_version' SET version='2'", folder_name);

	ret = camel_db_add_to_transaction (cdb, version_creation_query, error);
	ret = camel_db_add_to_transaction (cdb, version_insert_query, error);

	sqlite3_free (version_creation_query);
	sqlite3_free (version_insert_query);

	return ret;
}

static gint
read_version_callback (gpointer ref,
                       gint ncol,
                       gchar **cols,
                       gchar **name)
{
	gint *version = (gint *) ref;

	if (cols[0])
		*version = strtoul (cols [0], NULL, 10);

	return 0;
}

static gint
camel_db_get_folder_version (CamelDB *cdb,
                             const gchar *folder_name,
                             GError **error)
{
	gint version = -1;
	gchar *query;

	query = sqlite3_mprintf ("SELECT version FROM '%q_version'", folder_name);
	camel_db_select (cdb, query, read_version_callback, &version, error);
	sqlite3_free (query);

	return version;
}

/**
 * camel_db_prepare_message_info_table:
 *
 * Since: 2.24
 **/
gint
camel_db_prepare_message_info_table (CamelDB *cdb,
                                     const gchar *folder_name,
                                     GError **error)
{
	gint ret, current_version;
	gboolean in_transaction = TRUE;
	GError *err = NULL;

	/* Make sure we have the table already */
	camel_db_begin_transaction (cdb, &err);
	ret = camel_db_create_message_info_table (cdb, folder_name, &err);
	if (err)
		goto exit;

	camel_db_end_transaction (cdb, &err);
	in_transaction = FALSE;

	/* Migration stage zero: version fetch */
	current_version = camel_db_get_folder_version (cdb, folder_name, &err);
	if (err && err->message && strstr (err->message, "no such table") != NULL) {
		g_clear_error (&err);
		current_version = -1;
	}

	camel_db_begin_transaction (cdb, &err);
	in_transaction = TRUE;

	/* Migration stage one: storing the old data if necessary */
	ret = camel_db_migrate_folder_prepare (cdb, folder_name, current_version, &err);
	if (err)
		goto exit;

	/* Migration stage two: rewriting the old data if necessary */
	ret = camel_db_migrate_folder_recreate (cdb, folder_name, current_version, &err);
	if (err)
		goto exit;

	/* Final step: (over)write the current version label */
	ret = camel_db_write_folder_version (cdb, folder_name, current_version, &err);
	if (err)
		goto exit;

	camel_db_end_transaction (cdb, &err);
	in_transaction = FALSE;

exit:
	if (err && in_transaction)
		camel_db_abort_transaction (cdb, NULL);

	if (err)
		g_propagate_error (error, err);

	return ret;
}

static gint
write_mir (CamelDB *cdb,
           const gchar *folder_name,
           CamelMIRecord *record,
           GError **error,
           gboolean delete_old_record)
{
	gint ret;
	/*char *del_query;*/
	gchar *ins_query;

	if (!record) {
		g_warn_if_reached ();
		return -1;
	}

	/* FIXME: We should migrate from this DELETE followed by INSERT model to an INSERT OR REPLACE model as pointed out by pvanhoof */

	/* NB: UGLIEST Hack. We can't modify the schema now. We are using dirty (an unsed one to notify of FLAGGED/Dirty infos */

	ins_query = sqlite3_mprintf (
		"INSERT OR REPLACE INTO %Q VALUES ("
		"%Q, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, "
		"%lld, %lld, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %Q, "
		"%Q, %Q, %Q, %Q, %Q, "
		"strftime(\"%%s\", 'now'), "
		"strftime(\"%%s\", 'now') )",
		folder_name,
		record->uid,
		record->flags,
		record->msg_type,
		record->read,
		record->deleted,
		record->replied,
		record->important,
		record->junk,
		record->attachment,
		record->dirty,
		record->size,
		(gint64) record->dsent,
		(gint64) record->dreceived,
		record->subject,
		record->from,
		record->to,
		record->cc,
		record->mlist,
		record->followup_flag,
		record->followup_completed_on,
		record->followup_due_by,
		record->part,
		record->labels,
		record->usertags,
		record->cinfo,
		record->bdata);

	ret = camel_db_add_to_transaction (cdb, ins_query, error);

	sqlite3_free (ins_query);

	if (ret == 0) {
		ins_query = sqlite3_mprintf (
			"INSERT OR REPLACE INTO "
			"'%q_bodystructure' VALUES (%Q, %Q )",
			folder_name, record->uid, record->bodystructure);
		ret = camel_db_add_to_transaction (cdb, ins_query, error);
		sqlite3_free (ins_query);
	}

	return ret;
}

/**
 * camel_db_write_fresh_message_info_record:
 *
 * Since: 2.26
 **/
gint
camel_db_write_fresh_message_info_record (CamelDB *cdb,
                                          const gchar *folder_name,
                                          CamelMIRecord *record,
                                          GError **error)
{
	return write_mir (cdb, folder_name, record, error, FALSE);
}

/**
 * camel_db_write_message_info_record:
 *
 * Since: 2.24
 **/
gint
camel_db_write_message_info_record (CamelDB *cdb,
                                    const gchar *folder_name,
                                    CamelMIRecord *record,
                                    GError **error)
{
	return write_mir (cdb, folder_name, record, error, TRUE);
}

/**
 * camel_db_write_folder_info_record:
 *
 * Since: 2.24
 **/
gint
camel_db_write_folder_info_record (CamelDB *cdb,
                                   CamelFIRecord *record,
                                   GError **error)
{
	gint ret;

	gchar *del_query;
	gchar *ins_query;

	ins_query = sqlite3_mprintf (
		"INSERT INTO folders VALUES ("
		"%Q, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %Q ) ",
		record->folder_name,
		record->version,
		record->flags,
		record->nextuid,
		record->time,
		record->saved_count,
		record->unread_count,
		record->deleted_count,
		record->junk_count,
		record->visible_count,
		record->jnd_count,
		record->bdata);

	del_query = sqlite3_mprintf (
		"DELETE FROM folders WHERE folder_name = %Q",
		record->folder_name);

	ret = camel_db_add_to_transaction (cdb, del_query, error);
	ret = camel_db_add_to_transaction (cdb, ins_query, error);

	sqlite3_free (del_query);
	sqlite3_free (ins_query);

	return ret;
}

struct ReadFirData {
	GHashTable *columns_hash;
	CamelFIRecord *record;
};

static gint
read_fir_callback (gpointer ref,
                   gint ncol,
                   gchar **cols,
                   gchar **name)
{
	struct ReadFirData *rfd = ref;
	gint i;

	d (g_print ("\nread_fir_callback called \n"));

	for (i = 0; i < ncol; ++i) {
		if (!name[i] || !cols[i])
			continue;

		switch (camel_db_get_column_ident (&rfd->columns_hash, i, ncol, name)) {
			case CAMEL_DB_COLUMN_FOLDER_NAME:
				rfd->record->folder_name = g_strdup (cols[i]);
				break;
			case CAMEL_DB_COLUMN_VERSION:
				rfd->record->version = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_FLAGS:
				rfd->record->flags = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_NEXTUID:
				rfd->record->nextuid = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_TIME:
				rfd->record->time = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_SAVED_COUNT:
				rfd->record->saved_count = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_UNREAD_COUNT:
				rfd->record->unread_count = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_DELETED_COUNT:
				rfd->record->deleted_count = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_JUNK_COUNT:
				rfd->record->junk_count = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_VISIBLE_COUNT:
				rfd->record->visible_count = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_JND_COUNT:
				rfd->record->jnd_count = cols[i] ? strtoul (cols[i], NULL, 10) : 0;
				break;
			case CAMEL_DB_COLUMN_BDATA:
				rfd->record->bdata = g_strdup (cols[i]);
				break;
			default:
				g_warn_if_reached ();
				break;
		}
	}

	return 0;
}

/**
 * camel_db_read_folder_info_record:
 *
 * Since: 2.24
 **/
gint
camel_db_read_folder_info_record (CamelDB *cdb,
                                  const gchar *folder_name,
                                  CamelFIRecord *record,
                                  GError **error)
{
	struct ReadFirData rfd;
	gchar *query;
	gint ret;

	rfd.columns_hash = NULL;
	rfd.record = record;

	query = sqlite3_mprintf ("SELECT * FROM folders WHERE folder_name = %Q", folder_name);
	ret = camel_db_select (cdb, query, read_fir_callback, &rfd, error);
	sqlite3_free (query);

	if (rfd.columns_hash)
		g_hash_table_destroy (rfd.columns_hash);

	return ret;
}

/**
 * camel_db_read_message_info_record_with_uid:
 * @read_mir_callback: (scope async) (closure user_data):
 *
 * Since: 2.24
 **/
gint
camel_db_read_message_info_record_with_uid (CamelDB *cdb,
                                            const gchar *folder_name,
                                            const gchar *uid,
                                            gpointer user_data,
                                            CamelDBSelectCB read_mir_callback,
                                            GError **error)
{
	gchar *query;
	gint ret;

	query = sqlite3_mprintf (
		"SELECT uid, flags, size, dsent, dreceived, subject, "
		"mail_from, mail_to, mail_cc, mlist, part, labels, "
		"usertags, cinfo, bdata FROM %Q WHERE uid = %Q",
		folder_name, uid);
	ret = camel_db_select (cdb, query, read_mir_callback, user_data, error);
	sqlite3_free (query);

	return (ret);
}

/**
 * camel_db_read_message_info_records:
 * @read_mir_callback: (scope async) (closure user_data):
 *
 * Since: 2.24
 **/
gint
camel_db_read_message_info_records (CamelDB *cdb,
                                    const gchar *folder_name,
                                    gpointer user_data,
                                    CamelDBSelectCB read_mir_callback,
                                    GError **error)
{
	gchar *query;
	gint ret;

	query = sqlite3_mprintf (
		"SELECT uid, flags, size, dsent, dreceived, subject, "
		"mail_from, mail_to, mail_cc, mlist, part, labels, "
		"usertags, cinfo, bdata FROM %Q ", folder_name);
	ret = camel_db_select (cdb, query, read_mir_callback, user_data, error);
	sqlite3_free (query);

	return (ret);
}

/**
 * camel_db_create_deleted_table:
 *
 * Since: 2.24
 **/
static gint
camel_db_create_deleted_table (CamelDB *cdb,
                               GError **error)
{
	gint ret;
	gchar *table_creation_query;
	table_creation_query = sqlite3_mprintf (
		"CREATE TABLE IF NOT EXISTS Deletes ("
			"id INTEGER primary key AUTOINCREMENT not null, "
			"uid TEXT, "
			"time TEXT, "
			"mailbox TEXT)");
	ret = camel_db_add_to_transaction (cdb, table_creation_query, error);
	sqlite3_free (table_creation_query);
	return ret;
}

static gint
camel_db_trim_deleted_table (CamelDB *cdb,
                             GError **error)
{
	gint ret = 0;

	/* TODO: We need a mechanism to get rid of very old deletes, or something
	 * that keeps the list trimmed at a certain max (deleting upfront when
	 * appending at the back) */

	return ret;
}

/**
 * camel_db_delete_uid:
 *
 * Since: 2.24
 **/
gint
camel_db_delete_uid (CamelDB *cdb,
                     const gchar *folder,
                     const gchar *uid,
                     GError **error)
{
	gchar *tab;
	gint ret;

	camel_db_begin_transaction (cdb, error);

	ret = camel_db_create_deleted_table (cdb, error);

	tab = sqlite3_mprintf (
		"INSERT OR REPLACE INTO Deletes (uid, mailbox, time) "
		"SELECT uid, %Q, strftime(\"%%s\", 'now') FROM %Q "
		"WHERE uid = %Q", folder, folder, uid);
	ret = camel_db_add_to_transaction (cdb, tab, error);
	sqlite3_free (tab);

	ret = camel_db_trim_deleted_table (cdb, error);

	tab = sqlite3_mprintf ("DELETE FROM '%q_bodystructure' WHERE uid = %Q", folder, uid);
	ret = camel_db_add_to_transaction (cdb, tab, error);
	sqlite3_free (tab);

	tab = sqlite3_mprintf ("DELETE FROM %Q WHERE uid = %Q", folder, uid);
	ret = camel_db_add_to_transaction (cdb, tab, error);
	sqlite3_free (tab);

	ret = camel_db_end_transaction (cdb, error);

	CAMEL_DB_RELEASE_SQLITE_MEMORY;
	return ret;
}

static gint
cdb_delete_ids (CamelDB *cdb,
                const gchar *folder_name,
                GList *uids,
                const gchar *uid_prefix,
                const gchar *field,
                GError **error)
{
	gchar *tmp;
	gint ret = 0;
	gchar *tab;
	gboolean first = TRUE;
	GString *str = g_string_new ("DELETE FROM ");
	GList *iterator;
	GString *ins_str = NULL;

	if (strcmp (field, "vuid") != 0)
		ins_str = g_string_new ("INSERT OR REPLACE INTO Deletes (uid, mailbox, time) SELECT uid, ");

	camel_db_begin_transaction (cdb, error);

	if (ins_str) {
		ret = camel_db_create_deleted_table (cdb, error);
		if (ret == -1) {
			camel_db_abort_transaction (cdb, NULL);

			g_string_free (ins_str, TRUE);
			g_string_free (str, TRUE);

			return ret;
		}
	}

	if (ins_str) {
		tab = sqlite3_mprintf ("%Q, strftime(\"%%s\", 'now') FROM %Q WHERE %s IN (", folder_name, folder_name, field);
		g_string_append_printf (ins_str, "%s ", tab);
		sqlite3_free (tab);
	}

	tmp = sqlite3_mprintf ("%Q WHERE %s IN (", folder_name, field);
	g_string_append_printf (str, "%s ", tmp);
	sqlite3_free (tmp);

	iterator = uids;

	while (iterator) {
		gchar *foo = g_strdup_printf ("%s%s", uid_prefix, (gchar *) iterator->data);
		tmp = sqlite3_mprintf ("%Q", foo);
		g_free (foo);
		iterator = iterator->next;

		if (first == TRUE) {
			g_string_append_printf (str, " %s ", tmp);
			if (ins_str)
				g_string_append_printf (ins_str, " %s ", tmp);
			first = FALSE;
		} else {
			g_string_append_printf (str, ", %s ", tmp);
			if (ins_str)
				g_string_append_printf (ins_str, ", %s ", tmp);
		}

		sqlite3_free (tmp);
	}

	g_string_append (str, ")");
	if (ins_str) {
		g_string_append (ins_str, ")");
		ret = camel_db_add_to_transaction (cdb, ins_str->str, error);
		ret = ret == -1 ? ret : camel_db_trim_deleted_table (cdb, error);
	}

	ret = ret == -1 ? ret : camel_db_add_to_transaction (cdb, str->str, error);

	if (ret == -1)
		camel_db_abort_transaction (cdb, NULL);
	else
		ret = camel_db_end_transaction (cdb, error);

	CAMEL_DB_RELEASE_SQLITE_MEMORY;

	if (ins_str)
		g_string_free (ins_str, TRUE);
	g_string_free (str, TRUE);

	return ret;
}

/**
 * camel_db_delete_uids:
 * @cdb: a #CamelDB
 * @uids: (element-type utf8) (transfer none): A #GList of uids
 *
 * Since: 2.24
 **/
gint
camel_db_delete_uids (CamelDB *cdb,
                      const gchar *folder_name,
                      GList *uids,
                      GError **error)
{
	if (!uids || !uids->data)
		return 0;

	return cdb_delete_ids (cdb, folder_name, uids, "", "uid", error);
}

/**
 * camel_db_clear_folder_summary:
 *
 * Since: 2.24
 **/
gint
camel_db_clear_folder_summary (CamelDB *cdb,
                               const gchar *folder,
                               GError **error)
{
	gint ret;

	gchar *folders_del;
	gchar *msginfo_del;
	gchar *bstruct_del;
	gchar *tab;

	folders_del = sqlite3_mprintf ("DELETE FROM folders WHERE folder_name = %Q", folder);
	msginfo_del = sqlite3_mprintf ("DELETE FROM %Q ", folder);
	bstruct_del = sqlite3_mprintf ("DELETE FROM '%q_bodystructure' ", folder);

	camel_db_begin_transaction (cdb, error);

	ret = camel_db_create_deleted_table (cdb, error);

	tab = sqlite3_mprintf (
		"INSERT OR REPLACE INTO Deletes (uid, mailbox, time) "
		"SELECT uid, %Q, strftime(\"%%s\", 'now') FROM %Q",
		folder, folder);
	ret = camel_db_add_to_transaction (cdb, tab, error);
	sqlite3_free (tab);

	ret = camel_db_trim_deleted_table (cdb, error);

	camel_db_add_to_transaction (cdb, msginfo_del, error);
	camel_db_add_to_transaction (cdb, folders_del, error);
	camel_db_add_to_transaction (cdb, bstruct_del, error);

	ret = camel_db_end_transaction (cdb, error);

	sqlite3_free (folders_del);
	sqlite3_free (msginfo_del);
	sqlite3_free (bstruct_del);

	return ret;
}

/**
 * camel_db_delete_folder:
 *
 * Since: 2.24
 **/
gint
camel_db_delete_folder (CamelDB *cdb,
                        const gchar *folder,
                        GError **error)
{
	gint ret;
	gchar *del;
	gchar *tab;

	camel_db_begin_transaction (cdb, error);

	ret = camel_db_create_deleted_table (cdb, error);

	tab = sqlite3_mprintf (
		"INSERT OR REPLACE INTO Deletes (uid, mailbox, time) "
		"SELECT uid, %Q, strftime(\"%%s\", 'now') FROM %Q",
		folder, folder);
	ret = camel_db_add_to_transaction (cdb, tab, error);
	sqlite3_free (tab);

	ret = camel_db_trim_deleted_table (cdb, error);

	del = sqlite3_mprintf ("DELETE FROM folders WHERE folder_name = %Q", folder);
	ret = camel_db_add_to_transaction (cdb, del, error);
	sqlite3_free (del);

	del = sqlite3_mprintf ("DROP TABLE %Q ", folder);
	ret = camel_db_add_to_transaction (cdb, del, error);
	sqlite3_free (del);

	del = sqlite3_mprintf ("DROP TABLE '%q_bodystructure' ", folder);
	ret = camel_db_add_to_transaction (cdb, del, error);
	sqlite3_free (del);

	ret = camel_db_end_transaction (cdb, error);

	CAMEL_DB_RELEASE_SQLITE_MEMORY;
	return ret;
}

/**
 * camel_db_rename_folder:
 *
 * Since: 2.24
 **/
gint
camel_db_rename_folder (CamelDB *cdb,
                        const gchar *old_folder,
                        const gchar *new_folder,
                        GError **error)
{
	gint ret;
	gchar *cmd, *tab;

	camel_db_begin_transaction (cdb, error);

	ret = camel_db_create_deleted_table (cdb, error);

	tab = sqlite3_mprintf (
		"INSERT OR REPLACE INTO Deletes (uid, mailbox, time) "
		"SELECT uid, %Q, strftime(\"%%s\", 'now') FROM %Q",
		old_folder, old_folder);
	ret = camel_db_add_to_transaction (cdb, tab, error);
	sqlite3_free (tab);

	ret = camel_db_trim_deleted_table (cdb, error);

	cmd = sqlite3_mprintf ("ALTER TABLE %Q RENAME TO  %Q", old_folder, new_folder);
	ret = camel_db_add_to_transaction (cdb, cmd, error);
	sqlite3_free (cmd);

	cmd = sqlite3_mprintf ("ALTER TABLE '%q_version' RENAME TO  '%q_version'", old_folder, new_folder);
	ret = camel_db_add_to_transaction (cdb, cmd, error);
	sqlite3_free (cmd);

	cmd = sqlite3_mprintf ("UPDATE %Q SET modified=strftime(\"%%s\", 'now'), created=strftime(\"%%s\", 'now')", new_folder);
	ret = camel_db_add_to_transaction (cdb, cmd, error);
	sqlite3_free (cmd);

	cmd = sqlite3_mprintf ("UPDATE folders SET folder_name = %Q WHERE folder_name = %Q", new_folder, old_folder);
	ret = camel_db_add_to_transaction (cdb, cmd, error);
	sqlite3_free (cmd);

	ret = camel_db_end_transaction (cdb, error);

	CAMEL_DB_RELEASE_SQLITE_MEMORY;
	return ret;
}

/**
 * camel_db_camel_mir_free:
 *
 * Since: 2.24
 **/
void
camel_db_camel_mir_free (CamelMIRecord *record)
{
	if (record) {
		camel_pstring_free (record->uid);
		camel_pstring_free (record->subject);
		camel_pstring_free (record->from);
		camel_pstring_free (record->to);
		camel_pstring_free (record->cc);
		camel_pstring_free (record->mlist);
		camel_pstring_free (record->followup_flag);
		camel_pstring_free (record->followup_completed_on);
		camel_pstring_free (record->followup_due_by);
		g_free (record->part);
		g_free (record->labels);
		g_free (record->usertags);
		g_free (record->cinfo);
		g_free (record->bdata);
		g_free (record->bodystructure);

		g_free (record);
	}
}

/**
 * camel_db_sqlize_string:
 *
 * Since: 2.24
 **/
gchar *
camel_db_sqlize_string (const gchar *string)
{
	return sqlite3_mprintf ("%Q", string);
}

/**
 * camel_db_free_sqlized_string:
 *
 * Since: 2.24
 **/
void
camel_db_free_sqlized_string (gchar *string)
{
	sqlite3_free (string);
	string = NULL;
}

/*
"(  uid TEXT PRIMARY KEY ,
flags INTEGER ,
msg_type INTEGER ,
replied INTEGER ,
dirty INTEGER ,
size INTEGER ,
dsent NUMERIC ,
dreceived NUMERIC ,
mlist TEXT ,
followup_flag TEXT ,
followup_completed_on TEXT ,
followup_due_by TEXT ," */

/**
 * camel_db_get_column_name:
 *
 * Since: 2.24
 **/
gchar *
camel_db_get_column_name (const gchar *raw_name)
{
	if (!g_ascii_strcasecmp (raw_name, "Subject"))
		return g_strdup ("subject");
	else if (!g_ascii_strcasecmp (raw_name, "from"))
		return g_strdup ("mail_from");
	else if (!g_ascii_strcasecmp (raw_name, "Cc"))
		return g_strdup ("mail_cc");
	else if (!g_ascii_strcasecmp (raw_name, "To"))
		return g_strdup ("mail_to");
	else if (!g_ascii_strcasecmp (raw_name, "Flagged"))
		return g_strdup ("important");
	else if (!g_ascii_strcasecmp (raw_name, "deleted"))
		return g_strdup ("deleted");
	else if (!g_ascii_strcasecmp (raw_name, "junk"))
		return g_strdup ("junk");
	else if (!g_ascii_strcasecmp (raw_name, "Answered"))
		return g_strdup ("replied");
	else if (!g_ascii_strcasecmp (raw_name, "Seen"))
		return g_strdup ("read");
	else if (!g_ascii_strcasecmp (raw_name, "user-tag"))
		return g_strdup ("usertags");
	else if (!g_ascii_strcasecmp (raw_name, "user-flag"))
		return g_strdup ("labels");
	else if (!g_ascii_strcasecmp (raw_name, "Attachments"))
		return g_strdup ("attachment");
	else if (!g_ascii_strcasecmp (raw_name, "x-camel-mlist"))
		return g_strdup ("mlist");

	/* indicate the header name is not part of the summary */
	return NULL;
}

/**
 * camel_db_start_in_memory_transactions:
 *
 * Since: 2.26
 **/
gint
camel_db_start_in_memory_transactions (CamelDB *cdb,
                                       GError **error)
{
	gint ret;
	gchar *cmd = sqlite3_mprintf ("ATTACH DATABASE ':memory:' AS %s", CAMEL_DB_IN_MEMORY_DB);

	ret = camel_db_command (cdb, cmd, error);
	sqlite3_free (cmd);

	cmd = sqlite3_mprintf (
		"CREATE TEMPORARY TABLE %Q ( "
			"uid TEXT PRIMARY KEY , "
			"flags INTEGER , "
			"msg_type INTEGER , "
			"read INTEGER , "
			"deleted INTEGER , "
			"replied INTEGER , "
			"important INTEGER , "
			"junk INTEGER , "
			"attachment INTEGER , "
			"dirty INTEGER , "
			"size INTEGER , "
			"dsent NUMERIC , "
			"dreceived NUMERIC , "
			"subject TEXT , "
			"mail_from TEXT , "
			"mail_to TEXT , "
			"mail_cc TEXT , "
			"mlist TEXT , "
			"followup_flag TEXT , "
			"followup_completed_on TEXT , "
			"followup_due_by TEXT , "
			"part TEXT , "
			"labels TEXT , "
			"usertags TEXT , "
			"cinfo TEXT , "
			"bdata TEXT )",
		CAMEL_DB_IN_MEMORY_TABLE);
	ret = camel_db_command (cdb, cmd, error);
	if (ret != 0 )
		abort ();
	sqlite3_free (cmd);

	return ret;
}

/**
 * camel_db_flush_in_memory_transactions:
 *
 * Since: 2.26
 **/
gint
camel_db_flush_in_memory_transactions (CamelDB *cdb,
                                       const gchar *folder_name,
                                       GError **error)
{
	gint ret;
	gchar *cmd = sqlite3_mprintf ("INSERT INTO %Q SELECT * FROM %Q", folder_name, CAMEL_DB_IN_MEMORY_TABLE);

	ret = camel_db_command (cdb, cmd, error);
	sqlite3_free (cmd);

	cmd = sqlite3_mprintf ("DROP TABLE %Q", CAMEL_DB_IN_MEMORY_TABLE);
	ret = camel_db_command (cdb, cmd, error);
	sqlite3_free (cmd);

	cmd = sqlite3_mprintf ("DETACH %Q", CAMEL_DB_IN_MEMORY_DB);
	ret = camel_db_command (cdb, cmd, error);
	sqlite3_free (cmd);

	return ret;
}

static struct _known_column_names {
	const gchar *name;
	CamelDBKnownColumnNames ident;
} known_column_names[] = {
	{ "attachment",			CAMEL_DB_COLUMN_ATTACHMENT },
	{ "bdata",			CAMEL_DB_COLUMN_BDATA },
	{ "bodystructure",		CAMEL_DB_COLUMN_BODYSTRUCTURE },
	{ "cinfo",			CAMEL_DB_COLUMN_CINFO },
	{ "deleted",			CAMEL_DB_COLUMN_DELETED },
	{ "deleted_count",		CAMEL_DB_COLUMN_DELETED_COUNT },
	{ "dreceived",			CAMEL_DB_COLUMN_DRECEIVED },
	{ "dsent",			CAMEL_DB_COLUMN_DSENT },
	{ "flags",			CAMEL_DB_COLUMN_FLAGS },
	{ "folder_name",		CAMEL_DB_COLUMN_FOLDER_NAME },
	{ "followup_completed_on",	CAMEL_DB_COLUMN_FOLLOWUP_COMPLETED_ON },
	{ "followup_due_by",		CAMEL_DB_COLUMN_FOLLOWUP_DUE_BY },
	{ "followup_flag",		CAMEL_DB_COLUMN_FOLLOWUP_FLAG },
	{ "important",			CAMEL_DB_COLUMN_IMPORTANT },
	{ "jnd_count",			CAMEL_DB_COLUMN_JND_COUNT },
	{ "junk",			CAMEL_DB_COLUMN_JUNK },
	{ "junk_count",			CAMEL_DB_COLUMN_JUNK_COUNT },
	{ "labels",			CAMEL_DB_COLUMN_LABELS },
	{ "mail_cc",			CAMEL_DB_COLUMN_MAIL_CC },
	{ "mail_from",			CAMEL_DB_COLUMN_MAIL_FROM },
	{ "mail_to",			CAMEL_DB_COLUMN_MAIL_TO },
	{ "mlist",			CAMEL_DB_COLUMN_MLIST },
	{ "nextuid",			CAMEL_DB_COLUMN_NEXTUID },
	{ "part",			CAMEL_DB_COLUMN_PART },
	{ "preview",			CAMEL_DB_COLUMN_PREVIEW },
	{ "read",			CAMEL_DB_COLUMN_READ },
	{ "replied",			CAMEL_DB_COLUMN_REPLIED },
	{ "saved_count",		CAMEL_DB_COLUMN_SAVED_COUNT },
	{ "size",			CAMEL_DB_COLUMN_SIZE },
	{ "subject",			CAMEL_DB_COLUMN_SUBJECT },
	{ "time",			CAMEL_DB_COLUMN_TIME },
	{ "uid",			CAMEL_DB_COLUMN_UID },
	{ "unread_count",		CAMEL_DB_COLUMN_UNREAD_COUNT },
	{ "usertags",			CAMEL_DB_COLUMN_USERTAGS },
	{ "version",			CAMEL_DB_COLUMN_VERSION },
	{ "visible_count",		CAMEL_DB_COLUMN_VISIBLE_COUNT },
	{ "vuid",			CAMEL_DB_COLUMN_VUID }
};

/**
 * camel_db_get_column_ident:
 * @hash: (out) (transfer container) (element-type int int):
 * @col_names: (array length=ncols):
 *
 * Traverses column name from index @index into an enum
 * #CamelDBKnownColumnNames value.  The @col_names contains @ncols columns.
 * First time this is called is created the @hash from col_names indexes into
 * the enum, and this is reused for every other call.  The function expects
 * that column names are returned always in the same order.  When all rows
 * are read the @hash table can be freed with g_hash_table_destroy().
 *
 * Since: 3.4
 **/
CamelDBKnownColumnNames
camel_db_get_column_ident (GHashTable **hash,
                           gint index,
                           gint ncols,
                           gchar **col_names)
{
	gpointer value = NULL;

	g_return_val_if_fail (hash != NULL, CAMEL_DB_COLUMN_UNKNOWN);
	g_return_val_if_fail (col_names != NULL, CAMEL_DB_COLUMN_UNKNOWN);
	g_return_val_if_fail (ncols > 0, CAMEL_DB_COLUMN_UNKNOWN);
	g_return_val_if_fail (index >= 0, CAMEL_DB_COLUMN_UNKNOWN);
	g_return_val_if_fail (index < ncols, CAMEL_DB_COLUMN_UNKNOWN);

	if (!*hash) {
		gint ii, jj, from, max = G_N_ELEMENTS (known_column_names);

		*hash = g_hash_table_new (g_direct_hash, g_direct_equal);

		for (ii = 0, jj = 0; ii < ncols; ii++) {
			const gchar *name = col_names[ii];
			gboolean first = TRUE;

			if (!name)
				continue;

			for (from = jj; first || jj != from; jj = (jj + 1) % max, first = FALSE) {
				if (g_str_equal (name, known_column_names[jj].name)) {
					g_hash_table_insert (*hash, GINT_TO_POINTER (ii), GINT_TO_POINTER (known_column_names[jj].ident));
					break;
				}
			}

			if (from == jj && !first)
				g_warning ("%s: missing column name '%s' in a list of known columns", G_STRFUNC, name);
		}
	}

	g_return_val_if_fail (g_hash_table_lookup_extended (*hash, GINT_TO_POINTER (index), NULL, &value), CAMEL_DB_COLUMN_UNKNOWN);

	return GPOINTER_TO_INT (value);
}

static gint
get_number_cb (gpointer data,
	       gint argc,
	       gchar **argv,
	       gchar **azColName)
{
	guint64 *pui64 = data;

	if (argc == 1) {
		*pui64 = argv[0] ? g_ascii_strtoull (argv[0], NULL, 10) : 0;
	} else {
		*pui64 = 0;
	}

	return 0;
}

/**
 * camel_db_maybe_run_maintenance:
 * @cdb: a #CamelDB instance
 * @error: (allow-none): a #GError or %NULL
 *
 * Runs a @cdb maintenance, which includes vacuum, if necessary.
 *
 * Returns: Whether succeeded.
 *
 * Since: 3.16
 **/
gboolean
camel_db_maybe_run_maintenance (CamelDB *cdb,
				GError **error)
{
	GError *local_error = NULL;
	guint64 page_count = 0, freelist_count = 0;
	gboolean success = FALSE;

	g_return_val_if_fail (cdb != NULL, FALSE);

	cdb_writer_lock (cdb);

	if (cdb_sql_exec (cdb->db, "PRAGMA page_count;", get_number_cb, &page_count, NULL, &local_error) == SQLITE_OK &&
	    cdb_sql_exec (cdb->db, "PRAGMA freelist_count;", get_number_cb, &freelist_count, NULL, &local_error) == SQLITE_OK) {
		/* Vacuum, if there's more than 5% of the free pages */
		success = !page_count || !freelist_count || freelist_count * 1000 / page_count <= 50 ||
		    cdb_sql_exec (cdb->db, "vacuum;", NULL, NULL, NULL, &local_error) == SQLITE_OK;
	}

	cdb_writer_unlock (cdb);

	if (local_error) {
		g_propagate_error (error, local_error);
		success = FALSE;
	}

	return success;
}
