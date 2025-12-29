// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * DstDecoderBinding - High-level DST decoder implementation
 */

#include "dst_decoder.h"
#include "../decoder/decoder.h"

#include <cstring>

namespace Dst {

DecoderBinding::DecoderBinding() noexcept = default;

DecoderBinding::~DecoderBinding() noexcept = default;

int
DecoderBinding::Initialize(uint32_t channels, uint32_t frame_size) noexcept
{
	decoder_ = std::make_unique<FrameDecoder>();
	if (!decoder_)
		return -1;

	if (!decoder_->Initialize(channels, frame_size))
		return -1;

	channel_count_ = channels;
	frame_size_ = frame_size;
	output_buffer_.resize(decoder_->GetOutputSize());

	return 0;
}

int
DecoderBinding::Decode(std::vector<uint8_t> &dst_data) noexcept
{
	if (!decoder_ || !decoder_->IsInitialized())
		return 0;

	if (dst_data.empty())
		return 0;

	const auto result = decoder_->Decode(
		dst_data.data(), dst_data.size(),
		output_buffer_.data(), output_buffer_.size()
	);

	if (result != FrameDecoder::Result::Success)
		return 0;

	// Copy output to input buffer (API requirement)
	dst_data.resize(output_buffer_.size());
	std::memcpy(dst_data.data(), output_buffer_.data(), output_buffer_.size());

	return static_cast<int>(dst_data.size());
}

void
DecoderBinding::Flush() noexcept
{
	if (decoder_) {
		decoder_->Reset();
	}
}

bool
DecoderBinding::IsInitialized() const noexcept
{
	return decoder_ && decoder_->IsInitialized();
}

} // namespace Dst
