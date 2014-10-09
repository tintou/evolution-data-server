/*
 * e-source-mail-composition.c
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
 */

/**
 * SECTION: e-source-mail-composition
 * @include: libedataserver/libedataserver.h
 * @short_description: #ESource extension for mail composition settings
 *
 * The #ESourceMailComposition extension tracks settings to be applied
 * when composing a new mail message.
 *
 * Access the extension as follows:
 *
 * |[
 *   #include <libedataserver/libedataserver.h>
 *
 *   ESourceMailComposition *extension;
 *
 *   extension = e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_COMPOSITION);
 * ]|
 **/

#include "e-source-mail-composition.h"

#include <libedataserver/e-data-server-util.h>

#define E_SOURCE_MAIL_COMPOSITION_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_SOURCE_MAIL_COMPOSITION, ESourceMailCompositionPrivate))

struct _ESourceMailCompositionPrivate {
	GMutex property_lock;
	gchar **bcc;
	gchar **cc;
	gchar *drafts_folder;
	gchar *templates_folder;
	gboolean sign_imip;
};

enum {
	PROP_0,
	PROP_BCC,
	PROP_CC,
	PROP_DRAFTS_FOLDER,
	PROP_SIGN_IMIP,
	PROP_TEMPLATES_FOLDER
};

G_DEFINE_TYPE (
	ESourceMailComposition,
	e_source_mail_composition,
	E_TYPE_SOURCE_EXTENSION)

