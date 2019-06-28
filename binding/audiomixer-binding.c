/*
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <json-c/json.h>
#include <afb/afb-binding.h>
#include <systemd/sd-event.h>
#include "audiomixer.h"

static struct audiomixer *audiomixer;
static afb_event_t controls_changed;
static afb_event_t volume_changed;
static afb_event_t mute_changed;

static void
audiomixer_controls_changed_deferred(int signum, void *arg)
{
	AFB_DEBUG("controls changed");
	afb_event_push(controls_changed, NULL);
}

struct value_changed_data
{
	unsigned int change_mask;
	struct mixer_control control;
};

static void
audiomixer_value_changed_deferred(int signum, void *data)
{
	struct value_changed_data *d = data;
	json_object *json;

	AFB_DEBUG("value changed");

	json = json_object_new_object();
	json_object_object_add(json, "control",
		json_object_new_string(d->control.name));

	if (d->change_mask & MIXER_CONTROL_CHANGE_FLAG_VOLUME) {
		json_object_object_add(json, "value",
			json_object_new_double(d->control.volume));
		afb_event_push(volume_changed, json);
	} else if (d->change_mask & MIXER_CONTROL_CHANGE_FLAG_MUTE) {
		json_object_object_add(json, "value",
			json_object_new_int(d->control.mute));
		afb_event_push(mute_changed, json);
	}

	free(d);
}

/* called in audiomixer's thread */
static void
audiomixer_controls_changed(void *data)
{
	afb_api_t api = data;
	afb_api_queue_job (api, audiomixer_controls_changed_deferred, NULL,
		(void *) 0x1, 0);
}


/* called in audiomixer's thread */
static void
audiomixer_value_changed(void *data,
	unsigned int change_mask,
	const struct mixer_control *control)
{
	afb_api_t api = data;
	struct value_changed_data *d = calloc(1, sizeof(*d));

	d->change_mask = change_mask;
	d->control = *control;

	afb_api_queue_job (api, audiomixer_value_changed_deferred, d,
		(void *) 0x1, 0);
}

static const struct audiomixer_events audiomixer_events = {
	.controls_changed = audiomixer_controls_changed,
	.value_changed = audiomixer_value_changed,
};

static int
cleanup(sd_event_source *s, void *data)
{
	audiomixer_free(audiomixer);
	audiomixer = NULL;
	return 0;
}

static int
init(afb_api_t api)
{
	sd_event *e = afb_daemon_get_event_loop();

	controls_changed = afb_api_make_event(api, "controls_changed");
	volume_changed = afb_api_make_event(api, "volume_changed");
	mute_changed = afb_api_make_event(api, "mute_changed");

	audiomixer = audiomixer_new();
	sd_event_add_exit(e, NULL, cleanup, NULL);

	audiomixer_add_event_listener(audiomixer, &audiomixer_events, api);

	return 0;
}

static void
list_controls_cb(afb_req_t request)
{
	json_object *ret_json, *nest_json;
	const struct mixer_control **ctls;
	unsigned int n_controls, i;

	audiomixer_lock(audiomixer);

	if (audiomixer_ensure_connected(audiomixer, 3) < 0) {
		afb_req_fail(request, "failed",
			"Could not connect to the PipeWire daemon");
		goto unlock;
	}

	if (audiomixer_ensure_controls(audiomixer, 3) < 0) {
		AFB_REQ_NOTICE(request, "No mixer controls were exposed "
			"in PipeWire after 3 seconds");
	}

	ctls = audiomixer_get_active_controls(audiomixer, &n_controls);

	ret_json = json_object_new_array();
	for (i = 0; i < n_controls; i++) {
		nest_json = json_object_new_object();
		json_object_object_add(nest_json, "control",
				json_object_new_string(ctls[i]->name));
		json_object_object_add(nest_json, "volume",
				json_object_new_double(ctls[i]->volume));
		json_object_object_add(nest_json, "mute",
				json_object_new_int(ctls[i]->mute));
		json_object_array_add(ret_json, nest_json);
	}
	afb_req_success(request, ret_json, NULL);

unlock:
	audiomixer_unlock(audiomixer);
}

