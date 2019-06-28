/*
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "audiomixer.h"
#include <pipewire/pipewire.h>
#include <pipewire/array.h>
#include <pipewire/extensions/endpoint.h>

#define debug(...) pw_log_debug(__VA_ARGS__)

struct audiomixer
{
	struct pw_thread_loop *main_loop;

	struct pw_core *core;
	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_array endpoints;
	struct pw_array all_mixer_controls;

	const struct audiomixer_events *events;
	void *events_data;
};

enum endpoint_state {
	EP_STATE_INITIAL,
	EP_STATE_COLLECT_ENUM_STREAM,
	EP_STATE_COLLECT_ENUM_CONTROL,
	EP_STATE_COLLECT_CONTROL,
	EP_STATE_ACTIVE,
};

struct endpoint
{
	struct audiomixer *audiomixer;
	struct pw_endpoint_proxy *proxy;

	struct pw_properties *properties;
	enum endpoint_state state;

	struct spa_hook proxy_listener;
	struct spa_hook endpoint_listener;

	struct pw_array mixer_controls;
};

struct mixer_control_impl
{
	struct mixer_control pub;
	struct endpoint *endpoint;
	uint32_t stream_id;
	uint32_t volume_control_id;
	uint32_t mute_control_id;
};

static void
emit_controls_changed(struct audiomixer *self)
{
	pw_thread_loop_signal(self->main_loop, false);

	if (!self->events || !self->events->controls_changed)
		return;

	self->events->controls_changed(self->events_data);
}

static void
emit_value_changed(struct audiomixer *self,
	unsigned int change_mask,
	struct mixer_control *ctl)
{
	if (!self->events || !self->events->value_changed)
		return;

	self->events->value_changed(self->events_data, change_mask, ctl);
}

static void
advance_endpoint_state(struct endpoint *endpoint)
{
	debug("%p advance endpoint state, was:%d", endpoint, endpoint->state);

	switch (endpoint->state) {
	case EP_STATE_INITIAL:
		endpoint->state = EP_STATE_COLLECT_ENUM_STREAM;
		pw_endpoint_proxy_enum_params(endpoint->proxy, 0,
			PW_ENDPOINT_PARAM_EnumStream, 0, -1, NULL);
		pw_proxy_sync((struct pw_proxy *) endpoint->proxy, 0);
		break;
	case EP_STATE_COLLECT_ENUM_STREAM:
		endpoint->state = EP_STATE_COLLECT_ENUM_CONTROL;
		pw_endpoint_proxy_enum_params(endpoint->proxy, 0,
			PW_ENDPOINT_PARAM_EnumControl, 0, -1, NULL);
		pw_proxy_sync((struct pw_proxy *) endpoint->proxy, 0);
		break;
	case EP_STATE_COLLECT_ENUM_CONTROL: {
		uint32_t ids[1] = { PW_ENDPOINT_PARAM_Control };

		endpoint->state = EP_STATE_COLLECT_CONTROL;
		pw_endpoint_proxy_subscribe_params(endpoint->proxy, ids, 1);
		pw_proxy_sync((struct pw_proxy *) endpoint->proxy, 0);
		break;
	}
	case EP_STATE_COLLECT_CONTROL: {
		struct mixer_control_impl *ctl;
		struct audiomixer *self = endpoint->audiomixer;

		endpoint->state = EP_STATE_ACTIVE;
		pw_array_for_each(ctl, &endpoint->mixer_controls) {
			pw_array_add_ptr(&self->all_mixer_controls, ctl);
		}
		emit_controls_changed(self);
		break;
	}
	default:
		break;
	}
}

static void
endpoint_param (void *object, int seq, uint32_t id,
		uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct endpoint *endpoint = object;
	struct mixer_control_impl *ctl;
	const struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;

	if (!spa_pod_is_object(param)) {
		debug("endpoint_param: bad param - not an object");
		return;
	}

	switch (id) {
	case PW_ENDPOINT_PARAM_EnumStream:
		/* verify conditions */
		if (endpoint->state != EP_STATE_COLLECT_ENUM_STREAM) {
			debug("endpoint_param EnumStream: wrong state");
			return;
		}
		if (SPA_POD_OBJECT_TYPE(obj) != PW_ENDPOINT_OBJECT_ParamStream) {
			debug("endpoint_param EnumStream: invalid param");
			return;
		}

		/* create new mixer control */
		ctl = pw_array_add(&endpoint->mixer_controls, sizeof(*ctl));
		ctl->endpoint = endpoint;

		SPA_POD_OBJECT_FOREACH(obj, prop) {
			switch (prop->key) {
			case PW_ENDPOINT_PARAM_STREAM_id:
				spa_pod_get_int(&prop->value, &ctl->stream_id);
				break;
			case PW_ENDPOINT_PARAM_STREAM_name:
				spa_pod_copy_string(&prop->value,
					SPA_N_ELEMENTS(ctl->pub.name),
					ctl->pub.name);
				break;
			default:
				break;
			}
		}
		break;

	case PW_ENDPOINT_PARAM_EnumControl: {
		uint32_t tmp_id = -1;
		const char *name = NULL;

		/* verify conditions */
		if (endpoint->state != EP_STATE_COLLECT_ENUM_CONTROL) {
			debug("endpoint_param EnumControl: wrong state");
			return;
		}
		if (SPA_POD_OBJECT_TYPE(obj) != PW_ENDPOINT_OBJECT_ParamControl) {
			debug("endpoint_param EnumControl: invalid param");
			return;
		}

		/* find the mixer control */
		prop = spa_pod_object_find_prop(obj, NULL,
			PW_ENDPOINT_PARAM_CONTROL_stream_id);
		if (prop)
			spa_pod_get_int(&prop->value, &tmp_id);
		else {
			debug("endpoint_param EnumControl: invalid control without stream");
			return;
		}

		pw_array_for_each(ctl, &endpoint->mixer_controls) {
			if (ctl->stream_id == tmp_id)
				break;
		}

		/* check if we reached the end of the array
		 * without finding the stream */
		if (!pw_array_check(&endpoint->mixer_controls, ctl)) {
			debug("endpoint_param EnumControl: could not find "
				"stream id %u", tmp_id);
			return;
		}

		/* store the control id based on the control's name */
		prop = spa_pod_object_find_prop(obj, NULL,
			PW_ENDPOINT_PARAM_CONTROL_name);
		if (prop)
			spa_pod_get_string(&prop->value, &name);
		else {
			debug("endpoint_param EnumControl: invalid control without name");
			return;
		}

		prop = spa_pod_object_find_prop(obj, NULL,
			PW_ENDPOINT_PARAM_CONTROL_id);
		if (!prop) {
			debug("endpoint_param EnumControl: invalid control without id");
			return;
		}

		if (!strcmp (name, "volume")) {
			spa_pod_get_int(&prop->value, &ctl->volume_control_id);
			prop = spa_pod_object_find_prop(obj, NULL,
				PW_ENDPOINT_PARAM_CONTROL_type);
		} else if (!strcmp (name, "mute")) {
			spa_pod_get_int(&prop->value, &ctl->mute_control_id);
		}

		break;
	}
	case PW_ENDPOINT_PARAM_Control: {
		uint32_t tmp_id = -1;

		/* verify conditions */
		if (endpoint->state != EP_STATE_COLLECT_CONTROL &&
		    endpoint->state != EP_STATE_ACTIVE) {
			debug("endpoint_param Control: wrong state");
			return;
		}
		if (SPA_POD_OBJECT_TYPE(obj) != PW_ENDPOINT_OBJECT_ParamControl) {
			debug("endpoint_param Control: invalid param");
			return;
		}

		/* match the control id and set the value */
		prop = spa_pod_object_find_prop(obj, NULL,
			PW_ENDPOINT_PARAM_CONTROL_id);
		if (prop)
			spa_pod_get_int(&prop->value, &tmp_id);
		else {
			debug("endpoint_param Control: invalid control without id");
			return;
		}

		prop = spa_pod_object_find_prop(obj, NULL,
			PW_ENDPOINT_PARAM_CONTROL_value);
		if (!prop) {
			debug("endpoint_param Control: invalid control without value");
			return;
		}

		pw_array_for_each(ctl, &endpoint->mixer_controls) {
			if (ctl->volume_control_id == tmp_id) {
				spa_pod_get_double(&prop->value, &ctl->pub.volume);

				if (endpoint->state == EP_STATE_ACTIVE) {
					emit_value_changed(endpoint->audiomixer,
						MIXER_CONTROL_CHANGE_FLAG_VOLUME,
						&ctl->pub);
				}
				break;
			} else if (ctl->mute_control_id == tmp_id) {
				spa_pod_get_bool(&prop->value, &ctl->pub.mute);

				if (endpoint->state == EP_STATE_ACTIVE) {
					emit_value_changed(endpoint->audiomixer,
						MIXER_CONTROL_CHANGE_FLAG_MUTE,
						&ctl->pub);
				}
				break;
			}
		}

		break;
	}
	default:
		break;
	}
}

