/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2013 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 * Author: Tristan Van Berkom <tristanvb@openismus.com>
 */

#if !defined (__LIBEDATA_BOOK_H_INSIDE__) && !defined (LIBEDATA_BOOK_COMPILATION)
#error "Only <libedata-book/libedata-book.h> should be included directly."
#endif

#ifndef E_DATA_BOOK_CURSOR_H
#define E_DATA_BOOK_CURSOR_H

#include <gio/gio.h>
#include <libebook-contacts/libebook-contacts.h>

#define E_TYPE_DATA_BOOK_CURSOR        (e_data_book_cursor_get_type ())
#define E_DATA_BOOK_CURSOR(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), E_TYPE_DATA_BOOK_CURSOR, EDataBookCursor))
#define E_DATA_BOOK_CURSOR_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), E_TYPE_DATA_BOOK_CURSOR, EDataBookCursorClass))
#define E_IS_DATA_BOOK_CURSOR(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), E_TYPE_DATA_BOOK_CURSOR))
#define E_IS_DATA_BOOK_CURSOR_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), E_TYPE_DATA_BOOK_CURSOR))
#define E_DATA_BOOK_CURSOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), E_TYPE_DATA_BOOK_CURSOR, EDataBookCursorClass))

G_BEGIN_DECLS

struct _EBookBackend;

typedef struct _EDataBookCursor EDataBookCursor;
typedef struct _EDataBookCursorClass EDataBookCursorClass;
typedef struct _EDataBookCursorPrivate EDataBookCursorPrivate;


/*
 * The following virtual methods have typedefs in order to provide richer 
 * documentation about how to implement the EDataBookCursorClass.
 */

/**
 * EDataBookCursorSetSexpFunc:
 * @cursor: an #EDataBookCursor
 * @sexp: (allow-none): the search expression to set, or %NULL for unfiltered results
 * @error: (out) (allow-none): return location for a #GError, or %NULL
 *
 * Method type for #EDataBookCursorClass.set_sexp()
 *
 * A cursor implementation must implement this in order to modify the search
 * expression for @cursor. After this is called, the position and total will
 * be recalculated.
 *
 * If the cursor implementation is unable to deal with the #EContactFields
 * referred to in @sexp, then an %E_CLIENT_ERROR_INVALID_QUERY error should
 * be set to indicate this.
 *
 * Returns: %TRUE on Success, otherwise %FALSE is returned if any error occurred
 * and @error is set to reflect the error which occurred.
 *
 * Since: 3.10
 */
typedef gboolean (*EDataBookCursorSetSexpFunc) (EDataBookCursor     *cursor,
						const gchar         *sexp,
						GError             **error);

/**
 * EDataBookCursorMoveByFunc:
 * @cursor: an #EDataBookCursor
 * @revision_guard: (allow-none): The expected current addressbook revision, or %NULL
 * @origin: the #EBookCursorOrigin for this move
 * @count: a positive or negative amount of contacts to try and fetch
 * @results: (out) (allow-none) (element-type utf8) (transfer full):
 *   A return location to store the results, or %NULL to move the cursor without retrieving any results.
 * @error: (out) (allow-none): return location for a #GError, or %NULL
 *
 * Method type for #EDataBookCursorClass.move_by()
 *
 * As all cursor methods may be called either by the addressbook service or
 * directly by a client in Direct Read Access mode, it is important that the
 * operation be an atomic transaction with the underlying database.
 *
 * The @revision_guard, if specified, will be set to the %CLIENT_BACKEND_PROPERTY_REVISION
 * value at the time which the given client issued the call to move the cursor.
 * If the @revision_guard provided by the client does not match the stored addressbook
 * revision, then an %E_CLIENT_ERROR_OUT_OF_SYNC error should be set to indicate
 * that the revision was out of sync while attempting to move the cursor.
 *
 * <note><para>If the addressbook backend supports direct read access, then the
 * revision comparison and reading of the data store must be coupled into a
 * single atomic operation (the data read back from the store must be the correct
 * data for the given addressbook revision).</para></note>
 *
 * See e_data_book_cursor_move_by() for more details on the expected behaviour of this method.
 *
 * Returns: %TRUE on Success, otherwise %FALSE is returned if any error occurred
 * and @error is set to reflect the error which occurred.
 *
 * Since: 3.10
 */
