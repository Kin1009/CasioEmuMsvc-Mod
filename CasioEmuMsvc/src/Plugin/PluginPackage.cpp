#include "PluginPackage.h"

#include "miniz.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace casioemu::plugin {
namespace {

void SetError(Package& out, const std::string& code, const std::string& message) {
	out.valid = false;
	out.code = code;
	out.message = message;
}

std::string HexEncode(const unsigned char* data, size_t size) {
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string output;
	output.resize(size * 2);
	for (size_t i = 0; i < size; ++i) {
		output[i * 2] = kDigits[data[i] >> 4];
		output[i * 2 + 1] = kDigits[data[i] & 0xF];
	}
	return output;
}

} // namespace

const char* const kContentDigestFileName = ".plugin-content-sha256";

bool ComputeTreeSha256(const std::filesystem::path& root, std::string& digest, std::string& error) {
	digest.clear();
	std::error_code ec;
	if (!std::filesystem::is_directory(root, ec)) {
		error = "package content directory is missing";
		return false;
	}

	std::vector<std::string> names;
	for (std::filesystem::recursive_directory_iterator it(root, ec), end;
		!ec && it != end; it.increment(ec)) {
		const std::string name = it->path().lexically_relative(root).generic_string();
		if (name == kContentDigestFileName)
			continue;
		const auto status = std::filesystem::symlink_status(it->path(), ec);
		if (ec) {
			error = "cannot inspect package content: " + name + ": " + ec.message();
			return false;
		}
		if (std::filesystem::is_symlink(status)) {
			error = "symlink in package content: " + name;
			return false;
		}
		if (std::filesystem::is_regular_file(status))
			names.push_back(name);
		else if (!std::filesystem::is_directory(status)) {
			error = "non-regular file in package content: " + name;
			return false;
		}
	}
	if (ec) {
		error = "cannot enumerate package content: " + ec.message();
		return false;
	}
	std::sort(names.begin(), names.end());

	CC_SHA256_CTX context;
	CC_SHA256_Init(&context);
	std::array<char, 64 * 1024> buffer{};
	for (const auto& name : names) {
		CC_SHA256_Update(&context, name.data(), static_cast<CC_LONG>(name.size()));
		const char separator = '\0';
		CC_SHA256_Update(&context, &separator, 1);

		std::ifstream input(root / name, std::ios::binary);
		if (!input) {
			error = "cannot read package content: " + name;
			return false;
		}
		while (input.good()) {
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize read = input.gcount();
			if (read > 0)
				CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(read));
		}
		if (input.bad()) {
			error = "failed while reading package content: " + name;
			return false;
		}
	}

	unsigned char hash[CC_SHA256_DIGEST_LENGTH]{};
	CC_SHA256_Final(hash, &context);
	digest = HexEncode(hash, sizeof(hash));
	return true;
}

