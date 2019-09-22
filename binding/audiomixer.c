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

	WpRemoteState state;
	WpObjectManager *endpoints_om;
	GPtrArray *mixer_controls;

	/* just pointer, no ref */
	WpSession *session;

	const struct audiomixer_events *events;
	void *events_data;
};

struct mixer_control_impl
{
	struct mixer_control pub;
	gboolean is_master;
	guint32 id;
	WpEndpoint *endpoint;
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

static void
control_changed (WpEndpoint * ep, guint32 control_id, struct audiomixer * self)
{
	struct mixer_control_impl *ctl;

	for (guint i = 0; i < self->mixer_controls->len; i++) {
		ctl = g_ptr_array_index (self->mixer_controls, i);
		if (ctl->endpoint != ep)
			continue;

		guint change_mask = 0;

		switch (control_id) {
		case WP_ENDPOINT_CONTROL_VOLUME: {
			float vol;
			wp_endpoint_get_control_float (ep, control_id, &vol);
			ctl->pub.volume = vol;
			change_mask = MIXER_CONTROL_CHANGE_FLAG_VOLUME;
			break;
		}
		case WP_ENDPOINT_CONTROL_MUTE: {
			gboolean mute;
			wp_endpoint_get_control_boolean (ep, control_id, &mute);
			ctl->pub.mute = mute;
			change_mask = MIXER_CONTROL_CHANGE_FLAG_MUTE;
			break;
		}
		default:
			break;
		}

		if (self->events && self->events->value_changed)
			self->events->value_changed (self->events_data, change_mask, &ctl->pub);
		break;
	}
}

/* called with self->lock locked */
static void
populate_controls (struct audiomixer * self, WpSession * session)
{
	guint32 def_id = wp_session_get_default_endpoint (session,
			WP_DEFAULT_ENDPOINT_TYPE_AUDIO_SINK);
	g_autoptr (GPtrArray) arr = wp_object_manager_get_objects (
			self->endpoints_om, 0);

	for (guint i = 0; i < arr->len; i++) {
		WpEndpoint *ep = g_ptr_array_index (arr, i);
		guint32 id = wp_proxy_get_global_id (WP_PROXY (ep));
		struct mixer_control_impl *mixctl = NULL;
		gfloat volume;
		gboolean mute;

		if (id != def_id)
			continue;

		wp_endpoint_get_control_float (ep, WP_ENDPOINT_CONTROL_VOLUME, &volume);
		wp_endpoint_get_control_boolean (ep, WP_ENDPOINT_CONTROL_MUTE, &mute);

		mixctl = g_new0 (struct mixer_control_impl, 1);
		strncpy (mixctl->pub.name, "Master", sizeof (mixctl->pub.name));
		mixctl->pub.volume = volume;
		mixctl->pub.mute = mute;
		mixctl->is_master = TRUE;
		mixctl->id = id;
		mixctl->endpoint = g_object_ref (ep);
		g_ptr_array_add (self->mixer_controls, mixctl);

		g_signal_connect (ep, "control-changed", (GCallback) control_changed,
			self);

		/* wake up audiomixer_ensure_controls() */
		g_cond_broadcast (&self->cond);
		break;
	}
}

static void
default_endpoint_changed (WpSession * session, WpDefaultEndpointType type,
		guint32 new_id, struct audiomixer * self)
{
	if (session != self->session || type != WP_DEFAULT_ENDPOINT_TYPE_AUDIO_SINK)
		return;

	g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&self->lock);

	g_ptr_array_set_size (self->mixer_controls, 0);
	populate_controls (self, session);

	if (self->events && self->events->controls_changed)
		self->events->controls_changed (self->events_data);
}

static void
sessions_changed (WpObjectManager * om, struct audiomixer * self)
{
	g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&self->lock);
	g_autoptr (WpSession) session = NULL;
	g_autoptr (GPtrArray) arr = NULL;

	g_ptr_array_set_size (self->mixer_controls, 0);

	/* normally there is only 1 session */
	arr = wp_object_manager_get_objects (om, 0);
	if (arr->len > 0)
		session = WP_SESSION (g_object_ref (g_ptr_array_index (arr, 0)));

	if (session)
		populate_controls (self, session);

	/* just to be safe in case there are more sessions around */
	if (session != self->session) {
		g_signal_connect (session, "default-endpoint-changed",
			(GCallback) default_endpoint_changed, self);
		self->session = session;
	}

	if (self->events && self->events->controls_changed)
		self->events->controls_changed (self->events_data);
}

static void
remote_state_changed (WpCore *core, WpRemoteState state,
	struct audiomixer * self)
{
	g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&self->lock);
	self->state = state;
	g_cond_broadcast (&self->cond);
}

