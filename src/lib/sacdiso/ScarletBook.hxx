// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * ScarletBook - SACD (Super Audio CD) format structures
 *
 * Based on the Scarlet Book specification for SACD.
 * This file defines the on-disc structures for parsing SACD ISO images.
 */

#ifndef MPD_SACDISO_SCARLET_BOOK_HXX
#define MPD_SACDISO_SCARLET_BOOK_HXX

#include "util/PackedBigEndian.hxx"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Sacd {

/*
 * Constants
 */

/** Logical Sector Size (2048 bytes) */
inline constexpr std::size_t kLsnSize = 2048;

/** Physical Sector Size (2064 bytes, includes subchannel) */
inline constexpr std::size_t kPsnSize = 2064;

/** Start position of Master TOC */
inline constexpr uint32_t kMasterTocStart = 510;

/** Length of Master TOC in sectors */
inline constexpr uint32_t kMasterTocLength = 10;

/** SACD sampling frequency (2.8224 MHz) */
inline constexpr uint32_t kSamplingFrequency = 2822400;

/** Maximum number of tracks per area */
inline constexpr std::size_t kMaxTracks = 255;

/** Maximum language count */
inline constexpr std::size_t kMaxLanguages = 8;

/** Supported SACD version */
inline constexpr uint8_t kSupportedVersionMajor = 1;
inline constexpr uint8_t kSupportedVersionMinor = 20;

/*
 * Character sets for text encoding
 */

enum class CharacterSet : uint8_t {
	Iso646 = 0,      // US-ASCII
	Iso8859_1 = 1,   // Latin-1
	Ris506 = 2,      // Korean
	Gb2312 = 3,      // Simplified Chinese
	Big5 = 4,        // Traditional Chinese (Taiwan)
	ShiftJis = 5,    // Japanese
	Koi8 = 6,        // Russian
	Iso8859_1_2 = 7, // Latin-1 (duplicate)
};

/*
 * Frame format types
 * Note: DST is 0, DSD variants are 2 and 3!
 */

enum class FrameFormat : uint8_t {
	Dst = 0,           // DST encoded (lossless compression)
	Dsd_3_in_14 = 2,   // DSD 3-in-14
	Dsd_3_in_16 = 3,   // DSD 3-in-16 (most common)
};

/*
 * Area identifiers
 */

enum class AreaId {
	Stereo,       // 2-channel stereo area
	Multichannel, // Multi-channel surround area
	Both,         // Both areas (for iteration)
};

/*
 * Frame types returned during playback
 */

enum class FrameType {
	Invalid,
	Dsd,
	Dst,
};

/*
 * Loudspeaker configurations
 */

enum class LoudspeakerConfig : uint8_t {
	Unknown = 0,
	TwoChannel = 1,
	FiveChannel = 2,
	SixChannel = 3,
};

/*
 * Data types in audio packets
 */

enum class DataType : uint8_t {
	Audio = 2,
	Supplementary = 3,
	Padding = 7,
};

/*
 * On-disc structures (packed, big-endian)
 */

#pragma pack(push, 1)

/**
 * Version information
 */
struct Version {
	uint8_t major;
	uint8_t minor;
};

/**
 * Locale/language information
 */
struct Locale {
	std::array<char, 2> language_code;
	uint8_t character_set;
	uint8_t reserved;
};

/**
 * Genre information
 */
struct Genre {
	uint8_t category;
	uint8_t reserved;
	uint8_t genre;
	uint8_t reserved2;
};

/**
 * Master TOC header
 * Layout verified against original scarletbook.h
 */
struct MasterToc {
	std::array<char, 8> id;           // "SACDMTOC" - offset 0
	Version version;                   // offset 8
	uint8_t reserved1[6];              // offset 10
	PackedBE16 album_set_size;         // offset 16
	PackedBE16 album_sequence_number;  // offset 18
	uint8_t reserved2[4];              // offset 20
	std::array<char, 16> album_catalog_number;  // offset 24
	std::array<Genre, 4> album_genre;  // offset 40 (4 genres * 4 bytes = 16)
	uint8_t reserved3[8];              // offset 56
	// Area TOC pointers - ALL STARTS FIRST, then sizes!
	PackedBE32 area_1_toc_1_start;     // offset 64
	PackedBE32 area_1_toc_2_start;     // offset 68
	PackedBE32 area_2_toc_1_start;     // offset 72
	PackedBE32 area_2_toc_2_start;     // offset 76
	uint8_t disc_type_reserved : 7;    // offset 80
	uint8_t disc_type_hybrid : 1;
	uint8_t reserved4[3];              // offset 81
	PackedBE16 area_1_toc_size;        // offset 84
	PackedBE16 area_2_toc_size;        // offset 86
	std::array<char, 16> disc_catalog_number;  // offset 88
	std::array<Genre, 4> disc_genre;   // offset 104
	PackedBE16 disc_date_year;         // offset 120
	uint8_t disc_date_month;           // offset 122
	uint8_t disc_date_day;             // offset 123
	uint8_t reserved5[4];              // offset 124
	uint8_t text_area_count;           // offset 128
	uint8_t reserved6[7];              // offset 129
	std::array<Locale, kMaxLanguages> locales;  // offset 136

