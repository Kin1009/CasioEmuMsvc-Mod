#include "IPadOSScriptPlugins.h"

#include "IPadOSPlugins.h"
#include "PlatformTagsIPadOS.h"
#include "Plugin/PluginManifest.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

namespace casioemu {
namespace ipados {
namespace scripts {

namespace {

using OnceSet = std::shared_ptr<std::set<std::string>>;

ArchBridge& Bridge() {
	static ArchBridge bridge;
	return bridge;
}

HostCapabilities& Caps() {
	static HostCapabilities capabilities;
	return capabilities;
}

std::vector<LoadedScript>& LoadedScripts() {
	static std::vector<LoadedScript> scripts;
	return scripts;
}

std::set<std::string>& LoadedIds() {
	static std::set<std::string> ids;
	return ids;
}

std::string& LogBuffer() {
	static std::string log;
	return log;
}

void LogLine(const std::string& line) {
	LogBuffer() += "[ipados-scripts] " + line + "\n";
}

void Once(const OnceSet& seen, const std::string& key, const std::string& line) {
	if (seen->insert(key).second)
		LogLine(line);
}

unsigned PermissionsFor(const plugin::Manifest& manifest) {
	using plugin::ScriptPermission;
	unsigned mask = 0;
	if (manifest.HasPermission("emulator.read"))
		mask |= static_cast<unsigned>(ScriptPermission::Read);
	if (manifest.HasPermission("emulator.write"))
		mask |= static_cast<unsigned>(ScriptPermission::Write);
	if (manifest.HasPermission("emulator.inject"))
		mask |= static_cast<unsigned>(ScriptPermission::Inject);
	if (manifest.HasPermission("ui.shell"))
		mask |= static_cast<unsigned>(ScriptPermission::Shell);
	return mask;
}

plugin::ScriptRuntimeConfig MakeConfig(const plugin::Manifest& manifest, const OnceSet& seen) {
	plugin::ScriptRuntimeConfig config;
	config.pluginId = manifest.id;
	config.engine = manifest.scriptEngine;
	config.budget = manifest.scriptBudget;
	config.permissions = PermissionsFor(manifest);
	const std::string id = manifest.id;

	config.host.log = [id](const std::string& text) {
		LogLine("[script] " + id + ": " + text);
	};
	config.host.tick = []() -> std::uint64_t {
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
	};

	config.host.model = [seen, id]() -> std::string {
		if (!Caps().model) {
			Once(seen, "model:unbound", "[script] " + id + ": model unavailable (no emulator bound)");
			return {};
		}
		return Caps().model();
	};
	config.host.framebufferSize = [seen, id](int& width, int& height, int& pitch) -> bool {
		if (!Caps().framebufferSize || !Caps().framebufferSize(width, height, pitch)) {
			Once(seen, "fb:unbound", "[script] " + id + ": framebuffer metadata unavailable");
			return false;
		}
		return true;
	};
	config.host.readRegister = [seen, id](const std::string& name, std::uint32_t& value, unsigned& bits) -> bool {
		if (!Caps().readRegister || !Caps().readRegister(name, value, bits)) {
			Once(seen, "reg:unbound:" + name, "[script] " + id + ": register '" + name + "' unavailable");
			return false;
		}
		return true;
	};
	config.host.readMemory8 = [seen, id](std::uint32_t address, std::uint8_t& value) -> bool {
		if (!Caps().readMemory8 || !Caps().readMemory8(address, value)) {
			Once(seen, "mem:unbound", "[script] " + id + ": memory reads unavailable");
			return false;
		}
		return true;
	};
	config.host.writeMemory8 = [seen, id](std::uint32_t, std::uint8_t) {
		Once(seen, "memwrite:unbound", "[script] " + id + ": memory writes unavailable (no emulator bound)");
	};
	config.host.keyIsDown = [seen, id](int ki, int ko) -> bool {
		if (!Caps().keyIsDown) {
			Once(seen, "key:unbound", "[script] " + id + ": key state unavailable");
			return false;
		}
		return Caps().keyIsDown(ki, ko);
	};
	config.host.key = [seen, id](int ki, int ko, bool down) {
		if (!Caps().key) {
			Once(seen, "keyset:unbound", "[script] " + id + ": key injection unavailable");
			return;
		}
		Caps().key(ki, ko, down);
	};

	// Architecture control/status are the important data-only bridges. They
	// delegate to whatever the app bound; when nothing is bound (or the host
	// rejects a command) they report false/nil once, never fabricated state.
	config.host.archControl = [seen, id](const std::string& command, int arg) -> bool {
		ArchBridge& bridge = Bridge();
		if (!bridge.control) {
			Once(seen, "arch:control:unbound",
				"[script] " + id + ": arch_control unavailable (no architecture core bound)");
			return false;
		}
		const bool accepted = bridge.control(command, arg);
		if (!accepted) {
			Once(seen, "arch:control:rejected:" + command,
				"[script] " + id + ": arch_control('" + command + "') rejected by host");
		}
		return accepted;
	};
	config.host.archStatus = [seen, id](plugin::ScriptArchStatus& out) -> bool {
		ArchBridge& bridge = Bridge();
		if (!bridge.status || !bridge.status(out)) {
			Once(seen, "arch:status:unbound",
				"[script] " + id + ": arch_status unavailable (no emulator state bound)");
			return false;
		}
		return true;
	};

	// Script-owned ImGui windows/buttons are not part of this port yet. The
	// request is logged once and reported as unavailable to the script.
	config.host.addWindow = [seen, id](const std::string& title) {
		Once(seen, "ui:window:unbound",
			"[script] " + id + ": ui_add_window('" + title + "') unavailable (no script window UI)");
	};
	config.host.addButton = [seen, id](const std::string& window, const std::string& label,
		std::function<void()>) -> int {
		Once(seen, "ui:button:unbound",
			"[script] " + id + ": ui_add_button('" + label + "' on '" + window +
				"') unavailable (no script window UI)");
		return 0;
	};
	return config;
}

bool PlatformMatches(const plugin::Manifest& manifest) {
	return std::any_of(manifest.platforms.begin(), manifest.platforms.end(),
		[](const std::string& tag) {
			return tag == "any" || casioemu::platform_tags::MatchesCurrentPlatform(tag);
		});
}

} // namespace

void SetArchBridge(ArchBridge bridge) { Bridge() = std::move(bridge); }
void ClearArchBridge() { Bridge() = ArchBridge{}; }
void SetHostCapabilities(HostCapabilities capabilities) { Caps() = std::move(capabilities); }
void ClearHostCapabilities() { Caps() = HostCapabilities{}; }

ArchBridgeBinding::ArchBridgeBinding(ArchBridge bridge) { SetArchBridge(std::move(bridge)); }
ArchBridgeBinding::~ArchBridgeBinding() {
	ClearArchBridge();
	ClearHostCapabilities();
}

const std::vector<LoadedScript>& Loaded() { return LoadedScripts(); }
const std::string& Log() { return LogBuffer(); }

std::size_t LoadScriptPlugins(const std::filesystem::path& directory) {
	std::error_code ec;
	if (!std::filesystem::exists(directory, ec)) {
		LogLine("no script plugin directory at " + directory.string());
		return 0;
	}

	std::size_t loaded_count = 0;
	for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
		const std::filesystem::path manifest_path = it->path() / "plugin.json";
		if (!std::filesystem::exists(manifest_path, ec))
			continue;

		std::ifstream file(manifest_path);
		if (!file) {
			LogLine("cannot read " + manifest_path.string());
			continue;
		}
		std::ostringstream buffer;
		buffer << file.rdbuf();
		const auto result = plugin::ParseManifest(buffer.str());
		if (!result.ok) {
			LogLine(result.code + ": " + result.message + " (" + manifest_path.string() + ")");
			continue;
		}
		const auto& manifest = result.manifest;
		if (!manifest.IsScript())
			continue; // declarative plugins are handled by IPadOSPlugins.cpp

		// Defence in depth: a script plugin must never carry native code.
		if (manifest.IsNative()) {
			LogLine("skipped " + manifest.id + ": script plugins cannot contain native_entry");
			continue;
		}
		if (!PlatformMatches(manifest)) {
			LogLine("skipped " + manifest.id + ": platform tag does not include ipados/ios/any");
			continue;
		}
		bool unsafe_asset = false;
		for (const auto& asset : manifest.assets) {
			if (!plugin::IsSafeRelativePath(asset)) {
				unsafe_asset = true;
				break;
			}
		}
		if (unsafe_asset) {
			LogLine("skipped " + manifest.id + ": unsafe asset path");
			continue;
		}
		if (!plugin::IsSafeRelativePath(manifest.entryPoint)) {
			LogLine("skipped " + manifest.id + ": unsafe script entry_point");
			continue;
		}
		if (LoadedIds().count(manifest.id)) {
			LogLine("skipped " + manifest.id + ": already loaded");
			continue;
		}

		const std::filesystem::path source_path = it->path() / manifest.entryPoint;
		std::ifstream source_file(source_path);
		if (!source_file) {
			LogLine("skipped " + manifest.id + ": cannot read " + source_path.string());
			continue;
		}
		std::ostringstream source_buffer;
		source_buffer << source_file.rdbuf();
		const std::string source = source_buffer.str();
		constexpr std::size_t kMaxSourceBytes = 4ull * 1024ull * 1024ull;
		if (source.size() > kMaxSourceBytes) {
			LogLine("skipped " + manifest.id + ": script source exceeds 4 MiB");
			continue;
		}

		const OnceSet seen = std::make_shared<std::set<std::string>>();
		auto runtime = std::make_shared<plugin::ScriptRuntime>(MakeConfig(manifest, seen));
		std::string error;
		if (!runtime->Load(source, manifest.entryPoint, error)) {
			LogLine("error loading " + manifest.id + ": " + error);
			continue;
		}
		LoadedScripts().push_back({manifest.id, manifest.name, it->path(), std::move(runtime)});
		LoadedIds().insert(manifest.id);
		++loaded_count;
		LogLine("loaded script plugin " + manifest.id + " (" + manifest.scriptEngine + ")");
	}
	return loaded_count;
}

std::size_t LoadScriptPlugins() { return LoadScriptPlugins(plugins::BundlePluginsDirectory()); }

void OnFrame(double dt) {
	for (auto& script : LoadedScripts()) {
		if (!script.runtime)
			continue;
		std::string error;
		if (!script.runtime->OnFrame(dt, error))
			LogLine("[script] " + script.id + ": on_frame error: " + error);
	}
}

void OnKey(int ki, int ko, bool down) {
	for (auto& script : LoadedScripts()) {
		if (!script.runtime)
			continue;
		std::string error;
		if (!script.runtime->OnKey(ki, ko, down, error))
			LogLine("[script] " + script.id + ": on_key error: " + error);
	}
}

void UnloadAll() {
	for (auto& script : LoadedScripts()) {
		if (!script.runtime)
			continue;
		std::string error;
		if (!script.runtime->OnUnload(error))
			LogLine("[script] " + script.id + ": on_unload error: " + error);
	}
	LoadedScripts().clear();
	LoadedIds().clear();
}

} // namespace scripts
} // namespace ipados
} // namespace casioemu