static void
source_mail_composition_set_property (GObject *object,
                                      guint property_id,
                                      const GValue *value,
                                      GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_BCC:
			e_source_mail_composition_set_bcc (
				E_SOURCE_MAIL_COMPOSITION (object),
				g_value_get_boxed (value));
			return;

		case PROP_CC:
			e_source_mail_composition_set_cc (
				E_SOURCE_MAIL_COMPOSITION (object),
				g_value_get_boxed (value));
			return;

		case PROP_DRAFTS_FOLDER:
			e_source_mail_composition_set_drafts_folder (
				E_SOURCE_MAIL_COMPOSITION (object),
				g_value_get_string (value));
			return;

		case PROP_SIGN_IMIP:
			e_source_mail_composition_set_sign_imip (
				E_SOURCE_MAIL_COMPOSITION (object),
				g_value_get_boolean (value));
			return;

		case PROP_TEMPLATES_FOLDER:
			e_source_mail_composition_set_templates_folder (
				E_SOURCE_MAIL_COMPOSITION (object),
				g_value_get_string (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
source_mail_composition_get_property (GObject *object,
                                      guint property_id,
                                      GValue *value,
                                      GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_BCC:
			g_value_take_boxed (
				value,
				e_source_mail_composition_dup_bcc (
				E_SOURCE_MAIL_COMPOSITION (object)));
			return;

		case PROP_CC:
			g_value_take_boxed (
				value,
				e_source_mail_composition_dup_cc (
				E_SOURCE_MAIL_COMPOSITION (object)));
			return;

		case PROP_DRAFTS_FOLDER:
			g_value_take_string (
				value,
				e_source_mail_composition_dup_drafts_folder (
				E_SOURCE_MAIL_COMPOSITION (object)));
			return;

		case PROP_SIGN_IMIP:
			g_value_set_boolean (
				value,
				e_source_mail_composition_get_sign_imip (
				E_SOURCE_MAIL_COMPOSITION (object)));
			return;

		case PROP_TEMPLATES_FOLDER:
			g_value_take_string (
				value,
				e_source_mail_composition_dup_templates_folder (
				E_SOURCE_MAIL_COMPOSITION (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
source_mail_composition_finalize (GObject *object)
{
	ESourceMailCompositionPrivate *priv;

	priv = E_SOURCE_MAIL_COMPOSITION_GET_PRIVATE (object);

	g_mutex_clear (&priv->property_lock);

	g_strfreev (priv->bcc);
	g_strfreev (priv->cc);
	g_free (priv->drafts_folder);
	g_free (priv->templates_folder);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_source_mail_composition_parent_class)->
		finalize (object);
}

static void
e_source_mail_composition_class_init (ESourceMailCompositionClass *class)
{
	GObjectClass *object_class;
	ESourceExtensionClass *extension_class;

	g_type_class_add_private (
		class, sizeof (ESourceMailCompositionPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = source_mail_composition_set_property;
	object_class->get_property = source_mail_composition_get_property;
	object_class->finalize = source_mail_composition_finalize;

	extension_class = E_SOURCE_EXTENSION_CLASS (class);
	extension_class->name = E_SOURCE_EXTENSION_MAIL_COMPOSITION;

	g_object_class_install_property (
		object_class,
		PROP_BCC,
		g_param_spec_boxed (
			"bcc",
			"Bcc",
			"Recipients to blind carbon-copy",
			G_TYPE_STRV,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			G_PARAM_STATIC_STRINGS |
			E_SOURCE_PARAM_SETTING));

	g_object_class_install_property (
		object_class,
		PROP_CC,
		g_param_spec_boxed (
			"cc",
			"Cc",
			"Recipients to carbon-copy",
			G_TYPE_STRV,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			G_PARAM_STATIC_STRINGS |
			E_SOURCE_PARAM_SETTING));

	g_object_class_install_property (
		object_class,
		PROP_DRAFTS_FOLDER,
		g_param_spec_string (
			"drafts-folder",
			"Drafts Folder",
			"Preferred folder for draft messages",
			NULL,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			G_PARAM_STATIC_STRINGS |
			E_SOURCE_PARAM_SETTING));

	g_object_class_install_property (
		object_class,
		PROP_SIGN_IMIP,
		g_param_spec_boolean (
			"sign-imip",
			"Sign iMIP",
			"Include iMIP messages when signing",
			TRUE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			G_PARAM_STATIC_STRINGS |
			E_SOURCE_PARAM_SETTING));

	g_object_class_install_property (
		object_class,
		PROP_TEMPLATES_FOLDER,
		g_param_spec_string (
			"templates-folder",
			"Templates Folder",
			"Preferred folder for message templates",
			NULL,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			G_PARAM_STATIC_STRINGS |
			E_SOURCE_PARAM_SETTING));
}

static void
e_source_mail_composition_init (ESourceMailComposition *extension)
{
	extension->priv = E_SOURCE_MAIL_COMPOSITION_GET_PRIVATE (extension);
	g_mutex_init (&extension->priv->property_lock);
}

/**
 * e_source_mail_composition_get_bcc:
 * @extension: an #ESourceMailComposition
 *
 * Returns a %NULL-terminated string array of recipients which should
 * automatically be added to the blind carbon-copy (Bcc) list when
 * composing a new mail message.  The recipient strings should be of
 * the form "Full Name &lt;email-address&gt;".  The returned array is
 * owned by @extension and should not be modified or freed.
 *
 * Returns: (transfer none): a %NULL-terminated string array of Bcc recipients
 *
 * Since: 3.6
 **/
const gchar * const *
e_source_mail_composition_get_bcc (ESourceMailComposition *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension), NULL);

	return (const gchar * const *) extension->priv->bcc;
}

/**
 * e_source_mail_composition_dup_bcc:
 * @extension: an #ESourceMailComposition
 *
 * Thread-safe variation of e_source_mail_composition_get_bcc().
 * Use this function when accessing @extension from multiple threads.
 *
 * The returned string array should be freed with g_strfreev() when no
 * longer needed.
 *
 * Returns: (transfer full): a newly-allocated copy of
 * #ESourceMailComposition:bcc
 *
 * Since: 3.6
 **/
gchar **
e_source_mail_composition_dup_bcc (ESourceMailComposition *extension)
{
	const gchar * const *protected;
	gchar **duplicate;

	g_return_val_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension), NULL);

	g_mutex_lock (&extension->priv->property_lock);

	protected = e_source_mail_composition_get_bcc (extension);
	duplicate = g_strdupv ((gchar **) protected);

	g_mutex_unlock (&extension->priv->property_lock);

	return duplicate;
}

/**
 * e_source_mail_composition_set_bcc:
 * @extension: an #ESource
 * @bcc: (nullable) (optional): a %NULL-terminated string array of Bcc recipients
 *
 * Sets the recipients which should automatically be added to the blind
 * carbon-copy (Bcc) list when composing a new mail message.  The recipient
 * strings should be of the form "Full Name &lt;email-address&gt;".
 *
 * Since: 3.6
 **/
void
e_source_mail_composition_set_bcc (ESourceMailComposition *extension,
                                   const gchar * const *bcc)
{
	g_return_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension));

	g_mutex_lock (&extension->priv->property_lock);

	if (e_util_strv_equal (bcc, extension->priv->bcc)) {
		g_mutex_unlock (&extension->priv->property_lock);
		return;
	}

	g_strfreev (extension->priv->bcc);
	extension->priv->bcc = g_strdupv ((gchar **) bcc);

	g_mutex_unlock (&extension->priv->property_lock);

	g_object_notify (G_OBJECT (extension), "bcc");
}

/**
 * e_source_mail_composition_get_cc:
 * @extension: an #ESourceMailComposition
 *
 * Returns a %NULL-terminated string array of recipients which should
 * automatically be added to the carbon-copy (Cc) list when composing a
 * new mail message.  The recipient strings should be of the form "Full
 * Name <email-address>".  The returned array is owned by @extension and
 * should not be modified or freed.
 *
 * Returns: (transfer none): a %NULL-terminated string array of Cc recipients
 *
 * Since: 3.6
 **/
const gchar * const *
e_source_mail_composition_get_cc (ESourceMailComposition *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension), NULL);

	return (const gchar * const *) extension->priv->cc;
}

