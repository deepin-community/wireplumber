/* WirePlumber
 *
 * Copyright © 2020 Collabora Ltd.
 *    @author Julian Bouzas <julian.bouzas@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <errno.h>

#include "log.h"
#include "state.h"
#include "wp.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("wp-state")

#define DEFAULT_TIMEOUT_MS 1000
#define ESCAPED_CHARACTER '\\'

static char *
escape_string (const gchar *str)
{
  char *res = NULL;
  size_t str_size, i, j;

  g_return_val_if_fail (str, NULL);
  str_size = strlen (str);
  g_return_val_if_fail (str_size > 0, NULL);

  res = g_malloc_n ((str_size * 2) + 1, sizeof(gchar));

  j = 0;
  for (i = 0; i < str_size; i++) {
    switch (str[i]) {
      case ESCAPED_CHARACTER:
        res[j++] = ESCAPED_CHARACTER;
        res[j++] = ESCAPED_CHARACTER;
        break;
      case ' ':
        res[j++] = ESCAPED_CHARACTER;
        res[j++] = 's';
        break;
      case '=':
        res[j++] = ESCAPED_CHARACTER;
        res[j++] = 'e';
        break;
      case '[':
        res[j++] = ESCAPED_CHARACTER;
        res[j++] = 'o';
        break;
      case ']':
        res[j++] = ESCAPED_CHARACTER;
        res[j++] = 'c';
        break;
      default:
        res[j++] = str[i];
        break;
    }
  }
  res[j++] = '\0';

  return res;
}

static char *
compress_string (const gchar *str)
{
  char *res = NULL;
  size_t str_size, i, j;

  g_return_val_if_fail (str, NULL);
  str_size = strlen (str);
  g_return_val_if_fail (str_size > 0, NULL);

  res = g_malloc_n (str_size + 1, sizeof(gchar));

  j = 0;
  for (i = 0; i < str_size - 1; i++) {
    if (str[i] == ESCAPED_CHARACTER) {
      switch (str[i + 1]) {
        case ESCAPED_CHARACTER:
          res[j++] = ESCAPED_CHARACTER;
          break;
        case 's':
          res[j++] = ' ';
          break;
        case 'e':
          res[j++] = '=';
          break;
        case 'o':
          res[j++] = '[';
          break;
        case 'c':
          res[j++] = ']';
          break;
        default:
          res[j++] = str[i];
          break;
      }
      i++;
    } else {
      res[j++] = str[i];
    }
  }
  if (i < str_size)
    res[j++] = str[i];
  res[j++] = '\0';

  return res;
}

/*! \defgroup wpstate WpState */
/*!
 * \struct WpState
 *
 * The WpState class saves and loads properties from a file
 *
 * \gproperties
 * \gproperty{name, gchar *, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY,
 *   The file name where the state will be stored.}
 */

enum {
  PROP_0,
  PROP_NAME,
  PROP_TIMEOUT,
};

struct _WpState
{
  GObject parent;

  /* Props */
  gchar *name;
  guint timeout;

  gchar *location;
  GSource *timeout_source;
  WpProperties *timeout_props;
};

G_DEFINE_TYPE (WpState, wp_state, G_TYPE_OBJECT)

/* Gets the full path to the WirePlumber XDG_STATE_HOME subdirectory */
static const gchar *
wp_get_xdg_state_dir (void)
{
  static gchar xdg_dir[PATH_MAX] = {0};
  if (xdg_dir[0] == '\0') {
    g_autofree gchar *path = NULL;
    g_autofree gchar *base = g_strdup (g_getenv ("XDG_STATE_HOME"));
    if (!base)
      base = g_build_filename (g_get_home_dir (), ".local", "state", NULL);

    path = g_build_filename (base, "wireplumber", NULL);
    (void) g_strlcpy (xdg_dir, path, sizeof (xdg_dir));
  }
  return xdg_dir;
}

static char *
get_new_location (const char *name)
{
  const gchar *path = wp_get_xdg_state_dir ();

  /* Create the directory if it doesn't exist */
  if (g_mkdir_with_parents (path, 0700) < 0)
    wp_warning ("failed to create directory %s: %s", path, g_strerror (errno));

  return g_build_filename (path, name, NULL);
}

