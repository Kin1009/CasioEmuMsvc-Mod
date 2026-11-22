// FA05 host-side dispatch probe: before/after provider selection for the
// CG50, GIII and CG100 model configs. No SDL, no emulator link required.
// Registered with CTest as `arch_dispatch_probe`.
//
// The three new-format configs below carry the documented `arch_provider`
// descriptor keys; the fourth is the shipped legacy CG100 pack (board_path
// only). Each config is written and read back through ModelInfo (i.e. the
// v53 config.bin path) before dispatch is evaluated.
//
// BEFORE = main @ ad07ad1: ModelInfo remaps only
//          `hardware_id == HW_EPS6009 && !board_path.empty()`, and dispatch
//          calls FindProvider(hardware_id) -> cg100sh4 for any board.
// AFTER  = FA05: ModelInfo remaps board_path OR arch_provider to HW_ARCH,
//          dispatch calls FindArchitectureProviderById(descriptor.provider_id)
//          with the hardware-id lookup as a legacy fallback.

#include "Plugin/ArchDescriptor.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

using casioemu::ModelInfo;
namespace arch = casioemu::arch;

namespace {

// Stand-in for the real IArchitectureRegistry: id -> hardware id (0 = none).
struct MockProvider { const char* id; uint16_t hardware_id; };
const MockProvider kProviders[] = {
	{ "cg100sh4", casioemu::HW_ARCH }, // legacy provider advertises hardware id 14
	{ "cg50sh4", 0 },                   // new-format providers advertise no hw id
	{ "giii_sh4a", 0 },
};

const char* FindById(const char* id) {
	if (!id || !*id) return nullptr;
	for (const auto& p : kProviders)
		if (std::strcmp(p.id, id) == 0) return p.id;
	return nullptr;
}
const char* FindByHardwareId(uint16_t hw) {
	for (const auto& p : kProviders)
		if (p.hardware_id == hw) return p.id;
	return nullptr;
}

// Exact replica of pre-FA05 casioemu.cpp dispatch.
const char* SelectBefore(const ModelInfo& src) {
	uint16_t hw = src.hardware_id;
	if (hw == casioemu::HW_EPS6009 && !src.board_path.empty())
		hw = casioemu::HW_CG100; // pre-FA05 remap to dedicated board id 14
	if (!src.board_path.empty() && hw == casioemu::HW_CG100) {
		const char* p = FindByHardwareId(hw);
		return p ? p : "built-in";
	}
	return "built-in";
}

// Exact replica of FA05 casioemu.cpp dispatch.
const char* SelectAfter(const ModelInfo& m) {
	const arch::Descriptor d = arch::FromModel(m);
	if (!arch::IsBoardModel(m)) return "built-in";
	const char* p = d.provider_id.empty() ? nullptr : FindById(d.provider_id.c_str());
	if (!p) p = FindByHardwareId(m.hardware_id);
	return p ? p : "none";
}

struct Case {
	const char* name;
	ModelInfo source;   // as authored in config.bin
	const char* expected;
	const char* note;
};

int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

} // namespace

