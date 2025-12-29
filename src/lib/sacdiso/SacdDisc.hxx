// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * SacdDisc - SACD ISO image parser and track reader
 *
 * This class handles parsing SACD ISO images and reading
 * audio frames for playback.
 */

#ifndef MPD_SACDISO_DISC_HXX
#define MPD_SACDISO_DISC_HXX

#include "ScarletBook.hxx"
#include "SacdMedia.hxx"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

class TagHandler;

namespace Sacd {

/**
 * SACD ISO disc parser and reader.
 *
 * This class provides functionality to:
 * - Parse SACD ISO images (both 2048 and 2064 byte sector sizes)
 * - Extract metadata (album, track info)
 * - Read and decode audio frames (DSD or DST)
 *
 * Thread safety: Not thread-safe. External synchronization required.
 */
class Disc {
public:
	Disc() noexcept = default;
	~Disc() noexcept;

	Disc(const Disc&) = delete;
	Disc& operator=(const Disc&) = delete;

	/**
	 * Open an SACD ISO image.
	 *
	 * @param media The media source to read from
	 * @return true on success
	 * @throws std::runtime_error on parse errors
	 */
	bool Open(std::unique_ptr<Media> media);

	/**
	 * Close the disc and release resources.
	 */
	void Close() noexcept;

	/**
	 * Check if a disc is currently open.
	 */
	[[nodiscard]]
	bool IsOpen() const noexcept { return media_ != nullptr; }

	/*
	 * Disc information
	 */

	/**
	 * Get the disc information structure.
	 */
	[[nodiscard]]
	const DiscInfo& GetDiscInfo() const noexcept { return disc_info_; }

	/**
	 * Check if the disc has a stereo area.
	 */
	[[nodiscard]]
	bool HasStereoArea() const noexcept { return disc_info_.HasStereoArea(); }

	/**
	 * Check if the disc has a multichannel area.
	 */
	[[nodiscard]]
	bool HasMultichannelArea() const noexcept { return disc_info_.HasMultichannelArea(); }

	/**
	 * Get the number of tracks in the specified area.
	 */
	[[nodiscard]]
	std::size_t GetTrackCount(AreaId area_id = AreaId::Stereo) const noexcept;

	/**
	 * Get the total number of tracks across all areas.
	 */
	[[nodiscard]]
	std::size_t GetTotalTrackCount() const noexcept;

	/**
	 * Get track duration in seconds.
	 */
	[[nodiscard]]
	double GetTrackDuration(AreaId area_id, std::size_t track_index) const noexcept;

	/**
	 * Get channel count for the specified area.
	 */
	[[nodiscard]]
	unsigned GetChannelCount(AreaId area_id) const noexcept;

	/**
	 * Get the sampling rate (always 2822400 Hz for DSD64).
	 */
	[[nodiscard]]
	static constexpr unsigned GetSampleRate() noexcept { return kSamplingFrequency; }

	/**
	 * Get the frame rate (75 frames per second).
	 */
	[[nodiscard]]
	static constexpr unsigned GetFrameRate() noexcept { return 75; }

	/**
	 * Check if the specified area uses DST encoding.
	 */
	[[nodiscard]]
	bool IsDstEncoded(AreaId area_id) const noexcept;

	/**
	 * Fill a TagHandler with track metadata.
	 */
	void GetTrackInfo(AreaId area_id, std::size_t track_index,
	                  TagHandler& handler) const noexcept;

	/*
	 * Playback control
	 */

	/**
	 * Select an area for playback.
	 */
	void SelectArea(AreaId area_id) noexcept;

	/**
	 * Select a track for playback.
	 *
	 * @param track_index Track index within the current area (0-based)
	 * @param offset Starting LSN offset within the track
	 * @return true on success
	 */
	bool SelectTrack(std::size_t track_index, uint32_t offset = 0) noexcept;

	/**
	 * Read the next audio frame.
	 *
	 * @param buffer Output buffer for frame data
	 * @param frame_size On input, buffer size; on output, actual frame size
	 * @param frame_type Output frame type (DSD, DST, or Invalid)
	 * @return true if a frame was read, false at end of track
	 */
	bool ReadFrame(std::span<std::byte> buffer, std::size_t& frame_size,
	               FrameType& frame_type) noexcept;

	/**
	 * Seek to a position within the current track.
	 *
	 * @param seconds Time in seconds from track start
	 * @return true on success
	 */
	bool Seek(double seconds) noexcept;

	/**
	 * Get the current playback position in bytes.
	 */
	[[nodiscard]]
	uint64_t GetPosition() const noexcept;

	/**
	 * Get the total size of the current track in bytes.
	 */
	[[nodiscard]]
	uint64_t GetSize() const noexcept;

	/**
	 * Get the duration of the current track.
	 */
	[[nodiscard]]
	double GetDuration() const noexcept;

	/*
	 * Configuration
	 */

	/**
	 * Enable/disable edited master mode.
	 *
	 * When enabled, track boundaries are determined by the
	 * track start LSN of the next track, rather than the
	 * track length field. This can be useful for discs with
	 * inaccurate TOC data.
	 */
	void SetEditedMasterMode(bool enabled) noexcept { edited_master_mode_ = enabled; }

private:
	// Parsing methods
	bool ReadMasterToc();
	bool ReadAreaToc(AreaId area_id, uint32_t toc_start, uint16_t toc_size);
	bool ReadTrackList(AreaInfo& area, const std::byte* area_data, std::size_t area_size);
	bool ReadTrackText(AreaInfo& area, const std::byte* area_data, std::size_t area_size);
	std::string ConvertCharset(const char* data, std::size_t length, CharacterSet charset);

	// Reading methods
	bool ReadRawSector(uint32_t lsn, std::span<std::byte> buffer) noexcept;
	bool ReadRawSectors(uint32_t lsn, uint32_t count, std::span<std::byte> buffer) noexcept;

	// Media source
	std::unique_ptr<Media> media_;
	std::size_t sector_size_ = 0;
	std::size_t sector_offset_ = 0;  // Offset to data within sector (0 or 12)

	// Parsed disc info
	DiscInfo disc_info_;

	// Current playback state
	AreaId current_area_ = AreaId::Stereo;
	std::size_t current_track_ = 0;
	uint32_t track_start_lsn_ = 0;
	uint32_t track_length_lsn_ = 0;
	uint32_t current_lsn_ = 0;

	// Audio sector parsing state
	struct AudioSectorState {
		std::vector<std::byte> sector_buffer;
		std::array<AudioPacketInfo, 8> packets{};
		std::array<AudioFrameInfo, 8> frames{};
		uint8_t packet_count = 0;
		uint8_t frame_count = 0;
		uint8_t current_packet = 0;
		std::size_t buffer_offset = 0;
		bool dst_encoded = false;
		
		AudioSectorState() : sector_buffer(kPsnSize) {}
	};
	std::unique_ptr<AudioSectorState> audio_state_;

	// Frame assembly state
	struct FrameState {
		static constexpr std::size_t kMaxFrameSize = 64 * 1024;
		std::vector<std::byte> data;
		std::size_t size = 0;
		bool started = false;
		bool dst_encoded = false;
		
		FrameState() : data(kMaxFrameSize) {}
	};
	std::unique_ptr<FrameState> frame_state_;

	// Configuration
	bool edited_master_mode_ = false;
};

} // namespace Sacd

#endif // MPD_SACDISO_DISC_HXX
