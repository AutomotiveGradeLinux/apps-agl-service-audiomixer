/*
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "audiomixer.h"
#include <wp/wp.h>
#include <pipewire/pipewire.h>

#define DEFAULT_SINK_ENDPOINT_KEY "default.session.endpoint.sink"

struct audiomixer
{
	WpCore *core;
	GMainLoop *loop;
	GMainContext *context;
	GThread *thread;
	GMutex lock;
	GCond cond;

	GPtrArray *mixer_controls;
	GWeakRef session;
	GWeakRef metadata;

	WpObjectManager *metadatas_om;
	WpObjectManager *sessions_om;
	WpObjectManager *endpoints_om;

	const struct audiomixer_events *events;
	void *events_data;
};

struct mixer_control_impl
{
	struct mixer_control pub;
	gboolean is_master;
	guint32 id;
	WpPipewireObject *po;
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
get_pipewire_object_controls (WpPipewireObject * po, float * vol, gboolean * mute)
{
	g_autoptr (WpIterator) it = NULL;
	g_auto (GValue) item = G_VALUE_INIT;
	it = wp_pipewire_object_enum_params_sync (po, "Props", NULL);
	for (; wp_iterator_next (it, &item); g_value_unset (&item)) {
		WpSpaPod *param = g_value_get_boxed (&item);
		if (wp_spa_pod_get_object (param, NULL, "volume", "f", vol, "mute", "b", mute, NULL))
			return TRUE;
	}
	return FALSE;
}

static void
params_changed (WpPipewireObject * po, guint32 param_id, struct audiomixer * self)
{
	guint32 id = wp_proxy_get_bound_id (WP_PROXY (po));
	float vol = 1.0;
	gboolean mute = FALSE;

	/* Only handle param id 2 (Props) */
	if (param_id != 2)
		return;

	if (!get_pipewire_object_controls (po, &vol, &mute)) {
		g_warning ("failed to get object controls when params changed");
		return;
	}

	for (guint i = 0; i < self->mixer_controls->len; i++) {
		struct mixer_control_impl *ctl;
		guint change_mask = 0;

		ctl = g_ptr_array_index (self->mixer_controls, i);
		if (ctl->id != id)
			continue;

		if ((ctl->pub.volume - 0.001f) > vol || (ctl->pub.volume + 0.001f) < vol) {
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

static WpEndpoint *
find_default_session_endpoint (struct audiomixer * self, WpSession *session)
{
	g_autoptr (WpMetadata) metadata = NULL;
	const gchar *value = 0;

	metadata = g_weak_ref_get (&self->metadata);
	if (!metadata)
		return NULL;

	value = wp_metadata_find (metadata, wp_proxy_get_bound_id (WP_PROXY (session)),
		DEFAULT_SINK_ENDPOINT_KEY, NULL);
	if (!value)
		return NULL;

	return wp_session_lookup_endpoint (session, WP_CONSTRAINT_TYPE_G_PROPERTY,
		"bound-id", "=u", strtoul (value, NULL, 10), NULL);
}

static void
unhandle_master_control (struct audiomixer *self, struct mixer_control_impl *master_ctl)
{
	g_autoptr (WpIterator) it = NULL;
	g_auto (GValue) val = G_VALUE_INIT;

	g_signal_handlers_disconnect_by_data (master_ctl->po, self);
	it = wp_endpoint_new_streams_iterator (WP_ENDPOINT (master_ctl->po));
	for (; wp_iterator_next (it, &val); g_value_unset (&val)) {
		WpEndpointStream *stream = g_value_get_object (&val);
		g_signal_handlers_disconnect_by_data (WP_PIPEWIRE_OBJECT (stream), self);
	}
}

static void
add_control (struct audiomixer *self, WpPipewireObject *po, const char *name, gboolean is_master)
{
	struct mixer_control_impl *mixctl = NULL;
	gfloat volume = 1.0f;
	gboolean mute = FALSE;

	/* get current values */
	if (!get_pipewire_object_controls (po, &volume, &mute)) {
		g_warning ("failed to get object controls when populating controls");
		return;
	}

	/* create the Master control */
	mixctl = g_new0 (struct mixer_control_impl, 1);
	snprintf (mixctl->pub.name, sizeof (mixctl->pub.name), "%s", name);
	mixctl->pub.volume = volume;
	mixctl->pub.mute = mute;
	mixctl->is_master = is_master;
	mixctl->id = wp_proxy_get_bound_id (WP_PROXY (po));
	mixctl->po = g_object_ref (po);
	g_ptr_array_add (self->mixer_controls, mixctl);

	/* track changes */
	g_signal_connect (mixctl->po, "params-changed", (GCallback) params_changed, self);

	g_debug ("added control %s", mixctl->pub.name);
}

/* called with self->lock locked */
static void
check_and_populate_controls (struct audiomixer * self)
{
	g_autoptr (WpSession) session = NULL;
	g_autoptr (WpEndpoint) def_ep = NULL;
	guint32 def_id = 0;
	struct mixer_control_impl *master_ctl = NULL;

	/* find the default session endpoint */
	session = g_weak_ref_get (&self->session);
	if (session) {
		def_ep = find_default_session_endpoint (self, session);
		if (def_ep)
			def_id = wp_proxy_get_bound_id (WP_PROXY (def_ep));
	}

	/* find the audio sink endpoint that was the default before */
	for (guint i = 0; i < self->mixer_controls->len; i++) {
		struct mixer_control_impl *ctl = (struct mixer_control_impl *)
				g_ptr_array_index (self->mixer_controls, i);
		if (ctl->is_master) {
			master_ctl = ctl;
			break;
		}
	}

	g_debug ("check_and_populate: session:%p, def_ep:%p, def_id:%u, "
			"master_ctl:%p, master_id:%u", session, def_ep, def_id,
			master_ctl, master_ctl ? master_ctl->id : 0);

	/* case 1: there is a default endpoint but it doesn't match
	           what we currently expose -> clear and expose the new one */
	if (def_ep && (!master_ctl || master_ctl->id != def_id)) {
		g_autoptr (WpIterator) it = NULL;
		g_auto (GValue) val = G_VALUE_INIT;

		/* clear previous */
		if (master_ctl)
			unhandle_master_control (self, master_ctl);
		g_ptr_array_set_size (self->mixer_controls, 0);

		/* add master control */
		add_control (self, WP_PIPEWIRE_OBJECT (def_ep), "Master", TRUE);

		/* add stream controls, if any */
		it = wp_endpoint_new_streams_iterator (def_ep);
		for (; wp_iterator_next (it, &val); g_value_unset (&val)) {
			WpEndpointStream *stream = g_value_get_object (&val);
			const gchar *name = wp_endpoint_stream_get_name (stream);
			add_control (self, WP_PIPEWIRE_OBJECT (stream), name, FALSE);
		}

		/* wake up audiomixer_ensure_controls() */
		g_cond_broadcast (&self->cond);

		g_debug ("controls changed");

		/* notify subscribers */
		if (self->events && self->events->controls_changed)
			self->events->controls_changed (self->events_data);
	}
	/* case 2: there is no default endpoint but something is exposed -> clear */
	else if (!def_ep && master_ctl) {
		unhandle_master_control (self, master_ctl);
		g_ptr_array_set_size (self->mixer_controls, 0);

		g_debug ("controls cleared");

		/* notify subscribers */
		if (self->events && self->events->controls_changed)
			self->events->controls_changed (self->events_data);
	}
}

static void
default_metadata_changed (WpMetadata *metadata, guint32 subject,
		const gchar *key, const gchar *type, const gchar *value,
		struct audiomixer * self)
{
	if (!g_strcmp0 (key, DEFAULT_SINK_ENDPOINT_KEY) != 0)
		return;

	g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&self->lock);
	check_and_populate_controls (self);
}

static void
metadatas_changed (WpObjectManager * metadatas_om, struct audiomixer * self)
{
	g_autoptr (WpMetadata) old_m = NULL;
	g_autoptr (WpMetadata) m = NULL;
	g_autoptr (WpIterator) it = NULL;
	g_auto (GValue) val = G_VALUE_INIT;

	old_m = g_weak_ref_get (&self->metadata);

	/* normally there is only 1 metadata */
	m = wp_object_manager_lookup (self->metadatas_om, WP_TYPE_METADATA, NULL);

	g_debug ("metadatas changed, metadata:%p, old_metadata:%p", m, old_m);

	if (m != old_m) {
		if (old_m)
			g_signal_handlers_disconnect_by_data (old_m, self);
		if (m)
			g_signal_connect (m, "changed",
				(GCallback) default_metadata_changed, self);

		g_weak_ref_set (&self->metadata, m);
	}

	check_and_populate_controls (self);
}

static void
sessions_changed (WpObjectManager * sessions_om, struct audiomixer * self)
{
	g_autoptr (WpSession) old_session = NULL;
	g_autoptr (WpSession) session = NULL;

	old_session = g_weak_ref_get (&self->session);

	/* always get the audio session */
	session = wp_object_manager_lookup (self->sessions_om, WP_TYPE_SESSION,
		WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
		"session.name", "=s", "audio",
		NULL);

	g_debug ("sessions changed, session:%p, old_session:%p", session, old_session);

	g_weak_ref_set (&self->session, session);

	check_and_populate_controls (self);
}

static void
endpoints_changed (WpObjectManager * endpoints_om, struct audiomixer * self)
{
	g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&self->lock);
	check_and_populate_controls (self);
}

