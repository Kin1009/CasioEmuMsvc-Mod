// Host-side (Linux) tests for the iPadOS core/architecture layer.
// No SDL, no iOS SDK, no emulator link: only portable headers + patched Lua.
// Built by CMake as casioemu_ipados_core_host_tests.

#include "Plugin/ArchModelSelection.h"
#include "Plugin/PluginManifest.h"
#include "Plugin/StaticArchitecture.h"
#include "Plugin/ScriptRuntime.h"
#include "IPadOSScriptPlugins.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef CASIOEMU_IPADOS_TEST_FIXTURES
#define CASIOEMU_IPADOS_TEST_FIXTURES "tests/fixtures/ipados_scripts"
#endif

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define HAS(haystack, needle) CHECK(std::string(haystack).find(needle) != std::string::npos)

// ---------------------------------------------------------------------------
// Minimal stand-ins for PluginMan.cpp's host registry. The real TU needs SDL;
// these tests only exercise the generic bootstrap contract.
// ---------------------------------------------------------------------------
namespace {
class TestRegistry final : public casioemu::IArchitectureRegistry {
public:
	bool RegisterProvider(casioemu::IArchitectureProvider* provider) override {
		if (!provider) return false;
		for (auto* existing : providers)
			if (existing == provider) return true;
		providers.push_back(provider);
		return true;
	}
	casioemu::IArchitectureProvider* FindProvider(std::uint16_t hardware_id) override {
		for (auto* provider : providers) {
			std::size_t count = 0;
			const std::uint16_t* ids = provider->GetSupportedHardwareIds(&count);
			for (std::size_t i = 0; i < count; ++i)
				if (ids && ids[i] == hardware_id) return provider;
		}
		return nullptr;
	}
	std::size_t GetProviderCount() const override { return providers.size(); }
	casioemu::IArchitectureProvider* GetProvider(std::size_t index) const override {
		return index < providers.size() ? providers[index] : nullptr;
	}
private:
	std::vector<casioemu::IArchitectureProvider*> providers;
};

TestRegistry& Registry() {
	static TestRegistry registry;
	return registry;
}

class FakeStaticProvider final : public casioemu::IArchitectureProvider {
public:
	const char* GetId() const override { return "test.static.sh4"; }
	const char* GetName() const override { return "Test static SH4 core"; }
	std::uint32_t GetVersion() const override { return 1; }
	const std::uint16_t* GetSupportedHardwareIds(std::size_t* count) const override {
		static const std::uint16_t ids[] = {14};
		*count = 1;
		return ids;
	}
	casioemu::IEmulatorCore* CreateCore() override { return nullptr; }
	void DestroyCore(casioemu::IEmulatorCore*) override {}
};

casioemu::IArchitectureProvider* MakeFakeStaticProvider() { return new FakeStaticProvider(); }
} // namespace

casioemu::IArchitectureRegistry* GetArchitectureRegistry() { return &Registry(); }
bool RegisterStaticArchitectureProvider(casioemu::IArchitectureProvider* provider) {
	return Registry().RegisterProvider(provider);
}
casioemu::IArchitectureProvider* FindArchitectureProviderById(const char* id) {
	if (!id || !*id) return nullptr;
	for (std::size_t i = 0, n = Registry().GetProviderCount(); i < n; ++i) {
		auto* provider = Registry().GetProvider(i);
		if (provider && provider->GetId() && std::strcmp(provider->GetId(), id) == 0)
			return provider;
	}
	return nullptr;
}

CASIOEMU_REGISTER_STATIC_ARCH_PROVIDER(MakeFakeStaticProvider);

static casioemu::plugin::ManifestResult Parse(const std::string& json_text) {
	return casioemu::plugin::ParseManifest(json_text);
}

