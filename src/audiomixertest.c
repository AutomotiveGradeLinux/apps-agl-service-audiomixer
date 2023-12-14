/* WirePlumber
 *
 * Copyright © 2023 Collabora Ltd.
 *    @author Ashok Sidipotu <ashok.sidipotu@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <wp/wp.h>
#include <stdio.h>
#include <getopt.h>
#include <math.h>
#include "audiomixer.h"

typedef struct _AudioMixerTest AudioMixerTest;

struct _AudioMixerTest
{
	struct audiomixer *am;
	WpCore *core;
	const struct mixer_control **ctrls;
	unsigned int nctrls;
	GMainLoop *loop;
};

static void
audio_mixer_clear (AudioMixerTest *self)
{
	audiomixer_free (self->am);
	g_clear_pointer (&self->loop, g_main_loop_unref);
}

static gint
set_mute (AudioMixerTest *self, gint id)
{
	gint ret = -1;
	gboolean mute = FALSE;
	int i;

	for (i = 0; i < self->nctrls; i++) {
		const struct mixer_control *ctrl = self->ctrls[i];
		if (id == i) {

			if (g_str_equal ("bass", ctrl->name) || g_str_equal ("treble", ctrl->name))
				g_warning ("mute cannot be applied for %s control", ctrl->name);
			else {
				/* toggle mute value */
				mute = (ctrl->mute) ? FALSE : TRUE;
				audiomixer_change_mute (self->am, ctrl, mute);
				ret = TRUE;
			}

			break;
		}
	}

	return ret;
}

static gint
set_gain (AudioMixerTest *self, gint id, gfloat gain)
{
	gint ret = -1;
	int i;

	for (i = 0; i < self->nctrls; i++) {
		const struct mixer_control *ctrl = self->ctrls[i];

		if(id == i) {

			if (g_str_equal ("bass", ctrl->name) || g_str_equal ("treble", ctrl->name)) {
				if (fabs (ctrl->gain - gain) < 0.00001)
					g_warning ("gain already at requested level %f", ctrl->gain);
				else {
					audiomixer_change_gain (self->am, ctrl, gain);
					ret = TRUE;
				}
			}
			else
				g_warning ("gain cannot be applied for %s control", ctrl->name);

			break;
		}
	}

	return ret;
}

static gint
set_volume (AudioMixerTest *self, gint id, double vol)
{
	gint ret = -1;
	int i;

	for (i = 0; i < self->nctrls; i++) {
		const struct mixer_control *ctrl = self->ctrls[i];

		if (id == i) {
			if (g_str_equal ("bass", ctrl->name) || g_str_equal ("treble", ctrl->name))
				g_warning ("volume cannot be applied for %s control", ctrl->name);
			else {

				if (fabs (ctrl->volume - vol) < 0.00001)
					g_warning ("volume is already at requested level %f", ctrl->volume);
				else {
					audiomixer_change_volume (self->am, ctrl, vol);
					ret = TRUE;
				}

			}
			break;
		}
	}

	return ret;
}


static gint
set_channels_volume (AudioMixerTest *self, gint id, double lvol, double rvol)
{
	gint ret = -1;
	int i;
	gboolean set_left_channel_volume = TRUE;
	gboolean set_right_channel_volume = TRUE;

	for (i = 0; i < self->nctrls; i++) {
		const struct mixer_control *ctrl = self->ctrls[i];

		if (id == i) {
			if (g_str_equal ("bass", ctrl->name) || g_str_equal ("treble", ctrl->name))
				g_warning ("volume cannot be applied for %s control", ctrl->name);
			else {

				if (fabs (ctrl->lvolume - lvol) < 0.00001) {
					set_left_channel_volume = FALSE;
					lvol = ctrl->lvolume;
					g_warning ("left volume is already at requested level %f",
						ctrl->lvolume);
				}

				if (fabs (ctrl->rvolume - rvol) < 0.00001) {
					set_right_channel_volume = FALSE;
					rvol = ctrl->rvolume;
					g_warning ("right volume is already at requested level %f",
						ctrl->rvolume);
				}

				if (set_left_channel_volume || set_right_channel_volume) {
					audiomixer_change_channel_volume (self->am, ctrl, lvol, rvol);
					ret = TRUE;
				}

			}
			break;
		}
	}

	return ret;
}

