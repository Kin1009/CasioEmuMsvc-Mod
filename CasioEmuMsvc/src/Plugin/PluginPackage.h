#pragma once

#include "PluginManifest.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace casioemu::plugin {

struct Package {
	std::filesystem::path file;
	std::filesystem::path root;
	std::string sha256;
	Manifest manifest;
	bool valid = false;
	std::string code;
	std::string message;
};

// Hard limits applied while extracting a package.  A `.plugin` is untrusted
// input: cap entry count, per-entry size, total size and compression ratio.
struct ExtractionLimits {
	uint64_t maxEntries = 4096;
	uint64_t maxEntryBytes = 64ull * 1024 * 1024;
	uint64_t maxTotalBytes = 256ull * 1024 * 1024;
	// Entries larger than `ratioFloorBytes` must not exceed this ratio.
	uint64_t ratioFloorBytes = 1024 * 1024;
	uint64_t maxCompressionRatio = 200;
};

bool ComputeFileSha256(const std::filesystem::path& file, std::string& digest, std::string& error);

// Digest over the *extracted files* (sorted relative paths + bytes), stored
// beside them at extraction time.  The archive hash names the cache directory;
// this digest is what proves the files actually loaded are still the approved
// ones.  Symlinks are rejected and the digest file itself is not hashed.
extern const char* const kContentDigestFileName;
[[nodiscard]] bool ComputeTreeSha256(const std::filesystem::path& root, std::string& digest,
	std::string& error);
[[nodiscard]] bool VerifyPackageContent(const Package& package, std::string* error = nullptr);

bool ExtractPackage(const std::filesystem::path& file, const std::filesystem::path& cacheRoot, Package& out,
	const ExtractionLimits& limits = {});

} // namespace casioemu::plugin
