/*
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "audiomixer.h"
#include <wp/wp.h>
#include <pipewire/pipewire.h>

struct audiomixer
{
	WpCore *core;
	GMainLoop *loop;
	GMainContext *context;
	GThread *thread;
	GMutex lock;
	GCond cond;

	GPtrArray *mixer_controls;

	gint initialized;
	WpObjectManager *om;
	WpPlugin *default_nodes_api;
	WpPlugin *mixer_api;

	const struct audiomixer_events *events;
	void *events_data;
};

struct mixer_control_impl
{
	struct mixer_control pub;
	guint32 node_id;
};

struct action
{
	struct audiomixer *audiomixer;
	union {
		struct {
			guint32 id;
			gfloat volume;
		} change_volume;
		struct {
			guint32 id;
			gboolean mute;
		} change_mute;
	};
};

static gboolean
get_mixer_controls (struct audiomixer * self, guint32 node_id, gdouble * vol, gboolean * mute)
{
	g_autoptr (GVariant) v = NULL;
	g_signal_emit_by_name (self->mixer_api, "get-volume", node_id, &v);
	return v &&
		g_variant_lookup (v, "volume", "d", &vol, NULL) &&
		g_variant_lookup (v, "mute", "b", &mute, NULL);
}

static void
add_control (struct audiomixer *self, const char *name, guint32 node_id)
{
	struct mixer_control_impl *mixctl = NULL;
	gdouble volume = 1.0;
	gboolean mute = FALSE;

	/* get current values */
	if (!get_mixer_controls (self, node_id, &volume, &mute)) {
		g_warning ("failed to get object controls when populating controls");
		return;
	}

	/* create the control */
	mixctl = g_new0 (struct mixer_control_impl, 1);
	snprintf (mixctl->pub.name, sizeof (mixctl->pub.name), "%s", name);
	mixctl->pub.volume = volume;
	mixctl->pub.mute = mute;
	mixctl->node_id = node_id;
	g_ptr_array_add (self->mixer_controls, mixctl);

	g_debug ("added control %s", mixctl->pub.name);
}

static void
volume_changed (struct audiomixer * self, guint32 node_id)
{
	g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&self->lock);
	gdouble vol = 1.0;
	gboolean mute = FALSE;

	for (guint i = 0; i < self->mixer_controls->len; i++) {
		struct mixer_control_impl *ctl;
		guint change_mask = 0;

		ctl = g_ptr_array_index (self->mixer_controls, i);
		if (ctl->node_id != node_id)
			continue;

		if (!get_mixer_controls (self, node_id, &vol, &mute)) {
			g_warning ("failed to get object controls when volume changed");
			return;
		}

		if ((ctl->pub.volume - 0.01f) > vol || (ctl->pub.volume + 0.01f) < vol) {
			ctl->pub.volume = vol;
			change_mask |= MIXER_CONTROL_CHANGE_FLAG_VOLUME;
		}
		if (ctl->pub.mute != mute) {
			ctl->pub.mute = mute;
			change_mask |= MIXER_CONTROL_CHANGE_FLAG_MUTE;
		}

		if (self->events && self->events->value_changed)
			self->events->value_changed (self->events_data, change_mask, &ctl->pub);
		break;
	}
}

static void
rescan_controls (struct audiomixer * self)
{
	g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&self->lock);
	g_autoptr (WpIterator) it = NULL;
	g_auto (GValue) val = G_VALUE_INIT;
	guint32 id = -1;

	g_debug ("rescan");

	/* clear previous */
	g_ptr_array_set_size (self->mixer_controls, 0);

	/* add master controls */
	g_signal_emit_by_name (self->default_nodes_api, "get-default-node",
			"Audio/Sink", &id);
	if (id != (guint32)-1)
		add_control (self, "Master Playback", id);

	g_signal_emit_by_name (self->default_nodes_api, "get-default-node",
			"Audio/Source", &id);
	if (id != (guint32)-1)
		add_control (self, "Master Capture", id);

	/* add endpoints */
	it = wp_object_manager_new_iterator (self->om);
	for (; wp_iterator_next (it, &val); g_value_unset (&val)) {
		WpPipewireObject *ep = g_value_get_object (&val);
		const gchar *name = wp_pipewire_object_get_property (ep, "endpoint.description");
		const gchar *node = wp_pipewire_object_get_property (ep, "node.id");
		id = node ? atoi(node) : 0;
		if (name && id != 0 && id != (guint32)-1)
			add_control (self, name, id);
	}

	/* notify subscribers */
	if (self->events && self->events->controls_changed)
		self->events->controls_changed (self->events_data);
	g_cond_broadcast (&self->cond);
}