static void
core_connected (WpCore * core, struct audiomixer * self)
{
	/* install object manager for metadatas */
	self->metadatas_om = wp_object_manager_new ();
	wp_object_manager_add_interest (self->metadatas_om, WP_TYPE_METADATA,
		NULL);
	wp_object_manager_request_object_features (self->metadatas_om,
		WP_TYPE_METADATA, WP_OBJECT_FEATURES_ALL);
	g_signal_connect (self->metadatas_om, "objects-changed",
		(GCallback) metadatas_changed, self);
	wp_core_install_object_manager (self->core, self->metadatas_om);

	/* install object manager for sessions */
	self->sessions_om = wp_object_manager_new ();
	wp_object_manager_add_interest (self->sessions_om, WP_TYPE_SESSION,
		NULL);
	wp_object_manager_request_object_features (self->sessions_om,
		WP_TYPE_SESSION, WP_OBJECT_FEATURES_ALL);
	g_signal_connect (self->sessions_om, "objects-changed",
		(GCallback) sessions_changed, self);
	wp_core_install_object_manager (self->core, self->sessions_om);

	/* instal object manager for endpoints */
	self->endpoints_om = wp_object_manager_new ();
	wp_object_manager_add_interest (self->endpoints_om, WP_TYPE_ENDPOINT,
		WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY,
		PW_KEY_MEDIA_CLASS, "=s", "Audio/Sink", NULL);
	wp_object_manager_request_object_features (self->endpoints_om,
		WP_TYPE_ENDPOINT, WP_OBJECT_FEATURES_ALL);
	g_signal_connect (self->endpoints_om, "objects-changed",
		(GCallback) endpoints_changed, self);
	wp_core_install_object_manager (self->core, self->endpoints_om);
}