static void
volume_cb(afb_req_t request)
{
	json_object *ret_json;
	const char *control = afb_req_value(request, "control");
	const char *value = afb_req_value(request, "value");
	const struct mixer_control *ctl;
	double volume;

	audiomixer_lock(audiomixer);

	if (!control) {
		afb_req_fail(request, "failed",
			"Invalid arguments: missing 'control'");
		goto unlock;
	}

	if (audiomixer_ensure_connected(audiomixer, 3) < 0) {
		afb_req_fail(request, "failed",
			"Could not connect to the PipeWire daemon");
		goto unlock;
	}

	if (audiomixer_ensure_controls(audiomixer, 3) < 0) {
		AFB_REQ_NOTICE(request, "No mixer controls were exposed "
			"in PipeWire after 3 seconds");
	}

	ctl = audiomixer_find_control(audiomixer, control);
	if (!ctl) {
		afb_req_fail(request, "failed", "Could not find control");
		goto unlock;
	}

	if(value) {
		char *endptr;
		volume = strtod(value, &endptr);
		if (endptr == value || volume < -0.00001 || volume > 1.00001) {
			afb_req_fail(request, "failed",
				"Invalid volume value (must be between 0.0 and 1.0)");
			goto unlock;
		}

		audiomixer_change_volume(audiomixer, ctl, volume);
	} else {
		volume = ctl->volume;
	}

	ret_json = json_object_new_object();
	json_object_object_add(ret_json, "volume", json_object_new_double(volume));
	afb_req_success(request, ret_json, NULL);

unlock:
	audiomixer_unlock(audiomixer);
}

static void
mute_cb(afb_req_t request)
{
	json_object *ret_json;
	const char *control = afb_req_value(request, "control");
	const char *value = afb_req_value(request, "value");
	const struct mixer_control *ctl;
	int mute;

	audiomixer_lock(audiomixer);

	if (!control) {
		afb_req_fail(request, "failed",
			"Invalid arguments: missing 'control'");
		goto unlock;
	}

	if (audiomixer_ensure_connected(audiomixer, 3) < 0) {
		afb_req_fail(request, "failed",
			"Could not connect to the PipeWire daemon");
		goto unlock;
	}

	if (audiomixer_ensure_controls(audiomixer, 3) < 0) {
		AFB_REQ_NOTICE(request, "No mixer controls were exposed "
			"in PipeWire after 3 seconds");
	}

	ctl = audiomixer_find_control(audiomixer, control);
	if (!ctl) {
		afb_req_fail(request, "failed", "Could not find control");
		goto unlock;
	}

	if(value) {
		char *endptr;
		mute = (int) strtol(value, &endptr, 10);
		if (endptr == value || mute < 0 || mute > 1) {
			afb_req_fail(request, "failed",
				"Invalid mute value (must be integer 0 or 1)");
			goto unlock;
		}

		audiomixer_change_mute(audiomixer, ctl, mute);
	} else {
		mute = ctl->mute;
	}

	ret_json = json_object_new_object();
	json_object_object_add(ret_json, "mute", json_object_new_int(mute));
	afb_req_success(request, ret_json, NULL);

unlock:
	audiomixer_unlock(audiomixer);
}

static void
subscribe_cb(afb_req_t request)
{
	const char *eventstr = afb_req_value(request, "event");
	afb_event_t event;

	if (!eventstr) {
		afb_req_fail(request, "failed",
			"Invalid arguments: missing 'event'");
		return;
	}

	if (!strcmp(eventstr, "controls_changed"))
		event = controls_changed;
	else if (!strcmp(eventstr, "volume_changed"))
		event = volume_changed;
	else if (!strcmp(eventstr, "mute_changed"))
		event = mute_changed;
	else {
		afb_req_fail(request, "failed", "Invalid event name");
		return;
	}

	if (afb_req_subscribe(request, event) != 0)
		afb_req_fail(request, "failed", "Failed to subscribe to event");
	else
		afb_req_success(request, NULL, "Subscribed");
}

static void
unsubscribe_cb(afb_req_t request)
{
	const char *eventstr = afb_req_value(request, "event");
	afb_event_t event;

	if (!eventstr) {
		afb_req_fail(request, "failed",
			"Invalid arguments: missing 'event'");
		return;
	}

	if (!strcmp(eventstr, "controls_changed"))
		event = controls_changed;
	else if (!strcmp(eventstr, "volume_changed"))
		event = volume_changed;
	else if (!strcmp(eventstr, "mute_changed"))
		event = mute_changed;
	else {
		afb_req_fail(request, "failed", "Invalid event name");
		return;
	}

	if (afb_req_unsubscribe(request, event) != 0)
		afb_req_fail(request, "failed", "Failed to unsubscribe from event");
	else
		afb_req_success(request, NULL, "Unsubscribed");
}

static const afb_verb_t verbs[]= {
	{ .verb = "list_controls", .callback = list_controls_cb, .info = "List the available controls" },
	{ .verb = "volume",        .callback = volume_cb,        .info = "Get/Set volume" },
	{ .verb = "mute",          .callback = mute_cb,          .info = "Get/Set mute" },
	{ .verb = "subscribe",     .callback = subscribe_cb,     .info = "Subscribe to mixer events" },
	{ .verb = "unsubscribe",   .callback = unsubscribe_cb,   .info = "Unsubscribe from mixer events" },
	{ }
};

const afb_binding_t afbBindingV3 = {
	.api  = "audiomixer",
	.specification = "AudioMixer API",
	.verbs = verbs,
	.init = init,
};