	[[nodiscard]] bool IsValid() const noexcept {
		return id[0] == 'S' && id[1] == 'A' && id[2] == 'C' && id[3] == 'D' &&
		       id[4] == 'M' && id[5] == 'T' && id[6] == 'O' && id[7] == 'C';
	}
};

/**
 * Master text block (for album/disc metadata)
 */
struct MasterText {
	std::array<char, 8> id;           // "SACDText"
	uint8_t reserved[8];
	PackedBE16 album_title_position;
	PackedBE16 album_artist_position;
	PackedBE16 album_publisher_position;
	PackedBE16 album_copyright_position;
	PackedBE16 album_title_phonetic_position;
	PackedBE16 album_artist_phonetic_position;
	PackedBE16 album_publisher_phonetic_position;
	PackedBE16 album_copyright_phonetic_position;
	PackedBE16 disc_title_position;
	PackedBE16 disc_artist_position;
	PackedBE16 disc_publisher_position;
	PackedBE16 disc_copyright_position;
	PackedBE16 disc_title_phonetic_position;
	PackedBE16 disc_artist_phonetic_position;
	PackedBE16 disc_publisher_phonetic_position;
	PackedBE16 disc_copyright_phonetic_position;

	[[nodiscard]] bool IsValid() const noexcept {
		return id[0] == 'S' && id[1] == 'A' && id[2] == 'C' && id[3] == 'D' &&
		       id[4] == 'T' && id[5] == 'e' && id[6] == 'x' && id[7] == 't';
	}
};

/**
 * Area TOC header
 * Layout based on original scarletbook.h
 */
struct AreaToc {
	std::array<char, 8> id;           // "TWOCHTOC" or "MULCHTOC"
	Version version;                   // 1.20 / 0x0114
	PackedBE16 size;                   // total size of TOC
	uint8_t reserved1[4];
	PackedBE32 max_byte_rate;
	uint8_t sample_frequency;          // 0x04 = (64 * 44.1 kHz)
	uint8_t frame_format : 4;          // lower 4 bits: 0=DST, 2/3=DSD
	uint8_t reserved2 : 4;             // upper 4 bits
	uint8_t reserved3[10];
	uint8_t channel_count;
	uint8_t loudspeaker_config : 5;
	uint8_t extra_settings : 3;
	uint8_t max_available_channels;
	uint8_t area_mute_flags;
	uint8_t reserved4[12];
	uint8_t track_attribute : 4;
	uint8_t reserved5 : 4;
	uint8_t reserved6[15];
	uint8_t total_playtime_minutes;
	uint8_t total_playtime_seconds;
	uint8_t total_playtime_frames;
	uint8_t reserved7;
	uint8_t track_offset;
	uint8_t track_count;
	uint8_t reserved8[2];
	PackedBE32 track_start;
	PackedBE32 track_end;
	uint8_t text_area_count;
	uint8_t reserved9[7];
	std::array<Locale, 10> languages;
	PackedBE16 track_text_offset;
	PackedBE16 index_list_offset;
	PackedBE16 access_list_offset;
	uint8_t reserved10[10];
	PackedBE16 area_description_offset;
	PackedBE16 copyright_offset;
	PackedBE16 area_description_phonetic_offset;
	PackedBE16 copyright_phonetic_offset;

	[[nodiscard]] bool IsTwoChannel() const noexcept {
		return id[0] == 'T' && id[1] == 'W' && id[2] == 'O' && id[3] == 'C' &&
		       id[4] == 'H' && id[5] == 'T' && id[6] == 'O' && id[7] == 'C';
	}

	[[nodiscard]] bool IsMultiChannel() const noexcept {
		return id[0] == 'M' && id[1] == 'U' && id[2] == 'L' && id[3] == 'C' &&
		       id[4] == 'H' && id[5] == 'T' && id[6] == 'O' && id[7] == 'C';
	}

