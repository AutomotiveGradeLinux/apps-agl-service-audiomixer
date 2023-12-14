/*
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "audiomixer.h"
#include <wp/wp.h>
#include <math.h>
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
	WpObjectManager *eq_om;
	WpPipewireObject *eq_node;
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
			gfloat lvolume;
			gfloat rvolume;
		} change_channel_volume;
		struct {
			guint32 id;
			gfloat volume;
		} change_volume;
		struct {
			guint32 id;
			gboolean mute;
		} change_mute;
		struct {
			guint32 id;
			gchar *control;
			gfloat gain;
		} change_gain;
	};
};

static gboolean
get_mixer_controls (struct audiomixer *self, guint32 node_id, gdouble *vol,
	gdouble *lvol, gdouble *rvol, gboolean *mute)
{
	g_autoptr (GVariant) v = NULL;
	g_autoptr (GVariantIter) iter = NULL;
	gdouble val;
	gboolean bval;

	g_signal_emit_by_name (self->mixer_api, "get-volume", node_id, &v);
	if (!v)
		return FALSE;

	if (g_variant_lookup (v, "volume", "d", &val))
		*vol = val;
	if (g_variant_lookup (v, "mute", "b", &bval))
		*mute = bval;

	if (g_variant_lookup (v, "channelVolumes", "a{sv}", &iter)) {
		const gchar *idx_str = NULL;

		while (g_variant_iter_loop (iter, "{&sv}", &idx_str, &v)) {
			const gchar *channel_str = NULL;
			if (g_variant_lookup (v, "channel", "&s", &channel_str)) {
				if (g_variant_lookup (v, "volume", "d", &val)) {

					/* look for stereo channels only */
					if (g_str_equal (channel_str, "FL"))
						*lvol = val;
					else if (g_str_equal (channel_str, "FR"))
						*rvol = val;
					else
						g_warning ("unknown channel %s", channel_str);

				}
			}
		}
	}
	return TRUE;
}

static gboolean
get_gain (struct audiomixer *self, const char *name, gfloat *gain)
{
	g_autoptr (WpIterator) it = NULL;
	g_auto (GValue) val = G_VALUE_INIT;
	gchar param_name[20];
	gboolean ret = FALSE;

	snprintf (param_name, sizeof (param_name), "%s:%s", name, "Gain");

	it = wp_pipewire_object_enum_params_sync (self->eq_node, "Props", NULL);

	for (; it && wp_iterator_next (it, &val); g_value_unset (&val)) {
		WpSpaPod *props = g_value_get_boxed (&val);
		WpSpaPod *params = NULL;
		g_autoptr (WpIterator) it1 = NULL;
		g_auto (GValue) val1 = G_VALUE_INIT;
		gboolean param_found = FALSE;

		if (!wp_spa_pod_get_object (props, NULL, "params", "T", &params, NULL))
			continue;

		if (!wp_spa_pod_is_struct (params))
			continue;

		/* iterate through the params structure */
		for (it1 = wp_spa_pod_new_iterator (params);
				it1 && wp_iterator_next (it1, &val1);
				g_value_unset (&val1)) {

			WpSpaPod *sparams = g_value_get_boxed (&val1);

			if (sparams && wp_spa_pod_is_string (sparams)) {
				const gchar *token = NULL;
				wp_spa_pod_get_string (sparams, &token);
					if (g_str_equal (token, param_name)) {
						/* read the next field to get the gain value */
						param_found = TRUE;
						continue;
					}
			}
			else if (sparams && param_found && wp_spa_pod_is_float (sparams)) {
				if (wp_spa_pod_get_float (sparams, gain)) {
					g_debug ("gain for control(%s) is %f", param_name, *gain);
					ret = TRUE;
					break;
				}
			}

		}

		if (param_found)
			break;

	}
	return ret;
}

static void
add_eq_control (struct audiomixer *self, const char *name, guint32 node_id)
{
	struct mixer_control_impl *mixctl = NULL;
	gfloat gain = 0.0;

	/* get current gain */
	if (!get_gain (self, name, &gain)) {
		g_warning ("failed to get the gain value");
		return;
	}

	/* create the control */
	mixctl = g_new0 (struct mixer_control_impl, 1);
	snprintf (mixctl->pub.name, sizeof (mixctl->pub.name), "%s", name);
	mixctl->pub.gain = gain;
	mixctl->node_id = node_id;
	g_ptr_array_add (self->mixer_controls, mixctl);

	g_debug ("added (%s) eq control and its gain is %f", mixctl->pub.name, gain);
}

