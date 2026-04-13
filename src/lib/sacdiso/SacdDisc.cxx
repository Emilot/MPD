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

	if (!media || !media->IsValid()) {
		FmtWarning(sacdiso_domain, "Disc::Open - media is null or invalid");
		return false;
	}

	// Detect sector size
	sector_size_ = DetectSectorSize(*media);
	if (sector_size_ == 0) {
		FmtWarning(sacdiso_domain, "Disc::Open - DetectSectorSize failed");
		return false;
	}
	FmtWarning(sacdiso_domain, "Disc::Open - sector_size={}", sector_size_);

	// Set sector offset for PSN format
	sector_offset_ = (sector_size_ == kPsnSize) ? 12 : 0;

	media_ = std::move(media);

	// Read Master TOC
	if (!ReadMasterToc()) {
		FmtWarning(sacdiso_domain, "Disc::Open - ReadMasterToc failed");
		Close();
		return false;
	}
	FmtWarning(sacdiso_domain, "Disc::Open - ReadMasterToc OK, stereo={}, mch={}",
	           disc_info_.HasStereoArea(), disc_info_.HasMultichannelArea());

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
	if (track_index >= area.tracks.size()) {
		FmtWarning(sacdiso_domain, "SelectTrack - track {} out of range (size={})",
		           track_index, area.tracks.size());
		return false;
	}

	current_track_ = track_index;
	const auto& track = area.tracks[track_index];
	FmtWarning(sacdiso_domain, "SelectTrack - track={}, start_lsn={}, length_lsn={}",
	           track_index, track.start_lsn, track.length_lsn);

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
	static unsigned sector_log_count = 0;  // Limit logging

	// Ensure state is allocated
	if (!audio_state_ || !frame_state_) {
		FmtWarning(sacdiso_domain, "ReadFrame - state not allocated");
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
			if (!ReadRawSector(current_lsn_, audio_state_->sector_buffer)) {
				FmtWarning(sacdiso_domain, "ReadFrame - ReadRawSector failed at lsn={}", current_lsn_);
				return false;
			}

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
			
			// Log first few sectors (AFTER parsing)
			if (sector_log_count < 3) {
				FmtWarning(sacdiso_domain, "Sector header: dst={}, packets={}, frames={}, raw_byte=0x{:02x}",
				           audio_state_->dst_encoded, audio_state_->packet_count,
				           audio_state_->frame_count, static_cast<uint8_t>(data[0]));
				// Log raw bytes of first 30 bytes of sector
				FmtWarning(sacdiso_domain, "  Raw[0-9]: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
				           static_cast<uint8_t>(data[0]), static_cast<uint8_t>(data[1]),
				           static_cast<uint8_t>(data[2]), static_cast<uint8_t>(data[3]),
				           static_cast<uint8_t>(data[4]), static_cast<uint8_t>(data[5]),
				           static_cast<uint8_t>(data[6]), static_cast<uint8_t>(data[7]),
				           static_cast<uint8_t>(data[8]), static_cast<uint8_t>(data[9]));
				FmtWarning(sacdiso_domain, "  Raw[10-19]: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
				           static_cast<uint8_t>(data[10]), static_cast<uint8_t>(data[11]),
				           static_cast<uint8_t>(data[12]), static_cast<uint8_t>(data[13]),
				           static_cast<uint8_t>(data[14]), static_cast<uint8_t>(data[15]),
				           static_cast<uint8_t>(data[16]), static_cast<uint8_t>(data[17]),
				           static_cast<uint8_t>(data[18]), static_cast<uint8_t>(data[19]));
				// Log packet details with raw bytes
				for (uint8_t i = 0; i < audio_state_->packet_count && i < 3; ++i) {
					const auto& pkt = audio_state_->packets[i];
					FmtWarning(sacdiso_domain, "  Pkt[{}]: raw=0x{:02x}{:02x} fs={} dt={} len={}",
					           i, pkt.byte0, pkt.byte1,
					           pkt.IsFrameStart(), static_cast<int>(pkt.GetDataType()), pkt.GetPacketLength());
				}
				FmtWarning(sacdiso_domain, "  buffer_offset after header parse={}", audio_state_->buffer_offset);
				++sector_log_count;
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
							// Log first frame
							static bool logged_first_frame = false;
							if (!logged_first_frame) {
								FmtWarning(sacdiso_domain, "First frame complete: size={}, type={}",
								           frame_size, frame_state_->dst_encoded ? "DST" : "DSD");
								logged_first_frame = true;
							}
							frame_state_->started = false;
							return true;
						}
						// Buffer too small
						FmtWarning(sacdiso_domain, "Frame buffer too small: frame={}, buffer={}",
						           frame_state_->size, buffer.size());
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

	FmtWarning(sacdiso_domain, "ReadMasterToc: reading {} sectors from LSN {}",
	           kMasterTocLength, kMasterTocStart);

	if (!ReadRawSectors(kMasterTocStart, kMasterTocLength, master_data)) {
		FmtWarning(sacdiso_domain, "ReadMasterToc: ReadRawSectors failed");
		return false;
	}

	// Parse Master TOC header
	const auto* master_toc = reinterpret_cast<const MasterToc*>(master_data.data());
	
	// Log raw signature bytes
	FmtWarning(sacdiso_domain, "ReadMasterToc: signature bytes: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
	           static_cast<uint8_t>(master_data[0]), static_cast<uint8_t>(master_data[1]),
	           static_cast<uint8_t>(master_data[2]), static_cast<uint8_t>(master_data[3]),
	           static_cast<uint8_t>(master_data[4]), static_cast<uint8_t>(master_data[5]),
	           static_cast<uint8_t>(master_data[6]), static_cast<uint8_t>(master_data[7]));

	if (!master_toc->IsValid()) {
		FmtWarning(sacdiso_domain, "ReadMasterToc: Invalid signature (expected SACDMTOC)");
		return false;
	}

	// Check version - FIXED: check major first, then minor only if major is equal
	FmtWarning(sacdiso_domain, "ReadMasterToc: version={}.{} (supported={}.{})",
	           master_toc->version.major, master_toc->version.minor,
	           kSupportedVersionMajor, kSupportedVersionMinor);
	
	if (master_toc->version.major > kSupportedVersionMajor ||
	    (master_toc->version.major == kSupportedVersionMajor && 
	     master_toc->version.minor > kSupportedVersionMinor)) {
		FmtWarning(sacdiso_domain, "ReadMasterToc: Unsupported SACD version");
		return false;
	}

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
	FmtWarning(sacdiso_domain, "ReadMasterToc: area1_start={}, area1_size={}", 
	           area1_start, area1_size);
	
	if (area1_start != 0 && area1_size != 0) {
		if (!ReadAreaToc(AreaId::Stereo, area1_start, area1_size)) {
			FmtWarning(sacdiso_domain, "ReadMasterToc: ReadAreaToc(stereo) failed");
		}
	}

	const uint32_t area2_start = static_cast<uint32_t>(master_toc->area_2_toc_1_start);
	const uint16_t area2_size = static_cast<uint16_t>(master_toc->area_2_toc_size);
	FmtWarning(sacdiso_domain, "ReadMasterToc: area2_start={}, area2_size={}", 
	           area2_start, area2_size);
	
	if (area2_start != 0 && area2_size != 0) {
		if (!ReadAreaToc(AreaId::Multichannel, area2_start, area2_size)) {
			FmtWarning(sacdiso_domain, "ReadMasterToc: ReadAreaToc(mch) failed");
		}
	}

	FmtWarning(sacdiso_domain, "ReadMasterToc: HasStereo={}, HasMch={}",
	           disc_info_.HasStereoArea(), disc_info_.HasMultichannelArea());

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

	FmtWarning(sacdiso_domain, "ReadTrackList: track_count={}, area_size={}", 
	           track_count, area_size);

	if (track_count == 0)
		return false;

	// Find SACDTRL1 (track offsets)
	const std::byte* ptr = area_data + kLsnSize;
	const std::byte* end = area_data + area_size;
	bool found_trl1 = false;
	unsigned sector_num = 1;

	while (ptr + kLsnSize <= end) {
		const auto* trl1 = reinterpret_cast<const TrackListOffset*>(ptr);
		
		// Log first 8 bytes of each sector to find signatures
		if (sector_num <= 10) {
			FmtWarning(sacdiso_domain, "ReadTrackList: sector {} sig: {:c}{:c}{:c}{:c}{:c}{:c}{:c}{:c}",
			           sector_num,
			           static_cast<char>(ptr[0]), static_cast<char>(ptr[1]),
			           static_cast<char>(ptr[2]), static_cast<char>(ptr[3]),
			           static_cast<char>(ptr[4]), static_cast<char>(ptr[5]),
			           static_cast<char>(ptr[6]), static_cast<char>(ptr[7]));
		}
		
		if (trl1->IsValid()) {
			FmtWarning(sacdiso_domain, "ReadTrackList: Found SACDTRL1 at sector {}", sector_num);
			found_trl1 = true;
			
			// SACDTRL1 layout (from scarletbook.h):
			//   offset 0-7:    "SACDTRL1" signature (8 bytes)
			//   offset 8-1027: track_start_lsn[255] - ALL start LSNs first (255 * 4 = 1020 bytes)
			//   offset 1028-2047: track_length_lsn[255] - ALL length LSNs after (255 * 4 = 1020 bytes)
			// NOT interleaved!
			const auto* start_lsn_array = reinterpret_cast<const PackedBE32*>(ptr + 8);
			const auto* length_lsn_array = reinterpret_cast<const PackedBE32*>(ptr + 8 + 255 * 4);
			
			area.tracks.resize(track_count);
			for (uint8_t i = 0; i < track_count; ++i) {
				area.tracks[i].start_lsn = static_cast<uint32_t>(start_lsn_array[i]);
				area.tracks[i].length_lsn = static_cast<uint32_t>(length_lsn_array[i]);
				FmtWarning(sacdiso_domain, "ReadTrackList: track[{}] start={}, length={}",
				           i, area.tracks[i].start_lsn, area.tracks[i].length_lsn);
			}
			break;
		}
		ptr += kLsnSize;
		++sector_num;
	}

	if (!found_trl1) {
		FmtWarning(sacdiso_domain, "ReadTrackList: SACDTRL1 NOT FOUND after {} sectors!", sector_num);
	}

	// Find SACDTRL2 (track times)
	ptr = area_data + kLsnSize;
	sector_num = 1;
	while (ptr + kLsnSize <= end) {
		const auto* trl2 = reinterpret_cast<const TrackListTime*>(ptr);
		if (trl2->IsValid()) {
			FmtWarning(sacdiso_domain, "ReadTrackList: Found SACDTRL2 at sector {}", sector_num);
			
			// SACDTRL2 layout (from scarletbook.h):
			//   offset 0-7:    "SACDTRL2" signature (8 bytes)
			//   offset 8-1027: start[255] - start times (255 * 4 = 1020 bytes)
			//   offset 1028-2047: duration[255] - durations (255 * 4 = 1020 bytes)
			// TrackTimeDuration is 4 bytes (minutes, seconds, frames, flags)
			const auto* duration_array = reinterpret_cast<const TrackTimeDuration*>(ptr + 8 + 255 * 4);

			for (uint8_t i = 0; i < track_count && i < area.tracks.size(); ++i) {
				// Copy from on-disc format to runtime format
				area.tracks[i].duration.minutes = duration_array[i].minutes;
				area.tracks[i].duration.seconds = duration_array[i].seconds;
				area.tracks[i].duration.frames = duration_array[i].frames;
				FmtWarning(sacdiso_domain, "ReadTrackList: track[{}] duration={}:{:02d}.{:02d}",
				           i, area.tracks[i].duration.minutes, 
				           area.tracks[i].duration.seconds,
				           area.tracks[i].duration.frames);
			}
			break;
		}
		ptr += kLsnSize;
		++sector_num;
	}

	FmtWarning(sacdiso_domain, "ReadTrackList: returning with {} tracks", area.tracks.size());
	return !area.tracks.empty();
}