	[[nodiscard]] bool IsValid() const noexcept {
		return IsTwoChannel() || IsMultiChannel();
	}

	[[nodiscard]] bool IsDstEncoded() const noexcept {
		// DST = 0, DSD = 2 or 3
		return frame_format == 0;
	}
};

/**
 * Track list offset entry
 */
struct TrackListOffset {
	std::array<char, 8> id;           // "SACDTRL1"
	uint8_t reserved[8];

	[[nodiscard]] bool IsValid() const noexcept {
		return id[0] == 'S' && id[1] == 'A' && id[2] == 'C' && id[3] == 'D' &&
		       id[4] == 'T' && id[5] == 'R' && id[6] == 'L' && id[7] == '1';
	}
};

/**
 * Track offset data (follows TrackListOffset header)
 */
struct TrackOffset {
	PackedBE32 start_lsn;
	PackedBE32 length_lsn;
};

/**
 * Track list time entry
 */
struct TrackListTime {
	std::array<char, 8> id;           // "SACDTRL2"
	uint8_t reserved[8];

	[[nodiscard]] bool IsValid() const noexcept {
		return id[0] == 'S' && id[1] == 'A' && id[2] == 'C' && id[3] == 'D' &&
		       id[4] == 'T' && id[5] == 'R' && id[6] == 'L' && id[7] == '2';
	}
};

 */
struct AreaText {
/**
 * Track time/duration (on-disc format, 4 bytes)
 * Used in SACDTRL2 for both start times and durations
 */
struct TrackTimeDuration {
	uint8_t minutes;
	uint8_t seconds;
	uint8_t frames;
	uint8_t flags;  // Various flags (reserved/track_flags)

	[[nodiscard]] double ToSeconds() const noexcept {
		return minutes * 60.0 + seconds + frames / 75.0;
	}
};

/**
 * Track time (runtime format, for storing parsed data)
 */
struct TrackTime {
	uint8_t minutes = 0;
	uint8_t seconds = 0;
	uint8_t frames = 0;

	[[nodiscard]] double ToSeconds() const noexcept {
		return minutes * 60.0 + seconds + frames / 75.0;
	}
};

/**
 * Audio sector header (1 byte only!)
 * Byte layout (from original sacd_reader.cpp):
 *   bit 0: dst_encoded
 *   bit 1: reserved
 *   bits 2-4: frame_info_count
 *   bits 5-7: packet_info_count
 */
struct AudioSectorHeader {
	uint8_t data;

	[[nodiscard]] uint8_t GetPacketInfoCount() const noexcept {
		return (data >> 5) & 0x07;
	}

	[[nodiscard]] uint8_t GetFrameInfoCount() const noexcept {
		return (data >> 2) & 0x07;
	}

	[[nodiscard]] bool IsDstEncoded() const noexcept {
		return data & 0x01;
	}
};

/**
 * Audio packet info (2 bytes)
 * Byte 0 layout (MSB to LSB):
 *   bit 7: frame_start
 *   bit 6: reserved  
 *   bits 5-3: data_type
 *   bits 2-0: packet_length high 3 bits
 * Byte 1: packet_length low 8 bits
 */
struct AudioPacketInfo {
	uint8_t byte0;
	uint8_t byte1;

	[[nodiscard]] uint16_t GetPacketLength() const noexcept {
		return (static_cast<uint16_t>(byte0 & 0x07) << 8) | byte1;
	}

	[[nodiscard]] DataType GetDataType() const noexcept {
		return static_cast<DataType>((byte0 >> 3) & 0x07);
	}

	[[nodiscard]] bool IsFrameStart() const noexcept {
		return (byte0 >> 7) & 1;
	}
};

/**
 * Audio frame info
 * DST: 4 bytes (timecode + channel/sector info)
 * DSD: 3 bytes (only timecode)
 */
struct AudioFrameInfo {
	// Timecode (always present)
	uint8_t minutes;
	uint8_t seconds;
	uint8_t frames;
	// Only present for DST encoded frames
	uint8_t channel_bit_3 : 1;
	uint8_t channel_bit_2 : 1;
	uint8_t sector_count : 5;
	uint8_t channel_bit_1 : 1;