static void
add_control (struct audiomixer *self, const char *name, guint32 node_id)
{
	struct mixer_control_impl *mixctl = NULL;
	gdouble volume = 1.0, lvol = 1.0, rvol = 1.0;
	gboolean mute = FALSE;

	/* get current values */
	if (!get_mixer_controls (self, node_id, &volume, &lvol, &rvol, &mute)) {
		g_warning ("failed to get object controls when populating controls for %s",
			name);
		return;
	}

	/* create the control */
	mixctl = g_new0 (struct mixer_control_impl, 1);
	snprintf (mixctl->pub.name, sizeof (mixctl->pub.name), "%s", name);
	mixctl->pub.volume = volume;
	mixctl->pub.lvolume = lvol;
	mixctl->pub.rvolume = rvol;
	mixctl->pub.mute = mute;
	mixctl->node_id = node_id;
	g_ptr_array_add (self->mixer_controls, mixctl);

	g_debug ("added control %s", mixctl->pub.name);
}

static void
volume_changed (struct audiomixer * self, guint32 node_id)
{
	g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&self->lock);
	gdouble vol = 1.0, lvol = 1.0, rvol = 1.0;
	gboolean mute = FALSE;

	for (guint i = 0; i < self->mixer_controls->len; i++) {
		struct mixer_control_impl *ctl;
		guint change_mask = 0;

		ctl = g_ptr_array_index (self->mixer_controls, i);
		if (ctl->node_id != node_id)
			continue;

		if (!get_mixer_controls (self, node_id, &vol, &lvol, &rvol, &mute)) {
			g_warning ("failed to get object controls when volume changed");
			return;
		}

		if ((ctl->pub.volume - 0.01f) > vol || (ctl->pub.volume + 0.01f) < vol) {
			/* if volume changed */
			ctl->pub.volume = vol;
			change_mask |= MIXER_CONTROL_CHANGE_FLAG_VOLUME;
		}

		if (!(fabs (ctl->pub.lvolume - lvol) < 0.00001)) {
			/* if left channel volume changed */
			ctl->pub.lvolume = lvol;
			change_mask |= MIXER_CONTROL_CHANGE_FLAG_CHANNEL_VOLUME;
		}

		if (!(fabs (ctl->pub.rvolume - rvol) < 0.00001)) {
			/* if right channel volume changed */
			ctl->pub.rvolume = rvol;
			change_mask |= MIXER_CONTROL_CHANGE_FLAG_CHANNEL_VOLUME;
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

	if (self->eq_node) {
		id = wp_proxy_get_bound_id (WP_PROXY (self->eq_node));
		if (id != 0 && id != (guint32)-1) {
			add_eq_control (self, "bass", id);
			add_eq_control (self, "treble", id);
		}
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
		& WP_PLUGIN_FEATURE_ENABLED) {
			wp_core_install_object_manager (self->core, self->eq_om);
			wp_core_install_object_manager (self->core, self->om);
	}

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
		& WP_PLUGIN_FEATURE_ENABLED) {
			wp_core_install_object_manager (self->core, self->om);
			wp_core_install_object_manager (self->core, self->eq_om);
	}

	g_signal_connect_swapped (self->mixer_api, "changed",
		(GCallback) volume_changed, self);
}

static void
on_eq_params_changed (WpPipewireObject *obj, const gchar *param_name,
	struct audiomixer * self)
{
	gfloat gain = 0.0;
	guint32 node_id = wp_proxy_get_bound_id (WP_PROXY (obj));

	if (!g_str_equal (param_name, "Props"))
		return;

	for (guint i = 0; i < self->mixer_controls->len; i++) {
		struct mixer_control_impl *ctl;
		guint change_mask = 0;

		ctl = g_ptr_array_index (self->mixer_controls, i);

		if (ctl->node_id != node_id)
			continue;

		if (!get_gain (self, ctl->pub.name, &gain)) {
				g_warning ("failed to get cached gain value");
				return;
			}

		if (!(fabs (ctl->pub.gain - gain) < 0.00001)) {
			/* if gain changed */
			ctl->pub.gain = gain;
			change_mask |= MIXER_CONTROL_CHANGE_FLAG_GAIN;
			if (self->events && self->events->value_changed) {
				self->events->value_changed (self->events_data, change_mask, &ctl->pub);
			}
		}
		else
			continue;

		break;
	}
}
static void
on_eq_added (WpObjectManager *om, WpPipewireObject *node,
	struct audiomixer *self)
{
	self->eq_node = node;
	g_signal_connect (node, "params-changed", G_CALLBACK (on_eq_params_changed),
		self);
	rescan_controls (self);
}

static void
on_eq_removed (WpObjectManager *om, WpPipewireObject *node, struct audiomixer
	*self)
{
	self->eq_node = NULL;
	rescan_controls (self);
}

