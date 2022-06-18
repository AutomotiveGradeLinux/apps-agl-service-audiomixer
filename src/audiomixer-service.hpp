// SPDX-License-Identifier: Apache-2.0

#ifndef _AUDIOMIXER_SERVICE_HPP
#define _AUDIOMIXER_SERVICE_HPP

#include "vis-session.hpp"
#include "audiomixer.h"

class AudiomixerService : public VisSession
{
	struct audiomixer *m_audiomixer;

public:
	AudiomixerService(const VisConfig &config, net::io_context& ioc, ssl::context& ctx);

	~AudiomixerService();

	static void audiomixer_control_change_cb(void *data) {
		if (data)
			((AudiomixerService*) data)->handle_control_change();
	};

	static void audiomixer_value_change_cb(void *data,
					       unsigned int change_mask,
					       const struct mixer_control *control) {
		if (data)
			((AudiomixerService*) data)->handle_value_change(change_mask, control);
	}

protected:
	struct audiomixer_events m_audiomixer_events;

	virtual void handle_authorized_response(void) override;

	virtual void handle_get_response(std::string &path, std::string &value, std::string &timestamp) override;

	virtual void handle_notification(std::string &path, std::string &value, std::string &timestamp) override;

	virtual void handle_control_change(void);

	virtual void handle_value_change(unsigned int change_mask, const struct mixer_control *control);
};

#endif // _AUDIOMIXER_SERVICE_HPP
