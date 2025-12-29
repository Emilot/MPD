// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/*
 * SacdMedia - Abstract media interface for SACD ISO reading
 *
 * Provides an abstraction layer for reading SACD ISO images,
 * supporting both file-based and stream-based access.
 */

#ifndef MPD_SACDISO_MEDIA_HXX
#define MPD_SACDISO_MEDIA_HXX

#include "ScarletBook.hxx"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace Sacd {

/**
 * Abstract base class for SACD media access.
 *
 * This interface abstracts the underlying storage mechanism,
 * allowing the SACD parser to work with both local files
 * and MPD input streams.
 */
class Media {
public:
	virtual ~Media() noexcept = default;

	/**
	 * Seek to an absolute position in the media.
	 *
	 * @param position Byte offset from the beginning
	 * @return true on success, false on failure
	 */
	[[nodiscard]]
	virtual bool Seek(uint64_t position) noexcept = 0;

	/**
	 * Read data from the current position.
	 *
	 * @param buffer Destination buffer
	 * @return Number of bytes actually read
	 */
	[[nodiscard]]
	virtual std::size_t Read(std::span<std::byte> buffer) noexcept = 0;

	/**
	 * Get the current position in the media.
	 *
	 * @return Current byte offset
	 */
	[[nodiscard]]
	virtual uint64_t GetPosition() const noexcept = 0;

	/**
	 * Get the total size of the media.
	 *
	 * @return Total size in bytes, or 0 if unknown
	 */
	[[nodiscard]]
	virtual uint64_t GetSize() const noexcept = 0;

	/**
	 * Check if the media is valid and ready for reading.
	 *
	 * @return true if media is ready
	 */
	[[nodiscard]]
	virtual bool IsValid() const noexcept = 0;

	/*
	 * Convenience methods
	 */

	/**
	 * Read data into a typed buffer.
	 */
	template<typename T>
	[[nodiscard]]
	std::size_t Read(T& data) noexcept {
		return Read(std::as_writable_bytes(std::span{&data, 1}));
	}

	/**
	 * Read exactly the requested number of bytes.
	 *
	 * @param buffer Destination buffer
	 * @return true if all bytes were read, false otherwise
	 */
	[[nodiscard]]
	bool ReadFull(std::span<std::byte> buffer) noexcept {
		return Read(buffer) == buffer.size();
	}

	/**
	 * Read a complete sector at the given LSN.
	 *
	 * @param lsn Logical Sector Number
	 * @param buffer Destination buffer (must be at least kLsnSize bytes)
	 * @return true on success
	 */
	[[nodiscard]]
	bool ReadSector(uint32_t lsn, std::span<std::byte> buffer) noexcept;

	/**
	 * Read multiple consecutive sectors.
	 *
	 * @param lsn Starting Logical Sector Number
	 * @param count Number of sectors to read
	 * @param buffer Destination buffer
	 * @return true on success
	 */
	[[nodiscard]]
	bool ReadSectors(uint32_t lsn, uint32_t count, std::span<std::byte> buffer) noexcept;
};

/**
 * File-based SACD media implementation.
 *
 * Reads SACD ISO images directly from the filesystem.
 */
class FileMedia final : public Media {
public:
	FileMedia() noexcept = default;
	~FileMedia() noexcept override;

	FileMedia(const FileMedia&) = delete;
	FileMedia& operator=(const FileMedia&) = delete;

	/**
	 * Open a file for reading.
	 *
	 * @param path Path to the SACD ISO file
	 * @return true on success
	 */
	[[nodiscard]]
	bool Open(const char* path) noexcept;

	/**
	 * Close the file.
	 */
	void Close() noexcept;

	// Media interface
	[[nodiscard]] bool Seek(uint64_t position) noexcept override;
	[[nodiscard]] std::size_t Read(std::span<std::byte> buffer) noexcept override;
	[[nodiscard]] uint64_t GetPosition() const noexcept override;
	[[nodiscard]] uint64_t GetSize() const noexcept override;
	[[nodiscard]] bool IsValid() const noexcept override;

private:
	void* file_handle_ = nullptr;
	uint64_t file_size_ = 0;
	uint64_t position_ = 0;
};

/**
 * Detect the sector size of an SACD image.
 *
 * SACD images can have either 2048-byte (LSN) or 2064-byte (PSN) sectors.
 * This function detects the format by looking for the SACDMTOC signature.
 *
 * @param media The media to check
 * @return The sector size (kLsnSize or kPsnSize), or 0 if not a valid SACD
 */
[[nodiscard]]
std::size_t DetectSectorSize(Media& media) noexcept;

} // namespace Sacd

#endif // MPD_SACDISO_MEDIA_HXX