	[[nodiscard]] unsigned GetChannelCount() const noexcept {
		if (channel_bit_2 == 1 && channel_bit_3 == 0)
			return 6;
		else if (channel_bit_2 == 0 && channel_bit_3 == 1)
			return 5;
		else
			return 2;
	}
};

/**
 * Track text block header ("SACDTTxt")
 *
 * Layout:
 *   offset 0-7:  "SACDTTxt" signature
 *   offset 8+:   track_text_position[255] - uint16_t BE offsets
 *                 Each points to a per-track text record within the
 *                 SACDTTxt data (which may span multiple sectors).
 */
struct TrackTextHeader {
	std::array<char, 8> id;           // "SACDTTxt"

	[[nodiscard]] bool IsValid() const noexcept {
		return id[0] == 'S' && id[1] == 'A' && id[2] == 'C' && id[3] == 'D' &&
		       id[4] == 'T' && id[5] == 'T' && id[6] == 'x' && id[7] == 't';
	}
};

/**
 * ISRC/Genre data embedded in each track's text record (12 bytes)
 */
struct TrackIsrcGenre {
	char country_code[2];
	char owner_code[3];
	char recording_year[2];
	char designation_code[5];
};

/**
 * Per-track text record (pointed to by TrackTextHeader positions)
 *
 * Layout:
 *   byte 0:     track_type
 *   bytes 2-13: ISRC/genre info (12 bytes)
 *   bytes 14+:  uint16_t BE offsets[track_amount]
 *               Each offset is relative to the start of THIS record,
 *               pointing to a null-terminated text string.
 *
 * Text entry indices:
 *   0 = title, 1 = performer, 2 = songwriter,
 *   3 = composer, 4 = arranger, 5 = message
 */
struct TrackTextRecord {
	uint8_t track_type;
	uint8_t track_amount;
	TrackIsrcGenre isrc_genre;
	// Followed by track_amount × PackedBE16 text offsets
};

#pragma pack(pop)

/*
 * Runtime structures (for parsed data)
 */

/**
 * Track text information
 */
struct TrackText {
	std::string title;
	std::string performer;
	std::string songwriter;
	std::string composer;
	std::string arranger;
	std::string message;
};

/**
 * Master text information (album metadata)
 */
struct MasterTextInfo {
	std::string album_title;
	std::string album_artist;
	std::string album_publisher;
	std::string album_copyright;
	std::string disc_title;
	std::string disc_artist;
	std::string disc_publisher;
	std::string disc_copyright;
};

/**
 * Track information
 */
struct TrackInfo {
	uint32_t start_lsn = 0;
	uint32_t length_lsn = 0;
	TrackTime start_time{};
	TrackTime duration{};
	TrackText text;
	Genre genre{};
	std::string isrc;
};

/**
 * Area information (stereo or multichannel)
 */
struct AreaInfo {
	AreaId id = AreaId::Stereo;
	uint8_t channel_count = 0;
	LoudspeakerConfig loudspeaker_config = LoudspeakerConfig::Unknown;
	FrameFormat frame_format = FrameFormat::Dsd_3_in_16;  // Most common DSD format
	uint32_t track_start = 0;
	uint32_t track_end = 0;
	std::string description;
	std::string copyright;
	std::vector<TrackInfo> tracks;

	[[nodiscard]] bool IsDstEncoded() const noexcept {
		return frame_format == FrameFormat::Dst;
	}

	[[nodiscard]] std::size_t GetTrackCount() const noexcept {
		return tracks.size();
	}
};

/**
 * Disc information (complete SACD metadata)
 */
struct DiscInfo {
	Version version{};
	uint16_t album_set_size = 0;
	uint16_t album_sequence_number = 0;
	uint16_t disc_year = 0;
	uint8_t disc_month = 0;
	uint8_t disc_day = 0;
	bool is_hybrid = false;
	MasterTextInfo master_text;
	AreaInfo stereo_area;
	AreaInfo multichannel_area;

	[[nodiscard]] bool HasStereoArea() const noexcept {
		return !stereo_area.tracks.empty();
	}

	[[nodiscard]] bool HasMultichannelArea() const noexcept {
		return !multichannel_area.tracks.empty();
	}

	[[nodiscard]] const AreaInfo& GetArea(AreaId area_id) const noexcept {
		return area_id == AreaId::Multichannel ? multichannel_area : stereo_area;
	}

	[[nodiscard]] AreaInfo& GetArea(AreaId area_id) noexcept {
		return area_id == AreaId::Multichannel ? multichannel_area : stereo_area;
	}
};

} // namespace Sacd

#endif // MPD_SACDISO_SCARLET_BOOK_HXX