typedef gboolean (*EDataBookCursorMoveByFunc) (EDataBookCursor     *cursor,
					       const gchar         *revision_guard,
					       EBookCursorOrigin    origin,
					       gint                 count,
					       GSList             **results,
					       GError             **error);

/**
 * EDataBookCursorSetAlphabetIndexFunc:
 * @cursor: an #EDataBookCursor
 * @index: the alphabetic index
 * @locale: the locale in which @index is expected to be a valid alphabetic index
 * @error: (out) (allow-none): return location for a #GError, or %NULL
 *
 * Method type for #EDataBookCursorClass.set_alphabetic_index()
 *
 * Sets the current cursor position to point to an index into the
 * active alphabet.
 *
 * The implementing class must check that @locale matches the current
 * locale setting of the underlying database and report an %E_CLIENT_ERROR_OUT_OF_SYNC
 * error in the case that the locales do not match.
 *
 * Returns: %TRUE on Success, otherwise %FALSE is returned if any error occurred
 * and @error is set to reflect the error which occurred.
 *
 * Since: 3.10
 */
typedef gboolean (*EDataBookCursorSetAlphabetIndexFunc) (EDataBookCursor     *cursor,
							 gint                 index,
							 const gchar         *locale,
							 GError             **error);

/**
 * EDataBookCursorGetPositionFunc:
 * @cursor: an #EDataBookCursor
 * @total: (out): The total number of contacts matching @cursor's query expression
 * @position: (out): The current position of @cursor in it's result list
 * @error: (out) (allow-none): return location for a #GError, or %NULL
 *
 * Method type for #EDataBookCursorClass.get_position()
 *
 * Cursor implementations must implement this to count the total results
 * matching @cursor's query expression and to calculate the amount of contacts
 * leading up to the current cursor state (cursor inclusive).
 *
 * A cursor position is defined as an integer which is inclusive of the
 * current contact to which it points (if the cursor points to an exact
 * contact). A position of %0 indicates that the cursor is situated in
 * a position that is before and after the entire result set. The cursor
 * position should be %0 at creation time, and should start again from
 * the symbolic %0 position whenever %E_BOOK_CURSOR_ORIGIN_RESET is
 * specified in the #EDataBookCursorClass.move_by() method (or whenever
 * moving the cursor beyond the end of the result set).
 *
 * This method is called by e_data_book_cursor_recalculate() and in some
 * other cases where @cursor's current position and total must be
 * recalculated from scratch.
 *
 * Returns: %TRUE on Success, otherwise %FALSE is returned if any error occurred
 * and @error is set to reflect the error which occurred.
 *
 * Since: 3.10
 */
typedef gboolean (*EDataBookCursorGetPositionFunc) (EDataBookCursor     *cursor,
						    gint                *total,
						    gint                *position,
						    GError             **error);

/**
 * EDataBookCursorCompareFunc:
 * @cursor: an #EDataBookCursor
 * @contact: the #EContact to compare with @cursor
 * @matches_sexp: (out) (allow-none): return location to set whether @contact matched @cursor's search expression
 *
 * Method type for #EDataBookCursorClass.compare()
 *
 * Cursor implementations should implement this in order to compare a
 * contact with the current cursor state.
 *
 * This is called when the addressbook backends notify active cursors
 * that the addressbook has been modified with e_data_book_cursor_contact_added() and
 * e_data_book_cursor_contact_removed(). Comparing the changed contact details is usually
 * much less of an intensive operation than calling #EDataBookCursorClass.get_position(),
 * however if this method is left unimplemented then #EDataBookCursorClass.get_position()
 * will be used in it's place to recalculate the cursor position and total from scratch.
 *
 * Returns: A value that is less than, equal to, or greater than zero if @contact is found,
 * respectively, to be less than, to match, or be greater than the current value of @cursor.
 *
 * Since: 3.10
 */
typedef gint (*EDataBookCursorCompareFunc) (EDataBookCursor     *cursor,
					    EContact            *contact,
					    gboolean            *matches_sexp);