static void
on_core_connected (struct audiomixer *self)
{

	self->om = wp_object_manager_new ();
	wp_object_manager_add_interest (self->om, WP_TYPE_ENDPOINT,
		WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
		PW_KEY_MEDIA_CLASS, "#s", "Audio/*", NULL);
	wp_object_manager_request_object_features (self->om,
		WP_TYPE_ENDPOINT, WP_OBJECT_FEATURES_ALL);
	g_signal_connect_swapped (self->om, "objects-changed",
		(GCallback) rescan_controls, self);

	self->eq_node = NULL;
	self->eq_om = wp_object_manager_new ();
	/*
	 * "eq-sink" name matches with the name of sink node loaded by the equalizer
	 * module present in the config.
	 */
	wp_object_manager_add_interest (self->eq_om, WP_TYPE_NODE,
		WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY, PW_KEY_NODE_NAME, "=s", "eq-sink",
		WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "=s", "Audio/Sink",
		NULL);
	wp_object_manager_request_object_features (self->eq_om,
		WP_TYPE_NODE, WP_OBJECT_FEATURES_ALL);
	g_signal_connect (self->eq_om, "object-added", G_CALLBACK (on_eq_added),
		self);
	g_signal_connect (self->eq_om, "object-removed", G_CALLBACK (on_eq_removed),
		self);

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
	g_clear_object (&self->eq_om);
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
do_change_channel_volume (struct action *action)
{
	struct audiomixer *self = action->audiomixer;
	g_auto (GVariantBuilder) b =
		G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
	g_auto (GVariantBuilder) l_vol =
		G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
	g_auto (GVariantBuilder) r_vol =
		G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
	g_auto (GVariantBuilder) channel_vols =
		G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

	gboolean ret = FALSE;

	/* left channel */
	g_variant_builder_add (&l_vol, "{sv}",
		"volume", g_variant_new_double (action->change_channel_volume.lvolume));
	g_variant_builder_add (&l_vol, "{sv}",
		"channel", g_variant_new_string ("FL"));

	g_variant_builder_add (&channel_vols, "{sv}", "0",
		g_variant_builder_end (&l_vol));

	/* right channel */
	g_variant_builder_add (&r_vol, "{sv}",
		"volume", g_variant_new_double (action->change_channel_volume.rvolume));
	g_variant_builder_add (&r_vol, "{sv}",
		"channel", g_variant_new_string ("FR"));

	g_variant_builder_add (&channel_vols, "{sv}", "1",
		g_variant_builder_end (&r_vol));

	g_variant_builder_add (&b, "{sv}",
		"channelVolumes", g_variant_builder_end (&channel_vols));

	g_signal_emit_by_name (self->mixer_api, "set-volume",
		action->change_volume.id, g_variant_builder_end (&b), &ret);
	if (!ret)
		g_warning ("mixer api set-volume(channel vols) failed");

	return G_SOURCE_REMOVE;
}

void
audiomixer_change_channel_volume(struct audiomixer *self,
	const struct mixer_control *control,
	double left_channel_volume, double right_channel_volume)
{
	const struct mixer_control_impl *impl =
		(const struct mixer_control_impl *) control;
	struct action * action;

	g_return_if_fail (self->initialized == 1);

	/* schedule the action to run on the audiomixer thread */
	action = g_new0 (struct action, 1);
	action->audiomixer = self;
	action->change_volume.id = impl->node_id;
	action->change_channel_volume.lvolume = (gfloat)left_channel_volume;
	action->change_channel_volume.rvolume = (gfloat)right_channel_volume;
	wp_core_idle_add (self->core, NULL, (GSourceFunc)do_change_channel_volume,
		action, g_free);
}

static gboolean
do_change_mute (struct action * action)
{
	struct audiomixer *self = action->audiomixer;
	GVariantBuilder b = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
	gboolean ret = FALSE;

	g_variant_builder_add (&b, "{sv}", "mute",
		g_variant_new_boolean (action->change_mute.mute));
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

static gboolean
do_change_gain (struct action * action)
{
	struct audiomixer *self = action->audiomixer;
	gboolean ret = FALSE;
	gchar name[20];
	g_autoptr (WpSpaPodBuilder) s = wp_spa_pod_builder_new_struct ();

	snprintf (name, sizeof (name), "%s:%s", action->change_gain.control, "Gain");
	wp_spa_pod_builder_add_string (s, name);
	wp_spa_pod_builder_add_float (s, action->change_gain.gain);

	g_autoptr (WpSpaPod) props =
		wp_spa_pod_new_object ("Spa:Pod:Object:Param:Props", "Props",
			"params", "P", wp_spa_pod_builder_end (s), NULL);

	ret = wp_pipewire_object_set_param (self->eq_node, "Props", 0,
		g_steal_pointer (&props));

	if(!ret)
		g_warning ("set gain failed");

	return G_SOURCE_REMOVE;
}

void
audiomixer_change_gain(struct audiomixer *self,
	const struct mixer_control *control,
	float gain)
{
	const struct mixer_control_impl *impl = (struct mixer_control_impl *)control;
	struct action * action;

	g_return_if_fail (self->initialized == 1);

	/* schedule the action to run on the audiomixer thread */
	action = g_new0 (struct action, 1);
	action->audiomixer = self;
	action->change_gain.id = impl->node_id;
	action->change_gain.gain = gain;
	action->change_gain.control = (gchar *)impl->pub.name;
	wp_core_idle_add (self->core, NULL, (GSourceFunc) do_change_gain, action,
		g_free);
}
