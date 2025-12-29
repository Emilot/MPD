// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * SACD ISO Decoder Plugin
 *
 * This plugin enables playback of SACD ISO images, supporting both
 * 2-channel stereo and multi-channel surround areas.
 *
 * SACD (Super Audio CD) is a high-resolution audio format that stores
 * audio in DSD (Direct Stream Digital) format. This plugin reads SACD
 * ISO images and outputs native DSD data for playback on compatible
 * hardware.
 *
 * DST (Direct Stream Transfer) decoding is handled by our native
 * libdstdec implementation based on ISO/IEC 14496-3.
 *
 * Configuration options (in mpd.conf):
 *   decoder {
 *     plugin "sacdiso"
 *     edited_master "false"     # Use edited master track boundaries
 *     playable_area "stereo"    # "stereo", "multichannel", or "both"
 *     lsbitfirst "false"        # Bit order for DSD output
 *   }
 */

#include "SacdIsoDecoderPlugin.hxx"
#include "config.h"
#include "../DecoderAPI.hxx"
#include "lib/sacdiso/SacdDisc.hxx"
#include "lib/sacdiso/SacdMedia.hxx"
#include "lib/sacdiso/DstDecoder.hxx"
#include "lib/sacdiso/Domain.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "song/DetachedSong.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "thread/Mutex.hxx"
#include "util/BitReverse.hxx"
#include "util/Domain.hxx"
#include "util/ScopeExit.hxx"
#include "util/StringCompare.hxx"
#include "Log.hxx"

#include <forward_list>
#include <memory>

using std::string_view_literals::operator""sv;