/**
 * EDataBookCursorLoadLocaleFunc:
 * @cursor: an #EDataBookCursor
 * @locale: (out) (transfer full): return location to store the newly loaded locale
 * @error: (out) (allow-none): return location for a #GError, or %NULL
 *
 * Method type for #EDataBookCursorClass.load_locale()
 *
 * Fetches the locale setting from @cursor's addressbook
 *
 * If the locale setting has changed, the cursor must reload any
 * internal locale specific data and ensure that comparisons of
 * sort keys will function properly in the new locale.
 *
 * Upon locale changes, the implementation need not worry about
 * updating it's current cursor state, the cursor state will be
 * reset automatically for you.
 *
 * Returns: %TRUE on Success, otherwise %FALSE is returned if any error occurred
 * and @error is set to reflect the error which occurred.
 *
 * Since: 3.10
 */
typedef gboolean (*EDataBookCursorLoadLocaleFunc) (EDataBookCursor     *cursor,
						   gchar              **locale,
						   GError             **error);

/**
 * EDataBookCursor:
 *
 * An opaque handle for an addressbook cursor
 *
 * Since: 3.10
 */
struct _EDataBookCursor {
	GObject parent;

	EDataBookCursorPrivate *priv;
};

/**
 * EDataBookCursorClass:
 * @set_sexp: The #EDataBookCursorSetSexpFunc delegate to set the search expression
 * @move_by: The #EDataBookCursorMoveByFunc delegate to navigate the cursor
 * @set_alphabetic_index: The #EDataBookCursorSetAlphabetIndexFunc delegate to set the alphabetic position
 * @get_position: The #EDataBookCursorGetPositionFunc delegate to calculate the current total and position values
 * @compare: The #EDataBookCursorCompareFunc delegate to compare an #EContact with the the cursor position
 * @load_locale: The #EDataBookCursorLoadLocaleFunc delegate used to reload the locale setting
 *
 * Methods to implement on an #EDataBookCursor concrete class.
 *
 * Since 3.10
 */
struct _EDataBookCursorClass {
	/*< private >*/
	GObjectClass parent;

	/*< public >*/
	EDataBookCursorSetSexpFunc set_sexp;
	EDataBookCursorMoveByFunc move_by;
	EDataBookCursorSetAlphabetIndexFunc set_alphabetic_index;
	EDataBookCursorGetPositionFunc get_position;
	EDataBookCursorCompareFunc compare;
	EDataBookCursorLoadLocaleFunc load_locale;
};

GType			e_data_book_cursor_get_type		 (void);

struct _EBookBackend   *e_data_book_cursor_get_backend           (EDataBookCursor     *cursor);
gint                    e_data_book_cursor_get_total             (EDataBookCursor     *cursor);
gint                    e_data_book_cursor_get_position          (EDataBookCursor     *cursor);
gboolean                e_data_book_cursor_set_sexp              (EDataBookCursor     *cursor,
								  const gchar         *sexp,
								  GError             **error);
gboolean                e_data_book_cursor_move_by               (EDataBookCursor     *cursor,
								  const gchar         *revision_guard,
								  EBookCursorOrigin    origin,
								  gint                 count,
								  GSList             **results,
								  GError             **error);
gboolean                e_data_book_cursor_set_alphabetic_index  (EDataBookCursor     *cursor,
								  gint                 index,
								  const gchar         *locale,
								  GError             **error);
gboolean                e_data_book_cursor_recalculate           (EDataBookCursor     *cursor,
								  GError             **error);
gboolean                e_data_book_cursor_load_locale           (EDataBookCursor     *cursor,
								  gchar              **locale,
								  GError             **error);
void                    e_data_book_cursor_contact_added         (EDataBookCursor     *cursor,
								  EContact            *contact);
void                    e_data_book_cursor_contact_removed       (EDataBookCursor     *cursor,
								  EContact            *contact);
gboolean                e_data_book_cursor_register_gdbus_object (EDataBookCursor     *cursor,
								  GDBusConnection     *connection,
								  const gchar         *object_path,
								  GError             **error);

G_END_DECLS

#endif /* E_DATA_BOOK_CURSOR_H */