// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * DstDecoder - DST (Direct Stream Transfer) decoder for SACD
 */

#ifndef MPD_SACDISO_DST_DECODER_HXX
#define MPD_SACDISO_DST_DECODER_HXX

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Sacd {

/**
 * DST (Direct Stream Transfer) decoder.
 *
 * DST is a lossless compression algorithm specified in the
 * SACD standard (ISO/IEC 14496-3 Part 3 Subpart 10).
 */
class DstDecoder {
public:
	DstDecoder() noexcept;
	~DstDecoder() noexcept;

	DstDecoder(const DstDecoder &) = delete;
	DstDecoder &operator=(const DstDecoder &) = delete;

	/**
	 * Initialize the decoder.
	 *
	 * @param channel_count Number of audio channels (1-6)
	 * @param sample_rate DSD sample rate (2822400 or 5644800)
	 * @return true on success
	 */
	bool Initialize(unsigned channel_count, unsigned sample_rate) noexcept;

	/**
	 * Check if the decoder is initialized.
	 */
	[[nodiscard]]
	bool IsInitialized() const noexcept;

	/**
	 * Decode a DST frame to DSD.
	 *
	 * @param dst_data Input DST compressed data
	 * @param dsd_output Output buffer for DSD data
	 * @return true on success
	 */
	bool Decode(std::span<const std::byte> dst_data,
	            std::vector<std::byte> &dsd_output) noexcept;

	/**
	 * Get the expected DSD output size for one frame.
	 */
	[[nodiscard]]
	std::size_t GetOutputFrameSize() const noexcept;

	/**
	 * Reset the decoder state.
	 */
	void Reset() noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> pimpl_;
};

} // namespace Sacd

#endif // MPD_SACDISO_DST_DECODER_HXX