namespace {

using namespace Sacd;

/*
 * Plugin configuration
 */

struct SacdConfig {
	bool edited_master = false;
	bool lsbitfirst = false;
	AreaId playable_area = AreaId::Both;
};

SacdConfig config;

/*
 * Track naming format: %cC_AUDIO__TRACK%03u.iso
 * Where %c is '2' for stereo or 'M' for multichannel
 */

constexpr const char* kTrackNameFormat2ch = "2C_AUDIO__TRACK%03u.iso";
constexpr const char* kTrackNameFormatMch = "MC_AUDIO__TRACK%03u.iso";

/**
 * Parse a virtual track name to extract area and track index.
 *
 * @param name The track name (e.g., "2C_AUDIO__TRACK001.sacdtrack")
 * @param area_id Output: the area ID
 * @param track_index Output: the track index (0-based)
 * @return true if the name was parsed successfully
 */
[[nodiscard]]
bool
ParseTrackName(const char* name, AreaId& area_id, unsigned& track_index) noexcept
{
	if (name == nullptr)
		return false;

	unsigned index = 0;

	// Try 2-channel format first
	if (std::sscanf(name, "2C_AUDIO__TRACK%u.iso", &index) == 1) {
		area_id = AreaId::Stereo;
		if (index == 0)
			return false;
		track_index = index - 1;
		return true;
	}

	// Try multichannel format
	if (std::sscanf(name, "MC_AUDIO__TRACK%u.iso", &index) == 1) {
		area_id = AreaId::Multichannel;
		if (index == 0)
			return false;
		track_index = index - 1;
		return true;
	}

	return false;
}

/**
 * Generate a virtual track name.
 */
[[nodiscard]]
std::string
GenerateTrackName(AreaId area_id, unsigned track_index) noexcept
{
	char buffer[64];
	const char* format = (area_id == AreaId::Stereo) 
		? kTrackNameFormat2ch : kTrackNameFormatMch;
	std::snprintf(buffer, sizeof(buffer), format, track_index + 1);
	return buffer;
}

/**
 * Reverse bits in a buffer (for LSB-first DACs).
 */
void
BitReverseBuffer(std::byte* data, std::size_t size) noexcept
{
	for (std::size_t i = 0; i < size; ++i)
		data[i] = BitReverse(data[i]);
}

/*
 * Plugin callbacks
 */

bool
sacdiso_init(const ConfigBlock& block)
{
	config.edited_master = block.GetBlockValue("edited_master", false);
	config.lsbitfirst = block.GetBlockValue("lsbitfirst", false);

	const char* area_str = block.GetBlockValue("playable_area", nullptr);
	if (area_str != nullptr) {
		if (StringIsEqual(area_str, "stereo"))
			config.playable_area = AreaId::Stereo;
		else if (StringIsEqual(area_str, "multichannel"))
			config.playable_area = AreaId::Multichannel;
		else
			config.playable_area = AreaId::Both;
	}

	return true;
}

/**
 * Scan an SACD ISO container and return virtual tracks.
 */
std::forward_list<DetachedSong>
sacdiso_container_scan(Path path_fs)
{
	std::forward_list<DetachedSong> list;

	// Check file extension
	const char* suffix_ptr = path_fs.GetSuffix();
	if (suffix_ptr == nullptr)
		return list;

	const std::string_view suffix{suffix_ptr};

	// GetSuffix() may return with or without leading dot depending on MPD version
	// Handle both cases
	const std::string_view suffix_nodot = (suffix.size() > 0 && suffix[0] == '.') 
	                                      ? suffix.substr(1) : suffix;

	if (!StringIsEqualIgnoreCase(suffix_nodot, "iso"sv) &&
	    !StringIsEqualIgnoreCase(suffix_nodot, "dat"sv))
		return list;

	// Open the disc
	auto media = std::make_unique<FileMedia>();
	if (!media->Open(path_fs.c_str()))
		return list;

	Disc disc;
	if (!disc.Open(std::move(media)))
		return list;

	disc.SetEditedMasterMode(config.edited_master);

	TagBuilder tag_builder;
	auto tail = list.before_begin();
	unsigned total_tracks = 0;

	// Add stereo tracks
	if (disc.HasStereoArea() && config.playable_area != AreaId::Multichannel) {
		disc.SelectArea(AreaId::Stereo);
		const std::size_t track_count = disc.GetTrackCount(AreaId::Stereo);

		for (std::size_t i = 0; i < track_count; ++i) {
			tag_builder.Clear();
			AddTagHandler handler(tag_builder);
			disc.GetTrackInfo(AreaId::Stereo, i, handler);

			tail = list.emplace_after(
				tail,
				GenerateTrackName(AreaId::Stereo, i).c_str(),
				tag_builder.Commit());
			++total_tracks;
		}
	}

	// Add multichannel tracks
	if (disc.HasMultichannelArea() && config.playable_area != AreaId::Stereo) {
		disc.SelectArea(AreaId::Multichannel);
		const std::size_t track_count = disc.GetTrackCount(AreaId::Multichannel);

		for (std::size_t i = 0; i < track_count; ++i) {
			tag_builder.Clear();
			AddTagHandler handler(tag_builder);
			disc.GetTrackInfo(AreaId::Multichannel, i, handler);

			tail = list.emplace_after(
				tail,
				GenerateTrackName(AreaId::Multichannel, i).c_str(),
				tag_builder.Commit());
			++total_tracks;
		}
	}

	return list;
}

/**
 * Decode an SACD track.
 */
void
sacdiso_file_decode(DecoderClient& client, Path path_fs)
{
	// Get base name - MUST keep AllocatedPath alive to avoid dangling pointer
	const auto base_name = path_fs.GetBase();
	if (base_name.IsNull())
		return;

	const char* track_name = base_name.c_str();

	AreaId area_id;
	unsigned track_index;
	if (!ParseTrackName(track_name, area_id, track_index))
		return;

	// Get container path - MUST keep AllocatedPath alive
	const auto container_path = path_fs.GetDirectoryName();
	if (container_path.IsNull())
		return;

	auto media = std::make_unique<FileMedia>();
	if (!media->Open(container_path.c_str()))
		return;

	Disc disc;
	if (!disc.Open(std::move(media)))
		return;

	disc.SetEditedMasterMode(config.edited_master);

	// Select area and track
	disc.SelectArea(area_id);
	if (!disc.SelectTrack(track_index))
		return;

	// Set up audio format
	const unsigned channels = disc.GetChannelCount(area_id);
	const unsigned sample_rate = Disc::GetSampleRate();

	AudioFormat audio_format;
	try {
		audio_format = CheckAudioFormat(sample_rate / 8,
		                                 SampleFormat::DSD,
		                                 channels);
	} catch (...) {
		FmtError(sacdiso_domain, "Invalid audio format: {} channels at {} Hz",
		         channels, sample_rate);
		return;
	}

	// Initialize DST decoder if needed
	DstDecoder dst_decoder;
	const bool dst_encoded = disc.IsDstEncoded(area_id);

	if (dst_encoded) {
		if (!dst_decoder.Initialize(channels, sample_rate))
			return;
	}

	// Calculate duration and signal ready
	const auto duration = SongTime::FromMS(
		static_cast<unsigned>(disc.GetDuration() * 1000));
	client.Ready(audio_format, true, duration);

	// Playback loop
	const unsigned kbit_rate = channels * sample_rate / 1000;
	std::vector<std::byte> frame_buffer(64 * 1024);
	std::vector<std::byte> dsd_buffer;

	// DSD frame size is channels bytes (1 byte per channel)
	const std::size_t dsd_frame_size = channels;

	DecoderCommand cmd = DecoderCommand::NONE;

	while (cmd != DecoderCommand::STOP) {
		// Handle seek
		if (cmd == DecoderCommand::SEEK) {
			const double seek_time = client.GetSeekTime().ToDoubleS();
			if (disc.Seek(seek_time)) {
				client.CommandFinished();
				if (dst_encoded)
					dst_decoder.Reset();
			} else {
				client.SeekError();
			}
			cmd = client.GetCommand();
			continue;
		}

		// Read frame
		std::size_t frame_size = frame_buffer.size();
		FrameType frame_type;

		if (!disc.ReadFrame(frame_buffer, frame_size, frame_type))
			break;  // End of track

		if (frame_type == FrameType::Invalid) {
			// Skip invalid frames
			cmd = client.GetCommand();
			continue;
		}

		// Get DSD data
		std::span<std::byte> dsd_data;

		if (frame_type == FrameType::Dst) {
			// Decode DST to DSD
			if (!dst_decoder.Decode(
				std::span{frame_buffer.data(), frame_size}, dsd_buffer)) {
				// DST decode failed - output DSD silence instead
				const std::size_t expected_size = 
					dst_decoder.GetOutputFrameSize();
				if (expected_size > 0) {
					dsd_buffer.resize(expected_size);
					std::fill(dsd_buffer.begin(), dsd_buffer.end(), 
					          std::byte{0xAA});
					dsd_data = dsd_buffer;
				} else {
					cmd = client.GetCommand();
					continue;
				}
			} else if (dsd_buffer.empty()) {
				// Decoder needs more data
				cmd = client.GetCommand();
				continue;
			} else {
				dsd_data = dsd_buffer;
			}
		} else {
			// Raw DSD
			dsd_data = std::span{frame_buffer.data(), frame_size};
		}

		// Ensure frame alignment - buffer size must be multiple of frame size
		// For DSD, frame size = channels bytes (1 byte per channel)
		std::size_t aligned_size = dsd_data.size();
		if (aligned_size % dsd_frame_size != 0) {
			aligned_size = (aligned_size / dsd_frame_size) * dsd_frame_size;
			if (aligned_size == 0) {
				// Not enough data for a complete frame
				cmd = client.GetCommand();
				continue;
			}
			dsd_data = dsd_data.first(aligned_size);
		}

		// Apply bit reversal if needed
		if (config.lsbitfirst)
			BitReverseBuffer(dsd_data.data(), dsd_data.size());

		// Submit to client
		cmd = client.SubmitAudio(nullptr, dsd_data, kbit_rate);
	}
}

/**
 * Scan a track for metadata.
 */
bool
sacdiso_scan_file(Path path_fs, TagHandler& handler) noexcept
{
	// Get base name - MUST keep AllocatedPath alive to avoid dangling pointer
	const auto base_name = path_fs.GetBase();
	if (base_name.IsNull())
		return false;

	const char* track_name = base_name.c_str();

	AreaId area_id;
	unsigned track_index;
	if (!ParseTrackName(track_name, area_id, track_index))
		return false;

	// Get container path - MUST keep AllocatedPath alive
	const auto container_path = path_fs.GetDirectoryName();
	if (container_path.IsNull())
		return false;

	auto media = std::make_unique<FileMedia>();
	if (!media->Open(container_path.c_str()))
		return false;

	Disc disc;
	if (!disc.Open(std::move(media)))
		return false;

	disc.SelectArea(area_id);
	disc.GetTrackInfo(area_id, track_index, handler);

	return true;
}

/*
 * Supported suffixes:
 * - iso: Container files (SACD ISO images) and virtual tracks
 * - dat: Container files
 */
constexpr const char* sacdiso_suffixes[] = {
	"iso",
	"dat",
	nullptr,
};

} // anonymous namespace

const DecoderPlugin sacdiso_decoder_plugin =
	DecoderPlugin("sacdiso", sacdiso_file_decode, sacdiso_scan_file)
	.WithInit(sacdiso_init)
	.WithContainer(sacdiso_container_scan)
	.WithSuffixes(sacdiso_suffixes);