static void
wp_state_ensure_location (WpState *self)
{
  if (!self->location)
    self->location = get_new_location (self->name);
  g_return_if_fail (self->location);
}

static void
wp_state_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  WpState *self = WP_STATE (object);

  switch (property_id) {
  case PROP_NAME:
    g_clear_pointer (&self->name, g_free);
    self->name = g_value_dup_string (value);
    break;
  case PROP_TIMEOUT:
    self->timeout = g_value_get_uint (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_state_get_property (GObject * object, guint property_id, GValue * value,
    GParamSpec * pspec)
{
  WpState *self = WP_STATE (object);

  switch (property_id) {
  case PROP_NAME:
    g_value_set_string (value, self->name);
    break;
  case PROP_TIMEOUT:
    g_value_set_uint (value, self->timeout);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_state_finalize (GObject * object)
{
  WpState * self = WP_STATE (object);

  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->location, g_free);
  g_clear_pointer (&self->timeout_source, g_source_unref);
  g_clear_pointer (&self->timeout_props, wp_properties_unref);

  G_OBJECT_CLASS (wp_state_parent_class)->finalize (object);
}

static void
wp_state_init (WpState * self)
{
  self->timeout = DEFAULT_TIMEOUT_MS;
}

static void
wp_state_class_init (WpStateClass * klass)
{
  GObjectClass *object_class = (GObjectClass *) klass;

  object_class->finalize = wp_state_finalize;
  object_class->set_property = wp_state_set_property;
  object_class->get_property = wp_state_get_property;

  g_object_class_install_property (object_class, PROP_NAME,
      g_param_spec_string ("name", "name",
          "The file name where the state will be stored", NULL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_TIMEOUT,
      g_param_spec_uint ("timeout", "timeout",
          "The timeout in milliseconds to save the state", 0, G_MAXUINT,
          DEFAULT_TIMEOUT_MS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

/*!
 * \brief Constructs a new state object
 * \ingroup wpstate
 * \param name the state name
 * \returns (transfer full): the new WpState
 */
WpState *
wp_state_new (const gchar *name)
{
  g_return_val_if_fail (name, NULL);
  return g_object_new (wp_state_get_type (),
      "name", name,
      NULL);
}

/*!
 * \brief Gets the name of a state object
 * \ingroup wpstate
 * \param self the state
 * \returns the name of this state
 */
const gchar *
wp_state_get_name (WpState *self)
{
  g_return_val_if_fail (WP_IS_STATE (self), NULL);

  return self->name;
}

/*!
 * \brief Gets the location of a state object
 * \ingroup wpstate
 * \param self the state
 * \returns the location of this state
 */
const gchar *
wp_state_get_location (WpState *self)
{
  g_return_val_if_fail (WP_IS_STATE (self), NULL);
  wp_state_ensure_location (self);

  return self->location;
}

/*!
 * \brief Clears the state removing its file
 * \ingroup wpstate
 * \param self the state
 */
void
wp_state_clear (WpState *self)
{
  g_return_if_fail (WP_IS_STATE (self));
  wp_state_ensure_location (self);
  if (remove (self->location) < 0)
    wp_warning ("failed to remove %s: %s", self->location, g_strerror (errno));
}

/*!
 * \brief Saves new properties in the state, overwriting all previous data.
 * \ingroup wpstate
 * \param self the state
 * \param props (transfer none): the properties to save
 * \param error (out)(optional): return location for a GError, or NULL
 * \returns TRUE if the properties could be saved, FALSE otherwise
 */
gboolean
wp_state_save (WpState *self, WpProperties *props, GError ** error)
{
  g_autoptr (GKeyFile) keyfile = g_key_file_new ();
  g_autoptr (WpIterator) it = NULL;
  g_auto (GValue) item = G_VALUE_INIT;
  GError *err = NULL;

  g_return_val_if_fail (WP_IS_STATE (self), FALSE);
  g_return_val_if_fail (props, FALSE);
  wp_state_ensure_location (self);

  wp_info_object (self, "saving state into %s", self->location);

  /* Set the properties */
  for (it = wp_properties_new_iterator (props);
      wp_iterator_next (it, &item);
      g_value_unset (&item)) {
    WpPropertiesItem *pi = g_value_get_boxed (&item);
    const gchar *key = wp_properties_item_get_key (pi);
    const gchar *val = wp_properties_item_get_value (pi);
    g_autofree gchar *escaped_key = escape_string (key);
    if (escaped_key)
      g_key_file_set_string (keyfile, self->name, escaped_key, val);
  }

  if (!g_key_file_save_to_file (keyfile, self->location, &err)) {
    g_propagate_prefixed_error (error, err, "could not save %s: ", self->name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
timeout_save_state_callback (WpState *self)
{
  g_autoptr (GError) error = NULL;

  if (!wp_state_save (self, self->timeout_props, &error))
    wp_warning_object (self, "%s", error->message);

  g_clear_pointer (&self->timeout_source, g_source_unref);
  g_clear_pointer (&self->timeout_props, wp_properties_unref);

  return G_SOURCE_REMOVE;
}

/*!
 * \brief Saves new properties in the state, overwriting all previous data,
 *   after a timeout
 *
 * This is similar to wp_state_save() but it will save the state after a timeout
 * has elapsed. If the state is saved again before the timeout elapses, the
 * timeout is reset.
 *
 * This function is useful to avoid saving the state too often. When called
 * consecutively, it will save the state only once. Every time it is called,
 * it will cancel the previous timer and start a new one, resulting in timing
 * out only after the last call.
 *
 * \ingroup wpstate
 * \param self the state
 * \param core the core, used to add the timeout callback to the main loop
 * \param props (transfer none): the properties to save. This object will be
 *   referenced and kept alive until the timeout elapses, but not deep copied.
 * \since 0.5.0
 */
void
wp_state_save_after_timeout (WpState *self, WpCore *core, WpProperties *props)
{
  /* Clear the current timeout callback */
  if (self->timeout_source)
    g_source_destroy (self->timeout_source);
  g_clear_pointer (&self->timeout_source, g_source_unref);
  g_clear_pointer (&self->timeout_props, wp_properties_unref);

  self->timeout_props = wp_properties_ref (props);

  /* Add the timeout callback */
  wp_core_timeout_add_closure (core, &self->timeout_source, self->timeout,
      g_cclosure_new_object (G_CALLBACK (timeout_save_state_callback),
          G_OBJECT (self)));
}

/*!
 * \brief Loads the state data from the file system
 *
 * This function will never fail. If it cannot load the state, for any reason,
 * it will simply return an empty WpProperties, behaving as if there was no
 * previous state stored.
 *
 * \ingroup wpstate
 * \param self the state
 * \returns (transfer full): a new WpProperties containing the state data
 */
WpProperties *
wp_state_load (WpState *self)
{
  g_autoptr (GKeyFile) keyfile = g_key_file_new ();
  g_autoptr (WpProperties) props = wp_properties_new_empty ();
  gchar ** keys = NULL;

  g_return_val_if_fail (WP_IS_STATE (self), NULL);
  wp_state_ensure_location (self);

  /* Open */
  if (!g_key_file_load_from_file (keyfile, self->location,
      G_KEY_FILE_NONE, NULL))
    return g_steal_pointer (&props);

  /* Load all keys */
  keys = g_key_file_get_keys (keyfile, self->name, NULL, NULL);
  if (!keys)
    return g_steal_pointer (&props);

  for (guint i = 0; keys[i]; i++) {
    g_autofree gchar *compressed_key = NULL;
    const gchar *key = keys[i];
    g_autofree gchar *val = NULL;
    val = g_key_file_get_string (keyfile, self->name, key, NULL);
    if (!val)
      continue;
    compressed_key = compress_string (key);
    if (compressed_key)
      wp_properties_set (props, compressed_key, val);
  }

  g_strfreev (keys);

  return g_steal_pointer (&props);
}
