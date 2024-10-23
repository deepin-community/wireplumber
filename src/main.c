/* WirePlumber
 *
 * Copyright © 2019-2021 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <wp/wp.h>
#include <glib-unix.h>
#include <pipewire/pipewire.h>
#include <locale.h>

WP_DEFINE_LOCAL_LOG_TOPIC ("wireplumber")

enum WpExitCode
{
  /* based on sysexits.h */
  WP_EXIT_OK = 0,
  WP_EXIT_USAGE = 64,       /* command line usage error */
  WP_EXIT_UNAVAILABLE = 69, /* service unavailable */
  WP_EXIT_SOFTWARE = 70,    /* internal software error */
  WP_EXIT_CONFIG = 78,      /* configuration error */
};

static gboolean show_version = FALSE;
static gchar * config_file = NULL;
static gchar * profile = NULL;

static GOptionEntry entries[] =
{
  { "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &show_version,
    "Show version", NULL },
  { "config-file", 'c', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &config_file,
    "The configuration file to use", NULL },
  { "profile", 'p', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &profile,
    "The profile to load", NULL },
  { NULL }
};

/*** WpDaemon ***/

typedef struct
{
  WpCore *core;
  GMainLoop *loop;
  gint exit_code;
} WpDaemon;

static void
daemon_clear (WpDaemon * self)
{
  g_clear_pointer (&self->loop, g_main_loop_unref);
  g_clear_object (&self->core);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WpDaemon, daemon_clear)

static void
daemon_exit (WpDaemon * d, gint code)
{
  /* replace OK with an error, but do not replace error with OK */
  if (d->exit_code == WP_EXIT_OK)
    d->exit_code = code;
  g_main_loop_quit (d->loop);
}

static void
on_disconnected (WpCore *core, WpDaemon * d)
{
  wp_notice ("disconnected from pipewire");
  daemon_exit (d, WP_EXIT_OK);
}

static gboolean
signal_handler (int signal, gpointer data)
{
  WpDaemon *d = data;
  wp_notice ("stopped by signal: %s", strsignal (signal));
  daemon_exit (d, WP_EXIT_OK);
  return G_SOURCE_CONTINUE;
}

static gboolean
signal_handler_int (gpointer data)
{
  return signal_handler (SIGINT, data);
}

static gboolean
signal_handler_hup (gpointer data)
{
  return signal_handler (SIGHUP, data);
}

static gboolean
signal_handler_term (gpointer data)
{
  return signal_handler (SIGTERM, data);
}

static void
on_core_activated (WpObject * core, GAsyncResult * res, WpDaemon * d)
{
  g_autoptr (GError) error = NULL;

  if (!wp_object_activate_finish (core, res, &error)) {
    fprintf (stderr, "%s\n", error->message);

    switch (error->code) {
      case WP_LIBRARY_ERROR_SERVICE_UNAVAILABLE:
        daemon_exit (d, WP_EXIT_UNAVAILABLE);
        break;

      case WP_LIBRARY_ERROR_INVALID_ARGUMENT:
        daemon_exit (d, WP_EXIT_CONFIG);
        break;

      default:
        daemon_exit (d, WP_EXIT_SOFTWARE);
    }
  }
}

