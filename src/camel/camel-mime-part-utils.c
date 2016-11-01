/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; fill-column: 160 -*- */
/* camel-mime-part-utils : Utility for mime parsing and so on
 *
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
 * Authors: Bertrand Guiheneuf <bertrand@helixcode.com>
 *          Michael Zucchi <notzed@ximian.com>
 *          Jeffrey Stedfast <fejj@ximian.com>
 */

#include "evolution-data-server-config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "camel-charset-map.h"
#include "camel-html-parser.h"
#include "camel-iconv.h"
#include "camel-mime-filter-basic.h"
#include "camel-mime-filter-charset.h"
#include "camel-mime-filter-crlf.h"
#include "camel-mime-message.h"
#include "camel-mime-part-utils.h"
#include "camel-multipart-encrypted.h"
#include "camel-multipart-signed.h"
#include "camel-multipart.h"
#include "camel-stream-filter.h"
#include "camel-stream-fs.h"
#include "camel-stream-mem.h"
#include "camel-stream-buffer.h"
#include "camel-utf8.h"

#define d(x) /* (printf("%s(%d): ", __FILE__, __LINE__),(x)) */

/* simple data wrapper */
static gboolean
simple_data_wrapper_construct_from_parser (CamelDataWrapper *dw,
                                           CamelMimeParser *mp,
                                           GCancellable *cancellable,
                                           GError **error)
{
	gchar *buf;
	GByteArray *buffer;
	CamelStream *mem;
	gsize len;
	gboolean success;

	d (printf ("simple_data_wrapper_construct_from_parser()\n"));

	/* read in the entire content */
	buffer = g_byte_array_new ();
	while (camel_mime_parser_step (mp, &buf, &len) != CAMEL_MIME_PARSER_STATE_BODY_END) {
		d (printf ("appending o/p data: %d: %.*s\n", len, len, buf));
		g_byte_array_append (buffer, (guint8 *) buf, len);
	}

	d (printf ("message part kept in memory!\n"));

	mem = camel_stream_mem_new_with_byte_array (buffer);
	success = camel_data_wrapper_construct_from_stream_sync (
		dw, mem, cancellable, error);
	g_object_unref (mem);

	return success;
}

/**
 * camel_mime_part_construct_content_from_parser:
 *
 * Since: 2.24
 **/
gboolean
camel_mime_part_construct_content_from_parser (CamelMimePart *dw,
                                               CamelMimeParser *mp,
                                               GCancellable *cancellable,
                                               GError **error)
{
	CamelDataWrapper *content = NULL;
	CamelContentType *ct;
	gchar *encoding;
	gboolean success = TRUE;

	g_return_val_if_fail (CAMEL_IS_MIME_PART (dw), FALSE);

	ct = camel_mime_parser_content_type (mp);

	encoding = camel_content_transfer_encoding_decode (camel_mime_parser_header (mp, "Content-Transfer-Encoding", NULL));

	switch (camel_mime_parser_state (mp)) {
	case CAMEL_MIME_PARSER_STATE_HEADER:
		d (printf ("Creating body part\n"));
		/* multipart/signed is some type that we must treat as binary data. */
		if (camel_content_type_is (ct, "multipart", "signed")) {
			content = (CamelDataWrapper *) camel_multipart_signed_new ();
			camel_multipart_construct_from_parser ((CamelMultipart *) content, mp);
		} else {
			content = camel_data_wrapper_new ();
			success = simple_data_wrapper_construct_from_parser (
				content, mp, cancellable, error);
		}
		break;
	case CAMEL_MIME_PARSER_STATE_MESSAGE:
		d (printf ("Creating message part\n"));
		content = (CamelDataWrapper *) camel_mime_message_new ();
		success = camel_mime_part_construct_from_parser_sync (
			(CamelMimePart *) content, mp, cancellable, error);
		break;
	case CAMEL_MIME_PARSER_STATE_MULTIPART:
		d (printf ("Creating multi-part\n"));
		if (camel_content_type_is (ct, "multipart", "encrypted"))
			content = (CamelDataWrapper *) camel_multipart_encrypted_new ();
		else if (camel_content_type_is (ct, "multipart", "signed"))
			content = (CamelDataWrapper *) camel_multipart_signed_new ();
		else
			content = (CamelDataWrapper *) camel_multipart_new ();

		camel_multipart_construct_from_parser ((CamelMultipart *) content, mp);
		d (printf ("Created multi-part\n"));
		break;
	default:
		g_warning ("Invalid state encountered???: %u", camel_mime_parser_state (mp));
	}

	if (content) {
		if (encoding)
			content->encoding = camel_transfer_encoding_from_string (encoding);

		/* would you believe you have to set this BEFORE you set the content object???  oh my god !!!! */
		camel_data_wrapper_set_mime_type_field (content, camel_mime_part_get_content_type (dw));
		camel_medium_set_content ((CamelMedium *) dw, content);
		g_object_unref (content);
	}

	g_free (encoding);

	return success;
}