int main() {
	// 1. Static provider registration hookup (no dlopen path).
	{
		CHECK(casioemu::arch::RegisterStaticProviderFactory(MakeFakeStaticProvider) == false); // duplicate
		std::string log;
		const auto report = casioemu::arch::RegisterStaticArchitectureProviders(
			[&log](const std::string& line) { log += line + "\n"; });
		CHECK(report.registered >= 1);
		CHECK(FindArchitectureProviderById("test.static.sh4") != nullptr);
		HAS(log, "static provider 'test.static.sh4' registered at build time (no dlopen)");
		const auto again = casioemu::arch::RegisterStaticArchitectureProviders(
			[&log](const std::string& line) { log += line + "\n"; });
		CHECK(again.factories == report.factories);
		CHECK(again.registered == report.registered);
	}

	// 2. Manifest support for data-only script plugins and iPadOS tags.
	{
		const char* valid = R"({"schema":1,"id":"com.casioemu.test.script","name":"S","version":"1.0.0",
			"type":"script","platforms":["ipados"],"permissions":["emulator.read"],
			"entry_point":"main.lua","assets":["main.lua"]})";
		const auto parsed = Parse(valid);
		CHECK(parsed.ok);
		CHECK(parsed.manifest.IsScript());
		CHECK(!parsed.manifest.IsNative());
		CHECK(parsed.manifest.scriptEngine == casioemu::plugin::kScriptEngineLua54);
		CHECK(parsed.manifest.scriptApi == casioemu::plugin::kScriptApiVersion);
		CHECK(parsed.manifest.scriptBudget == casioemu::plugin::kDefaultScriptBudget);

		// Regression: bundled iPadOS manifests used to fail platform validation.
		const char* theme = R"({"schema":1,"id":"com.casioemu.test.theme","name":"T","version":"1.0.0",
			"type":"theme","platforms":["ipados","ios"],"permissions":[],
			"entry_point":"theme.json","assets":["theme.json"]})";
		CHECK(Parse(theme).ok);

		const char* native = R"({"schema":1,"id":"com.casioemu.test.native","name":"N","version":"1.0.0",
			"type":"script","platforms":["ipados"],"permissions":[],
			"entry_point":"main.lua","native_entry":"libx.dylib","assets":[]})";
		const auto native_result = Parse(native);
		CHECK(!native_result.ok);
		HAS(native_result.code, "manifest.field");

		const char* wrong_engine = R"({"schema":1,"id":"com.casioemu.test.engine","name":"E","version":"1.0.0",
			"type":"script","platforms":["ipados"],"permissions":[],
			"entry_point":"main.lua","assets":[],"script_engine":"python"})";
		const auto engine_result = Parse(wrong_engine);
		CHECK(!engine_result.ok);
		HAS(engine_result.code, "manifest.script_engine");

		const char* wrong_extension = R"({"schema":1,"id":"com.casioemu.test.ext","name":"X","version":"1.0.0",
			"type":"script","platforms":["ipados"],"permissions":[],
			"entry_point":"main.bin","assets":[]})";
		CHECK(!Parse(wrong_extension).ok);
	}

	// 3. Descriptor-driven model selection with honest fallback.
	{
		casioemu::ModelInfo legacy;
		legacy.model_name = "fx-CG100";
		legacy.board_path = "board.svg";
		casioemu::ModelInfo cg50;
		cg50.model_name = "fx-CG50";
		cg50.board_path = "board.svg";
		cg50.extra["arch_provider"] = "cg50sh4";
		casioemu::ModelInfo builtin;
		builtin.model_name = "fx-9860G";
		std::vector<casioemu::arch::ModelCandidate> candidates{
			{"models/fx-CG100", legacy}, {"models/fx-CG50", cg50}, {"models/fx-9860G", builtin}};
		const auto available = [](const casioemu::arch::Descriptor& d) {
			return d.provider_id == "cg50sh4";
		};

		const auto explicit_choice = casioemu::arch::SelectModel(candidates, available, "models/fx-CG100");
		CHECK(explicit_choice.found);
		CHECK(explicit_choice.path == "models/fx-CG100");

		const auto board_choice = casioemu::arch::SelectModel(candidates, available);
		CHECK(board_choice.found);
		CHECK(board_choice.board);
		CHECK(board_choice.path == "models/fx-CG50");
		CHECK(board_choice.descriptor.provider_id == "cg50sh4");

		const auto no_providers = casioemu::arch::SelectModel(candidates,
			[](const casioemu::arch::Descriptor&) { return false; });
		CHECK(no_providers.found);
		CHECK(!no_providers.board);
		CHECK(no_providers.path == "models/fx-9860G");

		const auto only_board = casioemu::arch::SelectModel({{"models/fx-CG50", cg50}},
			[](const casioemu::arch::Descriptor&) { return false; });
		CHECK(only_board.board);
		CHECK(only_board.provider_missing);
		HAS(casioemu::arch::DescribeSelection(only_board), "no provider is registered");
	}

	// 4. Data-only Lua plugin loading and arch_control/arch_status wiring.
	{
		using namespace casioemu::ipados::scripts;
		int resets = 0;
		int diags = 0;
		SetArchBridge({
			[&](const std::string& command, int) -> bool {
				if (command == "reset") { ++resets; return true; }
				if (command == "power_on") return true;
				if (command == "enter_diag") { ++diags; return false; }
				return true;
			},
			[](casioemu::plugin::ScriptArchStatus& out) -> bool {
				out.running = true;
				out.paused = false;
				out.cycles = 1234;
				out.model = "TEST";
				out.hardwareId = 99;
				return true;
			}});
		SetHostCapabilities({
			[]() -> std::string { return "TEST"; },
			[](int& width, int& height, int& pitch) -> bool {
				width = 396; height = 224; pitch = 396; return true;
			},
			{}, {}, {}, {}, {}});

		const std::size_t loaded = LoadScriptPlugins(CASIOEMU_IPADOS_TEST_FIXTURES);
		CHECK(loaded == 2);
		CHECK(Loaded().size() == 2);
		for (const auto& script : Loaded())
			CHECK(script.id != "com.casioemu.test.declarative");
		CHECK(LoadScriptPlugins(CASIOEMU_IPADOS_TEST_FIXTURES) == 0); // idempotent

		OnFrame(0.016);
		const std::string log = Log();
		HAS(log, "STATUS model=TEST hw=99 running=true");
		HAS(log, "CONTROL reset=true");
		HAS(log, "CONTROL diag=false");
		HAS(log, "CONTROL bogus=false"); // unknown commands never reach the bridge
		HAS(log, "FB 396x224 pitch=396");
		HAS(log, "script plugins cannot include native_entry");
		HAS(log, "unknown script_engine");
		HAS(log, "platform tag does not include ipados/ios/any");
		CHECK(resets == 1);
		CHECK(diags == 1);
		for (const auto& script : Loaded()) {
			if (script.id == "com.casioemu.test.no-inject")
				CHECK(script.runtime->DeniedCount() > 0); // no emulator.inject permission
		}

		// Graceful fallback: no bound bridge -> honest "unavailable" logs.
		ClearArchBridge();
		ClearHostCapabilities();
		UnloadAll();
		CHECK(Loaded().empty());
		CHECK(LoadScriptPlugins(CASIOEMU_IPADOS_TEST_FIXTURES) == 2);
		OnFrame(0.016);
		HAS(Log(), "arch_status unavailable");
		HAS(Log(), "arch_control unavailable");
		HAS(Log(), "framebuffer metadata unavailable");
		UnloadAll();
	}

	if (failures == 0)
		std::printf("ipados_core_host_tests: PASS (static provider, manifest, selection, Lua bridge)\n");
	else
		std::printf("ipados_core_host_tests: FAIL (%d checks)\n", failures);
	return failures ? 1 : 0;
}
