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

	double m_volume = 0.5;
	double m_volume_faded = 0.5;
	int m_balance = 0;
	int m_fade = 0;
	double m_nav_volume = 0.5;
	bool m_nav_mute = false;

	void HandleSignalChange(const std::string &path, const Datapoint &dp);

	void HandleSignalSetError(const std::string &path, const Error &error);

	void HandleSubscribeDone(const SubscribeRequest *request, const Status &status);

	void Resubscribe(const SubscribeRequest *request);

	void HandleMixerControlChange(void);

	void HandleMixerValueChange(unsigned int change_mask, const struct mixer_control *control);

	void SetMainVolume(void);

	void SetMixerVolume(const std::string &path, const std::string &mixer, const double volume);

	void SetMixerMute(const std::string path, const std::string mixer, const bool mute);

	void SetEqualizerGain(const std::string path, const std::string mixer, const double gain);
};

#endif // _AUDIOMIXER_SERVICE_H