static void *
audiomixer_thread (struct audiomixer * self)
{
	g_autoptr (WpObjectManager) sessions_om = wp_object_manager_new ();
	g_autoptr (WpObjectManager) endpoints_om =
			self->endpoints_om = wp_object_manager_new ();
	GVariantBuilder b;

	g_main_context_push_thread_default (self->context);

	/* install object manager for sessions */
	wp_object_manager_add_proxy_interest (sessions_om, PW_TYPE_INTERFACE_Session,
		NULL, WP_PROXY_FEATURE_INFO | WP_PROXY_SESSION_FEATURE_DEFAULT_ENDPOINT);
	g_signal_connect (sessions_om, "objects-changed",
		(GCallback) sessions_changed, self);
	wp_core_install_object_manager (self->core, sessions_om);

	/* install object manager for Audio/Sink endpoints */
	g_variant_builder_init (&b, G_VARIANT_TYPE ("aa{sv}"));
	g_variant_builder_open (&b, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add (&b, "{sv}", "type",
		g_variant_new_int32 (WP_OBJECT_MANAGER_CONSTRAINT_PW_GLOBAL_PROPERTY));
	g_variant_builder_add (&b, "{sv}", "name",
		g_variant_new_string (PW_KEY_MEDIA_CLASS));
	g_variant_builder_add (&b, "{sv}", "value",
		g_variant_new_string ("Audio/Sink"));
	g_variant_builder_close (&b);

	wp_object_manager_add_proxy_interest (self->endpoints_om,
		PW_TYPE_INTERFACE_Endpoint,
		g_variant_builder_end (&b),
		WP_PROXY_FEATURE_INFO | WP_PROXY_ENDPOINT_FEATURE_CONTROLS);
	wp_core_install_object_manager (self->core, self->endpoints_om);

	g_signal_connect (self->core, "remote-state-changed",
		(GCallback) remote_state_changed, self);

	g_main_loop_run (self->loop);
	g_main_context_pop_thread_default (self->context);

	return NULL;
}

static void
mixer_control_impl_free (struct mixer_control_impl * impl)
{
	if (impl->endpoint)
		g_signal_handlers_disconnect_matched (impl->endpoint,
			G_SIGNAL_MATCH_FUNC, 0, 0, NULL, control_changed, NULL);
	g_clear_object (&impl->endpoint);
	g_free (impl);
}

struct audiomixer *
audiomixer_new (void)
{
	struct audiomixer *self = calloc(1, sizeof(struct audiomixer));

	g_mutex_init (&self->lock);
	g_cond_init (&self->cond);
	self->context = g_main_context_new ();
	self->loop = g_main_loop_new (self->context, FALSE);
	self->core = wp_core_new (self->context, NULL);

	self->state = WP_REMOTE_STATE_UNCONNECTED;
	self->mixer_controls = g_ptr_array_new_with_free_func (
		(GDestroyNotify) mixer_control_impl_free);

	self->thread = g_thread_new ("audiomixer", (GThreadFunc) audiomixer_thread,
		self);

	return self;
}

void
audiomixer_free(struct audiomixer *self)
{
	g_main_loop_quit (self->loop);
	g_thread_join (self->thread);

	g_ptr_array_unref (self->mixer_controls);
	g_object_unref (self->core);
	g_main_loop_unref (self->loop);
	g_main_context_unref (self->context);
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
connect_in_idle (struct audiomixer * self)
{
	wp_core_connect (self->core);
	return G_SOURCE_REMOVE;
}

int
audiomixer_ensure_connected(struct audiomixer *self, int timeout_sec)
{
	gint64 end_time = g_get_monotonic_time () + timeout_sec * G_TIME_SPAN_SECOND;

	/* already connected */
	if (self->state == WP_REMOTE_STATE_CONNECTED)
		return 0;

	/* if disconnected, for any reason, try to connect again */
	if (self->state != WP_REMOTE_STATE_CONNECTING)
		wp_core_idle_add (self->core, (GSourceFunc) connect_in_idle, self, NULL);

	while (true) {
		if (!g_cond_wait_until (&self->cond, &self->lock, end_time))
			return -ETIMEDOUT;

		if (self->state == WP_REMOTE_STATE_CONNECTED)
			return 0;
		else if (self->state == WP_REMOTE_STATE_ERROR)
			return -EIO;
	}
}

int
audiomixer_ensure_controls(struct audiomixer *self, int timeout_sec)
{
	gint64 end_time = g_get_monotonic_time () + timeout_sec * G_TIME_SPAN_SECOND;
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
	struct mixer_control_impl *ctl;
	struct audiomixer *self = action->audiomixer;

	for (guint i = 0; i < self->mixer_controls->len; i++) {
		ctl = g_ptr_array_index (self->mixer_controls, i);
		if (ctl->id != action->change_volume.id)
			continue;
		if (ctl->is_master)
			wp_endpoint_set_control_float (ctl->endpoint,
				WP_ENDPOINT_CONTROL_VOLUME, action->change_volume.volume);
	}
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

	/* schedule the action to run on the audiomixer thread */
	action = g_new0 (struct action, 1);
	action->audiomixer = self;
	action->change_volume.id = impl->id;
	action->change_volume.volume = (gfloat) volume;
	wp_core_idle_add (self->core, (GSourceFunc) do_change_volume, action,
		g_free);
}

static gboolean
do_change_mute (struct action * action)
{
	struct mixer_control_impl *ctl;
	struct audiomixer *self = action->audiomixer;

	for (guint i = 0; i < self->mixer_controls->len; i++) {
		ctl = g_ptr_array_index (self->mixer_controls, i);
		if (ctl->id != action->change_mute.id)
			continue;
		if (ctl->is_master)
			wp_endpoint_set_control_boolean (ctl->endpoint,
				WP_ENDPOINT_CONTROL_MUTE, action->change_mute.mute);
	}
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

	/* schedule the action to run on the audiomixer thread */
	action = g_new0 (struct action, 1);
	action->audiomixer = self;
	action->change_mute.id = impl->id;
	action->change_mute.mute = mute;
	wp_core_idle_add (self->core, (GSourceFunc) do_change_mute, action,
		g_free);
}
