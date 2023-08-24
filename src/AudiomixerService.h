/*
 * Copyright (C) 2022,2023 Konsulko Group
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _AUDIOMIXER_SERVICE_H
#define _AUDIOMIXER_SERVICE_H

#include <glib.h>

#include "KuksaConfig.h"
#include "KuksaClient.h"
#include "audiomixer.h"

class AudiomixerService
{
public:
	AudiomixerService(const KuksaConfig &config, GMainLoop *loop = NULL);

	~AudiomixerService();

	// Callbacks for WirePlumber API

	static void mixer_control_change_cb(void *data) {
		if (data)
			((AudiomixerService*) data)->HandleMixerControlChange();
	};

	static void mixer_value_change_cb(void *data,
					  unsigned int change_mask,
					  const struct mixer_control *control) {
		if (data)
			((AudiomixerService*) data)->HandleMixerValueChange(change_mask, control);
	}

	// Callback for KuksaClient subscribe API reconnect

	static gboolean resubscribe_cb(gpointer data) {
		struct resubscribe_data *d = (struct resubscribe_data*) data;
		if (d && d->self) {
			((AudiomixerService*) d->self)->Resubscribe(d->request);
		}
		return FALSE;
	}

private:
	struct resubscribe_data {
		AudiomixerService *self;
		const SubscribeRequest *request;
	};

	GMainLoop *m_loop;
	KuksaConfig m_config;
	KuksaClient *m_broker;
	struct audiomixer *m_audiomixer;
	struct audiomixer_events m_audiomixer_events;

	void HandleSignalChange(const std::string &path, const Datapoint &dp);

	void HandleSignalSetError(const std::string &path, const Error &error);

	void HandleSubscribeDone(const SubscribeRequest *request, const Status &status);

	void Resubscribe(const SubscribeRequest *request);

	void HandleMixerControlChange(void);

	void HandleMixerValueChange(unsigned int change_mask, const struct mixer_control *control);
};

#endif // _AUDIOMIXER_SERVICE_H
