// Host-side (Linux) smoke test for the data-driven architecture descriptor.
// Builds standalone: g++ -std=c++20 -I<src> arch_descriptor_smoke.cpp && ./a.out
// No SDL, no iOS SDK, no emulator link required.

#include "Plugin/ArchDescriptor.h"

#include <cassert>
#include <cstdio>
#include <sstream>
#include <string>

using casioemu::ModelInfo;
using casioemu::arch::Descriptor;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
	// 1. Ordinary built-in model: no board_path, no descriptor keys.
	{
		ModelInfo m;
		m.hardware_id = casioemu::HW_EPS6009; // 10
		Descriptor d = casioemu::arch::FromModel(m);
		CHECK(!d.valid());
		CHECK(!casioemu::arch::IsBoardModel(m));
		CHECK(casioemu::arch::FramebufferWidth(d) == 396u);
		CHECK(casioemu::arch::FramebufferHeight(d) == 224u);
		CHECK(casioemu::arch::CycleBudget(d) == 200000000u);
	}

	// 2. Legacy CG100 pack: board_path set, no explicit keys -> legacy provider
	//    id + CG100 defaults. This is the behaviour-preservation case.
	{
		ModelInfo m;
		m.hardware_id = 10; // legacy stable-tree id
		m.board_path = "board.svg";
		Descriptor d = casioemu::arch::FromModel(m);
		CHECK(d.valid());
		CHECK(casioemu::arch::IsBoardModel(m));
		CHECK(d.from_legacy_pack);
		CHECK(d.provider_id == "cg100sh4");
		CHECK(d.architecture == "sh4");
		CHECK(casioemu::arch::IsLegacyCg100(d));
		CHECK(casioemu::arch::CabiSymbol(d, "cycles") == "cg100_cycles");
		CHECK(d.power_key_row == 6 && d.power_key_col == 9);
	}

	// 3. Hypothetical future fx-CG50 board: explicit provider id, different
	//    framebuffer and cycle budget. No host code change is involved.
	{
		ModelInfo m;
		m.hardware_id = 10; // any id; provider chosen by arch_provider
		m.board_path = "board-cg50.svg";
		m.extra["arch_provider"] = "cg50sh4";
		m.extra["arch_architecture"] = "sh4";
		m.extra["arch_fb_width"] = "384";
		m.extra["arch_fb_height"] = "192";
		m.extra["arch_cycles_per_second"] = "0x0BEBC200"; // 200,000,000
		m.extra["arch_cabi_prefix"] = "cg50_";
		m.extra["arch_power_key_row"] = "7";
		m.extra["arch_power_key_col"] = "0";
		Descriptor d = casioemu::arch::FromModel(m);
		CHECK(d.provider_id == "cg50sh4");
		CHECK(!casioemu::arch::IsLegacyCg100(d));
		CHECK(casioemu::arch::IsBoardModel(m));
		CHECK(casioemu::arch::FramebufferWidth(d) == 384u);
		CHECK(casioemu::arch::FramebufferHeight(d) == 192u);
		CHECK(casioemu::arch::CycleBudget(d) == 200000000u);
		CHECK(casioemu::arch::CabiSymbol(d, "cycles") == "cg50_cycles");
		CHECK(d.power_key_row == 7 && d.power_key_col == 0);
	}

	// 4. Malformed numeric values fall back to defaults, never throw/accept 0.
	{
		ModelInfo m;
		m.board_path = "b.svg";
		m.extra["arch_fb_width"] = "nonsense";
		m.extra["arch_fb_height"] = "0";
		m.extra["arch_cycles_per_second"] = "12x";
		Descriptor d = casioemu::arch::FromModel(m);
		CHECK(casioemu::arch::FramebufferWidth(d) == 396u);
		CHECK(casioemu::arch::FramebufferHeight(d) == 224u);
		CHECK(casioemu::arch::CycleBudget(d) == 200000000u);
	}

	// 5. Descriptor-only board (arch_provider key, NO board_path): the config
	//    round-trip must remap it to HW_ARCH, matching arch::IsBoardModel().
	//    Without this, a board would keep an EPS hardware id and get a broken
	//    half-configured emulator (chipset gated off, EPS paths still on).
	{
		ModelInfo src;
		src.hardware_id = casioemu::HW_EPS6009; // 10, would be EPS6009
		src.extra["arch_provider"] = "cg50sh4";
		std::stringstream stream;
		src.Write(stream);
		ModelInfo m;
		m.Read(stream);
		CHECK(m.hardware_id == casioemu::HW_ARCH);
		CHECK(casioemu::arch::IsBoardModel(m));
		CHECK(casioemu::arch::FromModel(m).provider_id == "cg50sh4");
	}

	if (failures == 0)
		std::printf("arch_descriptor_smoke: PASS (all descriptor cases)\n");
	else
		std::printf("arch_descriptor_smoke: FAIL (%d checks)\n", failures);
	return failures ? 1 : 0;
}