static void
warn_about_deprecated_config (void)
{
  g_autoptr (WpIterator) it = NULL;
  g_auto (GValue) val = G_VALUE_INIT;
  gboolean detected = FALSE;

  /* test only in directories that were used in 0.4 */
  const WpBaseDirsFlags flags = WP_BASE_DIRS_ENV_CONFIG |
      WP_BASE_DIRS_XDG_CONFIG_HOME |
      WP_BASE_DIRS_BUILD_SYSCONFDIR |
      WP_BASE_DIRS_BUILD_DATADIR |
      WP_BASE_DIRS_FLAG_SUBDIR_WIREPLUMBER;

  it = wp_base_dirs_new_files_iterator (
      flags, "main.lua.d", ".lua");
  for (; wp_iterator_next (it, &val); g_value_unset (&val)) {
    const gchar *file = g_value_get_string (&val);
    wp_notice ("Old configuration file detected: %s", file);
    detected = TRUE;
  }
  g_clear_pointer (&it, wp_iterator_unref);

  it = wp_base_dirs_new_files_iterator (
      flags, "policy.lua.d", ".lua");
  for (; wp_iterator_next (it, &val); g_value_unset (&val)) {
    const gchar *file = g_value_get_string (&val);
    wp_notice ("Old configuration file detected: %s", file);
    detected = TRUE;
  }
  g_clear_pointer (&it, wp_iterator_unref);

  it = wp_base_dirs_new_files_iterator (
      flags, "bluetooth.lua.d", ".lua");
  for (; wp_iterator_next (it, &val); g_value_unset (&val)) {
    const gchar *file = g_value_get_string (&val);
    wp_notice ("Old configuration file detected: %s", file);
    detected = TRUE;
  }
  g_clear_pointer (&it, wp_iterator_unref);

  if (detected) {
    wp_warning (
       "Lua configuration files are NOT supported in WirePlumber 0.5. "
       "You need to port them to the new format if you want to use them.\n"
       "-> See https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/migration.html");
  }
}

gint
main (gint argc, gchar **argv)
{
  g_auto (WpDaemon) d = {0};
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WpProperties) properties = NULL;
  g_autoptr (WpConf) conf = NULL;

  setlocale (LC_ALL, "");
  setlocale (LC_NUMERIC, "C");
  wp_init (WP_INIT_ALL);

  context = g_option_context_new ("- PipeWire Session/Policy Manager");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    fprintf (stderr, "%s\n", error->message);
    return WP_EXIT_USAGE;
  }

  if (show_version) {
    g_print ("%s\n"
        "Compiled with libwireplumber %s\n"
        "Linked with libwireplumber %s\n",
        argv[0],
        WIREPLUMBER_VERSION,
        wp_get_library_version());
    return WP_EXIT_OK;
  }

  if (!config_file)
    config_file = "wireplumber.conf";
  if (!profile)
    profile = "main";

  /* load configuration */
  conf = wp_conf_new_open (config_file, NULL, &error);
  if (!conf) {
    fprintf (stderr, "Failed to load configuration: %s\n", error->message);
    return WP_EXIT_CONFIG;
  }

  warn_about_deprecated_config ();

  /* prepare core properties */
  properties = wp_properties_new (
      PW_KEY_APP_VERSION, WIREPLUMBER_VERSION,
      "wireplumber.daemon", "true",
      "wireplumber.profile", profile,
      NULL);

  if (g_str_equal (profile, "main")) {
    wp_properties_set (properties, PW_KEY_APP_NAME, "WirePlumber");
  } else {
    g_autofree gchar *app_name = g_strdup_printf ("WirePlumber (%s)", profile);
    wp_properties_set (properties, PW_KEY_APP_NAME, app_name);
  }

  /* prefer manager socket */
  if (pw_check_library_version(0, 3, 84))
    wp_properties_set (properties, PW_KEY_REMOTE_NAME,
        ("[" PW_DEFAULT_REMOTE "-manager," PW_DEFAULT_REMOTE "]"));

  /* init wireplumber daemon */
  d.loop = g_main_loop_new (NULL, FALSE);
  d.core = wp_core_new (NULL, g_steal_pointer (&conf),
      g_steal_pointer (&properties));
  g_signal_connect (d.core, "disconnected", G_CALLBACK (on_disconnected), &d);

  /* watch for exit signals */
  g_unix_signal_add (SIGINT, signal_handler_int, &d);
  g_unix_signal_add (SIGTERM, signal_handler_term, &d);
  g_unix_signal_add (SIGHUP, signal_handler_hup, &d);

  wp_object_activate (WP_OBJECT (d.core), WP_OBJECT_FEATURES_ALL, NULL,
      (GAsyncReadyCallback) on_core_activated, &d);

  /* run */
  g_main_loop_run (d.loop);
  wp_core_disconnect (d.core);
  return d.exit_code;
}
