// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "SacdMedia.hxx"
#include "Domain.hxx"
#include "Log.hxx"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace Sacd {

/*
 * Media base class
 */

bool
Media::ReadSector(uint32_t lsn, std::span<std::byte> buffer) noexcept
{
	if (buffer.size() < kLsnSize)
		return false;

	if (!Seek(static_cast<uint64_t>(lsn) * kLsnSize))
		return false;

	return Read(buffer.first(kLsnSize)) == kLsnSize;
}

bool
Media::ReadSectors(uint32_t lsn, uint32_t count, std::span<std::byte> buffer) noexcept
{
	const std::size_t total_size = static_cast<std::size_t>(count) * kLsnSize;
	if (buffer.size() < total_size)
		return false;

	if (!Seek(static_cast<uint64_t>(lsn) * kLsnSize))
		return false;

	return Read(buffer.first(total_size)) == total_size;
}

/*
 * FileMedia implementation
 */

FileMedia::~FileMedia() noexcept
{
	Close();
}

bool
FileMedia::Open(const char* path) noexcept
{
	Close();

	FILE* f = std::fopen(path, "rb");
	if (f == nullptr) {
		FmtDebug(sacdiso_domain, "Failed to open file: {}", path);
		return false;
	}

	// Get file size
#ifdef _WIN32
	if (_fseeki64(f, 0, SEEK_END) != 0) {
		std::fclose(f);
		return false;
	}
	const auto size = _ftelli64(f);
#else
	if (fseeko(f, 0, SEEK_END) != 0) {
		std::fclose(f);
		return false;
	}
	const auto size = ftello(f);
#endif

	if (size < 0) {
		std::fclose(f);
		return false;
	}

#ifdef _WIN32
	if (_fseeki64(f, 0, SEEK_SET) != 0) {
#else
	if (fseeko(f, 0, SEEK_SET) != 0) {
#endif
		std::fclose(f);
		return false;
	}

	file_handle_ = f;
	file_size_ = static_cast<uint64_t>(size);
	position_ = 0;

	return true;
}

void
FileMedia::Close() noexcept
{
	if (file_handle_ != nullptr) {
		std::fclose(static_cast<FILE*>(file_handle_));
		file_handle_ = nullptr;
		file_size_ = 0;
		position_ = 0;
	}
}

bool
FileMedia::Seek(uint64_t position) noexcept
{
	if (file_handle_ == nullptr)
		return false;

#ifdef _WIN32
	if (_fseeki64(static_cast<FILE*>(file_handle_),
	              static_cast<__int64>(position), SEEK_SET) != 0)
		return false;
#else
	if (fseeko(static_cast<FILE*>(file_handle_),
	           static_cast<off_t>(position), SEEK_SET) != 0)
		return false;
#endif

	position_ = position;
	return true;
}

std::size_t
FileMedia::Read(std::span<std::byte> buffer) noexcept
{
	if (file_handle_ == nullptr)
		return 0;

	const std::size_t bytes_read = std::fread(buffer.data(), 1, buffer.size(),
	                                          static_cast<FILE*>(file_handle_));
	position_ += bytes_read;
	return bytes_read;
}

uint64_t
FileMedia::GetPosition() const noexcept
{
	return position_;
}

uint64_t
FileMedia::GetSize() const noexcept
{
	return file_size_;
}

bool
FileMedia::IsValid() const noexcept
{
	return file_handle_ != nullptr;
}

/*
 * Utility functions
 */

std::size_t
DetectSectorSize(Media& media) noexcept
{
	std::array<char, 8> signature{};

	// Try 2048-byte sectors (standard ISO)
	if (media.Seek(static_cast<uint64_t>(kMasterTocStart) * kLsnSize)) {
		if (media.Read(std::as_writable_bytes(std::span{signature})) == 8) {
			if (std::memcmp(signature.data(), "SACDMTOC", 8) == 0)
				return kLsnSize;
		}
	}

	// Try 2064-byte sectors (raw/physical)
	// The signature is at offset +12 within each sector
	if (media.Seek(static_cast<uint64_t>(kMasterTocStart) * kPsnSize + 12)) {
		if (media.Read(std::as_writable_bytes(std::span{signature})) == 8) {
			if (std::memcmp(signature.data(), "SACDMTOC", 8) == 0)
				return kPsnSize;
		}
	}

	return 0; // Not a valid SACD image
}

} // namespace Sacd
