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

static const std::string VSS_VOLUME = "Vehicle.Cabin.Infotainment.Media.Volume";
static const std::string VSS_NAV_VOLUME = "Vehicle.Cabin.Infotainment.Navigation.Volume";
static const std::string VSS_NAV_MUTE = "Vehicle.Cabin.Infotainment.Navigation.Mute";
static const std::string VSS_SWITCH_VOL_UP = "Vehicle.Cabin.SteeringWheel.Switches.VolumeUp";
static const std::string VSS_SWITCH_VOL_DOWN = "Vehicle.Cabin.SteeringWheel.Switches.VolumeDown";
static const std::string VSS_SWITCH_VOL_MUTE = "Vehicle.Cabin.SteeringWheel.Switches.VolumeMute";
static const std::string VSS_BALANCE = "Vehicle.Cabin.Infotainment.Media.Audio.Balance";
static const std::string VSS_FADE = "Vehicle.Cabin.Infotainment.Media.Audio.Fade";
static const std::string VSS_BASS = "Vehicle.Cabin.Infotainment.Media.Audio.Bass";
static const std::string VSS_TREBLE = "Vehicle.Cabin.Infotainment.Media.Audio.Treble";

static const char* PLAYBACK_MAIN = "Master Playback";
static const char* PLAYBACK_NAV = "Playback: Navigation";

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
		signals[VSS_VOLUME] = true;
		signals[VSS_NAV_VOLUME] = true;
		signals[VSS_NAV_MUTE] = true;
		// AGL additions
		signals[VSS_SWITCH_VOL_UP] = false;
		signals[VSS_SWITCH_VOL_DOWN] = false;
		signals[VSS_SWITCH_VOL_MUTE] = false;
		signals[VSS_BALANCE] = true;
		signals[VSS_FADE] = true;
		signals[VSS_BASS] = true;
		signals[VSS_TREBLE] = true;

		m_broker->subscribe(signals,
				    [this](const std::string &path, const Datapoint &dp) {
					    HandleSignalChange(path, dp);
				    },
				    [this](const SubscribeRequest *request, const Status &s) {
					    HandleSubscribeDone(request, s);
				    });

		// Set initial volume
		// Push out default value of 50%, ideally there would be some
		// form of persistence scheme to restore the last value on
		// restart.
		SetMainVolume();

		// Push out new value
		// NOTE: This is a workaround for current observed behavior
		//       where no updates seem to get processed until playback
		//       has happened.
		uint32_t value = (m_volume + 0.005) * 100;
		if (value <= 100) {
			m_broker->set(VSS_VOLUME,
				      value,
				      [this](const std::string &path, const Error &error) {
					      HandleSignalSetError(path, error);
				      });
		}
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

	if (path == VSS_VOLUME) {
		if (dp.has_uint32()) {
			double volume = (double) dp.uint32() / 100.0;
			if (volume >= 0.0 && volume <= 1.0) {
				m_volume = volume;
				SetMainVolume();
			}
		}
	} else if (path == VSS_NAV_VOLUME) {
		if (dp.has_uint32()) {
			double volume = (double) dp.uint32() / 100.0;
			if (volume >= 0.0 && volume <= 1.0) {
				m_nav_volume = volume;
				SetMixerVolume(path, PLAYBACK_NAV, volume);
			}
		}
	} else if (path == VSS_NAV_MUTE) {
		if (dp.has_string()) {
			std::string value = dp.string();
			bool mute = false;
			if (value.length()) {
				if (value == "MUTED")
					mute = true;
				// else UNMUTED or ALERT_ONLY (which is not handled)

				m_nav_mute = mute;
				SetMixerMute(path, PLAYBACK_NAV, mute);
			}
		}
	} else if (path == VSS_SWITCH_VOL_UP) {
		if (dp.has_bool_() && dp.bool_()) {
			double volume = m_volume;
			volume += 0.05; // up 5%
			if (volume > 1.0)
				volume = 1.0; // clamp to 100%
			if (m_config.verbose() > 1)
				std::cout << "Increasing volume to " << volume << std::endl;
			m_volume = volume;
			SetMainVolume();
		}
	} else if (path == VSS_SWITCH_VOL_DOWN) {
		if (dp.has_bool_() && dp.bool_()) {
			double volume = m_volume;
			volume -= 0.05; // down 5%
			if (volume < 0.0)
				volume = 0.0; // clamp to 0%
			if (m_config.verbose() > 1)
				std::cout << "Decreasing volume to " << volume << std::endl;
			m_volume = volume;
			SetMainVolume();
		}
	} else if (path == VSS_SWITCH_VOL_MUTE) {
		if (dp.has_bool_() && dp.bool_()) {
			// No equivalent VSS signal, so call audiomixer API directly
			const struct mixer_control *ctl =
				audiomixer_find_control(m_audiomixer, PLAYBACK_MAIN);
			if (!ctl)
				goto control_error;

			if (m_config.verbose() > 1) {
				if (ctl->mute)
					std::cout << "Unmuting" << std::endl;
				else
					std::cout << "Muting" << std::endl;
			}
			audiomixer_change_mute(m_audiomixer, ctl, !ctl->mute);
		}
	} else if (path == VSS_BALANCE) {
		if (dp.has_int32()) {
			int32_t value = dp.int32();
			if (value >= -100 && value <= 100) {
				if (m_config.verbose() > 1)
					std::cout << "AudiomixerService::HandleSignalChange: Value =  "
						  << value << std::endl;
				m_balance = value;
				SetMainVolume();
				
				// Push out new value
				m_broker->set(VSS_BALANCE,
					      value,
					      [this](const std::string &path, const Error &error) {
						      HandleSignalSetError(path, error);
					      });
			}
		}
	} else if (path == VSS_FADE) {
		if (dp.has_int32()) {
			int32_t value = dp.int32();
			if (value >= -100 && value <= 100) {
				if (m_config.verbose() > 1)
					std::cout << "AudiomixerService::HandleSignalChange: Value =  "
						  << value << std::endl;
				m_fade = value;
				SetMainVolume();

				// Push out new value
				m_broker->set(VSS_FADE,
					      value,
					      [this](const std::string &path, const Error &error) {
						      HandleSignalSetError(path, error);
					      });
			}
		}
	} else if (path == VSS_BASS) {
		if (dp.has_int32()) {
			int32_t value = dp.int32();
			if (m_config.verbose() > 1)
				std::cout << "AudiomixerService::HandleSignalChange: Value =  "
					  << value << std::endl;
			float gain = value * 0.12;
			SetEqualizerGain(path, "bass", gain);
		}
	} else if (path == VSS_TREBLE) {
		if (dp.has_int32()) {
			int32_t value = dp.int32();
			if (m_config.verbose() > 1)
				std::cout << "AudiomixerService::HandleSignalChange: Value =  "
					  << value << std::endl;
			float gain = value * 0.12;
			SetEqualizerGain(path, "treble", gain);
		}
	}
	// else ignore