static void
print_ctrls (AudioMixerTest *self)
{
	const struct mixer_control **ctrls = self->ctrls;
	unsigned int nctrls = self->nctrls;
	int i;

	fprintf (stdout, "\nControls:");
	for (i = 0; i < nctrls; i++) {
		const struct mixer_control *ctrl = ctrls[i];
		if (g_str_equal ("bass", ctrl->name) || g_str_equal ("treble", ctrl->name))
			fprintf(stdout, "\n%2d. %-25s   [gain: %.2f]", i, ctrl->name,
				ctrl->gain);
		else
			fprintf (stdout, "\n%2d. %-25s   [vol: %.2f, lvol: %.2f, rvol: %.2f,"
				" mute:%d]", i, ctrl->name, ctrl->volume, ctrl->lvolume, ctrl->rvolume,
				ctrl->mute);
	}
	fprintf (stdout, "\n");
}

static void
refresh_ctrls (AudioMixerTest *self)
{
	self->ctrls = audiomixer_get_active_controls (self->am, &self->nctrls);
	print_ctrls (self);
}

static void show_help (void)
{
	fprintf (stdout,
		"\n"
		"  -h, --help           Show this help\n"
		"  -p, --print-controls prints controls\n"
		"  -i, --id             control id(serial#) of the control, take a look at the controls to get the id of control\n"
		"                       Examples:\n"
		"                       audio-mixer-test -> prints the controls and help text\n"
		"                       audio-mixer-test -p -> prints only the controls\n"
		"  -v, --set-volume     set volume level for a volume control(all controls except bass and treble) this option sets\n"
		"                       volume for all the channels, for setting channel specific volumes check -l or -r options\n"
		"                       Examples:\n"
		"                       audio-mixer-test -v 0.2                   -> sets volume(for all channels) of the 1st control(default id)\n"
		"                                                                    with 0.2\n"
		"                       audio-mixer-test -i 9 -v 0.2              -> sets volume of the 9th control with 0.2\n"
		"  -l, --left-chan-vol  set left channel volume for a volume control(all controls except bass and treble)\n"
		"  -r, --right-chan-vol set right channel volume for a volume control(all controls except bass and treble)\n"
		"                       only stereo channels are supported for now, and vol update on both the channels is required\n"
		"                       Examples:\n"
		"                       audio-mixer-test -l 0.2                   -> gives an error as only one channel is updated\n"
		"                       audio-mixer-test -l 0.1 -r 0.2            -> sets left channel volume of 0.1 and right channel volume of\n"
		"                                                                    0.2 on the 1st control (default id) with 0.2\n"
		"                       audio-mixer-test -i 9 -l 0.1 -r 0.2       -> sets left channel volume of 0.1 and right channel volume\n"
		"                                                                    of 0.2 on the 9th control with 0.2\n"
		"  -g, --set-gain       set gain level for gain controls like bass and treble\n"
		"                       Examples:\n"
		"                       audio-mixer-test -i 12 -g 0.8             -> sets gain of the 11th control with 0.8\n"
		"  -m, --set-mute       mute/unmute volume controls(all controls except bass and treble) takes no arguments\n"
		"                       Examples:\n"
		"                       audio-mixer-test -m                       -> mutes the 1st control (default id)\n"
		"                       audio-mixer-test -m                       -> unmutes the 1st control (default id), if it is issued after\n"
		"                                                                    the above command\n"
		"                       audio-mixer-test -i 9 -m                  -> mutes 9th control (Multimedia) with 0.8\n");
}

static void
mixer_value_change_cb (void *data,
	unsigned int change_mask,
	const struct mixer_control *ctrl)
{
	AudioMixerTest *self = (AudioMixerTest *)data;
	refresh_ctrls (self);
	g_main_loop_quit (self->loop);
}

static void
mixer_controls_changed (void *data)
{
	AudioMixerTest *self = (AudioMixerTest *)data;
	g_main_loop_quit (self->loop);
}