static void
on_default_nodes_activated (WpObject * p, GAsyncResult * res, struct audiomixer * self)
{
	g_autoptr (GError) error = NULL;
	if (!wp_object_activate_finish (p, res, &error)) {
		g_warning ("%s", error->message);
	}

	if (wp_object_get_active_features (WP_OBJECT (self->mixer_api))
			& WP_PLUGIN_FEATURE_ENABLED)
		wp_core_install_object_manager (self->core, self->om);

	g_signal_connect_swapped (self->default_nodes_api, "changed",
		(GCallback) rescan_controls, self);
}

static void
on_mixer_activated (WpObject * p, GAsyncResult * res, struct audiomixer * self)
{
	g_autoptr (GError) error = NULL;
	if (!wp_object_activate_finish (p, res, &error)) {
		g_warning ("%s", error->message);
	}

	if (wp_object_get_active_features (WP_OBJECT (self->default_nodes_api))
			& WP_PLUGIN_FEATURE_ENABLED)
		wp_core_install_object_manager (self->core, self->om);

	g_signal_connect_swapped (self->mixer_api, "changed",
		(GCallback) volume_changed, self);
}

static void
on_core_connected (struct audiomixer * self)
{
	self->om = wp_object_manager_new ();
	wp_object_manager_add_interest (self->om, WP_TYPE_ENDPOINT,
		WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
		PW_KEY_MEDIA_CLASS, "#s", "Audio/*", NULL);
	wp_object_manager_request_object_features (self->om,
		WP_TYPE_ENDPOINT, WP_OBJECT_FEATURES_ALL);
	g_signal_connect_swapped (self->om, "objects-changed",
		(GCallback) rescan_controls, self);

	wp_object_activate (WP_OBJECT (self->default_nodes_api),
		WP_PLUGIN_FEATURE_ENABLED, NULL,
		(GAsyncReadyCallback) on_default_nodes_activated, self);

	wp_object_activate (WP_OBJECT (self->mixer_api),
		WP_PLUGIN_FEATURE_ENABLED, NULL,
		(GAsyncReadyCallback) on_mixer_activated, self);
}

static void
on_core_disconnected (struct audiomixer * self)
{
	g_ptr_array_set_size (self->mixer_controls, 0);
	g_clear_object (&self->om);
	g_signal_handlers_disconnect_by_data (self->default_nodes_api, self);
	g_signal_handlers_disconnect_by_data (self->mixer_api, self);
	wp_object_deactivate (WP_OBJECT (self->default_nodes_api), WP_PLUGIN_FEATURE_ENABLED);
	wp_object_deactivate (WP_OBJECT (self->mixer_api), WP_PLUGIN_FEATURE_ENABLED);
}

static void
audiomixer_init_in_thread (struct audiomixer * self)
{
	g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&self->lock);
	g_autoptr (GError) error = NULL;

	self->context = g_main_context_new ();
	g_main_context_push_thread_default (self->context);

	self->loop = g_main_loop_new (self->context, FALSE);
	self->core = wp_core_new (self->context, NULL);

	/* load required API modules */
	if (!wp_core_load_component (self->core,
			"libwireplumber-module-default-nodes-api", "module", NULL, &error)) {
		g_warning ("%s", error->message);
		self->initialized = -1;
		goto out;
	}
	if (!wp_core_load_component (self->core,
			"libwireplumber-module-mixer-api", "module", NULL, &error)) {
		g_warning ("%s", error->message);
		self->initialized = -1;
		goto out;
	}

	self->default_nodes_api = wp_plugin_find (self->core, "default-nodes-api");
	self->mixer_api = wp_plugin_find (self->core, "mixer-api");
	g_object_set (G_OBJECT (self->mixer_api), "scale", 1 /* cubic */, NULL);

	g_signal_connect_swapped (self->core, "connected",
		G_CALLBACK (on_core_connected), self);
	g_signal_connect_swapped (self->core, "disconnected",
		G_CALLBACK (on_core_disconnected), self);

	self->initialized = 1;

out:
	g_cond_broadcast (&self->cond);
}

static void *
audiomixer_thread (struct audiomixer * self)
{
	audiomixer_init_in_thread (self);

	/* main loop for the thread; quits only when audiomixer_free() is called */
	g_main_loop_run (self->loop);

	wp_core_disconnect (self->core);
	g_clear_object (&self->default_nodes_api);
	g_clear_object (&self->mixer_api);
	g_object_unref (self->core);

	g_main_context_pop_thread_default (self->context);
	g_main_loop_unref (self->loop);
	g_main_context_unref (self->context);

	return NULL;
}