control_error:
	audiomixer_unlock(m_audiomixer);
}

void AudiomixerService::HandleSignalSetError(const std::string &path, const Error &error)
{
	std::cerr << "Error setting " << path << ": " << error.code() << " - " << error.reason() << std::endl;
}

void AudiomixerService::HandleSubscribeDone(const SubscribeRequest *request, const Status &status)
{
	if (m_config.verbose())
		std::cout << "Subscribe status = " << status.error_code()
			  << " (" << status.error_message() << ")" << std::endl;

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
		if (std::string(control->name) == PLAYBACK_MAIN) {
			// Detecting changes is complicated by the reported volume
			// always being the same as the left channel volume.
			// With our balance logic, the greater of the two volumes
			// should always equal what we consider the overall volume
			// to be.  We do not handle calculating a new balance
			// value from an external change.
			double volume = control->lvolume;
			if (control->rvolume > control->lvolume)
				volume = control->rvolume;
			if (fabs(m_volume_faded - volume) >= 0.00001) {
				std::cout << "WARNING: external volume change seen" << std::endl;

				// Attempt to rebase our state
				m_volume_faded = volume;
				if (m_fade < 0)
					m_volume = volume / (1.0 - (m_fade / -100.0));
				else
					m_volume = volume;
			}

			// Push out new value
			uint32_t value = (m_volume + 0.005) * 100;
			if (value <= 100) {
				m_broker->set(VSS_VOLUME,
					      value,
					      [this](const std::string &path, const Error &error) {
						      HandleSignalSetError(path, error);
					      });
			}
		} else if (std::string(control->name) == PLAYBACK_NAV) {
			if (control->volume != m_nav_volume) {
				std::cout << "WARNING: external Navigation volume change seen" << std::endl;
				m_nav_volume = control->volume;
			}

			// Push out new value
			uint32_t value = (control->volume + 0.005) * 100;
			if (value <= 100) {
				m_broker->set(VSS_NAV_VOLUME,
					      value,
					      [this](const std::string &path, const Error &error) {
						      HandleSignalSetError(path, error);
					      });
			}
		}
	} else if (change_mask & MIXER_CONTROL_CHANGE_FLAG_MUTE) {
		if (std::string(control->name) == PLAYBACK_NAV) {
			if (control->mute != m_nav_mute) {
				std::cout << "WARNING: external Navigation mute change seen" << std::endl;
				m_nav_mute = control->mute;
			}

			// Push out new value
			const char *value = "UNMUTED";
			if (control->mute)
				value = "MUTED";
			m_broker->set(VSS_NAV_MUTE,
				      value,
				      [this](const std::string &path, const Error &error) {
					      HandleSignalSetError(path, error);
				      });
		}
		// Else, do nothing, new state is in control->mute
	}
	// External gain control changes ignored for now
}

