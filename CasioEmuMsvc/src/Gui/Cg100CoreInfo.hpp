#pragma once
// Architecture-core debugger helpers (host side).
//
// Board models are driven by an IEmulatorCore created by an architecture
// provider, NOT by the built-in nX-U16/U8 chipset. The built-in MMU/RAM/ROM
// arrays are therefore empty for them. These helpers expose only what the host
// can honestly display:
//
//   * core identity + status from the C++ IEmulatorCore interface;
//   * the live counters exported by the plugin's plain-C ABI, with the symbol
//     prefix taken from the board descriptor (legacy SH4 pack: "cg100_*");
//   * the CG100 flash/FAT12 layout, shown ONLY for the legacy CG100 pack.
//
// Namespace `cg100info` is kept as a compatibility shim; `archinfo` is the
// generic alias for new call sites. Fields the plugin does not expose (SH4
// registers, MMIO registers such as PFC/KEYSC/RTC/ETMU/flash controller) are
// explicitly labelled unavailable - we never invent values for them.
//
// Header-only on purpose: the desktop target globs Gui/*.cpp without
// CONFIGURE_DEPENDS, so adding a new translation unit would need a CMake
// reconfigure.  Everything below is inline.

#include "Ui.hpp"                       // ::m_emu, Emulator, IEmulatorCore
#include "ModelInfo.h"                  // HW_ARCH (legacy alias HW_CG100)
#include "Plugin/IArchitectureProvider.h"
#include "Plugin/ArchDescriptor.h"      // data-driven board descriptor
#include "imgui/imgui.h"

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
# include <windows.h>
#else
# include <dlfcn.h>
#endif