/**
 * camel_message_content_info_new:
 *
 * Allocate a new #CamelMessageContentInfo.
 *
 * Returns: a newly allocated #CamelMessageContentInfo
 **/
CamelMessageContentInfo *
camel_message_content_info_new (void)
{
	return g_slice_alloc0 (sizeof (CamelMessageContentInfo));
}

/**
 * camel_message_content_info_free:
 * @ci: a #CamelMessageContentInfo
 *
 * Recursively frees the content info @ci, and all associated memory.
 **/
void
camel_message_content_info_free (CamelMessageContentInfo *ci)
{
	CamelMessageContentInfo *pw, *pn;

	pw = ci->childs;

	camel_content_type_unref (ci->type);
	g_free (ci->id);
	g_free (ci->description);
	g_free (ci->encoding);
	g_slice_free1 (sizeof (CamelMessageContentInfo), ci);

	while (pw) {
		pn = pw->next;
		camel_message_content_info_free (pw);
		pw = pn;
	}
}

CamelMessageContentInfo *
camel_message_content_info_new_from_parser (CamelMimeParser *mp)
{
	CamelMessageContentInfo *ci = NULL;
	CamelNameValueArray *headers = NULL;

	g_return_val_if_fail (CAMEL_IS_MIME_PARSER (mp), NULL);

	switch (camel_mime_parser_state (mp)) {
	case CAMEL_MIME_PARSER_STATE_HEADER:
	case CAMEL_MIME_PARSER_STATE_MESSAGE:
	case CAMEL_MIME_PARSER_STATE_MULTIPART:
		headers = camel_mime_parser_dup_headers (mp);
		ci = camel_message_content_info_new_from_header (headers);
		camel_name_value_array_free (headers);
		if (ci) {
			if (ci->type)
				camel_content_type_unref (ci->type);
			ci->type = camel_mime_parser_content_type (mp);
			camel_content_type_ref (ci->type);
		}
		break;
	default:
		g_error ("Invalid parser state");
	}

	return ci;
}

CamelMessageContentInfo *
camel_message_content_info_new_from_message (CamelMimePart *mp)
{
	CamelMessageContentInfo *ci = NULL;
	CamelNameValueArray *header = NULL;
	g_return_val_if_fail (CAMEL_IS_MIME_PART (mp), NULL);

	header = camel_medium_dup_headers (CAMEL_MEDIUM (mp));
	ci = camel_message_content_info_new_from_header (header);
	camel_name_value_array_free (header);
	return ci;
}

CamelMessageContentInfo *
camel_message_content_info_new_from_header (CamelNameValueArray *h)
{
	CamelMessageContentInfo *ci;
	const gchar *charset;

	ci = camel_message_content_info_new ();

	charset = camel_iconv_locale_charset ();
	ci->id = camel_header_msgid_decode (camel_name_value_array_get_named (h, TRUE, "content-id"));
	ci->description = camel_header_decode_string (camel_name_value_array_get_named (h, TRUE,"content-description"), charset);
	ci->encoding = camel_content_transfer_encoding_decode (camel_name_value_array_get_named (h, TRUE,"content-transfer-encoding"));
	ci->type = camel_content_type_decode (camel_name_value_array_get_named (h, TRUE,"content-type"));

	return ci;
}

void
camel_message_content_info_dump (CamelMessageContentInfo *ci,
				 gint depth)
{
	gchar *p;

	p = alloca (depth * 4 + 1);
	memset (p, ' ', depth * 4);
	p[depth * 4] = 0;

	if (ci == NULL) {
		printf ("%s<empty>\n", p);
		return;
	}

	if (ci->type)
		printf (
			"%scontent-type: %s/%s\n",
			p, ci->type->type ? ci->type->type : "(null)",
			ci->type->subtype ? ci->type->subtype : "(null)");
	else
		printf ("%scontent-type: <unset>\n", p);
	printf (
		"%scontent-transfer-encoding: %s\n",
		p, ci->encoding ? ci->encoding : "(null)");
	printf (
		"%scontent-description: %s\n",
		p, ci->description ? ci->description : "(null)");
	printf ("%ssize: %lu\n", p, (gulong) ci->size);
	ci = ci->childs;
	while (ci) {
		camel_message_content_info_dump (ci, depth + 1);
		ci = ci->next;
	}
}
