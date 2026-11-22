#pragma once
// Data-only Lua script plugins for iPadOS.
//
// A script plugin is a bundle directory with plugin.json (`type: script`) and
// a text `.lua` entry point.  It is compiled and run in-process by the host
// ScriptRuntime (Lua 5.4.8).  No dlopen, no JIT, no native_entry: everything
// the script can do goes through the host table, whose architecture functions
// are wired to the bridge below.  Unbound capabilities return a safe value
// and log once instead of being faked.

#include "Plugin/ScriptRuntime.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace casioemu {
namespace ipados {
namespace scripts {

/// Host services the app binds once an Emulator/architecture core exists.
/// Empty functions are safe: the corresponding host API logs once and returns
/// false/nil (see ScriptRuntime's own graceful-degradation rules).
struct ArchBridge {
	std::function<bool(const std::string& cmd, int arg)> control;
	std::function<bool(plugin::ScriptArchStatus& out)> status;
};

struct HostCapabilities {
	std::function<std::string()> model;
	std::function<bool(int&, int&, int&)> framebufferSize;
	std::function<bool(const std::string&, std::uint32_t&, unsigned&)> readRegister;
	std::function<bool(std::uint32_t, std::uint8_t&)> readMemory8;
	std::function<void(std::uint32_t, std::uint8_t)> writeMemory8;
	std::function<bool(int, int)> keyIsDown;
	std::function<void(int, int, bool)> key;
};

void SetArchBridge(ArchBridge bridge);
void ClearArchBridge();
void SetHostCapabilities(HostCapabilities capabilities);
void ClearHostCapabilities();

/// RAII binding: clears the global bridge and host capabilities when the
/// Emulator leaves scope (late script taps then see an unbound host).
class ArchBridgeBinding {
public:
	explicit ArchBridgeBinding(ArchBridge bridge);
	~ArchBridgeBinding();
	ArchBridgeBinding(const ArchBridgeBinding&) = delete;
	ArchBridgeBinding& operator=(const ArchBridgeBinding&) = delete;
};

struct LoadedScript {
	std::string id;
	std::string name;
	std::filesystem::path root;
	std::shared_ptr<plugin::ScriptRuntime> runtime;
};

/// Load every `type: script` plugin in `directory`. Returns the number of
/// runtimes successfully loaded; details go to Log(). Safe to call twice:
/// plugin ids are deduplicated.
std::size_t LoadScriptPlugins(const std::filesystem::path& directory);
/// Load from the app bundle's data-only plugin directory.
std::size_t LoadScriptPlugins();

[[nodiscard]] const std::vector<LoadedScript>& Loaded();
[[nodiscard]] const std::string& Log();

/// Optional script lifecycle callbacks. Errors are logged, never thrown.
void OnFrame(double dt);
void OnKey(int ki, int ko, bool down);
void UnloadAll();

} // namespace scripts
} // namespace ipados
} // namespace casioemu
