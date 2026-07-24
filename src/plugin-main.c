/*
OTR Video Switcher
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <plugin-support.h>

#include <graphics/vec4.h>
#include <obs-interaction.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("OTR")

#define SOURCE_ID "otr_video_switcher"

#define SETTING_VIDEO_1 "video_1"
#define SETTING_VIDEO_2 "video_2"
#define SETTING_VIDEO_3 "video_3"
#define SETTING_TRANSITION_MS "transition_ms"
#define SETTING_WHOOSH_ENABLED "whoosh_enabled"
#define SETTING_WHOOSH_MONITOR "whoosh_monitor"
#define SETTING_WHOOSH_VOLUME "whoosh_volume"
#define SETTING_OUTPUT_WIDTH "output_width"
#define SETTING_OUTPUT_HEIGHT "output_height"

#define START_SCALE 1.18f
#define END_SCALE 1.00f
#define SWITCH_DEBOUNCE_NS 120000000ULL

struct otr_video_switcher {
	obs_source_t *source;
	obs_scene_t *audio_scene;
	obs_source_t *audio_bus;
	obs_source_t *media;
	obs_source_t *whoosh;
	gs_texrender_t *render;

	pthread_mutex_t mutex;
	char *paths[3];
	uint32_t output_width;
	uint32_t output_height;
	float transition_seconds;
	float whoosh_volume;
	float animation_elapsed;
	float startup_elapsed;
	float restart_elapsed;
	uint64_t last_switch_ns;
	int active_slot;
	bool whoosh_enabled;
	bool whoosh_monitor;
	bool has_media;
	bool animation_waiting;
	bool animation_running;
	bool whoosh_pending;
	bool audio_bus_linked;
	bool audio_bus_showing;
	bool mutex_initialized;

	obs_hotkey_id slot_hotkeys[3];
	obs_hotkey_id stop_hotkey;
};

static void otr_update(void *data, obs_data_t *settings);
static void otr_select_slot(struct otr_video_switcher *switcher, int slot);
static void otr_stop(struct otr_video_switcher *switcher);

static int otr_find_slot(struct otr_video_switcher *switcher, int direction)
{
	int selected = 0;

	pthread_mutex_lock(&switcher->mutex);
	const int current = switcher->active_slot;
	for (int step = 1; step <= 3; step++) {
		int candidate;
		if (current >= 1 && current <= 3) {
			candidate = (current - 1 + direction * step) % 3;
			if (candidate < 0)
				candidate += 3;
			candidate += 1;
		} else {
			candidate = direction > 0 ? step : 4 - step;
		}

		const char *path = switcher->paths[candidate - 1];
		if (path && *path) {
			selected = candidate;
			break;
		}
	}
	pthread_mutex_unlock(&switcher->mutex);

	return selected;
}

static void load_default_hotkeys(struct otr_video_switcher *switcher)
{
	const uint32_t modifiers = INTERACT_CONTROL_KEY | INTERACT_ALT_KEY;
	obs_key_combination_t combinations[4] = {
		{modifiers, OBS_KEY_1},
		{modifiers, OBS_KEY_2},
		{modifiers, OBS_KEY_3},
		{modifiers, OBS_KEY_0},
	};

	for (size_t i = 0; i < 3; i++) {
		if (switcher->slot_hotkeys[i] != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_load_bindings(switcher->slot_hotkeys[i], &combinations[i], 1);
	}
	if (switcher->stop_hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_load_bindings(switcher->stop_hotkey, &combinations[3], 1);
}

static const char *otr_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Source.Name");
}

static obs_source_t *create_media_source(const char *path, bool clear_on_end)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", path ? path : "");
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "clear_on_media_end", clear_on_end);
	obs_data_set_bool(settings, "hw_decode", false);

	obs_source_t *source = obs_source_create_private("ffmpeg_source", NULL, settings);
	obs_data_release(settings);

	if (source)
		obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_NONE);

	return source;
}

static void update_media_path(obs_source_t *media, const char *path)
{
	obs_data_t *settings = obs_source_get_settings(media);
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", path);
	obs_data_set_bool(settings, "looping", false);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "clear_on_media_end", true);
	obs_source_update(media, settings);
	obs_data_release(settings);
}

static void play_whoosh(struct otr_video_switcher *switcher)
{
	bool enabled;
	float volume;

	pthread_mutex_lock(&switcher->mutex);
	enabled = switcher->whoosh_enabled;
	volume = switcher->whoosh_volume;
	pthread_mutex_unlock(&switcher->mutex);

	if (!enabled || !switcher->whoosh)
		return;

	obs_source_set_volume(switcher->whoosh, volume);
	obs_source_set_monitoring_type(switcher->whoosh, OBS_MONITORING_TYPE_NONE);
	obs_source_media_stop(switcher->whoosh);
	obs_source_media_restart(switcher->whoosh);
}

static void otr_select_slot(struct otr_video_switcher *switcher, int slot)
{
	if (!switcher || slot < 1 || slot > 3)
		return;

	const uint64_t now = os_gettime_ns();
	char *path = NULL;

	pthread_mutex_lock(&switcher->mutex);
	if (now - switcher->last_switch_ns < SWITCH_DEBOUNCE_NS) {
		pthread_mutex_unlock(&switcher->mutex);
		return;
	}

	switcher->last_switch_ns = now;
	if (switcher->paths[slot - 1] && *switcher->paths[slot - 1])
		path = bstrdup(switcher->paths[slot - 1]);
	pthread_mutex_unlock(&switcher->mutex);

	if (!path) {
		blog(LOG_WARNING, "[OTR Video Switcher] Video %d has no file assigned", slot);
		return;
	}

	if (!os_file_exists(path)) {
		blog(LOG_WARNING, "[OTR Video Switcher] Video %d file does not exist: %s", slot, path);
		bfree(path);
		return;
	}

	obs_source_media_stop(switcher->media);
	update_media_path(switcher->media, path);
	obs_source_set_monitoring_type(switcher->media, OBS_MONITORING_TYPE_NONE);
	obs_source_media_restart(switcher->media);

	pthread_mutex_lock(&switcher->mutex);
	switcher->active_slot = slot;
	switcher->has_media = true;
	switcher->animation_elapsed = 0.0f;
	switcher->startup_elapsed = 0.0f;
	switcher->restart_elapsed = 0.0f;
	switcher->animation_waiting = true;
	switcher->animation_running = false;
	switcher->whoosh_pending = true;
	pthread_mutex_unlock(&switcher->mutex);

	blog(LOG_INFO, "[OTR Video Switcher] Playing Video %d", slot);
	bfree(path);
}

static void otr_stop(struct otr_video_switcher *switcher)
{
	if (!switcher)
		return;

	obs_source_media_stop(switcher->media);
	obs_source_media_stop(switcher->whoosh);

	pthread_mutex_lock(&switcher->mutex);
	switcher->active_slot = 0;
	switcher->has_media = false;
	switcher->animation_waiting = false;
	switcher->animation_running = false;
	switcher->whoosh_pending = false;
	switcher->animation_elapsed = 0.0f;
	switcher->startup_elapsed = 0.0f;
	switcher->restart_elapsed = 0.0f;
	pthread_mutex_unlock(&switcher->mutex);

	blog(LOG_INFO, "[OTR Video Switcher] Stopped all videos");
}

static void slot_1_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		otr_select_slot(data, 1);
}

static void slot_2_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		otr_select_slot(data, 2);
}

static void slot_3_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		otr_select_slot(data, 3);
}

static void stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		otr_stop(data);
}

static bool slot_1_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	otr_select_slot(data, 1);
	return true;
}

static bool slot_2_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	otr_select_slot(data, 2);
	return true;
}

static bool slot_3_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	otr_select_slot(data, 3);
	return true;
}

static bool stop_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	otr_stop(data);
	return true;
}

static bool restore_hotkeys_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	load_default_hotkeys(data);
	blog(LOG_INFO, "[OTR Video Switcher] Restored Ctrl+Alt+1/2/3/0 Stream Deck shortcuts");
	return true;
}

static void otr_destroy(void *data)
{
	struct otr_video_switcher *switcher = data;
	if (!switcher)
		return;

	if (switcher->audio_bus_linked)
		obs_source_remove_active_child(switcher->source, switcher->audio_bus);
	if (switcher->audio_bus_showing)
		obs_source_dec_showing(switcher->audio_bus);

	obs_scene_release(switcher->audio_scene);
	obs_source_release(switcher->media);
	obs_source_release(switcher->whoosh);

	if (switcher->render) {
		obs_enter_graphics();
		gs_texrender_destroy(switcher->render);
		obs_leave_graphics();
	}

	for (size_t i = 0; i < 3; i++)
		bfree(switcher->paths[i]);

	if (switcher->mutex_initialized)
		pthread_mutex_destroy(&switcher->mutex);
	bfree(switcher);
}

static void *otr_create(obs_data_t *settings, obs_source_t *source)
{
	struct otr_video_switcher *switcher = bzalloc(sizeof(*switcher));
	switcher->source = source;
	switcher->slot_hotkeys[0] = OBS_INVALID_HOTKEY_ID;
	switcher->slot_hotkeys[1] = OBS_INVALID_HOTKEY_ID;
	switcher->slot_hotkeys[2] = OBS_INVALID_HOTKEY_ID;
	switcher->stop_hotkey = OBS_INVALID_HOTKEY_ID;

	pthread_mutex_init_value(&switcher->mutex);
	if (pthread_mutex_init(&switcher->mutex, NULL) != 0)
		goto fail;
	switcher->mutex_initialized = true;

	switcher->media = create_media_source("", true);

	char *whoosh_path = obs_module_file("whoosh2.flac");
	switcher->whoosh = create_media_source(whoosh_path ? whoosh_path : "", true);
	bfree(whoosh_path);

	if (!switcher->media || !switcher->whoosh)
		goto fail;

	switcher->audio_scene = obs_scene_create_private("OTR Video Switcher Audio");
	if (!switcher->audio_scene)
		goto fail;

	if (!obs_scene_add(switcher->audio_scene, switcher->media) ||
	    !obs_scene_add(switcher->audio_scene, switcher->whoosh))
		goto fail;

	switcher->audio_bus = obs_scene_get_source(switcher->audio_scene);
	obs_source_inc_showing(switcher->audio_bus);
	switcher->audio_bus_showing = true;

	if (!obs_source_add_active_child(source, switcher->audio_bus))
		goto fail;
	switcher->audio_bus_linked = true;

	otr_update(switcher, settings);

	switcher->slot_hotkeys[0] =
		obs_hotkey_register_source(source, "OTRVideoSwitcher.Video1", obs_module_text("Hotkey.Video1"),
					   slot_1_hotkey, switcher);
	switcher->slot_hotkeys[1] =
		obs_hotkey_register_source(source, "OTRVideoSwitcher.Video2", obs_module_text("Hotkey.Video2"),
					   slot_2_hotkey, switcher);
	switcher->slot_hotkeys[2] =
		obs_hotkey_register_source(source, "OTRVideoSwitcher.Video3", obs_module_text("Hotkey.Video3"),
					   slot_3_hotkey, switcher);
	switcher->stop_hotkey = obs_hotkey_register_source(
		source, "OTRVideoSwitcher.Stop", obs_module_text("Hotkey.Stop"), stop_hotkey, switcher);
	load_default_hotkeys(switcher);

	return switcher;

fail:
	otr_destroy(switcher);
	return NULL;
}

static void otr_update(void *data, obs_data_t *settings)
{
	struct otr_video_switcher *switcher = data;
	const char *new_paths[3] = {
		obs_data_get_string(settings, SETTING_VIDEO_1),
		obs_data_get_string(settings, SETTING_VIDEO_2),
		obs_data_get_string(settings, SETTING_VIDEO_3),
	};

	pthread_mutex_lock(&switcher->mutex);
	for (size_t i = 0; i < 3; i++) {
		bfree(switcher->paths[i]);
		switcher->paths[i] = bstrdup(new_paths[i]);
	}

	switcher->output_width = (uint32_t)obs_data_get_int(settings, SETTING_OUTPUT_WIDTH);
	switcher->output_height = (uint32_t)obs_data_get_int(settings, SETTING_OUTPUT_HEIGHT);
	switcher->transition_seconds = (float)obs_data_get_int(settings, SETTING_TRANSITION_MS) / 1000.0f;
	switcher->whoosh_enabled = obs_data_get_bool(settings, SETTING_WHOOSH_ENABLED);
	switcher->whoosh_monitor = obs_data_get_bool(settings, SETTING_WHOOSH_MONITOR);
	switcher->whoosh_volume = (float)obs_data_get_int(settings, SETTING_WHOOSH_VOLUME) / 100.0f;
	pthread_mutex_unlock(&switcher->mutex);

	obs_source_set_volume(switcher->whoosh, switcher->whoosh_volume);
	obs_source_set_monitoring_type(switcher->whoosh, OBS_MONITORING_TYPE_NONE);
	obs_source_set_monitoring_type(switcher->media, OBS_MONITORING_TYPE_NONE);
	obs_source_set_monitoring_type(switcher->source,
				       switcher->whoosh_monitor ? OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT
							 : OBS_MONITORING_TYPE_NONE);
}

static void otr_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_OUTPUT_WIDTH, 1920);
	obs_data_set_default_int(settings, SETTING_OUTPUT_HEIGHT, 1080);
	obs_data_set_default_int(settings, SETTING_TRANSITION_MS, 900);
	obs_data_set_default_bool(settings, SETTING_WHOOSH_ENABLED, true);
	obs_data_set_default_bool(settings, SETTING_WHOOSH_MONITOR, true);
	obs_data_set_default_int(settings, SETTING_WHOOSH_VOLUME, 65);
}

static obs_properties_t *otr_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "instructions", obs_module_text("Properties.Instructions"), OBS_TEXT_INFO);
	obs_properties_add_text(props, "stream_deck_instructions",
				obs_module_text("Properties.StreamDeckInstructions"), OBS_TEXT_INFO);

	const char *filter = obs_module_text("Properties.VideoFilter");
	obs_properties_add_path(props, SETTING_VIDEO_1, obs_module_text("Properties.Video1"), OBS_PATH_FILE, filter,
				NULL);
	obs_properties_add_path(props, SETTING_VIDEO_2, obs_module_text("Properties.Video2"), OBS_PATH_FILE, filter,
				NULL);
	obs_properties_add_path(props, SETTING_VIDEO_3, obs_module_text("Properties.Video3"), OBS_PATH_FILE, filter,
				NULL);

	obs_property_t *duration = obs_properties_add_int_slider(
		props, SETTING_TRANSITION_MS, obs_module_text("Properties.Duration"), 150, 2000, 10);
	obs_property_int_set_suffix(duration, " ms");

	obs_properties_add_bool(props, SETTING_WHOOSH_ENABLED, obs_module_text("Properties.WhooshEnabled"));
	obs_properties_add_bool(props, SETTING_WHOOSH_MONITOR, obs_module_text("Properties.WhooshMonitor"));
	obs_property_t *volume = obs_properties_add_int_slider(
		props, SETTING_WHOOSH_VOLUME, obs_module_text("Properties.WhooshVolume"), 0, 100, 1);
	obs_property_int_set_suffix(volume, "%");

	obs_properties_add_int(props, SETTING_OUTPUT_WIDTH, obs_module_text("Properties.Width"), 320, 7680, 1);
	obs_properties_add_int(props, SETTING_OUTPUT_HEIGHT, obs_module_text("Properties.Height"), 180, 4320, 1);

	obs_properties_add_button(props, "play_video_1", obs_module_text("Properties.PlayVideo1"), slot_1_button);
	obs_properties_add_button(props, "play_video_2", obs_module_text("Properties.PlayVideo2"), slot_2_button);
	obs_properties_add_button(props, "play_video_3", obs_module_text("Properties.PlayVideo3"), slot_3_button);
	obs_properties_add_button(props, "stop_all", obs_module_text("Properties.Stop"), stop_button);
	obs_properties_add_button(props, "restore_hotkeys", obs_module_text("Properties.RestoreHotkeys"),
				  restore_hotkeys_button);

	UNUSED_PARAMETER(data);
	return props;
}

static uint32_t otr_width(void *data)
{
	struct otr_video_switcher *switcher = data;
	pthread_mutex_lock(&switcher->mutex);
	const uint32_t width = switcher->output_width;
	pthread_mutex_unlock(&switcher->mutex);
	return width;
}

static uint32_t otr_height(void *data)
{
	struct otr_video_switcher *switcher = data;
	pthread_mutex_lock(&switcher->mutex);
	const uint32_t height = switcher->output_height;
	pthread_mutex_unlock(&switcher->mutex);
	return height;
}

static void otr_tick(void *data, float seconds)
{
	struct otr_video_switcher *switcher = data;
	bool trigger_whoosh = false;
	bool retry_media = false;

	pthread_mutex_lock(&switcher->mutex);
	if (switcher->animation_waiting) {
		switcher->startup_elapsed += seconds;
		switcher->restart_elapsed += seconds;
		const enum obs_media_state state = obs_source_media_get_state(switcher->media);
		if (state != OBS_MEDIA_STATE_NONE && state != OBS_MEDIA_STATE_STOPPED &&
		    state != OBS_MEDIA_STATE_ENDED && state != OBS_MEDIA_STATE_ERROR &&
		    obs_source_get_width(switcher->media) > 0 && obs_source_get_height(switcher->media) > 0) {
			switcher->animation_waiting = false;
			switcher->animation_running = true;
			switcher->animation_elapsed = 0.0f;
			trigger_whoosh = switcher->whoosh_pending;
			switcher->whoosh_pending = false;
		} else if (switcher->restart_elapsed >= 0.25f && switcher->startup_elapsed <= 2.0f) {
			switcher->restart_elapsed = 0.0f;
			retry_media = true;
		}
	} else if (switcher->animation_running) {
		switcher->animation_elapsed += seconds;
		if (switcher->animation_elapsed >= switcher->transition_seconds) {
			switcher->animation_elapsed = switcher->transition_seconds;
			switcher->animation_running = false;
		}
	}
	pthread_mutex_unlock(&switcher->mutex);

	if (retry_media)
		obs_source_media_restart(switcher->media);
	if (trigger_whoosh)
		play_whoosh(switcher);
}

static void draw_render_texture(struct otr_video_switcher *switcher, uint32_t width, uint32_t height)
{
	gs_texture_t *texture = gs_texrender_get_texture(switcher->render);
	if (!texture)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, width, height);
}

static void otr_render(void *data, gs_effect_t *unused_effect)
{
	struct otr_video_switcher *switcher = data;
	uint32_t output_width;
	uint32_t output_height;
	float progress;
	bool has_media;
	bool animation_waiting;

	pthread_mutex_lock(&switcher->mutex);
	output_width = switcher->output_width;
	output_height = switcher->output_height;
	has_media = switcher->has_media;
	animation_waiting = switcher->animation_waiting;
	progress = switcher->animation_running && switcher->transition_seconds > 0.0f
			   ? switcher->animation_elapsed / switcher->transition_seconds
			   : 1.0f;
	pthread_mutex_unlock(&switcher->mutex);

	if (!has_media || animation_waiting || !output_width || !output_height)
		return;

	const uint32_t media_width = obs_source_get_width(switcher->media);
	const uint32_t media_height = obs_source_get_height(switcher->media);
	if (!media_width || !media_height)
		return;

	if (!switcher->render)
		switcher->render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!switcher->render)
		return;

	if (progress < 0.0f)
		progress = 0.0f;
	else if (progress > 1.0f)
		progress = 1.0f;
	const float animation_scale = START_SCALE + (END_SCALE - START_SCALE) * progress;
	const float scale_x = ((float)output_width / (float)media_width) * animation_scale;
	const float scale_y = ((float)output_height / (float)media_height) * animation_scale;
	const float translated_width = (float)output_width / scale_x;
	const float translated_height = (float)output_height / scale_y;
	const float offset_x = (translated_width - (float)media_width) * 0.5f;
	const float offset_y = (translated_height - (float)media_height) * 0.5f;

	gs_texrender_reset(switcher->render);
	if (gs_texrender_begin(switcher->render, output_width, output_height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)output_width, 0.0f, (float)output_height, -100.0f, 100.0f);

		gs_matrix_push();
		gs_matrix_scale3f(scale_x, scale_y, 1.0f);
		gs_matrix_translate3f(offset_x, offset_y, 0.0f);
		obs_source_video_render(switcher->media);
		gs_matrix_pop();

		gs_texrender_end(switcher->render);
	}

	draw_render_texture(switcher, output_width, output_height);
	UNUSED_PARAMETER(unused_effect);
}

static inline bool copy_audio_mix(obs_source_t *source, uint64_t *ts_out,
				  struct obs_source_audio_mix *audio_output, uint32_t mixers, size_t channels)
{
	if (!source || obs_source_audio_pending(source))
		return false;

	const uint64_t source_ts = obs_source_get_audio_timestamp(source);
	if (!source_ts)
		return false;

	struct obs_source_audio_mix child_audio;
	obs_source_get_audio_mix(source, &child_audio);

	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t channel = 0; channel < channels; channel++) {
			float *out = audio_output->output[mix].data[channel];
			float *in = child_audio.output[mix].data[channel];
			memcpy(out, in, AUDIO_OUTPUT_FRAMES * sizeof(float));
		}
	}

	*ts_out = source_ts;
	return true;
}

static bool otr_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output,
			     uint32_t mixers, size_t channels, size_t sample_rate)
{
	struct otr_video_switcher *switcher = data;
	UNUSED_PARAMETER(sample_rate);
	return copy_audio_mix(switcher->audio_bus, ts_out, audio_output, mixers, channels);
}

static void otr_enum_sources(void *data, obs_source_enum_proc_t callback, void *param)
{
	struct otr_video_switcher *switcher = data;
	if (switcher->audio_bus)
		callback(switcher->source, switcher->audio_bus, param);
}

static void otr_media_play_pause(void *data, bool pause)
{
	struct otr_video_switcher *switcher = data;
	int active_slot;
	pthread_mutex_lock(&switcher->mutex);
	const bool has_media = switcher->has_media;
	active_slot = switcher->active_slot;
	pthread_mutex_unlock(&switcher->mutex);

	if (!has_media) {
		if (!pause) {
			const int slot = otr_find_slot(switcher, 1);
			if (slot > 0)
				otr_select_slot(switcher, slot);
		}
		return;
	}

	const enum obs_media_state state = obs_source_media_get_state(switcher->media);
	if (!pause && (state == OBS_MEDIA_STATE_NONE || state == OBS_MEDIA_STATE_STOPPED ||
		       state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_ERROR)) {
		otr_select_slot(switcher, active_slot);
		return;
	}

	obs_source_media_play_pause(switcher->media, pause);
}

static void otr_media_restart(void *data)
{
	struct otr_video_switcher *switcher = data;
	int slot;
	pthread_mutex_lock(&switcher->mutex);
	slot = switcher->active_slot;
	pthread_mutex_unlock(&switcher->mutex);
	if (slot > 0)
		otr_select_slot(switcher, slot);
}

static void otr_media_stop(void *data)
{
	otr_stop(data);
}

static void otr_media_next(void *data)
{
	struct otr_video_switcher *switcher = data;
	const int slot = otr_find_slot(switcher, 1);
	if (slot > 0)
		otr_select_slot(switcher, slot);
}

static void otr_media_previous(void *data)
{
	struct otr_video_switcher *switcher = data;
	const int slot = otr_find_slot(switcher, -1);
	if (slot > 0)
		otr_select_slot(switcher, slot);
}

static int64_t otr_media_duration(void *data)
{
	struct otr_video_switcher *switcher = data;
	pthread_mutex_lock(&switcher->mutex);
	const bool has_media = switcher->has_media;
	pthread_mutex_unlock(&switcher->mutex);
	return has_media ? obs_source_media_get_duration(switcher->media) : 0;
}

static int64_t otr_media_time(void *data)
{
	struct otr_video_switcher *switcher = data;
	pthread_mutex_lock(&switcher->mutex);
	const bool has_media = switcher->has_media;
	pthread_mutex_unlock(&switcher->mutex);
	return has_media ? obs_source_media_get_time(switcher->media) : 0;
}

static void otr_media_set_time(void *data, int64_t ms)
{
	struct otr_video_switcher *switcher = data;
	pthread_mutex_lock(&switcher->mutex);
	const bool has_media = switcher->has_media;
	pthread_mutex_unlock(&switcher->mutex);
	if (has_media)
		obs_source_media_set_time(switcher->media, ms);
}

static enum obs_media_state otr_media_state(void *data)
{
	struct otr_video_switcher *switcher = data;
	pthread_mutex_lock(&switcher->mutex);
	const bool has_media = switcher->has_media;
	pthread_mutex_unlock(&switcher->mutex);
	return has_media ? obs_source_media_get_state(switcher->media) : OBS_MEDIA_STATE_STOPPED;
}

static enum gs_color_space otr_color_space(void *data, size_t count,
					   const enum gs_color_space *preferred_spaces)
{
	struct otr_video_switcher *switcher = data;
	if (!switcher->media)
		return count ? preferred_spaces[0] : GS_CS_SRGB;
	return obs_source_get_color_space(switcher->media, count, preferred_spaces);
}

static struct obs_source_info otr_source_info = {
	.id = SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE |
			OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = otr_get_name,
	.create = otr_create,
	.destroy = otr_destroy,
	.get_width = otr_width,
	.get_height = otr_height,
	.get_defaults = otr_defaults,
	.get_properties = otr_properties,
	.update = otr_update,
	.video_tick = otr_tick,
	.video_render = otr_render,
	.audio_render = otr_audio_render,
	.enum_active_sources = otr_enum_sources,
	.enum_all_sources = otr_enum_sources,
	.video_get_color_space = otr_color_space,
	.media_play_pause = otr_media_play_pause,
	.media_restart = otr_media_restart,
	.media_stop = otr_media_stop,
	.media_next = otr_media_next,
	.media_previous = otr_media_previous,
	.media_get_duration = otr_media_duration,
	.media_get_time = otr_media_time,
	.media_set_time = otr_media_set_time,
	.media_get_state = otr_media_state,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};

bool obs_module_load(void)
{
	obs_register_source(&otr_source_info);
	obs_log(LOG_INFO, "loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "unloaded");
}