/**
 * e_source_mail_composition_dup_cc:
 * @extension: an #ESourceMailComposition
 *
 * Thread-safe variation of e_source_mail_composition_get_cc().
 * Use this function when accessing @extension from multiple threads.
 *
 * The returned string array should be freed with g_strfreev() when no
 * longer needed.
 *
 * Returns: (transfer full): a newly-allocated copy of
 * #ESourceMailComposition:cc
 *
 * Since: 3.6
 **/
gchar **
e_source_mail_composition_dup_cc (ESourceMailComposition *extension)
{
	const gchar * const *protected;
	gchar **duplicate;

	g_return_val_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension), NULL);

	g_mutex_lock (&extension->priv->property_lock);

	protected = e_source_mail_composition_get_cc (extension);
	duplicate = g_strdupv ((gchar **) protected);

	g_mutex_unlock (&extension->priv->property_lock);

	return duplicate;
}

/**
 * e_source_mail_composition_set_cc:
 * @extension: an #ESourceMailComposition
 * @cc: (nullable) (optional): a %NULL-terminated string array of Cc recipients
 *
 * Sets the recipients which should automatically be added to the carbon
 * copy (Cc) list when composing a new mail message.  The recipient strings
 * should be of the form "Full Name &lt;email-address&gt;".
 *
 * Since: 3.6
 **/
void
e_source_mail_composition_set_cc (ESourceMailComposition *extension,
                                  const gchar * const *cc)
{
	g_return_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension));

	g_mutex_lock (&extension->priv->property_lock);

	if (e_util_strv_equal (cc, extension->priv->cc)) {
		g_mutex_unlock (&extension->priv->property_lock);
		return;
	}

	g_strfreev (extension->priv->cc);
	extension->priv->cc = g_strdupv ((gchar **) cc);

	g_mutex_unlock (&extension->priv->property_lock);

	g_object_notify (G_OBJECT (extension), "cc");
}

/**
 * e_source_mail_composition_get_drafts_folder:
 * @extension: an #ESourceMailComposition
 * 
 * Returns a string identifying the preferred folder for draft messages.
 * The format of the identifier string is defined by the client application.
 *
 * Returns: an identifier for the preferred drafts folder
 *
 * Since: 3.6
 **/
const gchar *
e_source_mail_composition_get_drafts_folder (ESourceMailComposition *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension), NULL);

	return extension->priv->drafts_folder;
}

/**
 * e_source_mail_composition_dup_drafts_folder:
 * @extension: an #ESourceMailComposition
 *
 * Thread-safe variation of e_source_mail_composition_get_drafts_folder().
 * Use this function when accessing @extension from multiple threads.
 *
 * The returned string should be freed with g_free() when no longer needed.
 *
 * Returns: a newly-allocated copy of #ESourceMailComposition:drafts-folder
 *
 * Since: 3.6
 **/
gchar *
e_source_mail_composition_dup_drafts_folder (ESourceMailComposition *extension)
{
	const gchar *protected;
	gchar *duplicate;

	g_return_val_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension), NULL);

	g_mutex_lock (&extension->priv->property_lock);

	protected = e_source_mail_composition_get_drafts_folder (extension);
	duplicate = g_strdup (protected);

	g_mutex_unlock (&extension->priv->property_lock);

	return duplicate;
}

/**
 * e_source_mail_composition_set_drafts_folder:
 * @extension: an #ESourceMailComposition
 * @drafts_folder: (nullable) (optional): an identifier for the preferred drafts
 *                 folder, or %NULL
 *
 * Sets the preferred folder for draft messages by an identifier string.
 * The format of the identifier string is defined by the client application.
 *
 * The internal copy of @drafts_folder is automatically stripped of
 * leading and trailing whitespace.  If the resulting string is empty,
 * %NULL is set instead.
 *
 * Since: 3.6
 **/
