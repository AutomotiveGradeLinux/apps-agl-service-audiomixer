/*
 * Copyright (C) 2022,2023 Konsulko Group
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>

#include "AudiomixerService.h"

AudiomixerService::AudiomixerService(const KuksaConfig &config, GMainLoop *loop) :
	m_loop(loop),
	m_config(config)
{
	m_audiomixer = audiomixer_new();
	if (m_audiomixer) {
		// Set up callbacks for WirePlumber events
		m_audiomixer_events.controls_changed = mixer_control_change_cb;
		m_audiomixer_events.value_changed = mixer_value_change_cb;
		audiomixer_add_event_listener(m_audiomixer, &m_audiomixer_events, this);

		// Drive connecting to PipeWire core and refreshing controls list
		audiomixer_lock(m_audiomixer);
		audiomixer_ensure_controls(m_audiomixer, 3);
		audiomixer_unlock(m_audiomixer);
	} else {
		std::cerr << "Could not create WirePlumber connection" << std::endl;
	}

	// Set initial volume
	if (m_audiomixer) {
		audiomixer_lock(m_audiomixer);
		const struct mixer_control *ctl = audiomixer_find_control(m_audiomixer, "Master Playback");
		if (ctl) {
			audiomixer_change_volume(m_audiomixer, ctl, 0.5);
		}
		audiomixer_unlock(m_audiomixer);
	}

	// Create gRPC channel
	std::string host = m_config.hostname();
	host += ":";
	std::stringstream ss;
	ss << m_config.port();
	host += ss.str();

	std::shared_ptr<grpc::Channel> channel;
	if (!m_config.caCert().empty()) {
		grpc::SslCredentialsOptions options;
		options.pem_root_certs = m_config.caCert();
		if (!m_config.tlsServerName().empty()) {
			grpc::ChannelArguments args;
			auto target = m_config.tlsServerName();
			std::cout << "Overriding TLS target name with " << target << std::endl;
			args.SetString(GRPC_SSL_TARGET_NAME_OVERRIDE_ARG, target);
			channel = grpc::CreateCustomChannel(host, grpc::SslCredentials(options), args);
		} else {
			channel = grpc::CreateChannel(host, grpc::SslCredentials(options));
		}
	} else {
		channel = grpc::CreateChannel(host, grpc::InsecureChannelCredentials());
	}

	// Wait for the channel to be ready
	std::cout << "Waiting for Databroker gRPC channel" << std::endl;
	while (!channel->WaitForConnected(std::chrono::system_clock::now() +
					  std::chrono::milliseconds(500))) ;
	std::cout << "Databroker gRPC channel ready" << std::endl;

	m_broker = new KuksaClient(channel, m_config);
	if (m_broker) {
		// Listen to actuator target updates for the volume signal and
		// values for the steering wheel volume switch signals.
		std::map<std::string, bool> signals;
		signals["Vehicle.Cabin.Infotainment.Media.Volume"] = true;
		signals["Vehicle.Cabin.SteeringWheel.Switches.VolumeUp"] = false;
		signals["Vehicle.Cabin.SteeringWheel.Switches.VolumeDown"] = false;
		signals["Vehicle.Cabin.SteeringWheel.Switches.VolumeMute"] = false;
		m_broker->subscribe(signals,
				    [this](const std::string &path, const Datapoint &dp) {
					    HandleSignalChange(path, dp);
				    },
				    [this](const SubscribeRequest *request, const Status &s) {
					    HandleSubscribeDone(request, s);
				    });

		// Set initial volume in VSS
		// Push out the default value of 50 which matches the default in the
		// homescreen app. Ideally there would be some form of persistence
		// scheme to restore the last value on restart.
		m_broker->set("Vehicle.Cabin.Infotainment.Media.Volume",
			      50U,
			      [this](const std::string &path, const Error &error) {
				      HandleSignalSetError(path, error);
			      });
	} else {
		std::cerr << "Could not create Kuksa API client" << std::endl;
	}
}

AudiomixerService::~AudiomixerService()
{
	delete m_broker;

	audiomixer_free(m_audiomixer);
}

// Private

void AudiomixerService::HandleSignalChange(const std::string &path, const Datapoint &dp)
{
	if (m_config.verbose() > 1)
		std::cout << "AudiomixerService::HandleSignalChange: Value received for " << path << std::endl;

	if (!m_audiomixer)
		return;

	audiomixer_lock(m_audiomixer);

	const struct mixer_control *ctl = audiomixer_find_control(m_audiomixer, "Master Playback");
	if (!ctl) {
		audiomixer_unlock(m_audiomixer);
		return;
	}
	if (path == "Vehicle.Cabin.Infotainment.Media.Volume") {
		if (dp.has_uint32()) {
			uint32_t volume = dp.uint32();
			if (volume >= 0 && volume <= 100) {
				double v = (double) volume / 100.0;
				if (m_config.verbose() > 1)
					std::cout << "Setting volume to " << v << std::endl;
				audiomixer_change_volume(m_audiomixer, ctl, v);

				// Push out new value
				m_broker->set("Vehicle.Cabin.Infotainment.Media.Volume",
					      volume,
					      [this](const std::string &path, const Error &error) {
						      HandleSignalSetError(path, error);
					      });
			}
		}
	} else if (path == "Vehicle.Cabin.SteeringWheel.Switches.VolumeUp") {
		if (dp.has_bool_() && dp.bool_()) {
			double volume = ctl->volume;
			volume += 0.05; // up 5%
			if (volume > 1.0)
				volume = 1.0; // clamp to 100%
			if (m_config.verbose() > 1)
				std::cout << "Increasing volume to " << volume << std::endl;
			audiomixer_change_volume(m_audiomixer, ctl, volume);

			// Push out new value
			m_broker->set("Vehicle.Cabin.Infotainment.Media.Volume",
				      (unsigned) (volume * 100),
				      [this](const std::string &path, const Error &error) {
					      HandleSignalSetError(path, error);
				      });
		}
	} else if (path == "Vehicle.Cabin.SteeringWheel.Switches.VolumeDown") {
		if (dp.has_bool_() && dp.bool_()) {
			double volume = ctl->volume;
			volume -= 0.05; // down 5%
			if (volume < 0.0)
				volume = 0.0; // clamp to 0%
			if (m_config.verbose() > 1)
				std::cout << "Decreasing volume to " << volume << std::endl;
			audiomixer_change_volume(m_audiomixer, ctl, volume);

			// Push out new value
			m_broker->set("Vehicle.Cabin.Infotainment.Media.Volume",
				      (unsigned) (volume * 100),
				      [this](const std::string &path, const Error &error) {
					      HandleSignalSetError(path, error);
				      });
		}
	} else if (path == "Vehicle.Cabin.SteeringWheel.Switches.VolumeMute") {
		if (dp.has_bool_() && dp.bool_()) {
			if (m_config.verbose() > 1) {
				if (ctl->mute)
					std::cout << "Unmuting" << std::endl;
				else
					std::cout << "Muting" << std::endl;
			}
			audiomixer_change_mute(m_audiomixer, ctl, !ctl->mute);
		}
	}
	// else ignore

	audiomixer_unlock(m_audiomixer);
}

void AudiomixerService::HandleSignalSetError(const std::string &path, const Error &error)
{
	std::cerr << "Error setting " << path << ": " << error.code() << " - " << error.reason() << std::endl;
}

void AudiomixerService::HandleSubscribeDone(const SubscribeRequest *request, const Status &status)
{
	if (m_config.verbose())
		std::cout << "Subscribe status = " << status.error_code() <<
			" (" << status.error_message() << ")" << std::endl;

	if (status.error_code() == grpc::CANCELLED) {
		if (m_config.verbose())
			std::cerr << "Subscribe canceled, assuming shutdown" << std::endl;
		return;
	}

	// Queue up a resubcribe via the GLib event loop callback
	struct resubscribe_data *data = new (struct resubscribe_data);
	if (!data) {
		std::cerr << "Could not create resubcribe_data" << std::endl;
		exit(1);
	}
	data->self = this;
	// Need to copy request since the one we have been handed is from the
	// finished subscribe and will be going away.
	data->request = new SubscribeRequest(*request);
	if (!data->request) {
		std::cerr << "Could not create resubscribe SubscribeRequest" << std::endl;
		exit(1);
	}

	// NOTE: Waiting 100 milliseconds for now; it is possible that some
	//       randomization and/or back-off may need to be added if many
	//       subscribes are active, or switching to some other resubscribe
	//       scheme altogether (e.g. post subscribes to a thread that waits
	//       for the channel to become connected again).
	g_timeout_add_full(G_PRIORITY_DEFAULT,
			   100,
			   resubscribe_cb,
			   data,
			   NULL);
}

void AudiomixerService::Resubscribe(const SubscribeRequest *request)
{
	if (!(m_broker && request))
		return;

	m_broker->subscribe(request,
			    [this](const std::string &path, const Datapoint &dp) {
				    HandleSignalChange(path, dp);
			    },
			    [this](const SubscribeRequest *request, const Status &s) {
				    HandleSubscribeDone(request, s);
			    });
}

void AudiomixerService::HandleMixerControlChange(void)
{
	// Ignore for now
}

void AudiomixerService::HandleMixerValueChange(unsigned int change_mask, const struct mixer_control *control)
{
	if (!control)
		return;

	if (change_mask & MIXER_CONTROL_CHANGE_FLAG_VOLUME) {
		if (std::string(control->name) == "Master Playback") {
			// Push out new value
			unsigned value = control->volume * 100;
			m_broker->set("Vehicle.Cabin.Infotainment.Media.Volume",
				      value,
				      [this](const std::string &path, const Error &error) {
					      HandleSignalSetError(path, error);
				      });
		}
	} else if (change_mask & MIXER_CONTROL_CHANGE_FLAG_MUTE) {
		// For now, do nothing, new state is in control->mute
	}
}