bool
Disc::ReadTrackText(AreaInfo& area, const std::byte* area_data, std::size_t area_size)
{
	const auto* area_toc = reinterpret_cast<const AreaToc*>(area_data);
	const uint8_t track_count = area_toc->track_count;

	if (track_count == 0 || area.tracks.empty())
		return false;

	// Get character set from first locale
	const auto charset = static_cast<CharacterSet>(
		area_toc->languages[0].character_set & 0x07);

	// Search for SACDTTxt signature in area data
	const std::byte* ptr = area_data + kLsnSize;  // Skip first sector (AreaToc header)
	const std::byte* end = area_data + area_size;
	bool found_text = false;
	unsigned sector_num = 1;

	while (ptr + 8 <= end) {
		const auto* text_header = reinterpret_cast<const TrackTextHeader*>(ptr);
		if (text_header->IsValid()) {
			FmtWarning(sacdiso_domain, "ReadTrackText: Found SACDTTxt at sector {}",
			           sector_num);
			found_text = true;

			// Calculate total bytes available from SACDTTxt start to end of area data
			const std::size_t remaining_bytes =
				static_cast<std::size_t>(end - ptr);

			// Position array starts at offset 8 (after signature)
			const auto* positions = reinterpret_cast<const PackedBE16*>(ptr + 8);

			FmtWarning(sacdiso_domain,
			           "ReadTrackText: remaining_bytes={}, track_count={}",
			           remaining_bytes, track_count);

			// Parse text for each track
			for (uint8_t i = 0; i < track_count && i < area.tracks.size(); ++i) {
				const uint16_t text_pos = static_cast<uint16_t>(positions[i]);

				// Validate position within entire SACDTTxt block
				// (may span multiple sectors, so positions >= 2048 are valid)
				if (text_pos == 0 || text_pos >= remaining_bytes)
					continue;

				const auto* record = reinterpret_cast<const TrackTextRecord*>(
					ptr + text_pos);

				// Validate record is within bounds
				if (text_pos + sizeof(TrackTextRecord) > remaining_bytes)
					continue;

				const uint8_t text_amount = record->track_amount;
				if (text_amount == 0 || text_amount > 6)
					continue;

				// Text offset array follows the TrackTextRecord header
				const auto* text_offsets = reinterpret_cast<const PackedBE16*>(
					ptr + text_pos + sizeof(TrackTextRecord));

				// Validate offset array is within bounds
				if (text_pos + sizeof(TrackTextRecord) +
				    text_amount * sizeof(PackedBE16) > remaining_bytes)
					continue;

				// Extract each text field
				for (uint8_t j = 0; j < text_amount; ++j) {
					const uint16_t str_offset =
						static_cast<uint16_t>(text_offsets[j]);

					// String offset is relative to the TrackTextRecord
					const std::size_t abs_offset = text_pos + str_offset;
					if (str_offset == 0 || abs_offset >= remaining_bytes)
						continue;

					const char* str = reinterpret_cast<const char*>(
						ptr + abs_offset);

					// Find string length (null-terminated, bounded)
					const std::size_t max_len = remaining_bytes - abs_offset;
					std::size_t str_len = 0;
					while (str_len < max_len && str[str_len] != '\0')
						++str_len;

					if (str_len == 0)
						continue;

					std::string text = ConvertCharset(str, str_len, charset);

					// Assign to appropriate field
					switch (j) {
					case 0:
						area.tracks[i].text.title = std::move(text);
						FmtWarning(sacdiso_domain,
						           "ReadTrackText: track[{}] title='{}'",
						           i, area.tracks[i].text.title);
						break;
					case 1:
						area.tracks[i].text.performer = std::move(text);
						break;
					case 2:
						area.tracks[i].text.songwriter = std::move(text);
						break;
					case 3:
						area.tracks[i].text.composer = std::move(text);
						break;
					case 4:
						area.tracks[i].text.arranger = std::move(text);
						break;
					case 5:
						area.tracks[i].text.message = std::move(text);
						break;
					}
				}
			}
			break;  // Found and processed SACDTTxt
		}
		ptr += kLsnSize;
		++sector_num;
	}

	if (!found_text) {
		FmtWarning(sacdiso_domain,
		           "ReadTrackText: SACDTTxt NOT FOUND after {} sectors",
		           sector_num);
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