int main() {
	Case cases[4];

	// 1. fx-CG50 (PKGCG50): hw 14, board.svg + descriptor keys.
	cases[0].name = "CG50";
	cases[0].expected = "cg50sh4";
	cases[0].note = "new-format pack (arch_provider=cg50sh4)";
	cases[0].source.hardware_id = 14;
	cases[0].source.board_path = "board.svg";
	cases[0].source.extra["arch_provider"] = "cg50sh4";
	cases[0].source.extra["arch_architecture"] = "sh4";
	cases[0].source.extra["arch_fb_width"] = "396";
	cases[0].source.extra["arch_fb_height"] = "224";
	cases[0].source.extra["arch_cycles_per_second"] = "118000000";
	cases[0].source.extra["arch_cabi_prefix"] = "cg50_";
	cases[0].source.extra["arch_power_key_row"] = "0";
	cases[0].source.extra["arch_power_key_col"] = "0";

	// 2. fx-9860GIII: descriptor-only (no board_path), SH-4A 128x64.
	cases[1].name = "GIII";
	cases[1].expected = "giii_sh4a";
	cases[1].note = "descriptor-only (arch_provider=giii_sh4a, no board_path)";
	cases[1].source.hardware_id = 10;
	cases[1].source.extra["arch_provider"] = "giii_sh4a";
	cases[1].source.extra["arch_architecture"] = "sh4a";
	cases[1].source.extra["arch_fb_width"] = "128";
	cases[1].source.extra["arch_fb_height"] = "64";
	cases[1].source.extra["arch_cycles_per_second"] = "58000000";
	cases[1].source.extra["arch_cabi_prefix"] = "giii_";
	cases[1].source.extra["arch_power_key_row"] = "0";
	cases[1].source.extra["arch_power_key_col"] = "7";

	// 3. New-format CG100: descriptor-only, provider cg100sh4.
	cases[2].name = "CG100-new";
	cases[2].expected = "cg100sh4";
	cases[2].note = "descriptor-only (arch_provider=cg100sh4, no board_path)";
	cases[2].source.hardware_id = 10;
	cases[2].source.extra["arch_provider"] = "cg100sh4";
	cases[2].source.extra["arch_architecture"] = "sh4";
	cases[2].source.extra["arch_fb_width"] = "396";
	cases[2].source.extra["arch_fb_height"] = "224";
	cases[2].source.extra["arch_cycles_per_second"] = "200000000";
	cases[2].source.extra["arch_cabi_prefix"] = "cg100_";
	cases[2].source.extra["arch_power_key_row"] = "6";
	cases[2].source.extra["arch_power_key_col"] = "9";

	// 4. Legacy shipped CG100 pack: board_path only, no descriptor keys.
	cases[3].name = "CG100-legacy";
	cases[3].expected = "cg100sh4";
	cases[3].note = "legacy pack (board.svg only) - behaviour preservation";
	cases[3].source.hardware_id = 10;
	cases[3].source.board_path = "board.svg";

	int before_mismatches = 0;
	int after_mismatches = 0;
	std::printf("FA05 dispatch probe (config.bin -> provider id)\n");
	std::printf("%-14s %-12s %-12s %-12s  %s\n", "case", "expected", "before", "after", "note");
	for (const auto& c : cases) {
		std::stringstream stream;
		c.source.Write(stream);
		ModelInfo m;
		m.Read(stream); // v53 config.bin path, including FA05 HW_ARCH remap

		const char* before = SelectBefore(c.source);
		const char* after = SelectAfter(m);
		const bool before_ok = std::strcmp(before, c.expected) == 0;
		const bool after_ok = std::strcmp(after, c.expected) == 0;
		if (!before_ok) ++before_mismatches;
		if (!after_ok) ++after_mismatches;

		std::printf("%-14s %-12s %-12s %-12s  %s\n",
			c.name, c.expected, before, after, c.note);

		// Every board config must be remapped to HW_ARCH by ModelInfo::Read().
		CHECK(m.hardware_id == casioemu::HW_ARCH);
		CHECK(arch::IsBoardModel(m));
		CHECK(after_ok);
	}
	std::printf("mismatches: before=%d after=%d\n", before_mismatches, after_mismatches);
	CHECK(before_mismatches == 3); // documented pre-FA05 baseline
	CHECK(after_mismatches == 0);

	// Legacy CG100 specifics must be preserved exactly.
	const arch::Descriptor legacy = arch::FromModel(cases[3].source);
	CHECK(legacy.from_legacy_pack);
	CHECK(legacy.provider_id == "cg100sh4");
	CHECK(arch::FramebufferWidth(legacy) == 396u);
	CHECK(arch::FramebufferHeight(legacy) == 224u);
	CHECK(arch::CycleBudget(legacy) == 200000000u);
	CHECK(legacy.power_key_row == 6 && legacy.power_key_col == 9);

	if (failures == 0)
		std::printf("arch_dispatch_probe: PASS (3 mismatches -> 0, legacy CG100 preserved)\n");
	else
		std::printf("arch_dispatch_probe: FAIL (%d checks)\n", failures);
	return failures ? 1 : 0;
}
