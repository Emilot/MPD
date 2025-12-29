// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * DstFrameDecoder - Core DST decoding algorithm implementation
 * 
 * New implementation following MPD code standards.
 * Based on ISO/IEC 14496-3 Part 3 Subpart 10 specification.
 */

#include "decoder.h"
#include <algorithm>
#include <cstring>
#include <cstdio>  // For debug logging
#include <cstdarg> // For va_list

namespace Dst {

// Debug logging - set to true to enable
static constexpr bool kDebugLogging = false;
static unsigned debug_frame_count = 0;

static void DebugLog(const char *fmt, ...) noexcept
{
	if (!kDebugLogging) return;
	if (debug_frame_count > 2) return;  // Only log first few frames
	
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "DST: ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

// Arithmetic coder constants per ISO/IEC 14496-3 DST specification
// The AC uses 12-bit precision (8 bits for probability + 4 overhead bits)
static constexpr unsigned kAcProbBits = 8;      // Bits for probability values
static constexpr unsigned kAcOverhead = 4;      // Overhead bits for precision  
static constexpr unsigned kAcTotalBits = kAcProbBits + kAcOverhead;  // = 12
static constexpr uint32_t kAcOne = 1U << kAcTotalBits;   // = 4096
static constexpr uint32_t kAcHalf = 1U << (kAcTotalBits - 1);  // = 2048

// Lookup table for Reverse7LSBs function
// Per DST spec: takes 7 LSBs of 9-bit coefficient, reverses bit order, adds 1
// Used for initial arithmetic coder state conditioning
static constexpr uint8_t kReverse7LSBs[128] = {
	1, 65, 33, 97, 17, 81, 49, 113, 9, 73, 41, 105, 25, 89, 57, 121,
	5, 69, 37, 101, 21, 85, 53, 117, 13, 77, 45, 109, 29, 93, 61, 125,
	3, 67, 35, 99, 19, 83, 51, 115, 11, 75, 43, 107, 27, 91, 59, 123,
	7, 71, 39, 103, 23, 87, 55, 119, 15, 79, 47, 111, 31, 95, 63, 127,
	2, 66, 34, 98, 18, 82, 50, 114, 10, 74, 42, 106, 26, 90, 58, 122,
	6, 70, 38, 102, 22, 86, 54, 118, 14, 78, 46, 110, 30, 94, 62, 126,
	4, 68, 36, 100, 20, 84, 52, 116, 12, 76, 44, 108, 28, 92, 60, 124,
	8, 72, 40, 104, 24, 88, 56, 120, 16, 80, 48, 112, 32, 96, 64, 128
};

static inline uint32_t Reverse7LSBs(int16_t c) noexcept
{
	// SIZE_PREDCOEF = 9 bits, mask with 127 to get 7 LSBs
	return kReverse7LSBs[(c + (1 << 9)) & 127];
}

// Prediction coefficients for Rice coding of filter coefficients
// These are fixed per ISO/IEC 14496-3 DST specification
static constexpr uint32_t kFilterCPredOrder[4] = { 1, 2, 3, 1 };
static constexpr int kFilterCPredCoef[4][3] = {
	{ -8, 0, 0 },       // Method 0: order 1
	{ -16, 8, 0 },      // Method 1: order 2
	{ -9, -5, 6 },      // Method 2: order 3
	{ 8, 0, 0 }         // Method 3: order 1 (alternative)
};

// Prediction coefficients for Rice coding of probability table entries
static constexpr uint32_t kPtableCPredOrder[3] = { 1, 2, 3 };
static constexpr int kPtableCPredCoef[3][3] = {
	{ -8, 0, 0 },       // Method 0: order 1
	{ -16, 8, 0 },      // Method 1: order 2
	{ -24, 24, -8 }     // Method 2: order 3
};

// Gray code helper tables for efficient filter table computation
// GC_ICoefSign[i] = sign of gray code delta at position i (+1, -1, or 0)
// GC_ICoefIndex[i] = which bit position changes in gray code at i
static int GC_ICoefSign[256];
static uint32_t GC_ICoefIndex[256];
static bool GC_Initialized = false;

static uint32_t IntAbs(int n) noexcept
{
	const int mask = n >> 31;
	return static_cast<uint32_t>((n + mask) ^ mask);
}

static uint32_t IntLog2(uint32_t n) noexcept
{
	return (n > 1) ? 1 + IntLog2(n >> 1) : 0;
}

static void InitGrayCodeTables() noexcept
{
	if (GC_Initialized) return;
	
	GC_ICoefSign[0] = 0;
	GC_ICoefIndex[0] = static_cast<uint32_t>(-1);
	
	for (int i = 1; i < 256; ++i) {
		// Gray code delta: current gray - previous gray
		const int gray_curr = i ^ (i >> 1);
		const int gray_prev = (i - 1) ^ ((i - 1) >> 1);
		const int gray_delta = gray_curr - gray_prev;
		const uint32_t gray_delta_abs = IntAbs(gray_delta);
		const uint32_t gray_index = IntLog2(gray_delta_abs);
		
		if (gray_delta > 0) {
			GC_ICoefSign[i] = +1;
		} else if (gray_delta < 0) {
			GC_ICoefSign[i] = -1;
		} else {
			GC_ICoefSign[i] = 0;
		}
		GC_ICoefIndex[i] = gray_index;
	}
	
	GC_Initialized = true;
}

FrameDecoder::FrameDecoder() noexcept = default;
FrameDecoder::~FrameDecoder() noexcept = default;

bool
FrameDecoder::Initialize(uint32_t channels, uint32_t frame_size) noexcept
{
	if (channels == 0 || channels > kMaxChannels)
		return false;

	channel_count_ = channels;
	frame_size_ = frame_size;
	output_size_ = frame_size * channels;
	initialized_ = true;

	Reset();
	return true;
}

void
FrameDecoder::Reset() noexcept
{
	InitChannelStatus();
	// Reset debug frame counter so each track gets debug output
	debug_frame_count = 0;
}

void
FrameDecoder::SetBitStream(const uint8_t *data, std::size_t size) noexcept
{
	bit_data_ = data;
	bit_size_ = size * 8;
	bit_pos_ = 0;
}

uint32_t
FrameDecoder::GetBit() noexcept
{
	if (bit_pos_ >= bit_size_)
		return 0;

	const std::size_t byte_idx = bit_pos_ >> 3;
	const unsigned bit_idx = 7 - (bit_pos_ & 7);
	++bit_pos_;

	return (bit_data_[byte_idx] >> bit_idx) & 1;
}

uint32_t
FrameDecoder::GetBits(unsigned count) noexcept
{
	uint32_t value = 0;
	for (unsigned i = 0; i < count; ++i) {
		value = (value << 1) | GetBit();
	}
	return value;
}

int32_t
FrameDecoder::GetSignedBits(unsigned count) noexcept
{
	const uint32_t value = GetBits(count);
	// Sign extend
	const uint32_t sign_bit = 1U << (count - 1);
	if (value & sign_bit) {
		return static_cast<int32_t>(value | (~0U << count));
	}
	return static_cast<int32_t>(value);
}

uint32_t
FrameDecoder::Log2RoundUp(uint32_t x) noexcept
{
	uint32_t y = 0;
	while (x >= (1U << y)) {
		++y;
	}
	return y;
}

int32_t
FrameDecoder::RiceDecode(uint32_t m) noexcept
{
	// Run length
	uint32_t run_length = 0;
	while (GetBit() == 0) {
		++run_length;
	}

	// LSBs
	const uint32_t lsbs = GetBits(m);
	int32_t value = static_cast<int32_t>((run_length << m) | lsbs);

	// Sign
	if (value != 0) {
		if (GetBit()) {
			value = -value;
		}
	}

	return value;
}

FrameDecoder::Result
FrameDecoder::Decode(const uint8_t *dst_input, std::size_t dst_size,
                     uint8_t *dsd_output, std::size_t dsd_size) noexcept
{
	if (!initialized_)
		return Result::InternalError;

	if (dst_input == nullptr || dst_size == 0)
		return Result::InvalidData;

	if (dsd_output == nullptr || dsd_size < output_size_)
		return Result::InternalError;

	// Track frame count for debug
	++debug_frame_count;
	const bool do_debug = (debug_frame_count <= 2);

	if (do_debug) {
		DebugLog("=== Frame %u: input_size=%zu, channels=%u, frame_size=%u ===",
		         debug_frame_count, dst_size, channel_count_, frame_size_);
		DebugLog("First 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
		         dst_input[0], dst_input[1], dst_input[2], dst_input[3],
		         dst_input[4], dst_input[5], dst_input[6], dst_input[7]);
	}

	// Setup bit stream
	SetBitStream(dst_input, dst_size);

	// Parse frame header
	auto result = ParseFrameHeader();
	if (result != Result::Success)
		return result;

	if (do_debug) {
		DebugLog("dst_coded=%d", header_.dst_coded ? 1 : 0);
	}

	// Check if raw DSD
	if (!header_.dst_coded) {
		// Raw DSD - copy directly
		const std::size_t copy_size = std::min(dst_size - 1, dsd_size);
		std::memcpy(dsd_output, dst_input + 1, copy_size);
		if (copy_size < dsd_size) {
			std::memset(dsd_output + copy_size, kDsdSilence, 
			            dsd_size - copy_size);
		}
		return Result::Success;
	}

	// Parse segmentation - CORRECT ORDER per ISO/IEC 14496-3 Table 10.5:
	// 1. Same_Segmentation flag (1 bit)
	// 2. Filter segmentation
	// 3. (if !Same_Segmentation) Ptable segmentation
	bool same_seg = GetBit();  // Same_Segmentation flag FIRST
	
	result = ParseSegmentation(filter_segment_, 32, 1);
	if (result != Result::Success)
		return result;

	if (same_seg) {
		ptable_segment_ = filter_segment_;
	} else {
		result = ParseSegmentation(ptable_segment_, 32, 1);
		if (result != Result::Success)
			return result;
	}

	// Parse mapping - CORRECT ORDER per ISO/IEC 14496-3 Table 10.8:
	// 1. Same_Mapping flag (1 bit)
	// 2. Filter mapping
	// 3. (if !Same_Mapping) Ptable mapping
	// 4. HalfProb flags for each channel
	bool same_map = GetBit();  // Same_Mapping flag FIRST
	
	result = ParseMapping(filter_segment_, header_.nr_of_filters);
	if (result != Result::Success)
		return result;

	if (same_map) {
		header_.nr_of_ptables = header_.nr_of_filters;
		for (uint32_t ch = 0; ch < channel_count_; ++ch) {
			ptable_segment_.table_for_segment[ch] = 
				filter_segment_.table_for_segment[ch];
		}
	} else {
		result = ParseMapping(ptable_segment_, header_.nr_of_ptables);
		if (result != Result::Success)
			return result;
	}

	// Half probability flags (per channel, after mapping)
	for (uint32_t ch = 0; ch < channel_count_; ++ch) {
		header_.half_prob[ch] = GetBit();
	}

	// Parse filter coefficients
	result = ParseFilterCoefs();
	if (result != Result::Success)
		return result;

	// Parse probability tables
	result = ParseProbabilityTables();
	if (result != Result::Success)
		return result;

	if (do_debug) {
		DebugLog("After parsing: nr_filters=%u, nr_ptables=%u",
		         header_.nr_of_filters, header_.nr_of_ptables);
		DebugLog("filter_seg: nr_segs[0]=%u, resolution=%u",
		         filter_segment_.nr_of_segments[0], filter_segment_.resolution);
		for (uint32_t f = 0; f < header_.nr_of_filters && f < 2; ++f) {
			DebugLog("filter[%u]: pred_order=%u, coef[0]=%d, coef[1]=%d",
			         f, header_.pred_order[f], header_.coef[f][0], header_.coef[f][1]);
		}
		for (uint32_t p = 0; p < header_.nr_of_ptables && p < 2; ++p) {
			DebugLog("ptable[%u]: len=%u, p_one[0]=%u, p_one[1]=%u",
			         p, header_.ptable_len[p], p_one_[p][0], p_one_[p][1]);
		}
		DebugLog("half_prob[0]=%d, nr_half_bits[0]=%u",
		         header_.half_prob[0] ? 1 : 0, header_.nr_of_half_bits[0]);
	}

	// Get arithmetic coded data position (in bits)
	const std::size_t ac_bit_pos = GetBitPosition();

	// Initialize filter tables and channel status for this frame
	InitFilterTables();
	InitChannelStatus();

	if (do_debug) {
		// Debug filter table values at key status indices
		DebugLog("FilterTable[0][0][0x00]=%d, [0xAA]=%d, [0xFF]=%d",
		         filter_table_[0][0][0x00], filter_table_[0][0][0xAA], filter_table_[0][0][0xFF]);
		// Debug channel status initial state
		DebugLog("Initial status[0]: %02x %02x %02x %02x %02x %02x %02x %02x",
		         channel_status_[0][0], channel_status_[0][1], channel_status_[0][2], channel_status_[0][3],
		         channel_status_[0][4], channel_status_[0][5], channel_status_[0][6], channel_status_[0][7]);
		// Debug: show what RunFilter returns for initial status
		int16_t init_predict = RunFilter(0, 0);
		DebugLog("Initial prediction for ch0 with filter0: %d", init_predict);
	}

	// Initialize arithmetic coder starting from current bit position
	const int total_bits = static_cast<int>(dst_size * 8);
	if (static_cast<int>(ac_bit_pos) >= total_bits) {
		// Not enough data for arithmetic decoding
		std::memset(dsd_output, kDsdSilence, dsd_size);
		return Result::Success;
	}
	AcInit(dst_input, total_bits, static_cast<int>(ac_bit_pos));

	if (do_debug) {
		DebugLog("AC init: ac_bit_pos=%zu, total_bits=%d", ac_bit_pos, total_bits);
		// Debug: show raw bytes around AC start position
		const std::size_t ac_byte_start = ac_bit_pos / 8;
		const unsigned ac_bit_offset = ac_bit_pos & 7;
		if (ac_byte_start + 4 <= dst_size) {
			DebugLog("AC raw bytes at byte %zu (bit offset %u): %02x %02x %02x %02x",
			         ac_byte_start, ac_bit_offset,
			         dst_input[ac_byte_start], dst_input[ac_byte_start+1],
			         dst_input[ac_byte_start+2], dst_input[ac_byte_start+3]);
		}
		DebugLog("AC state after init: code=0x%x, range=0x%x, bit_pos=%d",
		         ac_.code, ac_.range, ac_.bit_pos);
	}

	// Priming call - required to synchronize AC state
	// Reference does: AC.decodeBit_Decode(&ACError, reverse7LSBs(ICoefA[0][0]), ...)
	// The result is used for validation at end of frame (we skip that for now)
	const uint32_t priming_prob = Reverse7LSBs(header_.coef[0][0]);
	(void)AcDecodeBit(priming_prob);

	if (do_debug) {
		DebugLog("Priming: coef[0][0]=%d, reverse7LSBs=%u", header_.coef[0][0], priming_prob);
		DebugLog("AC state after priming: code=0x%x, range=0x%x, bit_pos=%d",
		         ac_.code, ac_.range, ac_.bit_pos);
	}

	// Build per-bit lookup tables for filter/ptable selection
	FillTable4Bit(filter_segment_, filter_4bit_);
	FillTable4Bit(ptable_segment_, ptable_4bit_);

	// Clear output buffer - we write every bit so initial value doesn't matter
	// Using 0x00 because we use |= to set bits (can only set 1s, not clear)
	std::memset(dsd_output, 0, dsd_size);

	// Decode frame
	const uint32_t bits_per_channel = frame_size_ * 8;

	for (uint32_t bit_nr = 0; bit_nr < bits_per_channel; ++bit_nr) {
		for (uint32_t ch = 0; ch < channel_count_; ++ch) {
			// Get filter and ptable from precomputed lookup tables
			const uint32_t filter_nr = GetNibble(filter_4bit_[ch].data(), bit_nr);
			const uint32_t ptable_nr = GetNibble(ptable_4bit_[ch].data(), bit_nr);

			// Calculate prediction
			const int16_t predict = RunFilter(filter_nr, ch);

			// Determine probability
			uint32_t probability;
			if (header_.half_prob[ch] && bit_nr < header_.nr_of_half_bits[ch]) {
				probability = 128; // 0.5 probability
			} else {
				const uint32_t ptable_idx = GetPtableIndex(predict, 
					header_.ptable_len[ptable_nr]);
				probability = p_one_[ptable_nr][ptable_idx];
			}

			// Decode bit
			const uint8_t residual = AcDecodeBit(probability);

			// Calculate output bit
			const int bit_val = ((static_cast<uint16_t>(predict) >> 15) ^ residual) & 1;

			// Debug first few bits - show AC state too
			if (do_debug && bit_nr < 8 && ch == 0) {
				DebugLog("bit[%u][%u]: filter=%u, predict=%d, prob=%u, res=%u, out=%d, AC(code=0x%x,range=0x%x)",
				         bit_nr, ch, filter_nr, predict, probability, residual, bit_val,
				         ac_.code, ac_.range);
			}

			// Store in output
			const std::size_t byte_idx = (bit_nr / 8) * channel_count_ + ch;
			const unsigned bit_idx = 7 - (bit_nr & 7);
			if (byte_idx < dsd_size) {
				dsd_output[byte_idx] |= static_cast<uint8_t>(bit_val << bit_idx);
			}

			// Update channel status
			UpdateChannelStatus(ch, bit_val);
		}
	}

	if (do_debug) {
		DebugLog("Output first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
		         dsd_output[0], dsd_output[1], dsd_output[2], dsd_output[3],
		         dsd_output[4], dsd_output[5], dsd_output[6], dsd_output[7]);
		DebugLog("Channel status[0]: %02x %02x %02x %02x",
		         channel_status_[0][0], channel_status_[0][1],
		         channel_status_[0][2], channel_status_[0][3]);
	}

	return Result::Success;
}

FrameDecoder::Result
FrameDecoder::ParseFrameHeader() noexcept
{
	header_.dst_coded = GetBit();

	if (!header_.dst_coded) {
		// Raw DSD frame - check reserved bits
		const uint32_t reserved = GetBits(7);
		if (reserved != 0) {
			return Result::InvalidHeader;
		}
	}

	return Result::Success;
}

FrameDecoder::Result
FrameDecoder::ParseSegmentation(Segment &seg, uint32_t max_segments,
                                [[maybe_unused]] uint32_t min_length) noexcept
{
	bool same_for_all = GetBit();

	seg.resolution = 1;
	bool resolution_read = false;

	if (same_for_all) {
		// Same segmentation for all channels
		uint32_t segment_nr = 0;

		while (!GetBit() && segment_nr < max_segments) {
			if (!resolution_read) {
				const uint32_t bits = Log2RoundUp(frame_size_);
				seg.resolution = GetBits(bits);
				if (seg.resolution == 0)
					seg.resolution = 1;
				resolution_read = true;
			}

			const uint32_t bits = Log2RoundUp(frame_size_ / seg.resolution);
			seg.segment_length[0][segment_nr] = GetBits(bits);
			++segment_nr;
		}

		seg.nr_of_segments[0] = segment_nr + 1;
		seg.segment_length[0][segment_nr] = 0;

		// Copy to all channels
		for (uint32_t ch = 1; ch < channel_count_; ++ch) {
			seg.nr_of_segments[ch] = seg.nr_of_segments[0];
			for (uint32_t s = 0; s <= segment_nr; ++s) {
				seg.segment_length[ch][s] = seg.segment_length[0][s];
			}
		}
	} else {
		// Different segmentation per channel
		for (uint32_t ch = 0; ch < channel_count_; ++ch) {
			uint32_t segment_nr = 0;

			while (!GetBit() && segment_nr < max_segments) {
				if (!resolution_read) {
					const uint32_t bits = Log2RoundUp(frame_size_);
					seg.resolution = GetBits(bits);
					if (seg.resolution == 0)
						seg.resolution = 1;
					resolution_read = true;
				}

				const uint32_t bits = Log2RoundUp(frame_size_ / seg.resolution);
				seg.segment_length[ch][segment_nr] = GetBits(bits);
				++segment_nr;
			}

			seg.nr_of_segments[ch] = segment_nr + 1;
			seg.segment_length[ch][segment_nr] = 0;
		}
	}

	return Result::Success;
}

FrameDecoder::Result
FrameDecoder::ParseMapping(Segment &seg, uint32_t &nr_of_tables) noexcept
{
	bool same_for_all = GetBit();
	uint32_t count_tables = 1;

	seg.table_for_segment[0][0] = 0;

	if (same_for_all) {
		for (uint32_t s = 1; s < seg.nr_of_segments[0]; ++s) {
			const uint32_t bits = Log2RoundUp(count_tables);
			const uint32_t table = GetBits(bits);
			seg.table_for_segment[0][s] = table;
			if (table == count_tables) {
				++count_tables;
			}
		}

		// Copy to all channels
		for (uint32_t ch = 1; ch < channel_count_; ++ch) {
			for (uint32_t s = 0; s < seg.nr_of_segments[ch]; ++s) {
				seg.table_for_segment[ch][s] = seg.table_for_segment[0][s];
			}
		}
	} else {
		for (uint32_t ch = 0; ch < channel_count_; ++ch) {
			for (uint32_t s = 0; s < seg.nr_of_segments[ch]; ++s) {
				if (ch != 0 || s != 0) {
					const uint32_t bits = Log2RoundUp(count_tables);
					const uint32_t table = GetBits(bits);
					seg.table_for_segment[ch][s] = table;
					if (table == count_tables) {
						++count_tables;
					}
				}
			}
		}
	}

	nr_of_tables = count_tables;
	return Result::Success;
}

FrameDecoder::Result
FrameDecoder::ParseFilterCoefs() noexcept
{
	for (uint32_t f = 0; f < header_.nr_of_filters; ++f) {
		// Prediction order (SIZE_CODEDPREDORDER=7 bits, +1) per DST spec
		header_.pred_order[f] = GetBits(7) + 1;

		// Coded flag
		const bool coded = GetBit();

		if (!coded) {
			// Direct coefficients
			for (uint32_t c = 0; c < header_.pred_order[f]; ++c) {
				header_.coef[f][c] = static_cast<int16_t>(GetSignedBits(9));
			}
		} else {
			// Rice coded coefficients
			const uint32_t method = GetBits(2);
			const uint32_t pred_coef_order = kFilterCPredOrder[method];

			// First coefficients direct
			for (uint32_t c = 0; c < pred_coef_order && c < header_.pred_order[f]; ++c) {
				header_.coef[f][c] = static_cast<int16_t>(GetSignedBits(9));
			}

			// Rice parameter
			const uint32_t m = GetBits(3);

			// Remaining coefficients - use CPredCoef for prediction!
			for (uint32_t c = pred_coef_order; c < header_.pred_order[f]; ++c) {
				int32_t x = 0;
				for (uint32_t t = 0; t < pred_coef_order; ++t) {
					x += kFilterCPredCoef[method][t] * header_.coef[f][c - t - 1];
				}

				int32_t delta = RiceDecode(m);
				int32_t coef;
				if (x >= 0) {
					coef = delta - (x + 4) / 8;
				} else {
					coef = delta + (-x + 3) / 8;
				}
				header_.coef[f][c] = static_cast<int16_t>(coef);
			}
		}
	}

	// Set half bits
	for (uint32_t ch = 0; ch < channel_count_; ++ch) {
		const uint32_t filter_nr = filter_segment_.table_for_segment[ch][0];
		header_.nr_of_half_bits[ch] = header_.pred_order[filter_nr];
	}

	return Result::Success;
}

FrameDecoder::Result
FrameDecoder::ParseProbabilityTables() noexcept
{
	for (uint32_t p = 0; p < header_.nr_of_ptables; ++p) {
		// Ptable length (AC_HISBITS=6 bits, +1) per DST spec
		header_.ptable_len[p] = GetBits(6) + 1;

		if (header_.ptable_len[p] > 1) {
			const bool coded = GetBit();

			if (!coded) {
				// Direct entries
				for (uint32_t e = 0; e < header_.ptable_len[p]; ++e) {
					p_one_[p][e] = GetBits(7) + 1;
				}
			} else {
				// Rice coded
				const uint32_t method = GetBits(2);
				const uint32_t pred_order = kPtableCPredOrder[method];

				// First entries direct
				for (uint32_t e = 0; e < pred_order && e < header_.ptable_len[p]; ++e) {
					p_one_[p][e] = GetBits(7) + 1;
				}

				// Rice parameter
				const uint32_t m = GetBits(3);

				// Remaining entries - use CPredCoef for prediction!
				for (uint32_t e = pred_order; e < header_.ptable_len[p]; ++e) {
					int32_t x = 0;
					for (uint32_t t = 0; t < pred_order; ++t) {
						x += kPtableCPredCoef[method][t] *
						     static_cast<int32_t>(p_one_[p][e - t - 1]);
					}

					int32_t delta = RiceDecode(m);
					int32_t value;
					if (x >= 0) {
						value = delta - (x + 4) / 8;
					} else {
						value = delta + (-x + 3) / 8;
					}

					// Clamp to valid range [1, 128]
					if (value < 1) value = 1;
					if (value > 128) value = 128;
					p_one_[p][e] = static_cast<uint32_t>(value);
				}
			}
		} else {
			p_one_[p][0] = 128;
		}
	}

	return Result::Success;
}

void
FrameDecoder::InitFilterTables() noexcept
{
	// Gray code optimized filter table computation
	// This matches the reference GC_InitCoefTables implementation.
	// 
	// The key insight is that consecutive Gray code values differ by exactly one bit.
	// We can incrementally compute filter table values by tracking which bit changed
	// and adjusting the previous value accordingly.
	//
	// The table is indexed by Gray code: value for pattern i is stored at gray(i).
	// Since RunFilter uses channel_status directly as index, and the channel_status
	// evolves through bit shifts, this Gray code indexing produces correct lookups.
	
	// Ensure Gray code tables are initialized
	InitGrayCodeTables();
	
	for (uint32_t f = 0; f < header_.nr_of_filters; ++f) {
		const uint32_t filter_length = header_.pred_order[f];

		for (uint32_t t = 0; t < 16; ++t) {
			// How many coefficients are in this 8-bit group?
			int k = static_cast<int>(filter_length) - static_cast<int>(t * 8);
			if (k > 8) k = 8;
			if (k < 0) k = 0;

			// Start with pattern 0: all bits are 0, so all contributions are -coef
			int cvalue = 0;
			for (int j = 0; j < k; ++j) {
				cvalue -= header_.coef[f][t * 8 + j];
			}
			filter_table_[f][t][0] = static_cast<int16_t>(cvalue);
			
			// For subsequent patterns, use Gray code incremental update
			for (int i = 1; i < 256; ++i) {
				const int i_gray = i ^ (i >> 1);  // Gray code of i
				const uint32_t j_gray = GC_ICoefIndex[i];  // Which bit changed
				
				if (j_gray < static_cast<uint32_t>(k)) {
					// Update: when bit flips 0->1, add 2*coef; when 1->0, subtract 2*coef
					cvalue += GC_ICoefSign[i] * (header_.coef[f][t * 8 + j_gray] << 1);
				}
				
				// Store at Gray code index
				filter_table_[f][t][i_gray] = static_cast<int16_t>(cvalue);
			}
		}
	}
}

void
FrameDecoder::InitChannelStatus() noexcept
{
	for (uint32_t ch = 0; ch < kMaxChannels; ++ch) {
		for (uint32_t t = 0; t < 16; ++t) {
			channel_status_[ch][t] = 0xAA; // Alternating bits
		}
	}
}

void
FrameDecoder::AcInit(const uint8_t *data, int total_bits, int start_bit) noexcept
{
	// Store arithmetic data for bit reading
	ac_.data = data;
	ac_.data_bits = total_bits;
	// Skip bit 0 (validation bit) - start from bit 1 of AC stream
	ac_.bit_pos = start_bit + 1;
	
	// Initialize range to ONE - 1 (4095 for 12-bit precision)
	ac_.range = kAcOne - 1;
	
	// Load initial ABITS (12) bits into code, reading bit by bit
	ac_.code = 0;
	for (unsigned i = 0; i < kAcTotalBits; ++i) {
		ac_.code <<= 1;
		if (ac_.bit_pos < ac_.data_bits) {
			ac_.code |= AcGetBit();
		}
	}
}

uint32_t
FrameDecoder::AcGetBit() noexcept
{
	// Read single bit from arithmetic coded data stream
	if (ac_.bit_pos >= ac_.data_bits)
		return 0;
	
	const int byte_idx = ac_.bit_pos >> 3;
	const int bit_idx = 7 - (ac_.bit_pos & 7);
	++ac_.bit_pos;
	
	return (ac_.data[byte_idx] >> bit_idx) & 1;
}

uint8_t
FrameDecoder::AcDecodeBit(uint32_t probability) noexcept
{
	// Calculate threshold with partial rounding per DST spec
	// Formula: ((A >> PBITS) | ((A >> (PBITS-1)) & 1)) * p
	const uint32_t scaled_range = (ac_.range >> kAcProbBits) | 
	                              ((ac_.range >> (kAcProbBits - 1)) & 1);
	const uint32_t threshold = ac_.range - scaled_range * probability;
	
	uint8_t bit;
	if (ac_.code >= threshold) {
		// Bit is 0
		bit = 0;
		ac_.code -= threshold;
		ac_.range = scaled_range * probability;
	} else {
		// Bit is 1  
		bit = 1;
		ac_.range = threshold;
	}
	
	// Renormalize: while range < HALF, shift left and read one bit
	while (ac_.range < kAcHalf) {
		ac_.range <<= 1;
		ac_.code <<= 1;
		// Read one bit from stream (insert 0 if past end)
		if (ac_.bit_pos < ac_.data_bits) {
			ac_.code |= AcGetBit();
		}
	}
	
	return bit;
}

int16_t
FrameDecoder::RunFilter(uint32_t filter_nr, uint32_t channel) noexcept
{
	// The filter table is indexed by coefficient group:
	// - filter_table[f][0] uses coef[0..7] (oldest coefficients)
	// - filter_table[f][15] uses coef[120..127] (newest coefficients)
	//
	// The channel status is stored with:
	// - channel_status[0] = most recent 8 bits (newest)
	// - channel_status[15] = oldest 8 bits
	//
	// Per DST spec: coef[0] multiplies the oldest bit, coef[N-1] the newest.
	// So we need to map:
	// - filter_table[f][0] (oldest coefs) with channel_status[15] (oldest bits)
	// - filter_table[f][15] (newest coefs) with channel_status[0] (newest bits)
	
	int16_t predict = 0;
	for (uint32_t t = 0; t < 16; ++t) {
		predict += filter_table_[filter_nr][t][channel_status_[channel][t]];
	}
	return predict;
}

uint32_t
FrameDecoder::GetPtableIndex(int16_t predict, uint32_t ptable_len) noexcept
{
	// Map prediction to ptable index per DST spec
	// AC_QSTEP = SIZE_PREDCOEF - AC_HISBITS = 9 - 6 = 3
	static constexpr unsigned kAcQstep = 3;
	
	const int32_t abs_predict = (predict >= 0) ? predict : -predict;
	uint32_t index = static_cast<uint32_t>(abs_predict >> kAcQstep);
	if (index >= ptable_len) {
		index = ptable_len - 1;
	}
	return index;
}

void
FrameDecoder::UpdateChannelStatus(uint32_t channel, int bit) noexcept
{
	// Shift all 16 bytes left by 1 bit, inserting new bit at the end
	// channel_status[0] = newest 8 bits (with newest bit in LSB)
	// channel_status[15] = oldest 8 bits (with oldest bit in MSB)
	//
	// After shift:
	// - The oldest bit (MSB of channel_status[15]) is discarded
	// - All other bits shift towards older positions
	// - The new bit becomes the newest (LSB of channel_status[0])
	
	auto *st = reinterpret_cast<uint64_t *>(channel_status_[channel].data());
	st[1] = (st[1] << 1) | (st[0] >> 63);
	st[0] = (st[0] << 1) | static_cast<uint64_t>(bit);
}

void
FrameDecoder::FillTable4Bit(const Segment &seg,
                            std::array<std::vector<uint8_t>, kMaxChannels> &table) noexcept
{
	const uint32_t bits_per_channel = frame_size_ * 8;

	for (uint32_t ch = 0; ch < channel_count_; ++ch) {
		// Allocate table: each byte holds 2 nibbles
		table[ch].resize((bits_per_channel + 1) / 2, 0);

		uint32_t start = 0;
		const uint32_t nr_segs = seg.nr_of_segments[ch];

		// Process all segments except the last
		for (uint32_t seg_nr = 0; seg_nr + 1 < nr_segs; ++seg_nr) {
			const uint8_t val = static_cast<uint8_t>(seg.table_for_segment[ch][seg_nr]);
			const uint32_t end = start + seg.resolution * 8 * seg.segment_length[ch][seg_nr];

			for (uint32_t bit_nr = start; bit_nr < end && bit_nr < bits_per_channel; ++bit_nr) {
				uint8_t *p = &table[ch][bit_nr / 2];
				const unsigned shift = (bit_nr & 1) << 2;  // 0 or 4
				*p = static_cast<uint8_t>((val << shift) | (*p & (0xF0 >> shift)));
			}
			start = end;
		}

		// Last segment fills remaining bits
		const uint8_t val = static_cast<uint8_t>(seg.table_for_segment[ch][nr_segs > 0 ? nr_segs - 1 : 0]);
		for (uint32_t bit_nr = start; bit_nr < bits_per_channel; ++bit_nr) {
			uint8_t *p = &table[ch][bit_nr / 2];
			const unsigned shift = (bit_nr & 1) << 2;
			*p = static_cast<uint8_t>((val << shift) | (*p & (0xF0 >> shift)));
		}
	}
}

uint32_t
FrameDecoder::GetNibble(const uint8_t *data, uint32_t index) noexcept
{
	// Extract 4-bit value from packed array
	// Even indices are in low nibble, odd in high nibble
	return (data[index >> 1] >> ((index & 1) << 2)) & 0x0F;
}

} // namespace Dst
