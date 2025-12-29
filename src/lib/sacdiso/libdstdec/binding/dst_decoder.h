// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * DstDecoderBinding - High-level DST decoder interface
 * 
 * Thread-safe wrapper around the core DST decoder.
 */

#ifndef MPD_DST_DECODER_BINDING_HXX
#define MPD_DST_DECODER_BINDING_HXX

#include <cstdint>
#include <memory>
#include <vector>

namespace Dst {

class FrameDecoder;

/**
 * High-level DST decoder with optional multi-threading support
 */
class DecoderBinding {
public:
	DecoderBinding() noexcept;
	~DecoderBinding() noexcept;

	DecoderBinding(const DecoderBinding &) = delete;
	DecoderBinding &operator=(const DecoderBinding &) = delete;

	/**
	 * Initialize decoder
	 * 
	 * @param channels Number of audio channels (1-6)
	 * @param frame_size Frame size in bytes per channel
	 * @return 0 on success, -1 on error
	 */
	int Initialize(uint32_t channels, uint32_t frame_size) noexcept;

	/**
	 * Decode a DST frame
	 * 
	 * @param dst_data Input DST data (modified to contain output DSD)
	 * @return Output size in bytes, or 0 on error
	 */
	int Decode(std::vector<uint8_t> &dst_data) noexcept;

	/**
	 * Flush any buffered data
	 */
	void Flush() noexcept;

	/**
	 * Check if initialized
	 */
	[[nodiscard]]
	bool IsInitialized() const noexcept;

private:
	std::unique_ptr<FrameDecoder> decoder_;
	std::vector<uint8_t> output_buffer_;
	uint32_t channel_count_ = 0;
	uint32_t frame_size_ = 0;
};

} // namespace Dst

#endif // MPD_DST_DECODER_BINDING_HXX
