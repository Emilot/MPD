// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * DstFrameDecoder - Core DST decoding algorithm
 * 
 * New implementation following MPD code standards.
 * Based on ISO/IEC 14496-3 Part 3 Subpart 10 specification.
 */

#ifndef MPD_DST_FRAME_DECODER_HXX
#define MPD_DST_FRAME_DECODER_HXX

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

namespace Dst {

// DST constants
inline constexpr std::size_t kMaxChannels = 6;
inline constexpr std::size_t kMaxFilters = 16;
inline constexpr std::size_t kMaxPtables = 16;
inline constexpr std::size_t kMaxPredOrder = 128;
inline constexpr std::size_t kAcHistMax = 128;

// DSD silence pattern - 0xAA (10101010) per SACD specification
// This matches the initial channel status used by the DST decoder
inline constexpr uint8_t kDsdSilence = 0xAA;

/**
 * Core DST frame decoder
 * 
 * Implements the DST algorithm as specified in ISO/IEC 14496-3.
 */
class FrameDecoder {
public:
	enum class Result {
		Success,
		InvalidHeader,
		InvalidData,
		InternalError,
	};

	FrameDecoder() noexcept;
	~FrameDecoder() noexcept;

	FrameDecoder(const FrameDecoder &) = delete;
	FrameDecoder &operator=(const FrameDecoder &) = delete;

	/**
	 * Initialize decoder for specific format
	 */
	[[nodiscard]]
	bool Initialize(uint32_t channels, uint32_t frame_size) noexcept;

	/**
	 * Decode one DST frame to DSD
	 */
	[[nodiscard]]
	Result Decode(const uint8_t *dst_input, std::size_t dst_size,
	              uint8_t *dsd_output, std::size_t dsd_size) noexcept;

	/**
	 * Reset decoder state
	 */
	void Reset() noexcept;

	/**
	 * Get expected output size
	 */
	[[nodiscard]]
	std::size_t GetOutputSize() const noexcept { return output_size_; }

	/**
	 * Check if initialized
	 */
	[[nodiscard]]
	bool IsInitialized() const noexcept { return initialized_; }

private:
	// Configuration
	uint32_t channel_count_ = 0;
	uint32_t frame_size_ = 0;
	std::size_t output_size_ = 0;
	bool initialized_ = false;

	// Bit stream reader state
	const uint8_t *bit_data_ = nullptr;
	std::size_t bit_size_ = 0;
	std::size_t bit_pos_ = 0;

	// Frame header
	struct FrameHeader {
		bool dst_coded = false;
		uint32_t nr_of_filters = 0;
		uint32_t nr_of_ptables = 0;
		std::array<uint32_t, kMaxFilters> pred_order{};
		std::array<uint32_t, kMaxFilters> ptable_len{};
		std::array<std::array<int16_t, kMaxPredOrder>, kMaxFilters> coef{};
		std::array<bool, kMaxChannels> half_prob{};
		std::array<uint32_t, kMaxChannels> nr_of_half_bits{};
	};
	FrameHeader header_;

	// Segmentation
	struct Segment {
		uint32_t resolution = 0;
		std::array<uint32_t, kMaxChannels> nr_of_segments{};
		std::array<std::array<uint32_t, 32>, kMaxChannels> segment_length{};
		std::array<std::array<uint32_t, 32>, kMaxChannels> table_for_segment{};
	};
	Segment filter_segment_;
	Segment ptable_segment_;

	// Probability tables
	std::array<std::array<uint32_t, kAcHistMax>, kMaxFilters> p_one_{};

	// Filter lookup tables
	std::array<std::array<std::array<int16_t, 256>, 16>, kMaxFilters> filter_table_{};
	std::array<std::array<uint8_t, 16>, kMaxChannels> channel_status_{};

	// Per-bit lookup tables for filter/ptable selection (packed 4-bit)
	// Each byte holds 2 table numbers (4 bits each)
	std::array<std::vector<uint8_t>, kMaxChannels> filter_4bit_{};
	std::array<std::vector<uint8_t>, kMaxChannels> ptable_4bit_{};

	// Arithmetic coder state - uses bit-based reading per DST spec
	struct AcState {
		uint32_t code = 0;      // Current code value (12-bit precision)
		uint32_t range = 0;     // Current range (A in spec)
		int bit_pos = 0;        // Bit position in arithmetic data
		const uint8_t *data = nullptr;  // Arithmetic coded data
		int data_bits = 0;      // Total bits in arithmetic data
	};
	AcState ac_;

	// Bit stream methods
	void SetBitStream(const uint8_t *data, std::size_t size) noexcept;
	[[nodiscard]] uint32_t GetBit() noexcept;
	[[nodiscard]] uint32_t GetBits(unsigned count) noexcept;
	[[nodiscard]] int32_t GetSignedBits(unsigned count) noexcept;
	[[nodiscard]] std::size_t GetBitPosition() const noexcept { return bit_pos_; }

	// Parsing methods
	[[nodiscard]] Result ParseFrameHeader() noexcept;
	[[nodiscard]] Result ParseSegmentation(Segment &seg, uint32_t max_segments, 
	                                       uint32_t min_length) noexcept;
	[[nodiscard]] Result ParseMapping(Segment &seg, uint32_t &nr_of_tables) noexcept;
	[[nodiscard]] Result ParseFilterCoefs() noexcept;
	[[nodiscard]] Result ParseProbabilityTables() noexcept;
	[[nodiscard]] int32_t RiceDecode(uint32_t m) noexcept;

	// Decoding methods
	void InitFilterTables() noexcept;
	void InitChannelStatus() noexcept;
	void AcInit(const uint8_t *data, int total_bits, int start_bit) noexcept;
	[[nodiscard]] uint8_t AcDecodeBit(uint32_t probability) noexcept;
	[[nodiscard]] uint32_t AcGetBit() noexcept;  // Read single bit from AC stream
	[[nodiscard]] int16_t RunFilter(uint32_t filter_nr, uint32_t channel) noexcept;
	[[nodiscard]] uint32_t GetPtableIndex(int16_t predict, uint32_t ptable_len) noexcept;
	void UpdateChannelStatus(uint32_t channel, int bit) noexcept;

	// Utility
	[[nodiscard]] uint32_t Log2RoundUp(uint32_t x) noexcept;

	// Segment lookup table helpers
	void FillTable4Bit(const Segment &seg, 
	                   std::array<std::vector<uint8_t>, kMaxChannels> &table) noexcept;
	[[nodiscard]] static uint32_t GetNibble(const uint8_t *data, uint32_t index) noexcept;
};

} // namespace Dst

#endif // MPD_DST_FRAME_DECODER_HXX