struct pw_endpoint_proxy_events endpoint_events = {
	PW_VERSION_ENDPOINT_PROXY_EVENTS,
	.param = endpoint_param,
};

static void
endpoint_proxy_destroyed(void *object)
{
	struct endpoint *endpoint = object;
	struct audiomixer *self = endpoint->audiomixer;
	struct mixer_control_impl *ctl;
	struct mixer_control **ctlptr;
	struct endpoint **epptr;

	debug("%p endpoint destroyed", endpoint);

	if (endpoint->properties)
		pw_properties_free(endpoint->properties);

	pw_array_for_each(ctl, &endpoint->mixer_controls) {
		pw_array_for_each(ctlptr, &self->all_mixer_controls) {
			if (*ctlptr == &ctl->pub) {
				pw_array_remove(&self->all_mixer_controls, ctlptr);
				break;
			}
		}
	}
	emit_controls_changed(self);
	pw_array_clear(&endpoint->mixer_controls);

	pw_array_for_each(epptr, &self->endpoints) {
		if (*epptr == endpoint) {
			pw_array_remove(&self->endpoints, epptr);
			break;
		}
	}
}

static void
endpoint_proxy_done(void *object, int seq)
{
	struct endpoint *endpoint = object;
	advance_endpoint_state(endpoint);
}

struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = endpoint_proxy_destroyed,
	.done = endpoint_proxy_done,
};

static void
registry_event_global(void *data, uint32_t id, uint32_t parent_id,
		uint32_t permissions, uint32_t type, uint32_t version,
		const struct spa_dict *props)
{
	struct audiomixer *self = data;
	const char *media_class;
	struct pw_proxy *proxy;
	struct endpoint *endpoint;

	if (type != PW_TYPE_INTERFACE_Endpoint)
		return;

	/* we are only interested in mixer endpoints */
	media_class = props ? spa_dict_lookup(props, "media.class") : NULL;
	if (!media_class || strcmp(media_class, "Mixer/Audio") != 0)
		return;

	proxy = pw_registry_proxy_bind(self->registry_proxy,
		id, type, PW_VERSION_ENDPOINT, sizeof(struct endpoint));

	endpoint = pw_proxy_get_user_data(proxy);
	endpoint->audiomixer = self;
	endpoint->proxy = (struct pw_endpoint_proxy *) proxy;
	endpoint->properties = props ? pw_properties_new_dict(props) : NULL;
	endpoint->state = EP_STATE_INITIAL;
	pw_array_init(&endpoint->mixer_controls, 4 * sizeof(struct mixer_control));

	pw_proxy_add_listener(proxy, &endpoint->proxy_listener,
		&proxy_events, endpoint);
	pw_endpoint_proxy_add_listener(endpoint->proxy,
		&endpoint->endpoint_listener,
		&endpoint_events, endpoint);

	debug("%p added endpoint: %u", endpoint, id);

	pw_array_add_ptr(&self->endpoints, endpoint);
	advance_endpoint_state(endpoint);
}


static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
	.global = registry_event_global,
};

static void
on_remote_state_changed(void *data, enum pw_remote_state old,
		enum pw_remote_state state, const char *error)
{
	struct audiomixer *self = data;

	if (state == PW_REMOTE_STATE_CONNECTED) {
		self->core_proxy = pw_remote_get_core_proxy(self->remote);
		self->registry_proxy = pw_core_proxy_get_registry(
			self->core_proxy,
			PW_TYPE_INTERFACE_Registry,
			PW_VERSION_REGISTRY, 0);
		pw_registry_proxy_add_listener(self->registry_proxy,
			&self->registry_listener,
			&registry_events, self);
	}

	pw_thread_loop_signal(self->main_loop, false);
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_remote_state_changed,
};

struct audiomixer *
audiomixer_new(void)
{
	struct audiomixer *self;
	struct pw_loop *loop;

	pw_init(NULL, NULL);

	self = calloc(1, sizeof(struct audiomixer));
	loop = pw_loop_new(NULL);
	self->main_loop = pw_thread_loop_new(loop, "audiomixer-loop");
	self->core = pw_core_new(loop, NULL, 0);
	self->remote = pw_remote_new(self->core, NULL, 0);
	pw_array_init(&self->endpoints, 1 * sizeof(void*));
	pw_array_init(&self->all_mixer_controls, 8 * sizeof(void*));

	pw_remote_add_listener(self->remote, &self->remote_listener,
		&remote_events, self);

	pw_module_load(self->core, "libpipewire-module-endpoint", NULL, NULL,
		NULL, NULL);
	pw_thread_loop_start(self->main_loop);

	return self;
}

