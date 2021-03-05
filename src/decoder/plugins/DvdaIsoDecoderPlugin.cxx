/*
 * Copyright (C) 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include <audio_stream.h>
#include <audio_track.h>
#include <stream_buffer.h>
#include <log_trunk.h>
#include <dvda_disc.h>
#include <dvda_metabase.h>
#include "DvdaIsoDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "song/DetachedSong.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "thread/Cond.hxx"
#include "thread/Mutex.hxx"
#include "util/Alloc.hxx"
#include "util/BitReverse.hxx"
#include "util/StringFormat.hxx"
#include "util/StringView.hxx"
#include "util/UriExtract.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <vector>

static constexpr Domain dvdaiso_domain("dvdaiso");

namespace dvdaiso {

constexpr const char* DVDA_TRACKXXX_FMT{ "AUDIO_TS__TRACK%03u%c.%3s" };
constexpr double SHORT_TRACK_SEC{ 2.0 };

bool        param_no_downmixes;
bool        param_no_short_tracks;
bool        param_no_untagged_tracks;
chmode_t    param_playable_area;
std::string param_tags_path;
bool        param_tags_with_iso;
bool        param_use_stdio;

std::string                      dvda_uri;
std::unique_ptr<dvda_media_t>    dvda_media;
std::unique_ptr<dvda_reader_t>   dvda_reader;
std::unique_ptr<dvda_metabase_t> dvda_metabase;

static unsigned
get_container_path_length(const char* path) {
	std::string container_path = path;
	container_path.resize(strrchr(container_path.c_str(), '/') - container_path.c_str());
	return container_path.length();
}

static std::string
get_container_path(const char* path) {
	std::string container_path = path;
	auto length = get_container_path_length(path);
	if (length >= 4) {
		container_path.resize(length);
		auto c_str = container_path.c_str();
		if (strcasecmp(c_str + length - 4, ".iso") != 0) {
			container_path.resize(0);
		}
	}
	return container_path;
}

static bool
get_subsong(const char* path, unsigned* track_index, bool* downmix) {
	auto length = get_container_path_length(path);
	int params = 0;
	if (length > 0) {
		const char* ptr = path + length + 1;
		unsigned track_number;
		char area = '\0';
		char suffix[4];
		params = sscanf(ptr, DVDA_TRACKXXX_FMT, &track_number, &area, &suffix);
		suffix[3] = '\0';
		*track_index = track_number - 1;
		*downmix = area == 'D';
	}
	return params == 3;
}

static bool
update_ifo(const char* path) {
	std::string curr_uri = path;
	if (path != nullptr) {
		if (!dvda_uri.compare(curr_uri)) {
			return true;
		}
	}
	else {
		if (dvda_uri.empty()) {
			return true;
		}
	}
	if (dvda_reader) {
		dvda_reader->close();
		dvda_reader.reset();
	}
	if (dvda_media) {
		dvda_media->close();
		dvda_media.reset();
	}
	dvda_metabase.reset();
	if (path != nullptr) {
		if (param_use_stdio) {
			dvda_media = std::make_unique<dvda_media_file_t>();
		}
		else {
			dvda_media = std::make_unique<dvda_media_stream_t>();
		}
		if (!dvda_media) {
			LogError(dvdaiso_domain, "new dvda_media_t() failed");
			dvda_uri.clear();
			return false;
		}
		dvda_reader = std::make_unique<dvda_disc_t>();
		if (!dvda_reader) {
			LogError(dvdaiso_domain, "new dvda_disc_t() failed");
			dvda_uri.clear();
			return false;
		}
		if (!dvda_media->open(path)) {
			std::string err;
			err  = "dvda_media->open('";
			err += path;
			err += "') failed";
			LogWarning(dvdaiso_domain, err.c_str());
			dvda_uri.clear();
			return false;
		}
		if (!dvda_reader->open(dvda_media.get())) {
			//LogWarning(dvdaiso_domain, "dvda_reader->open(...) failed");
			dvda_uri.clear();
			return false;
		}
		if (!param_tags_path.empty() || param_tags_with_iso) {
			std::string tags_file;
			if (param_tags_with_iso) {
				tags_file = path;
				tags_file.resize(tags_file.rfind('.') + 1);
				tags_file.append("xml");
			}
			dvda_metabase = std::make_unique<dvda_metabase_t>(reinterpret_cast<dvda_disc_t*>(dvda_reader.get()), param_tags_path.empty() ? nullptr : param_tags_path.c_str(), tags_file.empty() ? nullptr : tags_file.c_str());
		}
	}
	dvda_uri = curr_uri;
	return true;
}

static void
scan_info(unsigned track_index, bool downmix, TagHandler& handler) {
	auto tag_value = std::to_string(track_index + 1);
	handler.OnTag(TAG_TRACK, tag_value.c_str());
	handler.OnDuration(SongTime::FromS(dvda_reader->get_duration(track_index)));
	if (!dvda_metabase || (dvda_metabase && !dvda_metabase->get_track_info(track_index + 1, downmix, handler))) {
		dvda_reader->get_info(track_index, downmix, handler);
	}
	if (handler.WantPicture()) {
		if (dvda_metabase) {
			dvda_metabase->get_albumart(handler);
		}
	}
}

static bool
init(const ConfigBlock& block) {
	my_av_log_set_callback(mpd_av_log_callback);
	param_no_downmixes = block.GetBlockValue("no_downmixes", true);
	param_no_short_tracks = block.GetBlockValue("no_short_tracks", true);
	param_no_untagged_tracks = block.GetBlockValue("no_untagged_tracks", true);
	auto playable_area = block.GetBlockValue("playable_area", nullptr);
	param_playable_area = CHMODE_BOTH;
	if (playable_area != nullptr) {
		if (strcmp(playable_area, "stereo") == 0) {
			param_playable_area = CHMODE_TWOCH;
		}
		if (strcmp(playable_area, "multichannel") == 0) {
			param_playable_area = CHMODE_MULCH;
		}
	}
	param_tags_path = block.GetBlockValue("tags_path", "");
	param_tags_with_iso = block.GetBlockValue("tags_with_iso", false);
	param_use_stdio = block.GetBlockValue("use_stdio", true);
	return true;
}

static void
finish() noexcept {
	update_ifo(nullptr);
	my_av_log_set_default_callback();
}

static std::forward_list<DetachedSong>
container_scan(Path path_fs) {
	std::forward_list<DetachedSong> list;
	if (path_fs.IsNull() || !update_ifo(path_fs.c_str())) {
		return list;
	}
	TagBuilder tag_builder;
	auto tail = list.before_begin();
	auto suffix = path_fs.GetSuffix();
	for (auto track_index = 0u; track_index < dvda_reader->get_tracks(); track_index++) {
		if (dvda_reader->select_track(track_index)) {
			auto duration = dvda_reader->get_duration();
			if (param_no_short_tracks && duration < SHORT_TRACK_SEC) {
				continue;
			}
			auto add_track = false;
			auto add_downmix = false;
			switch (param_playable_area) {
			case CHMODE_MULCH:
				if (dvda_reader->get_channels() > 2) {
					add_track = true;
				}
				break;
			case CHMODE_TWOCH:
				if (dvda_reader->get_channels() <= 2) {
					add_track = true;
				}
				if (!param_no_downmixes && dvda_reader->can_downmix()) {
					add_downmix = true;
				}
				break;
			default:
				add_track = true;
				if (!param_no_downmixes && dvda_reader->can_downmix()) {
					add_downmix = true;
				}
				break;
			}
			if (add_track) {
				AddTagHandler h(tag_builder);
				scan_info(track_index, false, h);
				auto area = dvda_reader->get_channels() > 2 ? 'M' : 'S';
				tail = list.emplace_after(
					tail,
					StringFormat<64>(DVDA_TRACKXXX_FMT, track_index + 1, area, suffix),
					tag_builder.Commit()
				);
			}
			if (add_downmix) {
				AddTagHandler h(tag_builder);
				scan_info(track_index, true, h);
				auto area = 'D';
				tail = list.emplace_after(
					tail,
					StringFormat<64>(DVDA_TRACKXXX_FMT, track_index + 1, area, suffix),
					tag_builder.Commit()
				);
			}
		}
		else {
			LogError(dvdaiso_domain, "cannot select track");
		}
	}
	return list;
}

static void
file_decode(DecoderClient &client, Path path_fs) {
	auto path_container = get_container_path(path_fs.c_str());
	if (!update_ifo(path_container.c_str())) {
		return;
	}
	unsigned track;
	bool downmix;
	if (!get_subsong(path_fs.c_str(), &track, &downmix)) {
		LogError(dvdaiso_domain, "cannot get track number");
		return;
	}

	// initialize reader
	if (!dvda_reader->select_track(track)) {
		LogError(dvdaiso_domain, "cannot select track");
		return;
	}
	if (!dvda_reader->set_downmix(downmix)) {
		LogError(dvdaiso_domain, "cannot downmix track");
		return;
	}
	auto samplerate = dvda_reader->get_samplerate();
	auto channels = dvda_reader->get_downmix() ? 2u : dvda_reader->get_channels();
	std::vector<uint8_t> pcm_data(192000);

	// initialize decoder
	auto audio_format = CheckAudioFormat(samplerate, SampleFormat::S32, channels);
	auto songtime = SongTime::FromS(dvda_reader->get_duration(track));
	client.Ready(audio_format, true, songtime);

	// play
	auto cmd = client.GetCommand();
	for (;;) {
		auto pcm_size = pcm_data.size();
		if (dvda_reader->read_frame(pcm_data.data(), &pcm_size)) {
			if (pcm_size > 0) {
				cmd = client.SubmitData(nullptr, pcm_data.data(), pcm_size, channels * samplerate / 1000);
				if (cmd == DecoderCommand::STOP) {
					break;
				}
				if (cmd == DecoderCommand::SEEK) {
					auto seconds = client.GetSeekTime().ToDoubleS();
					if (dvda_reader->seek(seconds)) {
						client.CommandFinished();
					}
					else {
						client.SeekError();
					}
					cmd = client.GetCommand();
				}
			}
		}
		else {
			break;
		}
	}
}

static bool
scan_file(Path path_fs, TagHandler& handler) noexcept {
	auto path_container = get_container_path(path_fs.c_str());
	if (path_container.empty()) {
		return false;
	}
	if (!update_ifo(path_container.c_str())) {
		return false;
	}
	unsigned track_index;
	bool downmix;
	if (!get_subsong(path_fs.c_str(), &track_index, &downmix)) {
		LogError(dvdaiso_domain, "cannot get track number");
		return false;
	}
	scan_info(track_index, downmix, handler);
	return true;
}

static const char* const suffixes[] {
	"iso",
	nullptr
};

static const char* const mime_types[] {
	"application/x-iso",
	nullptr
};

}

constexpr DecoderPlugin dvdaiso_decoder_plugin =
	DecoderPlugin("dvdaiso", dvdaiso::file_decode, dvdaiso::scan_file)
	.WithInit(dvdaiso::init, dvdaiso::finish)
	.WithContainer(dvdaiso::container_scan)
	.WithSuffixes(dvdaiso::suffixes)
	.WithMimeTypes(dvdaiso::mime_types);
