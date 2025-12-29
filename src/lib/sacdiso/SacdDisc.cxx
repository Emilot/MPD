// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "SacdDisc.hxx"
#include "tag/Handler.hxx"
#include "tag/Type.hxx"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

namespace Sacd {

namespace {

// Audio sector header size
constexpr std::size_t kAudioSectorHeaderSize = 1;  // Only 1 byte!
constexpr std::size_t kAudioPacketInfoSize = 2;
constexpr std::size_t kAudioFrameInfoSizeDst = 4;  // For DST: timecode + channel byte
constexpr std::size_t kAudioFrameInfoSizeDsd = 3;  // For DSD: only timecode

// Character set names for iconv
constexpr const char* kCharsetNames[] = {
	"ISO-8859-1",  // Iso646 (fallback to Latin-1)
	"ISO-8859-1",  // Iso8859_1
	"EUC-KR",      // Korean
	"GB2312",      // Simplified Chinese
	"BIG5",        // Traditional Chinese
	"SHIFT_JIS",   // Japanese
	"KOI8-R",      // Russian
	"ISO-8859-1",  // Iso8859_1_2
};

} // anonymous namespace

Disc::~Disc() noexcept
{
	Close();
}

bool
Disc::Open(std::unique_ptr<Media> media)
{
	Close();

	if (!media || !media->IsValid())
		return false;

	// Detect sector size
	sector_size_ = DetectSectorSize(*media);
	if (sector_size_ == 0)
		return false;

	// Set sector offset for PSN format
	sector_offset_ = (sector_size_ == kPsnSize) ? 12 : 0;

	media_ = std::move(media);

	// Read Master TOC
	if (!ReadMasterToc()) {
		Close();
		return false;
	}

	return true;
}

void
Disc::Close() noexcept
{
	media_.reset();
	sector_size_ = 0;
	sector_offset_ = 0;
	disc_info_ = DiscInfo{};
	current_area_ = AreaId::Stereo;
	current_track_ = 0;
	track_start_lsn_ = 0;
	track_length_lsn_ = 0;
	current_lsn_ = 0;
	audio_state_.reset();
	frame_state_.reset();
}

std::size_t
Disc::GetTrackCount(AreaId area_id) const noexcept
{
	return disc_info_.GetArea(area_id).GetTrackCount();
}

std::size_t
Disc::GetTotalTrackCount() const noexcept
{
	return GetTrackCount(AreaId::Stereo) + GetTrackCount(AreaId::Multichannel);
}

double
Disc::GetTrackDuration(AreaId area_id, std::size_t track_index) const noexcept
{
	const auto& area = disc_info_.GetArea(area_id);
	if (track_index >= area.tracks.size())
		return 0.0;
	return area.tracks[track_index].duration.ToSeconds();
}

unsigned
Disc::GetChannelCount(AreaId area_id) const noexcept
{
	return disc_info_.GetArea(area_id).channel_count;
}

bool
Disc::IsDstEncoded(AreaId area_id) const noexcept
{
	return disc_info_.GetArea(area_id).IsDstEncoded();
}

void
Disc::GetTrackInfo(AreaId area_id, std::size_t track_index,
                   TagHandler& handler) const noexcept
{
	const auto& area = disc_info_.GetArea(area_id);
	if (track_index >= area.tracks.size())
		return;

	const auto& track = area.tracks[track_index];
	const auto& master = disc_info_.master_text;

	// Track number
	handler.OnTag(TAG_TRACK, std::to_string(track_index + 1).c_str());

	// Duration
	handler.OnDuration(SongTime::FromMS(
		static_cast<unsigned>(track.duration.ToSeconds() * 1000)));

	// Album info
	if (!master.album_title.empty()) {
		std::string album = master.album_title;
		album += " (";
		album += (area_id == AreaId::Stereo) ? "2CH" : "MCH";
		album += "-";
		album += area.IsDstEncoded() ? "DST" : "DSD";
		album += ")";
		handler.OnTag(TAG_ALBUM, album.c_str());
	}

	if (!master.album_artist.empty())
		handler.OnTag(TAG_ARTIST, master.album_artist.c_str());

	// Track info
	if (!track.text.title.empty())
		handler.OnTag(TAG_TITLE, track.text.title.c_str());

	if (!track.text.performer.empty())
		handler.OnTag(TAG_PERFORMER, track.text.performer.c_str());

	if (!track.text.composer.empty())
		handler.OnTag(TAG_COMPOSER, track.text.composer.c_str());

	// Date
	if (disc_info_.disc_year > 0)
		handler.OnTag(TAG_DATE, std::to_string(static_cast<unsigned>(disc_info_.disc_year)).c_str());

	// Disc number
	if (disc_info_.album_set_size > 1 && disc_info_.album_sequence_number > 0)
		handler.OnTag(TAG_DISC, 
		              std::to_string(static_cast<unsigned>(disc_info_.album_sequence_number)).c_str());
}

void
Disc::SelectArea(AreaId area_id) noexcept
{
	current_area_ = area_id;
}

bool
Disc::SelectTrack(std::size_t track_index, uint32_t offset) noexcept
{
	const auto& area = disc_info_.GetArea(current_area_);
	if (track_index >= area.tracks.size())
		return false;

	current_track_ = track_index;
	const auto& track = area.tracks[track_index];

	if (!edited_master_mode_) {
		track_start_lsn_ = track.start_lsn;
		track_length_lsn_ = track.length_lsn;
	} else {
		// Edited master mode: use next track's start as end
		if (track_index > 0) {
			track_start_lsn_ = track.start_lsn;
		} else {
			track_start_lsn_ = area.track_start;
		}

		if (track_index < area.tracks.size() - 1) {
			track_length_lsn_ = area.tracks[track_index + 1].start_lsn 
			                    - track_start_lsn_ + 1;
		} else {
			track_length_lsn_ = area.track_end - track_start_lsn_;
		}
	}

	current_lsn_ = track_start_lsn_ + offset;

	// Reset audio state - allocate on heap
	audio_state_ = std::make_unique<AudioSectorState>();
	frame_state_ = std::make_unique<FrameState>();

	// Seek to start position
	return media_->Seek(static_cast<uint64_t>(current_lsn_) * sector_size_);
}

bool
Disc::ReadFrame(std::span<std::byte> buffer, std::size_t& frame_size,
                FrameType& frame_type) noexcept
{
	// Ensure state is allocated
	if (!audio_state_ || !frame_state_) {
		frame_type = FrameType::Invalid;
		return false;
	}

	while (current_lsn_ < track_start_lsn_ + track_length_lsn_) {
		// Need to read a new sector?
		// Limit check to actual parsed packets (max 8)
		const uint8_t effective_packet_count = std::min(audio_state_->packet_count,
		                                                 static_cast<uint8_t>(audio_state_->packets.size()));
		if (audio_state_->current_packet >= effective_packet_count) {
			// Read next sector
			if (!ReadRawSector(current_lsn_, audio_state_->sector_buffer))
				return false;

			++current_lsn_;

			// Parse audio sector header
			const std::byte* data = audio_state_->sector_buffer.data() + sector_offset_;
			const auto* header = reinterpret_cast<const AudioSectorHeader*>(data);

			audio_state_->dst_encoded = header->IsDstEncoded();
			audio_state_->packet_count = header->GetPacketInfoCount();
			audio_state_->frame_count = header->GetFrameInfoCount();
			audio_state_->current_packet = 0;
			audio_state_->buffer_offset = kAudioSectorHeaderSize;

			// Parse packet info FIRST
			for (uint8_t i = 0; i < audio_state_->packet_count && i < 8; ++i) {
				std::memcpy(&audio_state_->packets[i],
				            data + audio_state_->buffer_offset,
				            kAudioPacketInfoSize);
				audio_state_->buffer_offset += kAudioPacketInfoSize;
			}

			// Parse frame info
			const std::size_t frame_info_size = audio_state_->dst_encoded 
				? kAudioFrameInfoSizeDst : kAudioFrameInfoSizeDsd;
			for (uint8_t i = 0; i < audio_state_->frame_count && i < 8; ++i) {
				std::memcpy(&audio_state_->frames[i],
				            data + audio_state_->buffer_offset,
				            frame_info_size);
				audio_state_->buffer_offset += frame_info_size;
			}
		}

		// Process packets (limit to array size to prevent out-of-bounds access)
		const uint8_t max_packets = std::min(audio_state_->packet_count,
		                                     static_cast<uint8_t>(audio_state_->packets.size()));
		while (audio_state_->current_packet < max_packets) {
			const auto& packet = audio_state_->packets[audio_state_->current_packet];
			const std::byte* sector_data = audio_state_->sector_buffer.data() 
			                               + sector_offset_;

			switch (packet.GetDataType()) {
			case DataType::Audio:
				if (frame_state_->started) {
					if (packet.IsFrameStart()) {
						// Complete frame ready
						if (frame_state_->size <= buffer.size()) {
							std::memcpy(buffer.data(), 
							            frame_state_->data.data(),
							            frame_state_->size);
							frame_size = frame_state_->size;
							frame_type = frame_state_->dst_encoded 
								? FrameType::Dst : FrameType::Dsd;
							frame_state_->started = false;
							return true;
						}
						// Buffer too small
						frame_state_->started = false;
						frame_type = FrameType::Invalid;
						return true;
					}
				} else {
					if (packet.IsFrameStart()) {
						frame_state_->size = 0;
						frame_state_->dst_encoded = audio_state_->dst_encoded;
						frame_state_->started = true;
					}
				}

				if (frame_state_->started) {
					const uint16_t packet_len = packet.GetPacketLength();
					if (frame_state_->size + packet_len <= FrameState::kMaxFrameSize &&
					    audio_state_->buffer_offset + packet_len <= kLsnSize) {
						std::memcpy(frame_state_->data.data() + frame_state_->size,
						            sector_data + audio_state_->buffer_offset,
						            packet_len);
						frame_state_->size += packet_len;
					}
				}
				break;

			case DataType::Supplementary:
			case DataType::Padding:
				// Skip these packet types
				break;
			}

			audio_state_->buffer_offset += packet.GetPacketLength();
			++audio_state_->current_packet;
		}
	}

	// End of track - return any pending frame
	if (frame_state_->started && frame_state_->size <= buffer.size()) {
		std::memcpy(buffer.data(), frame_state_->data.data(), frame_state_->size);
		frame_size = frame_state_->size;
		frame_type = frame_state_->dst_encoded ? FrameType::Dst : FrameType::Dsd;
		frame_state_->started = false;
		return true;
	}

	frame_type = FrameType::Invalid;
	return false;
}

bool
Disc::Seek(double seconds) noexcept
{
	const uint64_t size = GetSize();
	const double duration = GetDuration();
	if (duration <= 0)
		return false;

	const uint64_t offset = static_cast<uint64_t>(size * seconds / duration);
	const uint32_t lsn_offset = static_cast<uint32_t>(offset / sector_size_);

	return SelectTrack(current_track_, lsn_offset);
}

uint64_t
Disc::GetPosition() const noexcept
{
	return static_cast<uint64_t>(current_lsn_ - track_start_lsn_) * sector_size_;
}

uint64_t
Disc::GetSize() const noexcept
{
	return static_cast<uint64_t>(track_length_lsn_) * sector_size_;
}

double
Disc::GetDuration() const noexcept
{
	return GetTrackDuration(current_area_, current_track_);
}

bool
Disc::ReadMasterToc()
{
	// Allocate buffer for Master TOC
	std::vector<std::byte> master_data(kMasterTocLength * kLsnSize);

	if (!ReadRawSectors(kMasterTocStart, kMasterTocLength, master_data))
		return false;

	// Parse Master TOC header
	const auto* master_toc = reinterpret_cast<const MasterToc*>(master_data.data());

	if (!master_toc->IsValid())
		return false;

	// Check version
	if (master_toc->version.major > kSupportedVersionMajor ||
	    (master_toc->version.major == kSupportedVersionMajor && 
	     master_toc->version.minor > kSupportedVersionMinor))
		return false;

	// Store disc info
	disc_info_.version = master_toc->version;
	disc_info_.album_set_size = static_cast<uint16_t>(master_toc->album_set_size);
	disc_info_.album_sequence_number = static_cast<uint16_t>(master_toc->album_sequence_number);
	disc_info_.disc_year = static_cast<uint16_t>(master_toc->disc_date_year);
	disc_info_.disc_month = master_toc->disc_date_month;
	disc_info_.disc_day = master_toc->disc_date_day;
	disc_info_.is_hybrid = master_toc->disc_type_hybrid != 0;

	// Parse Master Text (first language only)
	const auto* master_text = reinterpret_cast<const MasterText*>(
		master_data.data() + kLsnSize);
	if (master_text->IsValid()) {
		const auto charset = static_cast<CharacterSet>(
			master_toc->locales[0].character_set & 0x07);
		const char* text_base = reinterpret_cast<const char*>(master_text);

		auto extract_text = [&](uint16_t offset) -> std::string {
			if (offset == 0)
				return {};
			const char* str = text_base + offset;
			return ConvertCharset(str, std::strlen(str), charset);
		};

		disc_info_.master_text.album_title = extract_text(
			static_cast<uint16_t>(master_text->album_title_position));
		disc_info_.master_text.album_artist = extract_text(
			static_cast<uint16_t>(master_text->album_artist_position));
		disc_info_.master_text.album_publisher = extract_text(
			static_cast<uint16_t>(master_text->album_publisher_position));
		disc_info_.master_text.album_copyright = extract_text(
			static_cast<uint16_t>(master_text->album_copyright_position));
		disc_info_.master_text.disc_title = extract_text(
			static_cast<uint16_t>(master_text->disc_title_position));
		disc_info_.master_text.disc_artist = extract_text(
			static_cast<uint16_t>(master_text->disc_artist_position));
	}

	// Read Area TOCs
	const uint32_t area1_start = static_cast<uint32_t>(master_toc->area_1_toc_1_start);
	const uint16_t area1_size = static_cast<uint16_t>(master_toc->area_1_toc_size);
	
	if (area1_start != 0 && area1_size != 0)
		ReadAreaToc(AreaId::Stereo, area1_start, area1_size);

	const uint32_t area2_start = static_cast<uint32_t>(master_toc->area_2_toc_1_start);
	const uint16_t area2_size = static_cast<uint16_t>(master_toc->area_2_toc_size);
	
	if (area2_start != 0 && area2_size != 0)
		ReadAreaToc(AreaId::Multichannel, area2_start, area2_size);

	return disc_info_.HasStereoArea() || disc_info_.HasMultichannelArea();
}

bool
Disc::ReadAreaToc([[maybe_unused]] AreaId area_id, uint32_t toc_start, uint16_t toc_size)
{
	std::vector<std::byte> area_data(static_cast<std::size_t>(toc_size) * kLsnSize);

	if (!ReadRawSectors(toc_start, toc_size, area_data))
		return false;

	const auto* area_toc = reinterpret_cast<const AreaToc*>(area_data.data());
	if (!area_toc->IsValid())
		return false;

	// Determine which area this is
	AreaId actual_area_id = area_toc->IsTwoChannel() 
		? AreaId::Stereo : AreaId::Multichannel;

	auto& area = disc_info_.GetArea(actual_area_id);
	area.id = actual_area_id;
	area.channel_count = area_toc->channel_count;
	area.loudspeaker_config = static_cast<LoudspeakerConfig>(area_toc->loudspeaker_config);
	area.frame_format = area_toc->IsDstEncoded() ? FrameFormat::Dst : FrameFormat::Dsd_3_in_16;
	area.track_start = static_cast<uint32_t>(area_toc->track_start);
	area.track_end = static_cast<uint32_t>(area_toc->track_end);

	// Parse track list and text
	ReadTrackList(area, area_data.data(), area_data.size());
	ReadTrackText(area, area_data.data(), area_data.size());

	return true;
}

bool
Disc::ReadTrackList(AreaInfo& area, const std::byte* area_data, std::size_t area_size)
{
	const auto* area_toc = reinterpret_cast<const AreaToc*>(area_data);
	const uint8_t track_count = area_toc->track_count;

	if (track_count == 0)
		return false;

	// Find SACDTRL1 (track offsets)
	const std::byte* ptr = area_data + kLsnSize;
	const std::byte* end = area_data + area_size;

	while (ptr + kLsnSize <= end) {
		const auto* trl1 = reinterpret_cast<const TrackListOffset*>(ptr);

		if (trl1->IsValid()) {
			// SACDTRL1 layout:
			//   offset 0-7:    "SACDTRL1" signature (8 bytes)
			//   offset 8-1027: track_start_lsn[255] (255 * 4 = 1020 bytes)
			//   offset 1028-2047: track_length_lsn[255] (255 * 4 = 1020 bytes)
			const auto* start_lsn_array = reinterpret_cast<const PackedBE32*>(ptr + 8);
			const auto* length_lsn_array = reinterpret_cast<const PackedBE32*>(ptr + 8 + 255 * 4);

			area.tracks.resize(track_count);
			for (uint8_t i = 0; i < track_count; ++i) {
				area.tracks[i].start_lsn = static_cast<uint32_t>(start_lsn_array[i]);
				area.tracks[i].length_lsn = static_cast<uint32_t>(length_lsn_array[i]);
			}
			break;
		}
		ptr += kLsnSize;
	}

	// Find SACDTRL2 (track times)
	ptr = area_data + kLsnSize;
	while (ptr + kLsnSize <= end) {
		const auto* trl2 = reinterpret_cast<const TrackListTime*>(ptr);
		if (trl2->IsValid()) {
			// SACDTRL2 layout:
			//   offset 0-7:    "SACDTRL2" signature (8 bytes)
			//   offset 8-1027: start[255] - start times (255 * 4 = 1020 bytes)
			//   offset 1028-2047: duration[255] - durations (255 * 4 = 1020 bytes)
			const auto* duration_array = reinterpret_cast<const TrackTimeDuration*>(ptr + 8 + 255 * 4);

			for (uint8_t i = 0; i < track_count && i < area.tracks.size(); ++i) {
				area.tracks[i].duration.minutes = duration_array[i].minutes;
				area.tracks[i].duration.seconds = duration_array[i].seconds;
				area.tracks[i].duration.frames = duration_array[i].frames;
			}
			break;
		}
		ptr += kLsnSize;
	}

	return !area.tracks.empty();
}

bool
Disc::ReadTrackText(AreaInfo& area, const std::byte* area_data, std::size_t area_size)
{
	const auto* area_toc = reinterpret_cast<const AreaToc*>(area_data);
	const uint8_t track_count = area_toc->track_count;

	if (track_count == 0 || area.tracks.empty())
		return false;

	// Get character set from first language entry
	const auto charset = static_cast<CharacterSet>(
		area_toc->languages[0].character_set & 0x07);

	// Find SACDTTxt block by scanning area data
	const std::byte* ptr = area_data + kLsnSize;
	const std::byte* end = area_data + area_size;
	bool found_text = false;

	while (ptr + kLsnSize <= end) {
		const auto* area_text = reinterpret_cast<const AreaText*>(ptr);

		if (area_text->IsValid()) {
			found_text = true;

			// SACDTTxt layout:
			// offset 0-7: "SACDTTxt" signature
			// offset 8+: track_text_position[track_count] - uint16_t big-endian
			const auto* positions = reinterpret_cast<const PackedBE16*>(ptr + 8);

			// Calculate remaining bytes from SACDTTxt position to end of area_data
			const std::size_t sacdttxt_offset = static_cast<std::size_t>(ptr - area_data);
			const std::size_t remaining_bytes = area_size - sacdttxt_offset;

			// Parse text for each track
			for (uint8_t i = 0; i < track_count && i < area.tracks.size(); ++i) {
				const uint16_t text_pos = static_cast<uint16_t>(positions[i]);

				if (text_pos == 0 || text_pos >= remaining_bytes)
					continue;

				// Track text data layout:
				// offset +0: track_amount (uint8_t) - number of text entries
				// offset +1 to +3: reserved (skip 3 bytes)
				// offset +4: first text entry
				// Each entry: track_type (uint8_t), unknown byte (0x20), null-terminated string
				const char* track_ptr = reinterpret_cast<const char*>(ptr) + text_pos;
				const uint8_t track_amount = static_cast<uint8_t>(*track_ptr);

				if (track_amount == 0)
					continue;

				// Skip to first entry (track_amount + 3 reserved bytes = 4 bytes)
				track_ptr += 4;

				// Calculate end of readable data for this track
				const char* data_end = reinterpret_cast<const char*>(ptr) + remaining_bytes;

				// Parse each text entry for this track
				for (uint8_t j = 0; j < track_amount; ++j) {
					if (track_ptr >= data_end)
						break;

					const auto track_type = static_cast<TrackTextType>(
						static_cast<uint8_t>(*track_ptr));
					track_ptr++;  // skip track_type
					track_ptr++;  // skip unknown byte (0x20)

					// Read null-terminated string
					if (track_ptr < data_end && *track_ptr != '\0') {
						const std::size_t max_len = static_cast<std::size_t>(data_end - track_ptr);
						const std::size_t str_len = std::min(std::strlen(track_ptr), max_len);
						std::string text = ConvertCharset(track_ptr, str_len, charset);

						// Store in appropriate field
						switch (track_type) {
						case TrackTextType::Title:
							area.tracks[i].text.title = std::move(text);
							break;
						case TrackTextType::Performer:
							area.tracks[i].text.performer = std::move(text);
							break;
						case TrackTextType::Songwriter:
							area.tracks[i].text.songwriter = std::move(text);
							break;
						case TrackTextType::Composer:
							area.tracks[i].text.composer = std::move(text);
							break;
						case TrackTextType::Arranger:
							area.tracks[i].text.arranger = std::move(text);
							break;
						case TrackTextType::Message:
							area.tracks[i].text.message = std::move(text);
							break;
						default:
							break;
						}
					}

					// Move to next entry (skip past null terminator)
					if (j < track_amount - 1) {
						while (track_ptr < data_end && *track_ptr != '\0')
							track_ptr++;
						while (track_ptr < data_end && *track_ptr == '\0')
							track_ptr++;
					}
				}
			}

			// Only process first SACDTTxt block (first language)
			break;
		}

		ptr += kLsnSize;
	}

	return found_text;
}

std::string
Disc::ConvertCharset(const char* data, std::size_t length, CharacterSet charset)
{
#ifdef HAVE_ICONV
	const unsigned charset_idx = static_cast<unsigned>(charset);
	if (charset_idx >= std::size(kCharsetNames))
		return std::string(data, length);

	iconv_t conv = iconv_open("UTF-8", kCharsetNames[charset_idx]);
	if (conv == reinterpret_cast<iconv_t>(-1))
		return std::string(data, length);

	// Allocate output buffer (UTF-8 can be up to 3x the input size)
	std::string result;
	result.resize(length * 3);

	char* inbuf = const_cast<char*>(data);
	std::size_t inbytes = length;
	char* outbuf = result.data();
	std::size_t outbytes = result.size();

	if (iconv(conv, &inbuf, &inbytes, &outbuf, &outbytes) != static_cast<std::size_t>(-1)) {
		result.resize(result.size() - outbytes);
	} else {
		result.assign(data, length);
	}

	iconv_close(conv);
	return result;
#else
	// No iconv - assume UTF-8 or Latin-1
	(void)charset;
	return std::string(data, length);
#endif
}

bool
Disc::ReadRawSector(uint32_t lsn, std::span<std::byte> buffer) noexcept
{
	if (buffer.size() < sector_size_)
		return false;

	if (!media_->Seek(static_cast<uint64_t>(lsn) * sector_size_))
		return false;

	return media_->Read(buffer.first(sector_size_)) == sector_size_;
}

bool
Disc::ReadRawSectors(uint32_t lsn, uint32_t count, std::span<std::byte> buffer) noexcept
{
	// For PSN format, we need to read sector by sector and skip subchannel data
	if (sector_size_ == kPsnSize) {
		const std::size_t output_size = static_cast<std::size_t>(count) * kLsnSize;
		if (buffer.size() < output_size)
			return false;

		std::array<std::byte, kPsnSize> sector_buf;
		std::byte* out_ptr = buffer.data();

		for (uint32_t i = 0; i < count; ++i) {
			if (!media_->Seek(static_cast<uint64_t>(lsn + i) * kPsnSize))
				return false;
			if (media_->Read(sector_buf) != kPsnSize)
				return false;
			// Skip 12-byte subchannel header
			std::memcpy(out_ptr, sector_buf.data() + 12, kLsnSize);
			out_ptr += kLsnSize;
		}
		return true;
	}

	// LSN format - direct read
	const std::size_t total_size = static_cast<std::size_t>(count) * kLsnSize;
	if (buffer.size() < total_size)
		return false;

	if (!media_->Seek(static_cast<uint64_t>(lsn) * kLsnSize))
		return false;

	return media_->Read(buffer.first(total_size)) == total_size;
}

} // namespace Sacd