void
e_source_mail_composition_set_drafts_folder (ESourceMailComposition *extension,
                                             const gchar *drafts_folder)
{
	g_return_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension));

	g_mutex_lock (&extension->priv->property_lock);

	if (g_strcmp0 (extension->priv->drafts_folder, drafts_folder) == 0) {
		g_mutex_unlock (&extension->priv->property_lock);
		return;
	}

	g_free (extension->priv->drafts_folder);
	extension->priv->drafts_folder = e_util_strdup_strip (drafts_folder);

	g_mutex_unlock (&extension->priv->property_lock);

	g_object_notify (G_OBJECT (extension), "drafts-folder");
}

/**
 * e_source_mail_composition_get_sign_imip:
 * @extension: an #ESourceMailComposition
 *
 * Returns whether outgoing iMIP messages such as meeting requests should
 * also be signed.  This is primarily intended as a workaround for certain
 * versions of Microsoft Outlook which can't handle signed iMIP messages.
 *
 * Returns: whether outgoing iMIP messages should be signed
 *
 * Since: 3.6
 **/
gboolean
e_source_mail_composition_get_sign_imip (ESourceMailComposition *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension), FALSE);

	return extension->priv->sign_imip;
}

/**
 * e_source_mail_composition_set_sign_imip:
 * @extension: an #ESourceMailComposition
 * @sign_imip: whether outgoing iMIP messages should be signed
 *
 * Sets whether outgoing iMIP messages such as meeting requests should
 * also be signed.  This is primarily intended as a workaround for certain
 * versions of Microsoft Outlook which can't handle signed iMIP messages.
 *
 * Since: 3.6
 **/
void
e_source_mail_composition_set_sign_imip (ESourceMailComposition *extension,
                                         gboolean sign_imip)
{
	g_return_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension));

	if (extension->priv->sign_imip == sign_imip)
		return;

	extension->priv->sign_imip = sign_imip;

	g_object_notify (G_OBJECT (extension), "sign-imip");
}

/**
 * e_source_mail_composition_get_templates_folder:
 * @extension: an #ESourceMailComposition
 *
 * Returns a string identifying the preferred folder for message templates.
 * The format of the identifier string is defined by the client application.
 *
 * Returns: an identifier for the preferred templates folder
 *
 * Since: 3.6
 **/
const gchar *
e_source_mail_composition_get_templates_folder (ESourceMailComposition *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension), NULL);

	return extension->priv->templates_folder;
}

/**
 * e_source_mail_composition_dup_templates_folder:
 * @extension: an #ESourceMailComposition
 *
 * Thread-safe variation of e_source_mail_composition_get_templates_folder().
 * Use this function when accessing @extension from multiple threads.
 *
 * The returned string should be freed with g_free() when no longer needed.
 *
 * Returns: a newly-allocated copy of #ESourceMailComposition:templates-folder
 *
 * Since: 3.6
 **/
gchar *
e_source_mail_composition_dup_templates_folder (ESourceMailComposition *extension)
{
	const gchar *protected;
	gchar *duplicate;

	g_return_val_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension), NULL);

	g_mutex_lock (&extension->priv->property_lock);

	protected = e_source_mail_composition_get_templates_folder (extension);
	duplicate = g_strdup (protected);

	g_mutex_unlock (&extension->priv->property_lock);

	return duplicate;
}

/**
 * e_source_mail_composition_set_templates_folder:
 * @extension: an #ESourceMailComposition
 * @templates_folder: (nullable) (optional): an identifier for the preferred templates
 *                    folder, or %NULL
 *
 * Sets the preferred folder for message templates by an identifier string.
 * The format of the identifier string is defined by the client application.
 *
 * The internal copy of @templates_folder is automatically stripped of
 * leading and trailing whitespace.  If the resulting string is empty,
 * %NULL is set instead.
 *
 * Since: 3.6
 **/
void
e_source_mail_composition_set_templates_folder (ESourceMailComposition *extension,
                                                const gchar *templates_folder)
{
	g_return_if_fail (E_IS_SOURCE_MAIL_COMPOSITION (extension));

	g_mutex_lock (&extension->priv->property_lock);

	if (g_strcmp0 (extension->priv->templates_folder, templates_folder) == 0) {
		g_mutex_unlock (&extension->priv->property_lock);
		return;
	}

	g_free (extension->priv->templates_folder);
	extension->priv->templates_folder = e_util_strdup_strip (templates_folder);

	g_mutex_unlock (&extension->priv->property_lock);

	g_object_notify (G_OBJECT (extension), "templates-folder");
}
