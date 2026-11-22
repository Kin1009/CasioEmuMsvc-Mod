#pragma once

// ---------------------------------------------------------------------------
// PlatformTags.h -- OS / CPU-architecture tags for multi-platform .plugin files
// ---------------------------------------------------------------------------
//
// A `.plugin` ZIP may ship raw native binaries for several OS/architecture
// combinations.  Files follow the pattern:
//
//     <name>-<os>-<arch>.<ext>
//
// Examples:
//     my-plugin1-macos-x64.dylib
//     my-plugin2-macos-arm64.dylib
//     my-plugin1-windows-x64.dll
//     my-plugin1-linux-arm64.so
//     my-plugin1-android-arm64-android.so
//     my-plugin-any-any.so            (wildcard OS + wildcard architecture)
//
// Selection is EXCLUSIVE, with a documented precedence (highest first):
//
//     1. exact OS + exact arch   e.g. CG100SH4-macos-x64.dylib on macos-x64
//     2. exact OS + arch "any"   e.g. CG100SH4-macos-any.dylib
//     3. OS "any" + exact arch   e.g. CG100SH4-any-x64.dylib
//     4. OS "any" + arch "any"   e.g. CG100SH4-any-any.dylib
//
// Only binaries in the single best-matching tier are handed to dlopen.  This
// prevents an "any-*" fallback shipped alongside a concrete binary from being
// loaded as well (which would double-register fPluginLoad/fPluginLoadArch and
// may produce a wrong-architecture dlopen error).  Several binaries TIED at
// the best tier are still all returned, so a host may legitimately ship more
// than one module for its exact platform.
//
// If a package contains platform-tagged binaries but NONE match the host, the
// plugin is rejected with the message produced by NoCompatibleBinaryError(),
// e.g. "No native binary for macos-x64".
//
// Tags (kept deliberately small and stable):
//   OS   : macos | linux | windows | android | ipados | ios | any
//   ARCH : x64 | x32 | arm64 | arm64-android | any
//
//   * x64            -> 64-bit x86 (AMD64 / Intel64), desktop/laptop
//   * x32            -> 32-bit x86 (IA-32)
//   * arm64          -> 64-bit ARM on laptops/desktops (Apple Silicon,
//                       Windows on ARM, Linux aarch64)
//   * arm64-android  -> 64-bit ARM on Android phones/tablets
//   * any            -> wildcard, matches every build
//
// The current build's OS/arch are exposed as CASIOEMU_PLATFORM_OS and
// CASIOEMU_PLATFORM_ARCH.  A build system may override them with
// `target_compile_definitions(... CASIOEMU_PLATFORM_OS="macos" ...)`;
// otherwise this header derives them from the compiler's own predefines so it
// is correct on every host without any build-system changes.
//
// The host owns the ABI function names (see ArchCoreABI.h); a plugin merely
// implements them.  This header only decides WHICH binaries to hand to dlopen.

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace casioemu::plugin {

// ---------------------------------------------------------------------------
// Current build platform.  A build system may override the tags with valid
// C string literals (cmake/CasioPlatform.cmake does this for cross-target
// builds such as ipados/ios).  Only fall back to the compiler's own target
// predefines when no override was supplied.
// ---------------------------------------------------------------------------

#ifndef CASIOEMU_PLATFORM_OS
#if defined(__ANDROID__)
#define CASIOEMU_PLATFORM_OS "android"
#elif defined(_WIN32)
#define CASIOEMU_PLATFORM_OS "windows"
#elif defined(__APPLE__)
#define CASIOEMU_PLATFORM_OS "macos"
#elif defined(__linux__)
#define CASIOEMU_PLATFORM_OS "linux"
#else
#define CASIOEMU_PLATFORM_OS "any"
#endif
#endif

#ifndef CASIOEMU_PLATFORM_ARCH
#if defined(__ANDROID__)
#if defined(__aarch64__) || defined(_M_ARM64)
#define CASIOEMU_PLATFORM_ARCH "arm64-android"
#else
// No dedicated 32-bit-ARM-Android tag in the format; treat as wildcard so a
// package can still supply an "any" binary for such a device.
#define CASIOEMU_PLATFORM_ARCH "any"
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define CASIOEMU_PLATFORM_ARCH "arm64"
#elif defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#define CASIOEMU_PLATFORM_ARCH "x64"
#elif defined(__i386__) || defined(_M_IX86)
#define CASIOEMU_PLATFORM_ARCH "x32"
#else
#define CASIOEMU_PLATFORM_ARCH "any"
#endif
#endif

// ---------------------------------------------------------------------------
// Known tags and helper constants.
// ---------------------------------------------------------------------------
inline constexpr std::string_view kKnownOsTags[] = {
	"macos", "linux", "windows", "android", "ipados", "ios", "any",
};
inline constexpr std::string_view kKnownArchTags[] = {
	"x64", "x32", "arm64", "arm64-android", "any",
};

inline constexpr std::string_view kOsMacos = "macos";
inline constexpr std::string_view kOsLinux = "linux";
inline constexpr std::string_view kOsWindows = "windows";
inline constexpr std::string_view kOsAndroid = "android";
inline constexpr std::string_view kOsAny = "any";

inline constexpr std::string_view kArchX64 = "x64";
inline constexpr std::string_view kArchX32 = "x32";
inline constexpr std::string_view kArchArm64 = "arm64";
inline constexpr std::string_view kArchArm64Android = "arm64-android";
inline constexpr std::string_view kArchAny = "any";

inline bool IsKnownOsTag(std::string_view tag) {
	for (std::string_view known : kKnownOsTags)
		if (known == tag)
			return true;
	return false;
}

inline bool IsKnownArchTag(std::string_view tag) {
	for (std::string_view known : kKnownArchTags)
		if (known == tag)
			return true;
	return false;
}

inline const char* OsTagDescription(std::string_view tag) {
	if (tag == "macos") return "Apple macOS (desktop / laptop)";
	if (tag == "linux") return "Linux";
	if (tag == "windows") return "Microsoft Windows";
	if (tag == "android") return "Android";
	// Data-only script plugins are the first type that can legitimately list
	// these tags: no dlopen/native code is involved on either platform.
	if (tag == "ipados") return "Apple iPadOS (script plugins only)";
	if (tag == "ios") return "Apple iOS (script plugins only)";
	if (tag == "any") return "Any operating system (wildcard)";
	return "Unknown OS tag";
}

inline const char* ArchTagDescription(std::string_view tag) {
	if (tag == "x64") return "64-bit x86 (AMD64 / Intel64)";
	if (tag == "x32") return "32-bit x86 (IA-32)";
	if (tag == "arm64") return "64-bit ARM on laptops / desktops (Apple Silicon, Windows ARM, Linux aarch64)";
	if (tag == "arm64-android") return "64-bit ARM on Android phones / tablets";
	if (tag == "any") return "Any CPU architecture (wildcard)";
	return "Unknown architecture tag";
}

// ---------------------------------------------------------------------------
// Matching a file's tags against a build.  "any" is a wildcard on either side.
// ---------------------------------------------------------------------------
inline bool OsTagMatches(std::string_view tag, std::string_view buildOs = CASIOEMU_PLATFORM_OS) {
	if (tag == "any" || buildOs == "any")
		return true;
	return tag == buildOs;
}

inline bool ArchTagMatches(std::string_view tag, std::string_view buildArch = CASIOEMU_PLATFORM_ARCH) {
	if (tag == "any" || buildArch == "any")
		return true;
	return tag == buildArch;
}

inline bool PlatformMatchesBuildImpl(std::string_view os, std::string_view arch,
	std::string_view buildOs, std::string_view buildArch) {
	return OsTagMatches(os, buildOs) && ArchTagMatches(arch, buildArch);
}

inline bool PlatformMatchesBuild(std::string_view os, std::string_view arch) {
	return PlatformMatchesBuildImpl(os, arch, CASIOEMU_PLATFORM_OS, CASIOEMU_PLATFORM_ARCH);
}

// ---------------------------------------------------------------------------
// Precedence ranking for a matching (os, arch) tag pair.  Higher is better;
// -1 means the pair does not match the build at all.  See the header comment
// for the four tiers.  Wildcards ("any") only win when no more specific tier
// is present, so a concrete binary and its any-* fallback never both load.
// ---------------------------------------------------------------------------
inline int PlatformMatchRank(std::string_view os, std::string_view arch,
	std::string_view buildOs, std::string_view buildArch) {
	if (!PlatformMatchesBuildImpl(os, arch, buildOs, buildArch))
		return -1;
	// When the *build* tag is "any" (unknown target OS/ABI) the wildcard rule
	// in OsTagMatches/ArchTagMatches makes every tag match.  Prefer an explicit
	// "any" tag on that side so a universal fallback wins exclusively instead
	// of tying with (and loading beside) concrete binaries.  Preserve the
	// lower ranks so concrete binaries still work as a last resort.
	const bool osExact = os != kOsAny && os == buildOs;
	const bool archExact = arch != kArchAny && arch == buildArch;
	const bool osBest = buildOs == kOsAny ? os == kOsAny : osExact;
	const bool archBest = buildArch == kArchAny ? arch == kArchAny : archExact;
	if (osBest && archBest)
		return 4;  // exact os + exact arch (or explicit any on a wildcard build)
	if (osBest)
		return 3;  // exact os, wildcard/unknown arch
	if (archBest)
		return 2;  // wildcard/unknown os, exact arch
	return 1;      // both wildcard/unknown
}

// "macos-x64", "windows-arm64", ... -- used in logs and error messages.
inline std::string MakePlatformKey(std::string_view os, std::string_view arch) {
	return std::string(os) + "-" + std::string(arch);
}

inline std::string CurrentPlatformKey() {
	return MakePlatformKey(CASIOEMU_PLATFORM_OS, CASIOEMU_PLATFORM_ARCH);
}

// Clear, host-independent mismatch message.  Shared by the registry, the
// early architecture pass and the verification harness so the exact same text
// is always reported.
inline std::string NoCompatibleBinaryError(
	std::string_view os = CASIOEMU_PLATFORM_OS,
	std::string_view arch = CASIOEMU_PLATFORM_ARCH) {
	return "No native binary for " + MakePlatformKey(os, arch);
}

// ---------------------------------------------------------------------------
// Tagged filename parsing.  Pattern: <name>-<os>-<arch>.<ext>
// The arch tag may itself contain a hyphen ("arm64-android"), so tags are
// matched as known suffixes rather than by splitting on '-'.
// ---------------------------------------------------------------------------
struct TaggedNativeFile {
	std::string name;  // base name before the -os-arch suffix
	std::string os;
	std::string arch;
	std::string ext;   // file extension, including the leading '.' (e.g. ".dylib")
};

inline bool IsNativeLibraryExtension(std::string_view ext) {
	return ext == ".dylib" || ext == ".so" || ext == ".dll" || ext == ".plugin";
}

inline std::optional<TaggedNativeFile> ParseTaggedNativeFileName(std::string_view fileName) {
	const size_t dot = fileName.rfind('.');
	if (dot == std::string_view::npos || dot == 0 || dot + 1 >= fileName.size())
		return std::nullopt;
	const std::string_view stem = fileName.substr(0, dot);
	const std::string extension(fileName.substr(dot));

	for (std::string_view os : kKnownOsTags) {
		for (std::string_view arch : kKnownArchTags) {
			std::string suffix = "-";
			suffix += os;
			suffix += '-';
			suffix += arch;
			if (stem.size() > suffix.size() &&
				stem.substr(stem.size() - suffix.size()) == suffix) {
				return TaggedNativeFile{
					std::string(stem.substr(0, stem.size() - suffix.size())),
					std::string(os),
					std::string(arch),
					extension,
				};
			}
		}
	}
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// Scanning an extracted package and selecting this host's binaries.
// ---------------------------------------------------------------------------
struct PlatformBinarySelection {
	enum class Status {
		NoTaggedBinaries,  // no platform-tagged files: use legacy native_entry
		Matched,           // one or more tagged files match this host
		NoMatch,           // tagged files exist, but none match -> error
	};

	Status status = Status::NoTaggedBinaries;
	// Absolute paths of the best-matching native binaries only: every entry is
	// in the highest precedence tier that matched this host (see
	// PlatformMatchRank).  The caller loads exactly these.
	std::vector<std::filesystem::path> matched;
	// Precedence rank of each entry in `matched` (parallel vector).
	std::vector<int> matchedRanks;
	// Highest rank among all matching binaries, or -1 if none matched.
	int bestRank = -1;
	// "<os>-<arch>" keys of all tagged binaries, for diagnostics.
	std::vector<std::string> available;
};

inline PlatformBinarySelection SelectPlatformBinariesForRoot(
	const std::filesystem::path& root,
	std::string_view buildOs = CASIOEMU_PLATFORM_OS,
	std::string_view buildArch = CASIOEMU_PLATFORM_ARCH) {
	PlatformBinarySelection selection;
	std::error_code ec;
	if (!std::filesystem::is_directory(root, ec))
		return selection;

	std::vector<int> allRanks;
	for (auto& item : std::filesystem::recursive_directory_iterator(root, ec)) {
		if (ec)
			break;
		std::error_code file_ec;
		if (!item.is_regular_file(file_ec))
			continue;
		const auto parsed = ParseTaggedNativeFileName(item.path().filename().string());
		if (!parsed || !IsNativeLibraryExtension(parsed->ext))
			continue;
		const std::string key = MakePlatformKey(parsed->os, parsed->arch);
		if (std::find(selection.available.begin(), selection.available.end(), key) == selection.available.end())
			selection.available.push_back(key);
		const int rank = PlatformMatchRank(parsed->os, parsed->arch, buildOs, buildArch);
		if (rank >= 0) {
			selection.matched.push_back(item.path());
			allRanks.push_back(rank);
		}
	}

	if (!allRanks.empty())
		selection.bestRank = *std::max_element(allRanks.begin(), allRanks.end());

	// Keep ONLY the best-precedence tier.  A concrete binary must not be
	// loaded together with a lower-priority any-* fallback.
	if (selection.bestRank >= 0) {
		std::vector<std::filesystem::path> bestMatches;
		std::vector<int> bestRanks;
		for (size_t i = 0; i < selection.matched.size(); ++i) {
			if (allRanks[i] == selection.bestRank) {
				bestMatches.push_back(selection.matched[i]);
				bestRanks.push_back(allRanks[i]);
			}
		}
		selection.matched = std::move(bestMatches);
		selection.matchedRanks = std::move(bestRanks);
	}

	std::sort(selection.matched.begin(), selection.matched.end());
	std::sort(selection.matchedRanks.begin(), selection.matchedRanks.end());
	std::sort(selection.available.begin(), selection.available.end());

	if (selection.available.empty())
		selection.status = PlatformBinarySelection::Status::NoTaggedBinaries;
	else if (selection.matched.empty())
		selection.status = PlatformBinarySelection::Status::NoMatch;
	else
		selection.status = PlatformBinarySelection::Status::Matched;
	return selection;
}

// Short, human-readable name of a precedence tier (for logs and diagnostics).
inline const char* PlatformMatchRankName(int rank) {
	switch (rank) {
	case 4: return "exact OS + exact arch";
	case 3: return "exact OS + any arch";
	case 2: return "any OS + exact arch";
	case 1: return "any OS + any arch";
	default: return "no match";
	}
}

} // namespace casioemu::plugin
