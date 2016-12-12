/*
 * Copyright (C) 2016 Samsung Electronics
 *   Author: Arun Raghavan <arun@osg.samsung.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION: gst-sync-client
 * @short_description: Provides a client object to receive information from a
 *                     #GstSyncServer to play a synchronised stream.
 *
 * The #GstSyncClient object provides API to connect to a #GstSyncServer in
 * order to receive and play back a stream synchronised with other clients on a
 * network.
 *
 * #GstSyncClient itself does not implement the network transport for receiving
 * messages from the server, but defers that to an object that implements the
 * #GstSyncControlClient interface. A default TCP-based implementation is
 * provided with this library.
 */

#include <gst/gst.h>
#include <gst/net/gstnet.h>

#include "sync-server-info.h"
#include "sync-client.h"
#include "sync-control-client.h"
#include "sync-control-tcp-client.h"

enum {
  NEED_SEEK,
  IN_SEEK,
  DONE_SEEK,
};

struct _GstSyncClient {
  GObject parent;

  gchar *control_addr;
  gint control_port;

  GstSyncServerInfo *info;
  GMutex info_lock;

  GstPipeline *pipeline;
  GstClock *clock;

  GstSyncControlClient *client;
  gboolean synchronised;

  /* See bus_cb() for why this needs to be atomic */
  volatile int seek_state;
  GstClockTime seek_offset;
};

struct _GstSyncClientClass {
  GObjectClass parent;
};

#define gst_sync_client_parent_class parent_class
G_DEFINE_TYPE (GstSyncClient, gst_sync_client, G_TYPE_OBJECT);

GST_DEBUG_CATEGORY_STATIC (sync_client_debug);
#define GST_CAT_DEFAULT sync_client_debug

enum {
  PROP_0,
  PROP_CONTROL_CLIENT,
  PROP_CONTROL_ADDRESS,
  PROP_CONTROL_PORT,
  PROP_PIPELINE,
};

#define DEFAULT_PORT 0
#define DEFAULT_SEEK_TOLERANCE (200 * GST_MSECOND)