struct audiomixer *
audiomixer_new (void)
{
	struct audiomixer *self = calloc(1, sizeof(struct audiomixer));

	wp_init (WP_INIT_ALL);

	g_mutex_init (&self->lock);
	g_cond_init (&self->cond);
	self->mixer_controls = g_ptr_array_new_with_free_func (g_free);

	g_mutex_lock (&self->lock);
	self->initialized = 0;
	self->thread = g_thread_new ("audiomixer", (GThreadFunc) audiomixer_thread,
		self);
	while (self->initialized == 0)
		g_cond_wait (&self->cond, &self->lock);
	g_mutex_unlock (&self->lock);

	return self;
}

void
audiomixer_free(struct audiomixer *self)
{
	g_main_loop_quit (self->loop);
	g_thread_join (self->thread);

	g_ptr_array_unref (self->mixer_controls);
	g_cond_clear (&self->cond);
	g_mutex_clear (&self->lock);

	free (self);
}

void
audiomixer_lock(struct audiomixer *self)
{
	g_mutex_lock (&self->lock);
}

void
audiomixer_unlock(struct audiomixer *self)
{
	g_mutex_unlock (&self->lock);
}

static gboolean
do_connect (WpCore * core)
{
	if (!wp_core_connect (core))
		g_warning ("Failed to connect to PipeWire");
	return G_SOURCE_REMOVE;
}

int
audiomixer_ensure_controls(struct audiomixer *self, int timeout_sec)
{
	gint64 end_time = g_get_monotonic_time () + timeout_sec * G_TIME_SPAN_SECOND;

	g_return_val_if_fail (self->initialized == 1, -EIO);

	if (!wp_core_is_connected (self->core))
		g_main_context_invoke (self->context, (GSourceFunc) do_connect, self->core);

	while (self->mixer_controls->len == 0) {
		if (!g_cond_wait_until (&self->cond, &self->lock, end_time))
			return -ETIMEDOUT;
	}
	return 0;
}

const struct mixer_control **
audiomixer_get_active_controls(struct audiomixer *self,
	unsigned int *n_controls)
{
	*n_controls = self->mixer_controls->len;
	return (const struct mixer_control **) self->mixer_controls->pdata;
}

const struct mixer_control *
audiomixer_find_control(struct audiomixer *self, const char *name)
{
	struct mixer_control *ctl;

	for (guint i = 0; i < self->mixer_controls->len; i++) {
		ctl = g_ptr_array_index (self->mixer_controls, i);
		if (!strcmp(ctl->name, name)) {
			return ctl;
		}
	}
	return NULL;
}

void
audiomixer_add_event_listener(struct audiomixer *self,
	const struct audiomixer_events *events,
	void *data)
{
	self->events = events;
	self->events_data = data;
}

static gboolean
do_change_volume (struct action * action)
{
	struct audiomixer *self = action->audiomixer;
	GVariantBuilder b = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
	gboolean ret = FALSE;

	g_variant_builder_add (&b, "{sv}", "volume",
		g_variant_new_double (action->change_volume.volume));
	g_signal_emit_by_name (self->mixer_api, "set-volume",
		action->change_volume.id, g_variant_builder_end (&b), &ret);
	if (!ret)
		g_warning ("mixer api set-volume failed");

	return G_SOURCE_REMOVE;
}

void
audiomixer_change_volume(struct audiomixer *self,
	const struct mixer_control *control,
	double volume)
{
	const struct mixer_control_impl *impl =
		(const struct mixer_control_impl *) control;
	struct action * action;

	g_return_if_fail (self->initialized == 1);

	/* schedule the action to run on the audiomixer thread */
	action = g_new0 (struct action, 1);
	action->audiomixer = self;
	action->change_volume.id = impl->node_id;
	action->change_volume.volume = (gfloat) volume;
	wp_core_idle_add (self->core, NULL, (GSourceFunc) do_change_volume, action,
		g_free);
}

static gboolean
do_change_mute (struct action * action)
{
	struct audiomixer *self = action->audiomixer;
	GVariantBuilder b = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
	gboolean ret = FALSE;

	g_variant_builder_add (&b, "{sv}", "mute",
		g_variant_new_double (action->change_mute.mute));
	g_signal_emit_by_name (self->mixer_api, "set-volume",
		action->change_mute.id, g_variant_builder_end (&b), &ret);
	if (!ret)
		g_warning ("mixer api set-volume failed");

	return G_SOURCE_REMOVE;
}

void
audiomixer_change_mute(struct audiomixer *self,
	const struct mixer_control *control,
	bool mute)
{
	const struct mixer_control_impl *impl =
		(const struct mixer_control_impl *) control;
	struct action * action;

	g_return_if_fail (self->initialized == 1);

	/* schedule the action to run on the audiomixer thread */
	action = g_new0 (struct action, 1);
	action->audiomixer = self;
	action->change_mute.id = impl->node_id;
	action->change_mute.mute = mute;
	wp_core_idle_add (self->core, NULL, (GSourceFunc) do_change_mute, action,
		g_free);
}
