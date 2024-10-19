/* WirePlumber
 *
 * Copyright © 2021 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "plugin.h"
#include "reserve-device.h"
#include "reserve-device-enums.h"
#include "../dbus-connection-state.h"

WP_LOG_TOPIC (log_topic_rd, "m-reserve-device")

G_DEFINE_TYPE (WpReserveDevicePlugin, wp_reserve_device_plugin, WP_TYPE_PLUGIN)

enum
{
  ACTION_CREATE_RESERVATION,
  ACTION_DESTROY_RESERVATION,
  ACTION_GET_RESERVATION,
  ACTION_GET_DBUS,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
rd_unref (gpointer data)
{
  WpReserveDevice *rd = data;
  g_signal_emit_by_name (rd, "release");
  g_object_unref (rd);
}

static void
clear_reservation (WpReserveDevicePlugin *self)
{
  g_hash_table_remove_all (self->reserve_devices);
  g_clear_object (&self->manager);
}

static void
on_dbus_state_changed (GObject * dbus, GParamSpec * spec,
    WpReserveDevicePlugin *self)
{
  WpDBusConnectionState state = -1;
  g_object_get (dbus, "state", &state, NULL);

  switch (state) {
    case WP_DBUS_CONNECTION_STATE_CONNECTED: {
      g_autoptr (GDBusConnection) conn = NULL;

      g_object_get (dbus, "connection", &conn, NULL);
      g_return_if_fail (conn);

      self->manager = g_dbus_object_manager_server_new (
          FDO_RESERVE_DEVICE1_PATH);
      g_dbus_object_manager_server_set_connection (self->manager, conn);
      break;
    }

    case WP_DBUS_CONNECTION_STATE_CONNECTING:
    case WP_DBUS_CONNECTION_STATE_CLOSED:
      clear_reservation (self);
      break;

    default:
      g_assert_not_reached ();
  }
}

static void
wp_reserve_device_plugin_init (WpReserveDevicePlugin * self)
{
  self->reserve_devices = g_hash_table_new_full (g_str_hash, g_str_equal,
      NULL, rd_unref);
}

static void
wp_reserve_device_plugin_finalize (GObject * object)
{
  WpReserveDevicePlugin *self = WP_RESERVE_DEVICE_PLUGIN (object);

  g_clear_pointer (&self->reserve_devices, g_hash_table_unref);

  G_OBJECT_CLASS (wp_reserve_device_plugin_parent_class)->finalize (object);
}

static void
wp_reserve_device_plugin_enable (WpPlugin * plugin, WpTransition * transition)
{
  WpReserveDevicePlugin *self = WP_RESERVE_DEVICE_PLUGIN (plugin);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));

  self->dbus = wp_plugin_find (core, "dbus-connection");
  if (!self->dbus) {
    wp_transition_return_error (transition, g_error_new (WP_DOMAIN_LIBRARY,
        WP_LIBRARY_ERROR_INVARIANT,
        "dbus-connection module must be loaded before reserve-device"));
    return;
  }

  g_signal_connect_object (self->dbus, "notify::state",
       G_CALLBACK (on_dbus_state_changed), self, 0);
  on_dbus_state_changed (G_OBJECT (self->dbus), NULL, self);

  wp_object_update_features (WP_OBJECT (self), WP_PLUGIN_FEATURE_ENABLED, 0);
}

static void
wp_reserve_device_plugin_disable (WpPlugin * plugin)
{
  WpReserveDevicePlugin *self = WP_RESERVE_DEVICE_PLUGIN (plugin);

  clear_reservation (self);
  g_clear_object (&self->dbus);

  wp_object_update_features (WP_OBJECT (self), 0, WP_PLUGIN_FEATURE_ENABLED);
}

static gpointer
wp_reserve_device_plugin_create_reservation (WpReserveDevicePlugin *self,
    const gchar *name, const gchar *app_name, const gchar *app_dev_name,
    gint priority)
{
  WpDBusConnectionState state = WP_DBUS_CONNECTION_STATE_CLOSED;
  g_object_get (self->dbus, "state", &state, NULL);

  if (state != WP_DBUS_CONNECTION_STATE_CONNECTED) {
    wp_notice_object (self, "not connected to D-Bus");
    return NULL;
  }

  WpReserveDevice *rd = g_object_new (wp_reserve_device_get_type (),
      "plugin", self,
      "name", name,
      "application-name", app_name,
      "application-device-name", app_dev_name,
      "priority", priority,
      NULL);

  /* use rd->name to avoid copying @em name again */
  g_hash_table_replace (self->reserve_devices, rd->name, rd);

  return g_object_ref (rd);
}