static void
core_disconnected (WpCore * core, struct audiomixer * self)
{
	g_clear_object (&self->metadatas_om);
	g_clear_object (&self->sessions_om);
	g_clear_object (&self->endpoints_om);
}


static void *
audiomixer_thread (struct audiomixer * self)
{
	g_main_context_push_thread_default (self->context);
	g_main_loop_run (self->loop);
	g_main_context_pop_thread_default (self->context);
	return NULL;
}

static void
mixer_control_impl_free (struct mixer_control_impl * impl)
{
	if (impl->po)
		g_signal_handlers_disconnect_matched (impl->po,
			G_SIGNAL_MATCH_FUNC, 0, 0, NULL, params_changed, NULL);
	g_clear_object (&impl->po);
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

	self->mixer_controls = g_ptr_array_new_with_free_func (
		(GDestroyNotify) mixer_control_impl_free);
	g_weak_ref_init (&self->metadata, NULL);
	g_weak_ref_init (&self->session, NULL);

	g_signal_connect (self->core, "connected", (GCallback) core_connected, self);
	g_signal_connect (self->core, "disconnected", (GCallback) core_disconnected, self);

	self->thread = g_thread_new ("audiomixer", (GThreadFunc) audiomixer_thread,
		self);

	wp_core_connect (self->core);

	return self;
}

void
audiomixer_free(struct audiomixer *self)
{
	g_main_loop_quit (self->loop);
	g_thread_join (self->thread);

	g_weak_ref_clear (&self->metadata);
	g_weak_ref_clear (&self->session);
	g_ptr_array_unref (self->mixer_controls);
	g_clear_object (&self->metadatas_om);
	g_clear_object (&self->sessions_om);
	g_clear_object (&self->endpoints_om);
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

int
audiomixer_ensure_connected(struct audiomixer *self)
{
	/* already connected */
	if (wp_core_is_connected (self->core))
		return 0;

	/* if disconnected, for any reason, try to connect again */
	return wp_core_connect (self->core) ? 0 : -EIO;
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
		wp_pipewire_object_set_param (WP_PIPEWIRE_OBJECT (ctl->po),
			"Props", 0,
			wp_spa_pod_new_object ("Spa:Pod:Object:Param:Props", "Props",
				"volume", "f", action->change_volume.volume, NULL));
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
	wp_core_idle_add (self->core, NULL, (GSourceFunc) do_change_volume, action,
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
		wp_pipewire_object_set_param (WP_PIPEWIRE_OBJECT (ctl->po),
			"Props", 0,
		        wp_spa_pod_new_object ("Spa:Pod:Object:Param:Props", "Props",
				  "mute", "b", action->change_mute.mute, NULL));
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
	wp_core_idle_add (self->core, NULL, (GSourceFunc) do_change_mute, action,
		g_free);
}
