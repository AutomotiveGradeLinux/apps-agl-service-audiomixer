/*
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _AUDIOMIXER_H
#define _AUDIOMIXER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

struct audiomixer;

struct mixer_control
{
	char name[32];
	double volume;
	float gain;
	bool mute;
};

struct audiomixer_events
{
	void (*controls_changed) (void *data);

	void (*value_changed) (void *data,
#define MIXER_CONTROL_CHANGE_FLAG_VOLUME (1<<0)
#define MIXER_CONTROL_CHANGE_FLAG_MUTE   (1<<1)
#define MIXER_CONTROL_CHANGE_FLAG_GAIN (1<<2)
				unsigned int change_mask,
				const struct mixer_control *control);
};

struct audiomixer * audiomixer_new(void);
void audiomixer_free(struct audiomixer *self);

/* locking is required to call any of the methods below
 * and to access any structure maintained by audiomixer */
void audiomixer_lock(struct audiomixer *self);
void audiomixer_unlock(struct audiomixer *self);

int audiomixer_ensure_controls(struct audiomixer *self, int timeout_sec);

const struct mixer_control ** audiomixer_get_active_controls(
	struct audiomixer *self,
	unsigned int *n_controls);

const struct mixer_control * audiomixer_find_control(
	struct audiomixer *self,
	const char *name);

void audiomixer_add_event_listener(struct audiomixer *self,
	const struct audiomixer_events *events,
	void *data);

void audiomixer_change_volume(struct audiomixer *self,
	const struct mixer_control *control,
	double volume);

void audiomixer_change_gain(struct audiomixer *self,
	const struct mixer_control *control,
	float gain);

void audiomixer_change_mute(struct audiomixer *self,
	const struct mixer_control *control,
	bool mute);

#ifdef __cplusplus
}
#endif

#endif // _AUDIOMIXER_H