static void
wp_reserve_device_plugin_destroy_reservation (WpReserveDevicePlugin *self,
    const gchar *name)
{
  WpDBusConnectionState state = WP_DBUS_CONNECTION_STATE_CLOSED;
  g_object_get (self->dbus, "state", &state, NULL);

  if (state != WP_DBUS_CONNECTION_STATE_CONNECTED) {
    wp_notice_object (self, "not connected to D-Bus");
    return;
  }
  g_hash_table_remove (self->reserve_devices, name);
}

static gpointer
wp_reserve_device_plugin_get_reservation (WpReserveDevicePlugin *self,
    const gchar *name)
{
  WpDBusConnectionState state = WP_DBUS_CONNECTION_STATE_CLOSED;
  g_object_get (self->dbus, "state", &state, NULL);

  if (state != WP_DBUS_CONNECTION_STATE_CONNECTED) {
    wp_notice_object (self, "not connected to D-Bus");
    return NULL;
  }

  WpReserveDevice *rd = g_hash_table_lookup (self->reserve_devices, name);
  return rd ? g_object_ref (rd) : NULL;
}

static gpointer
wp_reserve_device_plugin_get_dbus (WpReserveDevicePlugin *self)
{
  return self->dbus ? g_object_ref (self->dbus) : NULL;
}

static void
wp_reserve_device_plugin_class_init (WpReserveDevicePluginClass * klass)
{
  GObjectClass *object_class = (GObjectClass *) klass;
  WpPluginClass *plugin_class = (WpPluginClass *) klass;

  object_class->finalize = wp_reserve_device_plugin_finalize;

  plugin_class->enable = wp_reserve_device_plugin_enable;
  plugin_class->disable = wp_reserve_device_plugin_disable;

  /**
   * WpReserveDevicePlugin::create-reservation:
   *
   * @brief
   * @em name:
   * @em app_name:
   * @em app_dev_name:
   * @em priority:
   *
   * Returns: (transfer full): the reservation object
   */
  signals[ACTION_CREATE_RESERVATION] = g_signal_new_class_handler (
      "create-reservation", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      (GCallback) wp_reserve_device_plugin_create_reservation,
      NULL, NULL, NULL,
      G_TYPE_OBJECT, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);

  /**
   * WpReserveDevicePlugin::destroy-reservation:
   *
   * @brief
   * @em name:
   *
   */
  signals[ACTION_DESTROY_RESERVATION] = g_signal_new_class_handler (
      "destroy-reservation", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      (GCallback) wp_reserve_device_plugin_destroy_reservation,
      NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);

  /**
   * WpReserveDevicePlugin::get-reservation:
   *
   * @brief
   * @em name:
   *
   * Returns: (transfer full): the reservation object
   */
  signals[ACTION_GET_RESERVATION] = g_signal_new_class_handler (
      "get-reservation", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      (GCallback) wp_reserve_device_plugin_get_reservation,
      NULL, NULL, NULL,
      G_TYPE_OBJECT, 1, G_TYPE_STRING);

  /**
   * WpReserveDevicePlugin::get-dbus:
   *
   * Returns: (transfer full): the dbus object
   */
  signals[ACTION_GET_DBUS] = g_signal_new_class_handler (
      "get-dbus", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      (GCallback) wp_reserve_device_plugin_get_dbus,
      NULL, NULL, NULL,
      G_TYPE_OBJECT, 0);

}

WP_PLUGIN_EXPORT GObject *
wireplumber__module_init (WpCore * core, WpSpaJson * args, GError ** error)
{
  return G_OBJECT (g_object_new (wp_reserve_device_plugin_get_type (),
      "name", "reserve-device",
      "core", core,
      NULL));
}