bool VerifyPackageContent(const Package& package, std::string* error) {
	const auto fail = [&](const std::string& message) {
		if (error)
			*error = message;
		return false;
	};
	// `valid` is assigned only after the manifest check below, so verification
	// must rely on the root path rather than that flag.
	if (package.root.empty())
		return fail("package has no extracted content");
	std::error_code link_ec;
	if (std::filesystem::is_symlink(std::filesystem::symlink_status(package.root, link_ec)))
		return fail("package content directory is a symlink");

	std::ifstream marker(package.root / kContentDigestFileName, std::ios::binary);
	if (!marker)
		return fail("package content digest is missing");
	std::string expected((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
	while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r'))
		expected.pop_back();
	if (expected.size() != 64)
		return fail("package content digest is malformed");

	std::string actual;
	std::string hash_error;
	if (!ComputeTreeSha256(package.root, actual, hash_error))
		return fail(hash_error);
	if (actual != expected)
		return fail("extracted package content changed since approval");
	if (error)
		error->clear();
	return true;
}

bool ComputeFileSha256(const std::filesystem::path& file, std::string& digest, std::string& error) {
	std::ifstream input(file, std::ios::binary);
	if (!input) {
		error = "cannot open package for hashing";
		return false;
	}

	CC_SHA256_CTX context;
	CC_SHA256_Init(&context);
	std::array<char, 64 * 1024> buffer{};
	while (input.good()) {
		input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const std::streamsize read = input.gcount();
		if (read > 0)
			CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(read));
	}
	if (input.bad()) {
		error = "failed while reading package";
		return false;
	}

	unsigned char hash[CC_SHA256_DIGEST_LENGTH]{};
	CC_SHA256_Final(hash, &context);
	digest = HexEncode(hash, sizeof(hash));
	return true;
}

bool ExtractPackage(const std::filesystem::path& file, const std::filesystem::path& cacheRoot, Package& out,
	const ExtractionLimits& limits) {
	out = {};
	out.file = file;

	std::string hash_error;
	if (!ComputeFileSha256(file, out.sha256, hash_error)) {
		SetError(out, "package.sha256", hash_error);
		return false;
	}

	const std::filesystem::path cache_dir = cacheRoot / out.sha256;
	std::error_code ec;
	// A cache directory named after a hash is not proof of its contents: an
	// attacker could edit or symlink it after the original approval.  Verify
	// the content digest recorded at extraction time, otherwise re-extract
	// from the hash-bound archive.
	const bool cached_symlink = std::filesystem::is_symlink(
		std::filesystem::symlink_status(cache_dir, ec));
	if (!ec && !cached_symlink && std::filesystem::exists(cache_dir / "plugin.json", ec)) {
		out.root = cache_dir;
		std::string content_error;
		if (VerifyPackageContent(out, &content_error)) {
			ManifestResult manifest = LoadManifestFile(cache_dir / "plugin.json");
			out.code = manifest.code;
			out.message = manifest.message;
			out.manifest = std::move(manifest.manifest);
			out.valid = manifest.ok;
			return out.valid;
		}
		out.root.clear();
	}

	const std::filesystem::path temp_dir = cache_dir.string() + ".tmp-" + std::to_string(::getpid());
	std::filesystem::remove_all(temp_dir, ec);
	std::filesystem::create_directories(temp_dir, ec);
	if (ec) {
		SetError(out, "package.path", "cannot create plugin cache: " + ec.message());
		return false;
	}

	mz_zip_archive archive{};
	if (!mz_zip_reader_init_file(&archive, file.string().c_str(), 0)) {
		std::filesystem::remove_all(temp_dir, ec);
		SetError(out, "package.invalid_zip", "the file is not a readable zip archive");
		return false;
	}

	const mz_uint file_count = mz_zip_reader_get_num_files(&archive);
	if (file_count > limits.maxEntries) {
		mz_zip_reader_end(&archive);
		std::filesystem::remove_all(temp_dir, ec);
		SetError(out, "package.invalid_zip", "archive has " + std::to_string(file_count) +
			" entries; the limit is " + std::to_string(limits.maxEntries));
		return false;
	}

	bool ok = true;
	uint64_t total_bytes = 0;
	// Exact-name and case-insensitive collision tracking.  macOS volumes are
	// usually case-insensitive, so "A.txt" and "a.txt" would silently
	// overwrite each other during extraction.
	std::set<std::string> exact_names;
	std::set<std::string> folded_names;

	for (mz_uint index = 0; index < file_count && ok; ++index) {
		mz_zip_archive_file_stat stat{};
		if (!mz_zip_reader_file_stat(&archive, index, &stat)) {
			SetError(out, "package.invalid_zip", "cannot read a zip entry");
			ok = false;
			break;
		}

		const std::string name = stat.m_filename;
		if (!IsSafeRelativePath(name)) {
			SetError(out, "package.path", "unsafe zip entry: " + name);
			ok = false;
			break;
		}

		// Reject duplicate and case-colliding paths before writing anything.
		std::string folded = name;
		for (char& c : folded)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (!exact_names.insert(name).second) {
			SetError(out, "package.path", "duplicate zip entry: " + name);
			ok = false;
			break;
		}
		if (!folded_names.insert(folded).second) {
			SetError(out, "package.path", "case-colliding zip entry: " + name);
			ok = false;
			break;
		}

		if (stat.m_is_encrypted || !stat.m_is_supported) {
			SetError(out, "package.invalid_zip", "encrypted or unsupported entry: " + name);
			ok = false;
			break;
		}

		const uint32_t mode = static_cast<uint32_t>(stat.m_external_attr >> 16);
		if (mode != 0) {
			// ZIP stores no portable hard-link marker; hard links appear as
			// special (non-regular, non-directory) Unix modes.  Symlinks and
			// device/FIFO/socket entries are all refused.
			if (S_ISLNK(mode)) {
				SetError(out, "package.path", "symlink entries are not allowed: " + name);
				ok = false;
				break;
			}
			if (!S_ISREG(mode) && !S_ISDIR(mode)) {
				SetError(out, "package.path", "hard-link or special file entries are not allowed: " + name);
				ok = false;
				break;
			}
		}

		const bool directory = mz_zip_reader_is_file_a_directory(&archive, index) || stat.m_is_directory;
		if (!directory) {
			if (stat.m_uncomp_size > limits.maxEntryBytes) {
				SetError(out, "package.invalid_zip", "entry is larger than " +
					std::to_string(limits.maxEntryBytes) + " bytes: " + name);
				ok = false;
				break;
			}
			if (total_bytes + stat.m_uncomp_size > limits.maxTotalBytes) {
				SetError(out, "package.invalid_zip", "archive expands beyond " +
					std::to_string(limits.maxTotalBytes) + " bytes");
				ok = false;
				break;
			}
			// Compression-ratio guard against zip bombs.  Small entries are
			// exempt because their ratio is dominated by header overhead.
			if (stat.m_uncomp_size > limits.ratioFloorBytes && stat.m_comp_size > 0 &&
				stat.m_uncomp_size / stat.m_comp_size > limits.maxCompressionRatio) {
				SetError(out, "package.invalid_zip", "entry exceeds the maximum compression ratio: " + name);
				ok = false;
				break;
			}
			total_bytes += stat.m_uncomp_size;
		}

		const std::filesystem::path destination = temp_dir / name;
		if (directory) {
			std::filesystem::create_directories(destination, ec);
			if (ec) {
				SetError(out, "package.path", "cannot create directory: " + ec.message());
				ok = false;
			}
			continue;
		}

		std::filesystem::create_directories(destination.parent_path(), ec);
		if (ec) {
			SetError(out, "package.path", "cannot create directory: " + ec.message());
			ok = false;
			break;
		}

		if (!mz_zip_reader_extract_to_file(&archive, index, destination.string().c_str(), 0)) {
			SetError(out, "package.invalid_zip", "cannot extract: " + name);
			ok = false;
			break;
		}

		// Defence in depth: nothing the archive can express should create a
		// link, but verify the extracted inode is a regular single-link file.
		struct stat extracted {};
		if (::lstat(destination.string().c_str(), &extracted) == 0) {
			if (!S_ISREG(extracted.st_mode)) {
				SetError(out, "package.path", "extracted entry is not a regular file: " + name);
				ok = false;
				break;
			}
			if (extracted.st_nlink > 1) {
				SetError(out, "package.path", "hard-linked entries are not allowed: " + name);
				ok = false;
				break;
			}
		}
	}
	mz_zip_reader_end(&archive);

	if (!ok) {
		std::filesystem::remove_all(temp_dir, ec);
		return false;
	}

	ManifestResult manifest = LoadManifestFile(temp_dir / "plugin.json");
	if (!manifest.ok) {
		std::filesystem::remove_all(temp_dir, ec);
		out.code = manifest.code;
		out.message = manifest.message;
		out.valid = false;
		return false;
	}

	// Record a digest of the extracted bytes so a later warm-cache hit can
	// prove the files are still the ones that came from this hash-bound archive.
	std::string content_digest;
	std::string content_error;
	if (!ComputeTreeSha256(temp_dir, content_digest, content_error)) {
		std::filesystem::remove_all(temp_dir, ec);
		SetError(out, "package.content", content_error);
		return false;
	}
	{
		std::ofstream marker(temp_dir / kContentDigestFileName, std::ios::binary | std::ios::trunc);
		if (!marker) {
			std::filesystem::remove_all(temp_dir, ec);
			SetError(out, "package.content", "cannot write package content digest");
			return false;
		}
		marker << content_digest << '\n';
	}

	std::filesystem::remove_all(cache_dir, ec);
	std::filesystem::rename(temp_dir, cache_dir, ec);
	if (ec) {
		std::filesystem::remove_all(temp_dir, ec);
		SetError(out, "package.path", "cannot commit plugin cache: " + ec.message());
		return false;
	}

	out.root = cache_dir;
	out.manifest = std::move(manifest.manifest);
	out.valid = true;
	return true;
}

} // namespace casioemu::plugin