static void
gst_sync_client_dispose (GObject * object)
{
  GstSyncClient *self = GST_SYNC_CLIENT (object);

  if (self->pipeline) {
    gst_object_unref (self->pipeline);
    self->pipeline = NULL;
  }

  if (self->clock) {
    gst_object_unref (self->clock);
    self->clock = NULL;
  }

  g_free (self->control_addr);
  self->control_addr = NULL;

  if (self->info) {
    g_object_unref (self->info);
    self->info = NULL;
  }

  g_mutex_clear (&self->info_lock);

  if (self->client) {
    gst_sync_control_client_stop (self->client);
    g_object_unref (self->client);
    self->client = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
set_base_time (GstSyncClient * self)
{
  gst_element_set_start_time (GST_ELEMENT (self->pipeline),
      GST_CLOCK_TIME_NONE);

  GST_DEBUG_OBJECT (self, "Updating base time to: %lu",
      gst_sync_server_info_get_base_time (self->info) +
      gst_sync_server_info_get_base_time_offset (self->info) +
      self->seek_offset);
  gst_element_set_base_time (GST_ELEMENT (self->pipeline),
      gst_sync_server_info_get_base_time (self->info) +
      gst_sync_server_info_get_base_time_offset (self->info) +
      self->seek_offset);
}

/* Call with info_lock held */
static void
update_pipeline (GstSyncClient * self)
{
  gboolean is_live;
  gchar *uri;

  uri = gst_sync_server_info_get_uri (self->info);
  g_object_set (GST_OBJECT (self->pipeline), "uri", uri, NULL);

  gst_pipeline_set_latency (self->pipeline,
      gst_sync_server_info_get_latency (self->info));

  if (gst_sync_server_info_get_stopped (self->info)) {
    /* Just stop the pipeline and we're done */
    if (gst_element_set_state (GST_ELEMENT (self->pipeline), GST_STATE_NULL) ==
        GST_STATE_CHANGE_FAILURE)
      GST_WARNING_OBJECT (self, "Error while stopping pipeline");
    return;
  }

  switch (gst_element_set_state (GST_ELEMENT (self->pipeline),
        GST_STATE_PAUSED)) {
    case GST_STATE_CHANGE_FAILURE:
      GST_WARNING_OBJECT (self, "Could not play uri: %s", uri);
      break;

    case GST_STATE_CHANGE_NO_PREROLL:
      is_live = TRUE;
      GST_DEBUG_OBJECT (self, "Detected live pipeline");
      break;

    default:
      is_live = FALSE;
      break;
  }

  self->seek_offset = 0;
  g_atomic_int_set (&self->seek_state, is_live ? DONE_SEEK : NEED_SEEK);

  /* We need to do PAUSED and PLAYING in separate steps so we don't have a race
   * between us and reading seek_state in bus_cb() */
  if (!gst_sync_server_info_get_paused (self->info)) {
    set_base_time (self);
    gst_element_set_state (GST_ELEMENT (self->pipeline), GST_STATE_PLAYING);
  }

  g_free (uri);
}

static gboolean
bus_cb (GstBus * bus, GstMessage * message, gpointer user_data)
{
  GstSyncClient *self = GST_SYNC_CLIENT (user_data);

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ELEMENT: {
      const GstStructure *st;

      if (self->synchronised)
        break;

      st = gst_message_get_structure (message);
      if (!gst_structure_has_name (st, "gst-netclock-statistics"))
        break;

      gst_structure_get_boolean (st, "synchronised", &self->synchronised);
      if (!self->synchronised)
        break;

      if (!gst_clock_wait_for_sync (self->clock, 10 * GST_SECOND)) {
        GST_ERROR_OBJECT (self, "Could not synchronise clock");
        self->synchronised = FALSE;
        break;
      }

      GST_INFO_OBJECT (self, "Clock is synchronised, starting playback");

      g_mutex_lock (&self->info_lock);
      update_pipeline (self);
      g_mutex_unlock (&self->info_lock);

      break;
    }

    case GST_MESSAGE_STATE_CHANGED: {
      GstState old_state, new_state;
      GstClockTime cur_pos, now;

      if (g_atomic_int_get (&self->seek_state) != NEED_SEEK ||
          GST_MESSAGE_SRC (message) != GST_OBJECT (self->pipeline))
        break;

      gst_message_parse_state_changed (message, &old_state, &new_state, NULL);

      if (old_state != GST_STATE_PAUSED && new_state != GST_STATE_PLAYING)
        break;

      now = gst_clock_get_time (self->clock);
      g_atomic_int_set (&self->seek_state, IN_SEEK);

      g_mutex_lock (&self->info_lock);

      cur_pos = now -
        gst_sync_server_info_get_base_time (self->info) -
        gst_sync_server_info_get_base_time_offset (self->info);

      if (cur_pos > DEFAULT_SEEK_TOLERANCE) {
        /* Let's seek ahead to prevent excessive clipping */
        GST_INFO_OBJECT (self, "Seeking: %lu", cur_pos);

        if (!gst_element_seek_simple (GST_ELEMENT (self->pipeline),
              GST_FORMAT_TIME, GST_SEEK_FLAG_SNAP_AFTER |
              GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH,
              cur_pos)) {
          GST_WARNING_OBJECT (self, "Could not perform seek");

          g_atomic_int_set (&self->seek_state, DONE_SEEK);
        }
      } else {
        /* For the seek case, the base time will be set after the seek */
        GST_INFO_OBJECT (self, "Not seeking as we're within the threshold");
        g_atomic_int_set (&self->seek_state, DONE_SEEK);
      }

      g_mutex_unlock (&self->info_lock);

      break;
    }

    case GST_MESSAGE_ASYNC_DONE: {
      /* This message is first examined synchronously in the sync-message
       * signal.
       * The rationale for doing this is that (a) we want the most accurate
       * possible final seek position, and examining position asynchronously
       * will not guarantee that, and (b) setting the base time as early as
       * possible means we'll start rendering correctly synchronised buffers
       * sooner */
      if (g_atomic_int_get (&self->seek_state) != IN_SEEK)
        break;

      if (gst_element_query_position (GST_ELEMENT (self->pipeline),
            GST_FORMAT_TIME, &self->seek_offset)) {
        GST_INFO_OBJECT (self, "Adding offset: %lu", self->seek_offset);

        g_mutex_lock (&self->info_lock);
        set_base_time (self);
        g_mutex_unlock (&self->info_lock);
      }

      g_atomic_int_set (&self->seek_state, DONE_SEEK);

      break;
    }

    case GST_MESSAGE_EOS:
      /* Just wait until we get further instructions from the server */
      if (GST_MESSAGE_SRC (message) == GST_OBJECT (self->pipeline))
        gst_element_set_state (GST_ELEMENT (self->pipeline), GST_STATE_NULL);
      break;

    default:
      break;
  }

  return TRUE;
}

static void
update_sync_info (GstSyncClient * self, GstSyncServerInfo * info)
{
  g_mutex_lock (&self->info_lock);

  if (!self->info) {
    /* First sync info update */
    GstBus *bus;
    gchar *clock_addr;

    self->info = info;

    clock_addr = gst_sync_server_info_get_clock_address (self->info);
    self->clock = gst_net_client_clock_new ("sync-server-clock",
        clock_addr, gst_sync_server_info_get_clock_port (self->info), 0);
    g_free (clock_addr);

    gst_pipeline_use_clock (self->pipeline, self->clock);

    bus = gst_pipeline_get_bus (self->pipeline);
    g_object_set (self->clock, "bus", bus, NULL);

    gst_bus_add_watch (bus, bus_cb, self);
    /* See bus_cb() for why we do this */
    gst_bus_enable_sync_message_emission (bus);
    g_signal_connect (G_OBJECT (bus), "sync-message::async-done",
        G_CALLBACK (bus_cb), self);

    gst_object_unref (bus);
  } else {
    /* Sync info changed, figure out what did. We do not expect the clock
     * parameters or latency to change */
    GstSyncServerInfo *old_info;
    gchar *old_uri, *uri;

    old_info = self->info;
    self->info = info;

    old_uri = gst_sync_server_info_get_uri (old_info);
    uri = gst_sync_server_info_get_uri (self->info);

    if (gst_sync_server_info_get_stopped (old_info) !=
        gst_sync_server_info_get_stopped (self->info) ||
        (!g_str_equal (old_uri, uri))) {
      /* Pipeline was (un)stopped or URI changed, just reset completely */
      gst_element_set_state (GST_ELEMENT (self->pipeline), GST_STATE_NULL);
      update_pipeline (self);

    } else if (gst_sync_server_info_get_paused (old_info) !=
        gst_sync_server_info_get_paused (self->info)) {
      /* Paused or unpaused */
      if (!gst_sync_server_info_get_paused (self->info))
        set_base_time (self);

      gst_element_set_state (GST_ELEMENT (self->pipeline),
          gst_sync_server_info_get_paused (self->info) ?
            GST_STATE_PAUSED :
            GST_STATE_PLAYING);

    } else if (gst_sync_server_info_get_base_time (old_info) !=
        gst_sync_server_info_get_base_time (self->info)) {
      /* Base time changed, just reset pipeline completely */
      gst_element_set_state (GST_ELEMENT (self->pipeline), GST_STATE_NULL);
      update_pipeline (self);
    }

    g_free (old_uri);
    g_free (uri);
    g_object_unref (old_info);
  }

  g_mutex_unlock (&self->info_lock);
}

static void
gst_sync_client_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSyncClient *self = GST_SYNC_CLIENT (object);

  switch (property_id) {
    case PROP_CONTROL_CLIENT:
      if (self->client)
        g_object_unref (self->client);

      self->client = g_value_dup_object (value);
      break;

    case PROP_CONTROL_ADDRESS:
      if (self->control_addr)
        g_free (self->control_addr);

      self->control_addr = g_value_dup_string (value);
      break;

    case PROP_CONTROL_PORT:
      self->control_port = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_sync_client_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSyncClient *self = GST_SYNC_CLIENT (object);

  switch (property_id) {
    case PROP_CONTROL_CLIENT:
      g_value_set_object (value, self->client);
      break;

    case PROP_CONTROL_ADDRESS:
      g_value_set_string (value, self->control_addr);
      break;

    case PROP_CONTROL_PORT:
      g_value_set_int (value, self->control_port);
      break;

    case PROP_PIPELINE:
      g_value_set_object (value, self->pipeline);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_sync_client_class_init (GstSyncClientClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = GST_DEBUG_FUNCPTR (gst_sync_client_dispose);
  object_class->set_property =
    GST_DEBUG_FUNCPTR (gst_sync_client_set_property);
  object_class->get_property =
    GST_DEBUG_FUNCPTR (gst_sync_client_get_property);

  /**
   * GstSyncClient:control-client:
   *
   * The implementation of the control protocol that should be used to
   * communicate with the server. This object must implement the
   * #GstSyncControlClient interface. If set to NULL, a built-in TCP
   * implementation is used.
   */
  g_object_class_install_property (object_class, PROP_CONTROL_CLIENT,
      g_param_spec_object ("control-client", "Control client",
        "Control client object (NULL => use TCP control client)",
        G_TYPE_OBJECT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstSyncClient:control-address:
   *
   * The network address for the client to connect to.
   */
  g_object_class_install_property (object_class, PROP_CONTROL_ADDRESS,
      g_param_spec_string ("control-address", "Control address",
        "Address for control server", NULL,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /**
   * GstSyncClient:control-port:
   *
   * The network port for the client to connect to.
   */
  g_object_class_install_property (object_class, PROP_CONTROL_PORT,
      g_param_spec_int ("control-port", "Control port",
        "Port for control server", 0, 65535, DEFAULT_PORT,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /**
   * GstSyncClient:pipeline:
   *
   * A #GstPipeline object that is used for playing the synchronised stream.
   * The object will provide the same interface as #playbin, so that clients
   * can be configured appropriately for the platform (such as selecting the
   * video sink and setting it up, if required).
   */
  g_object_class_install_property (object_class, PROP_PIPELINE,
      g_param_spec_object ("pipeline", "Pipeline",
        "The pipeline for playback (having the URI property)",
        GST_TYPE_PIPELINE,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (sync_client_debug, "syncclient", 0, "GstSyncClient");
}

static void
sync_info_notify (GObject * object, GParamSpec * pspec, gpointer user_data)
{
  GstSyncClient *self = GST_SYNC_CLIENT (user_data);
  GstSyncServerInfo * info;
  gchar *clock_addr, *uri;

  info = gst_sync_control_client_get_sync_info (self->client);

  clock_addr = gst_sync_server_info_get_clock_address (info);
  uri = gst_sync_server_info_get_uri (info);

  GST_DEBUG_OBJECT (self, "Got sync information:");
  GST_DEBUG_OBJECT (self, "\tClk: %s:%u", clock_addr,
      gst_sync_server_info_get_clock_port (info));
  GST_DEBUG_OBJECT (self, "\tURI: %s", uri);
  GST_DEBUG_OBJECT (self, "\tBase time: %lu",
      gst_sync_server_info_get_uri (info));
  GST_DEBUG_OBJECT (self, "\tLatency: %lu",
      gst_sync_server_info_get_latency (info));
  GST_DEBUG_OBJECT (self, "\tStopped: %u",
      gst_sync_server_info_get_stopped (info));
  GST_DEBUG_OBJECT (self, "\tPaused: %u",
      gst_sync_server_info_get_paused (info));
  GST_DEBUG_OBJECT (self, "\tBase time offset: %lu",
      gst_sync_server_info_get_base_time_offset (info));

  update_sync_info (self, info /* transfers ownership of info */);
}

static void
gst_sync_client_init (GstSyncClient * self)
{
  self->control_addr = NULL;
  self->control_port = DEFAULT_PORT;

  self->info = NULL;
  g_mutex_init (&self->info_lock);

  self->pipeline = GST_PIPELINE (gst_element_factory_make ("playbin", NULL));
  if (!self->pipeline)
    GST_ERROR_OBJECT (self, "Could not instantiate playbin");

  self->synchronised = FALSE;

  self->seek_offset = 0;
  g_atomic_int_set (&self->seek_state, NEED_SEEK);
}

/**
 * gst_sync_client_new:
 * @control_addr: The network address that the client should connect to
 * @control_port: The network port that the client should connect to
 *
 * Creates a new #GstSyncClient object that will connect to a #GstSyncServer on
 * the given network address/port pair once started.
 *
 * Returns: (transfer full): A new #GstSyncServer object.
 */
GstSyncClient *
gst_sync_client_new (const gchar * control_addr, gint control_port)
{
  return
    g_object_new (GST_TYPE_SYNC_CLIENT,
        "control-address", control_addr,
        "control-port", control_port,
        NULL);
}

/**
 * gst_sync_client_start:
 * @client: The #GstSyncClient object
 * @error: If non-NULL, will be set to the appropriate #GError if starting the
 *         server fails.
 *
 * Starts the #GstSyncClient, connects to the configured server, and starts
 * playback of the currently configured stream.
 *
 * Returns: #TRUE on success, and #FALSE if the server could not be started.
 */
gboolean
gst_sync_client_start (GstSyncClient * client, GError ** err)
{
  gboolean ret;

  if (!client->client)
    client->client = g_object_new (GST_TYPE_SYNC_CONTROL_TCP_CLIENT, NULL);
  g_return_val_if_fail (GST_IS_SYNC_CONTROL_CLIENT (client->client), FALSE);

  if (client->control_addr) {
    gst_sync_control_client_set_address (client->client, client->control_addr);
    gst_sync_control_client_set_port (client->client, client->control_port);
  }

  /* FIXME: can this be moved into a convenience method like the rest of the
   * interface? */
  g_signal_connect (client->client, "notify::sync-info",
      G_CALLBACK (sync_info_notify), client);

  ret = gst_sync_control_client_start (client->client, err);

  return ret;
}

/**
 * gst_sync_client_stop:
 * @client: The #GstSyncClient object
 *
 * Disconnects from the server and stops playback.
 */
void
gst_sync_client_stop (GstSyncClient * client)
{
  gst_sync_control_client_stop (client->client);
}