void
audiomixer_free(struct audiomixer *self)
{
	struct pw_loop *loop;

	pw_thread_loop_lock(self->main_loop);
	self->events = NULL;
	self->events_data = NULL;
	pw_remote_disconnect(self->remote);
	pw_thread_loop_unlock(self->main_loop);
	pw_thread_loop_stop(self->main_loop);

	pw_array_clear(&self->endpoints);
	pw_array_clear(&self->all_mixer_controls);
	pw_remote_destroy(self->remote);
	pw_core_destroy(self->core);

	loop = pw_thread_loop_get_loop(self->main_loop);
	pw_thread_loop_destroy(self->main_loop);
	pw_loop_destroy(loop);

	free(self);
}

void
audiomixer_lock(struct audiomixer *self)
{
	pw_thread_loop_lock(self->main_loop);
}

void
audiomixer_unlock(struct audiomixer *self)
{
	pw_thread_loop_unlock(self->main_loop);
}

int
audiomixer_ensure_connected(struct audiomixer *self, int timeout_sec)
{
	enum pw_remote_state state;
	int res;

	state = pw_remote_get_state(self->remote, NULL);
	if (state == PW_REMOTE_STATE_CONNECTED)
		return 0;

	if ((res = pw_remote_connect(self->remote)) < 0)
		return res;

	while (true) {
		state = pw_remote_get_state(self->remote, NULL);
		if (state == PW_REMOTE_STATE_CONNECTED)
			return 0;
		else if (state == PW_REMOTE_STATE_ERROR)
			return -EIO;

		if (pw_thread_loop_timed_wait(self->main_loop, timeout_sec) != 0)
			return -ETIMEDOUT;
	}
}

int
audiomixer_ensure_controls(struct audiomixer *self, int timeout_sec)
{
	while (pw_array_get_len(&self->all_mixer_controls, void*) == 0) {
		if (pw_thread_loop_timed_wait(self->main_loop, timeout_sec) != 0)
			return -ETIMEDOUT;
	}
	return 0;
}

const struct mixer_control **
audiomixer_get_active_controls(struct audiomixer *self,
	unsigned int *n_controls)
{
	const struct mixer_control **ret;

	*n_controls = pw_array_get_len(&self->all_mixer_controls, void*);
	ret = (const struct mixer_control **)
		pw_array_first(&self->all_mixer_controls);

	return ret;
}

const struct mixer_control *
audiomixer_find_control(struct audiomixer *self, const char *name)
{
	struct mixer_control **ctlptr;

	pw_array_for_each(ctlptr, &self->all_mixer_controls) {
		if (!strcmp((*ctlptr)->name, name)) {
			return *ctlptr;
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

void
audiomixer_change_volume(struct audiomixer *self,
	const struct mixer_control *control,
	double volume)
{
	const struct mixer_control_impl *impl =
		(const struct mixer_control_impl *) control;
	struct endpoint *endpoint = impl->endpoint;
	char buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, 1024);
	struct spa_pod *param;

	param = spa_pod_builder_add_object(&b,
		PW_ENDPOINT_OBJECT_ParamControl, PW_ENDPOINT_PARAM_Control,
		PW_ENDPOINT_PARAM_CONTROL_id, SPA_POD_Int(impl->volume_control_id),
		PW_ENDPOINT_PARAM_CONTROL_value, SPA_POD_Double(volume),
		NULL);
	pw_endpoint_proxy_set_param(endpoint->proxy,
		PW_ENDPOINT_PARAM_Control, 0, param);
}

void
audiomixer_change_mute(struct audiomixer *self,
	const struct mixer_control *control,
	bool mute)
{
	const struct mixer_control_impl *impl =
		(const struct mixer_control_impl *) control;
	struct endpoint *endpoint = impl->endpoint;
	char buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, 1024);
	struct spa_pod *param;

	param = spa_pod_builder_add_object(&b,
		PW_ENDPOINT_OBJECT_ParamControl, PW_ENDPOINT_PARAM_Control,
		PW_ENDPOINT_PARAM_CONTROL_id, SPA_POD_Int(impl->mute_control_id),
		PW_ENDPOINT_PARAM_CONTROL_value, SPA_POD_Bool(mute),
		NULL);
	pw_endpoint_proxy_set_param(endpoint->proxy,
		PW_ENDPOINT_PARAM_Control, 0, param);
}