namespace casioemu {
namespace cg100info {

// True for ANY live architecture-core (board) session. Previously this was
// gated on hardware_id == HW_CG100; the descriptor makes that check obsolete,
// so a future CG50 board automatically gets the same generic debug surface.
inline bool IsActive() {
	return ::m_emu && ::m_emu->arch_core.load() != nullptr;
}

inline IEmulatorCore* Core() {
	return IsActive() ? ::m_emu->arch_core.load() : nullptr;
}

// The data-driven descriptor for the active board (empty when none).
inline const arch::Descriptor& Desc() {
	static const arch::Descriptor fallback{};
	return ::m_emu ? ::m_emu->arch_descriptor : fallback;
}

// True only for the legacy CG100 SH4 pack, i.e. the data whose flash map / FAT12
// layout documented in MEMORY_FILESYSTEMS.md applies. New boards get generic
// placeholders rather than CG100-specific addresses.
inline bool IsLegacyCg100() {
	return IsActive() && arch::IsLegacyCg100(Desc());
}

// Build an optional C ABI symbol name from the descriptor's prefix.
inline std::string CabiName(const char* suffix) {
	return arch::CabiSymbol(Desc(), suffix);
}

inline void* Resolve(const char* name) {
#if defined(_WIN32)
	return reinterpret_cast<void*>(GetProcAddress(GetModuleHandle(nullptr), name));
#else
	return dlsym(RTLD_DEFAULT, name);
#endif
}

template <typename Fn>
inline Fn Symbol(const char* name) {
	return reinterpret_cast<Fn>(Resolve(name));
}

using HandleFn = void* (*)(void*);
using U64Fn = uint64_t (*)(void*);
using StrFn = const char* (*)(void*);
using IntFn = int (*)(void*);
using ListFn = int (*)(void*, char*, int);
using ReadRawFn = int (*)(void*, uint32_t, uint8_t*, int);

// The plugin is dlopen()ed with RTLD_GLOBAL.  Its optional C ABI accessor
// converts the host's IEmulatorCore* into the opaque CG100Core handle used by
// the cg100_* exports.  Older plugins did not export it; in that case all
// live-Counter fields are shown as unavailable (never fabricated).
inline void* Handle() {
	IEmulatorCore* core = Core();
	if (!core)
		return nullptr;
	static HandleFn handle_fn = Symbol<HandleFn>("casioemu_arch_core_handle");
	return handle_fn ? handle_fn(core) : nullptr;
}

struct Info {
	const char* core_id = nullptr;
	const char* status = nullptr;
	void* handle = nullptr;
	bool abi = false;       // handle accessor + cg100_* exports resolved
	uint64_t cycles = 0;
	uint64_t pushes = 0;
	const char* fault = nullptr;
	int usb_state = -1;
	const char* usb_text = nullptr;
	std::string files;
};

inline Info Read() {
	Info out{};
	if (IEmulatorCore* core = Core()) {
		out.core_id = core->GetId();
		out.status = core->GetStatusText();
	}
	out.handle = Handle();
	out.abi = out.handle != nullptr;
	if (out.abi) {
		// Counter symbol prefix comes from the board descriptor; the legacy
		// SH4 pack resolves to "cg100_*" (behaviour preserved).
		U64Fn cycles_fn = Symbol<U64Fn>(CabiName("cycles").c_str());
		U64Fn pushes_fn = Symbol<U64Fn>(CabiName("pushes").c_str());
		StrFn fault_fn = Symbol<StrFn>(CabiName("fault").c_str());
		IntFn usb_fn = Symbol<IntFn>(CabiName("usb_status").c_str());
		StrFn usb_text_fn = Symbol<StrFn>(CabiName("usb_status_text").c_str());
		ListFn list_fn = Symbol<ListFn>(CabiName("usb_list").c_str());
		if (cycles_fn)
			out.cycles = cycles_fn(out.handle);
		if (pushes_fn)
			out.pushes = pushes_fn(out.handle);
		if (fault_fn)
			out.fault = fault_fn(out.handle);
		if (usb_fn)
			out.usb_state = usb_fn(out.handle);
		if (usb_text_fn)
			out.usb_text = usb_text_fn(out.handle);
		if (list_fn) {
			char buf[4096] = {};
			const int n = list_fn(out.handle, buf, static_cast<int>(sizeof(buf) - 1));
			if (n > 0) {
				buf[static_cast<size_t>(n) < sizeof(buf) - 1 ? static_cast<size_t>(n) : sizeof(buf) - 1] = '\0';
				out.files = buf;
			}
		}
	}
	return out;
}

inline bool ReadRaw(uint32_t addr, uint8_t* out, int n) {
	if (!Handle() || !out || n <= 0)
		return false;
	ReadRawFn read_fn = Symbol<ReadRawFn>(CabiName("usb_read_raw").c_str());
	return read_fn && read_fn(Handle(), addr, out, n) == n;
}

inline void Row(const char* label, const std::string& value) {
	ImGui::TextDisabled("%s", label);
	ImGui::SameLine(150.0f);
	ImGui::TextUnformatted(value.c_str());
}

inline std::string Hex(uint64_t value) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
	return buf;
}

// Common SH4 core/status block shared by the Hardware, Variables and Memory
// tool pages.  `compact` omits the longer explanatory text for narrow panes.
inline void RenderCoreStatus(bool compact = false) {
	if (!IsActive())
		return;
	const Info info = Read();
	Row("Provider core", info.core_id ? info.core_id : "unavailable");
	Row("Core status", info.status ? info.status : "unavailable");
	Row("C ABI bridge", info.abi
		? std::string("live (") + Desc().cabi_prefix + "* exports)"
		: std::string("not exported by plugin build"));
	Row("Cycles", info.abi ? Hex(info.cycles) : "unavailable");
	Row("LCD presents", info.abi ? std::to_string(info.pushes) : "unavailable");
	if (info.abi) {
		Row("Fault", info.fault && *info.fault ? info.fault : "none");
		Row("USB state", info.usb_text && *info.usb_text
			? std::string(std::to_string(info.usb_state)) + " - " + info.usb_text
			: std::to_string(info.usb_state));
	}
	else {
		Row("Fault", "unavailable");
		Row("USB state", "unavailable");
	}
	if (!compact) {
		ImGui::Spacing();
		ImGui::TextDisabled("SH4 registers (PC / SP / GBR / SR / MAC / PR):");
		ImGui::TextWrapped("No register-query API is exposed by IEmulatorCore; the plugin only "
			"provides identity, status and the counters above. Register/MMIO values are "
			"therefore not shown rather than guessed.");
	}
}

// Static CG100 flash map from MEMORY_FILESYSTEMS.md (no live values implied).
// Shown only for the legacy CG100 pack; other boards have a different layout
// and must not be described with CG100 addresses.
inline void RenderFlashMap() {
	if (!IsLegacyCg100()) {
		ImGui::TextDisabled("No flash-map descriptor for this board; layout unavailable.");
		return;
	}
	ImGui::TextDisabled("Boot 0x00000000-0x0001FFFF  (128 KiB boot code)");
	ImGui::TextDisabled("OS   0x00020000-0x000B5FFF  (CASIOWIN / youkan 2.00)");
	ImGui::TextDisabled("Marker 0x00B20000 (4 B state record)");
	ImGui::TextDisabled("Fugue metadata 0x00C80000-0x00C81FFF");
	ImGui::TextDisabled("fls0 FAT12 volume 0x00C83A00 (4,764,208 B / 4.55 MiB)");
	ImGui::TextDisabled("Unused / erased from 0x00CCE000");
}

inline void RenderFat12Summary() {
	if (!IsLegacyCg100()) {
		ImGui::TextDisabled("No filesystem descriptor for this board; layout unavailable.");
		return;
	}
	const Info info = Read();
	ImGui::TextDisabled("Label 'CASIO'  FAT12  512 B/sector  8 sectors/cluster (4 KiB)");
	ImGui::TextDisabled("2 FATs: 0x00C85000 / 0x00C85800   root 0x00C86000   data 0x00C8A000");
	ImGui::TextDisabled("Max data cluster 1157   media 0xF8   main memory = \\fls0\\@MainMem");
	if (info.abi && !info.files.empty()) {
		ImGui::Spacing();
		ImGui::TextUnformatted("Files on the emulated fls0 volume:");
		ImGui::TextUnformatted(info.files.c_str());
	}
	else if (info.abi) {
		ImGui::Spacing();
		ImGui::TextDisabled("Live file list empty: the emulated USB MSC is not connected in this session.");
	}
	else {
		ImGui::Spacing();
		ImGui::TextDisabled("Live file list unavailable (no C ABI bridge in this plugin build).");
	}
}

} // namespace cg100info

// Generic alias: new code should prefer `archinfo`; `cg100info` is retained as
// a compatibility shim so existing GUI call sites keep compiling unchanged.
namespace archinfo = cg100info;

} // namespace casioemu