gint
main (gint argc, gchar **argv)
{
	AudioMixerTest self = { 0 };
	g_autoptr (GError) error = NULL;
	gint c, ret = 0;
	struct audiomixer_events audiomixer_events = { 0 };

	gint id = -1;
	double vol = 0.0, lvol = 0.0, rvol = 0.0;
	gfloat gain = 0.0;
	gboolean set_right_channel_volume = FALSE;
	gboolean set_left_channel_volume = FALSE;

	self.loop = g_main_loop_new (NULL, FALSE);

	self.am = audiomixer_new ();

	if (!self.am) {
		g_warning ("unable to open audiomixer");
		goto exit;
	}

	audiomixer_lock (self.am);
	ret = audiomixer_ensure_controls (self.am, 3);
	audiomixer_unlock (self.am);
	if (ret < 0) {
		g_warning ("ensure controls failed");
		goto exit;
	}

	audiomixer_events.controls_changed = mixer_controls_changed;
	audiomixer_add_event_listener (self.am, &audiomixer_events, (void *)&self);

	g_debug ("waiting for controls to be available");

	do {
		self.ctrls = audiomixer_get_active_controls (self.am, &self.nctrls);

		/*
		 * not a clean check but it appears like this is the best we can do at the
		 * moment.
		 */
		if (self.nctrls <= 4)
			/* end points are not registered, wait for them to show up */
			g_main_loop_run (self.loop);
		else
			break;

	} while (1);

	if (argc == 1) {
		print_ctrls (&self);
		show_help ();
		return 0;
	}

	audiomixer_events.value_changed = mixer_value_change_cb;
	audiomixer_add_event_listener (self.am, &audiomixer_events, (void *)&self);

	static const struct option long_options[] = {
		{ "help",						no_argument,				NULL, 'h' },
		{ "print-controls",	no_argument,				NULL, 'p' },
		{ "id",							required_argument,	NULL, 'i' },
		{ "left-chan-vol",	required_argument,	NULL, 'l' },
		{ "right-chan-vol",	required_argument,	NULL, 'r' },
		{ "set-volume",			required_argument,	NULL, 'v' },
		{ "set-mute",				no_argument,				NULL, 'm' },
		{ "set-gain",				required_argument,	NULL, 'g' },
		{ NULL,	0, NULL, 0}
	};

	while ((c = getopt_long (argc, argv, "hpi:v:mg:l:r:", long_options, NULL)) != -1) {
		switch(c) {
		case 'h':
			show_help ();
			break;

		case 'p':
			print_ctrls (&self);
			break;

		case 'i':
			id = atoi (optarg);
			if (!(id >= 0 && id < self.nctrls)) {
				ret = -1;
				g_warning ("id(%d) is invalid", id);
			}
			break;

		case 'v':
			vol = (double)atof (optarg);
			if (id == -1) {
				g_warning ("control id not given defaulting it to 0(Master Playback)");
				id = 0;
			}

			ret = set_volume (&self, id, vol);
			if (ret != TRUE)
				g_warning ("set-volume failed");
			else
				/* wait for volume to be acked */
				g_main_loop_run (self.loop);

			break;

		case 'l':
			if (id == -1) {
				g_warning ("control id not given defaulting it to 0(Master Playback)");
				id = 0;
			}

			lvol = (double)atof (optarg);
			set_left_channel_volume = TRUE;
			break;

		case 'r':
			if (id == -1) {
				g_warning ("control id not given defaulting it to 0(Master Playback)");
				id = 0;
			}

			rvol = (double)atof (optarg);
			set_right_channel_volume = TRUE;
			break;

		case 'm':
			if (id == -1) {
				g_warning ("control id not given defaulting it to 0(Master Playback)");
				id = 0;
			}

			ret = set_mute (&self, id);
			if (ret != TRUE)
				g_warning ("set-mute failed");
			else
				/* wait for mute to be acked */
				g_main_loop_run (self.loop);

			break;

		case 'g':
			gain = atof (optarg);
			if (id == -1) {
				g_warning ("control id not given defaulting it to 11(bass)");
				id = 11; /* bass ctrl */
			}

			ret = set_gain (&self, id, gain);
			if (ret != TRUE)
				g_warning ("set-gain failed");
			else
				/* wait for gain to be acked */
				g_main_loop_run (self.loop);

			break;

		default:
			show_help ();
			break;
		}
	}

	if (set_left_channel_volume && set_right_channel_volume) {
		ret = set_channels_volume (&self, id, lvol, rvol);
		if (ret != TRUE)
			g_warning ("set_channels_volume failed");
		else
			/* wait for volume to be acked */
			g_main_loop_run (self.loop);
	}
	else if (set_left_channel_volume || set_right_channel_volume) {
		g_warning ("set volume of both left and right channels");
	}

exit:
	/* clean up at program exit */
	audio_mixer_clear (&self);
	return ret;
}
