// SPDX-License-Identifier: Apache-2.0

#include "audiomixer-service.hpp"
#include <iostream>
#include <algorithm>


AudiomixerService::AudiomixerService(const VisConfig &config, net::io_context& ioc, ssl::context& ctx) :
	VisSession(config, ioc, ctx)
{
	m_audiomixer = audiomixer_new();
	if (m_audiomixer) {
		// Set up callbacks for WirePlumber events
		m_audiomixer_events.controls_changed = audiomixer_control_change_cb;
		m_audiomixer_events.value_changed = audiomixer_value_change_cb;
		audiomixer_add_event_listener(m_audiomixer, &m_audiomixer_events, this);

		// Drive connecting to PipeWire core and refreshing controls list
		audiomixer_lock(m_audiomixer);
		audiomixer_ensure_controls(m_audiomixer, 3);
		audiomixer_unlock(m_audiomixer);
	} else {
		std::cerr << "Could not create WirePlumber connection" << std::endl;
	}
}

AudiomixerService::~AudiomixerService()
{
	audiomixer_free(m_audiomixer);
}

void AudiomixerService::handle_authorized_response(void)
{
	subscribe("Vehicle.Cabin.Infotainment.Media.Volume");
	subscribe("Vehicle.Cabin.SteeringWheel.Switches.VolumeUp");
	subscribe("Vehicle.Cabin.SteeringWheel.Switches.VolumeDown");
	subscribe("Vehicle.Cabin.SteeringWheel.Switches.VolumeMute");

	// Set initial volume in VSS
	// For now a value of 50 matches the default in the homescreen app.
	// Ideally there would be some form of persistence scheme to restore
	// the last value on restart.
	set("Vehicle.Cabin.Infotainment.Media.Volume", "50");
}

void AudiomixerService::handle_get_response(std::string &path, std::string &value, std::string &timestamp)
{
	// Placeholder since no gets are performed ATM
}

void AudiomixerService::handle_notification(std::string &path, std::string &value, std::string &timestamp)
{
	if (!m_audiomixer) {
		return;
	}

	audiomixer_lock(m_audiomixer);

	const struct mixer_control *ctl = audiomixer_find_control(m_audiomixer, "Master Playback");
	if (!ctl) {
		audiomixer_unlock(m_audiomixer);
		return;
	}

	if (path == "Vehicle.Cabin.Infotainment.Media.Volume") {
		try {
			int volume = std::stoi(value);
			if (volume >= 0 && volume <= 100) {
				double v = (double) volume / 100.0;
				if (m_config.verbose() > 1)
					std::cout << "Setting volume to " << v << std::endl;
				audiomixer_change_volume(m_audiomixer, ctl, v);
			}
		}
		catch (std::exception ex) {
			// ignore bad value
		}
	} else if (path == "Vehicle.Cabin.SteeringWheel.Switches.VolumeUp" && value == "true") {
		double volume = ctl->volume;
		volume += 0.05; // up 5%
		if (volume > 1.0)
			volume = 1.0; // clamp to 100%
		if (m_config.verbose() > 1)
			std::cout << "Increasing volume to " << volume << std::endl;
		audiomixer_change_volume(m_audiomixer, ctl, volume);

	} else if (path == "Vehicle.Cabin.SteeringWheel.Switches.VolumeDown" && value == "true") {
		double volume = ctl->volume;
		volume -= 0.05; // down 5%
		if (volume < 0.0)
			volume = 0.0; // clamp to 0%
		if (m_config.verbose() > 1)
			std::cout << "Decreasing volume to " << volume << std::endl;
		audiomixer_change_volume(m_audiomixer, ctl, volume);

	} else if (path == "Vehicle.Cabin.SteeringWheel.Switches.VolumeMute" && value == "true") {
		if (m_config.verbose() > 1) {
			if (ctl->mute)
				std::cout << "Unmuting" << std::endl;
			else
				std::cout << "Muting" << std::endl;
		}
		audiomixer_change_mute(m_audiomixer, ctl, !ctl->mute);
	}
	// else ignore

	audiomixer_unlock(m_audiomixer);
}

void AudiomixerService::handle_control_change(void)
{
	// Ignore for now
}

void AudiomixerService::handle_value_change(unsigned int change_mask, const struct mixer_control *control)
{
	if (!control)
		return;

	if (change_mask & MIXER_CONTROL_CHANGE_FLAG_VOLUME) {
		if (std::string(control->name) == "Master Playback") {
			// Push change into VIS
			std::string value = std::to_string((int) (control->volume * 100.0));
			set("Vehicle.Cabin.Infotainment.Media.Volume", value);
		}
	} else if (change_mask & MIXER_CONTROL_CHANGE_FLAG_MUTE) {
		// For now, do nothing, new state is in control->mute
	}
}
