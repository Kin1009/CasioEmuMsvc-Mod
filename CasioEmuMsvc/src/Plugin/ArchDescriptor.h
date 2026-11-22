#pragma once
// ArchDescriptor.h -- data-driven architecture-board descriptor.
//
// A non-nX "board" model (SH4 and any future architecture) is described
// ENTIRELY BY DATA in its config.bin.  A non-empty `board_path` marks the model
// as a board model, and the `extra` string map carries the optional
// architecture descriptor keys below.  The host never special-cases a board by
// hardware id or by model name, so adding e.g. an fx-CG50 board is a data-only
// change (a new model pack), NOT a code change.
//
// Optional config `extra` keys (defaults preserve the original CG100 pack):
//   arch_provider           provider GetId() string.  Legacy packs that only
//                           set board_path fall back to "cg100sh4".
//   arch_architecture       free-form architecture tag, e.g. "sh4" (default sh4)
//   arch_fb_width           framebuffer width in pixels   (default 396)
//   arch_fb_height          framebuffer height in pixels  (default 224)
//   arch_cycles_per_second  host cycle budget              (default 200000000)
//   arch_cabi_prefix        prefix of optional C ABI counters (default "cg100_")
//   arch_power_key_row      ON/contact matrix row          (default 6)
//   arch_power_key_col      ON/contact matrix column       (default 9)
//
// Hardware-id note: legacy CG100 packs were authored with hardware_id 10,
// which these trees use for HW_EPS6009.  ModelInfo::Read() remaps ANY board
// model to the reserved HW_ARCH id (14) so no EPS peripheral path ever sees a
// board model.  The architecture PROVIDER is selected by `arch_provider` (with
// a hardware-id fallback for old plugins), never by a hardcoded CG100 id.
//
// Header-only on purpose: both ports glob Plugin/*.cpp WITHOUT CONFIGURE_DEPENDS,
// so a new translation unit would require a CMake reconfigure.

#include "ModelInfo.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace casioemu {
namespace arch {

	inline constexpr const char* kLegacyProviderId = "cg100sh4";
	inline constexpr const char* kDefaultArchitecture = "sh4";
	inline constexpr unsigned kDefaultFramebufferWidth = 396;
	inline constexpr unsigned kDefaultFramebufferHeight = 224;
	inline constexpr unsigned kDefaultCycleBudget = 200000000u;   // 200 MIPS
	inline constexpr const char* kDefaultCabiPrefix = "cg100_";
	inline constexpr int kDefaultPowerKeyRow = 6;
	inline constexpr int kDefaultPowerKeyCol = 9;

	struct Descriptor {
		std::string provider_id;      // IArchitectureProvider::GetId() to dispatch to
		std::string architecture;     // informational tag, e.g. "sh4"
		std::string cabi_prefix;      // prefix of optional C ABI counter exports
		unsigned fb_width = 0;        // 0 => per-architecture default
		unsigned fb_height = 0;
		unsigned cycles_per_second = 0;
		int power_key_row = kDefaultPowerKeyRow;
		int power_key_col = kDefaultPowerKeyCol;
		bool from_legacy_pack = false; // true when inferred from board_path alone

		bool valid() const { return !provider_id.empty(); }
	};

	namespace detail {
		inline const std::string* Find(const ModelInfo& model, const char* key) {
			const auto it = model.extra.find(key);
			return it == model.extra.end() ? nullptr : &it->second;
		}
		inline unsigned Unsigned(const ModelInfo& model, const char* key, unsigned fallback) {
			const std::string* value = Find(model, key);
			if (!value || value->empty())
				return fallback;
			try {
				std::size_t pos = 0;
				const unsigned long long parsed = std::stoull(*value, &pos, 0);
				if (pos != value->size() || parsed == 0 || parsed > 0xFFFFFFFFull)
					return fallback;
				return static_cast<unsigned>(parsed);
			}
			catch (...) {
				return fallback;
			}
		}
		inline int Integer(const ModelInfo& model, const char* key, int fallback) {
			const std::string* value = Find(model, key);
			if (!value || value->empty())
				return fallback;
			try {
				std::size_t pos = 0;
				const long parsed = std::stol(*value, &pos, 0);
				if (pos != value->size() || parsed < -1 || parsed > 0xFFFF)
					return fallback;
				return static_cast<int>(parsed);
			}
			catch (...) {
				return fallback;
			}
		}
		// FA-05 (AUDITALL P3-3): the prefix is concatenated into dlsym symbol
		// names by CabiSymbol(). Unvalidated config text could inject arbitrary
		// symbol names, so accept only C-identifier characters.
		inline bool ValidCabiPrefix(const std::string& value) {
			if (value.empty() || !std::isalpha(static_cast<unsigned char>(value[0])) &&
				value[0] != '_')
				return false;
			for (char c : value)
				if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
					return false;
			return true;
		}
	}

	/// Build the architecture descriptor for a model. Never throws.
	inline Descriptor FromModel(const ModelInfo& model) {
		Descriptor d;
		if (const std::string* p = detail::Find(model, "arch_provider"))
			d.provider_id = *p;
		if (const std::string* p = detail::Find(model, "arch_architecture"))
			d.architecture = *p;
		if (const std::string* p = detail::Find(model, "arch_cabi_prefix")) {
			// Invalid text falls through to kDefaultCabiPrefix below.
			if (detail::ValidCabiPrefix(*p))
				d.cabi_prefix = *p;
		}

		d.fb_width = detail::Unsigned(model, "arch_fb_width", 0);
		d.fb_height = detail::Unsigned(model, "arch_fb_height", 0);
		d.cycles_per_second = detail::Unsigned(model, "arch_cycles_per_second", 0);
		d.power_key_row = detail::Integer(model, "arch_power_key_row", kDefaultPowerKeyRow);
		d.power_key_col = detail::Integer(model, "arch_power_key_col", kDefaultPowerKeyCol);

		// Legacy compatibility: a board pack that only carries board_path (the
		// original CG100 pack) is dispatched to the legacy SH4 provider id.
		if (d.provider_id.empty() && !model.board_path.empty()) {
			d.provider_id = kLegacyProviderId;
			d.from_legacy_pack = true;
		}
		if (d.architecture.empty())
			d.architecture = kDefaultArchitecture;
		if (d.cabi_prefix.empty())
			d.cabi_prefix = kDefaultCabiPrefix;
		return d;
	}

	/// True when the model is driven by an architecture core rather than the
	/// built-in nX chipset. Data-driven: no hardware id or model name involved.
	inline bool IsBoardModel(const ModelInfo& model) {
		return !model.board_path.empty() || FromModel(model).valid();
	}

	inline unsigned FramebufferWidth(const Descriptor& d) {
		return d.fb_width ? d.fb_width : kDefaultFramebufferWidth;
	}
	inline unsigned FramebufferHeight(const Descriptor& d) {
		return d.fb_height ? d.fb_height : kDefaultFramebufferHeight;
	}
	inline unsigned CycleBudget(const Descriptor& d) {
		return d.cycles_per_second ? d.cycles_per_second : kDefaultCycleBudget;
	}
	inline std::string CabiSymbol(const Descriptor& d, const char* suffix) {
		return d.cabi_prefix + suffix;
	}
	inline bool IsLegacyCg100(const Descriptor& d) {
		return d.provider_id == kLegacyProviderId;
	}

} // namespace arch
} // namespace casioemu
