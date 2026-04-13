// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "DstDecoder.hxx"
#include "Domain.hxx"
#include "Log.hxx"
#include "libdstdec/binding/dst_decoder.h"

#include <cstring>

namespace Sacd {

static constexpr unsigned kDsd64FrameBytes = 4704;

class DstDecoder::Impl {
public:
	Dst::DecoderBinding decoder;
	unsigned channel_count = 0;
	unsigned frame_size = 0;
	bool initialized = false;
	unsigned decode_calls = 0;
	unsigned small_frames = 0;
	unsigned large_frames = 0;
};

DstDecoder::DstDecoder() noexcept
	: pimpl_(std::make_unique<Impl>())
{
}

DstDecoder::~DstDecoder() noexcept = default;

bool
DstDecoder::Initialize(unsigned channel_count, unsigned sample_rate) noexcept
{
	LogWarning(sacdiso_domain, "DST Initialize called");

	if (channel_count == 0 || channel_count > 6)
		return false;

	unsigned frame_bytes;
	switch (sample_rate) {
	case 2822400:
		frame_bytes = kDsd64FrameBytes;
		break;
	case 5644800:
		frame_bytes = kDsd64FrameBytes * 2;
		break;
	case 11289600:
		frame_bytes = kDsd64FrameBytes * 4;
		break;
	default:
		frame_bytes = kDsd64FrameBytes;
		break;
	}

	pimpl_->channel_count = channel_count;
	pimpl_->frame_size = frame_bytes * channel_count;

	LogWarning(sacdiso_domain, "DST calling decoder.Initialize");

	int result = pimpl_->decoder.Initialize(channel_count, frame_bytes);
	if (result != 0) {
		LogWarning(sacdiso_domain, "DST decoder.Initialize failed");
		return false;
	}

	pimpl_->initialized = true;
	LogWarning(sacdiso_domain, "DST decoder initialized OK");
	return true;
}

bool
DstDecoder::IsInitialized() const noexcept
{
	return pimpl_ && pimpl_->initialized;
}

bool
DstDecoder::Decode(std::span<const std::byte> dst_data,
                   std::vector<std::byte> &dsd_output) noexcept
{
	if (!pimpl_->initialized)
		return false;

	if (dst_data.empty()) {
		dsd_output.clear();
		return true;
	}

	++pimpl_->decode_calls;

	// Log first 10 frames, or every 100th frame
	if (pimpl_->decode_calls <= 10 || (pimpl_->decode_calls % 100 == 0)) {
		FmtWarning(sacdiso_domain, "DST decode[{}]: input_size={}, first_byte=0x{:02x}",
		           pimpl_->decode_calls, dst_data.size(),
		           static_cast<uint8_t>(dst_data[0]));
	}

	// For very small frames (< 20 bytes), they cannot contain valid DST data
	// Output DSD silence instead of trying to decode
	// IMPORTANT: Use 0xAA to match the initial channel status of the decoder
	// Using 0x55 here would cause discontinuity when real decoding starts
	if (dst_data.size() < 20) {
		++pimpl_->small_frames;
		if (pimpl_->decode_calls <= 10) {
			FmtWarning(sacdiso_domain, "DST decode[{}]: frame too small ({}), outputting 0xAA silence v2 (total small={})",
			           pimpl_->decode_calls, dst_data.size(), pimpl_->small_frames);
		}
		dsd_output.resize(pimpl_->frame_size);
		std::memset(dsd_output.data(), 0xAA, dsd_output.size()); // DSD silence - must match initial channel status
		return true;
	}
	
	++pimpl_->large_frames;
	if (pimpl_->decode_calls <= 10 || (pimpl_->decode_calls % 100 == 0)) {
		FmtWarning(sacdiso_domain, "DST decode[{}]: LARGE frame size={} (total large={})",
		           pimpl_->decode_calls, dst_data.size(), pimpl_->large_frames);
	}

	std::vector<uint8_t> buffer(
		reinterpret_cast<const uint8_t *>(dst_data.data()),
		reinterpret_cast<const uint8_t *>(dst_data.data()) + dst_data.size()
	);

	int result = pimpl_->decoder.Decode(buffer);

	if (pimpl_->decode_calls <= 10 || (pimpl_->decode_calls % 100 == 0)) {
		FmtWarning(sacdiso_domain, "DST decode[{}]: decoder returned result={}",
		           pimpl_->decode_calls, result);
	}

	if (result <= 0) {
		// Decode failed or returned empty - output silence
		// Use 0xAA to match channel status
		if (pimpl_->decode_calls <= 10) {
			FmtWarning(sacdiso_domain, "DST decode[{}]: decode failed (result={}), outputting silence",
			           pimpl_->decode_calls, result);
		}
		dsd_output.resize(pimpl_->frame_size);
		std::memset(dsd_output.data(), 0xAA, dsd_output.size()); // DSD silence - must match channel status
		return true;
	}

	dsd_output.resize(static_cast<std::size_t>(result));
	std::memcpy(dsd_output.data(), buffer.data(), static_cast<std::size_t>(result));
	return true;
}

std::size_t
DstDecoder::GetOutputFrameSize() const noexcept
{
	return pimpl_->initialized ? pimpl_->frame_size : 0;
}

void
DstDecoder::Reset() noexcept
{
	if (pimpl_->initialized) {
		pimpl_->decoder.Flush();
		pimpl_->decode_calls = 0;
	}
}

} // namespace Sacd