// NOTE: These functions assume the audiomixer lock is held

void AudiomixerService::SetMainVolume()
{
	const struct mixer_control *ctl = audiomixer_find_control(m_audiomixer, PLAYBACK_MAIN);
	if (!ctl)
		return;

	if (m_balance == 0) {
		if (m_config.verbose() > 1)
			std::cout << "Setting 'Master Playback' volume to "
				  << m_volume
				  << std::endl;
		// Handle fade
		m_volume_faded = m_volume;
		if (m_fade < 0)
			m_volume_faded *= (1.0 - (m_fade / -100.0));

		audiomixer_change_volume(m_audiomixer, ctl, m_volume_faded);
	} else {
		// Handle fade
		m_volume_faded = m_volume;
		if (m_fade < 0)
			m_volume_faded *= (1.0 - (m_fade / -100.0));

		// Scale down the non-emphasized channel
		double lvol = m_volume_faded;
		double rvol = m_volume_faded;
		if (m_balance < 0)
			rvol = (1.0 - (m_balance / -100.0)) * m_volume_faded;
		else if (m_balance > 0)
			lvol = (1.0 - (m_balance / 100.0)) * m_volume_faded;

		if (m_config.verbose() > 1)
			std::cout << "Setting 'Master Playback' volume to ("
				  << lvol
				  << ", "
				  << rvol
				  << ")"
				  << std::endl;
		audiomixer_change_channel_volume(m_audiomixer, ctl, lvol, rvol);
	}
}

void AudiomixerService::SetMixerVolume(const std::string &path, const std::string &mixer, const double volume)
{
	if (volume < 0.0 || volume > 1.0)
		return;

	const struct mixer_control *ctl = audiomixer_find_control(m_audiomixer, mixer.c_str());
	if (!ctl)
		return;

	if (m_config.verbose() > 1)
		std::cout << "Setting '" << ctl->name << "' volume to " << volume << std::endl;
	audiomixer_change_volume(m_audiomixer, ctl, volume);
}

void AudiomixerService::SetMixerMute(const std::string path, const std::string mixer, const bool mute)
{
	const struct mixer_control *ctl = audiomixer_find_control(m_audiomixer, mixer.c_str());
	if (!ctl)
		return;

	if (m_config.verbose() > 1)
		std::cout << "Setting '" << mixer << "' mute to " << mute << std::endl;
	audiomixer_change_mute(m_audiomixer, ctl, mute);
}

void AudiomixerService::SetEqualizerGain(const std::string path, const std::string mixer, const double gain)
{
	if (gain < -12.0 || gain > 12.0)
		return;

	const struct mixer_control *ctl = audiomixer_find_control(m_audiomixer, mixer.c_str());
	if (!ctl)
		return;

	if (m_config.verbose() > 1)
		std::cout << "Setting '" << mixer << "' gain to " << gain << std::endl;
	audiomixer_change_gain(m_audiomixer, ctl, gain);

	// Push out new value
	m_broker->set(path,
		      (int32_t) (gain / 0.12),
		      [this](const std::string &path, const Error &error) {
			      HandleSignalSetError(path, error);
		      });
}
