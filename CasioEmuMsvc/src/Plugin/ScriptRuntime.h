#pragma once

// Data-only Lua script runtime for plugins (Phase 1 core runtime).
//
// One sandboxed lua_State per plugin.  The script never receives a pointer, a
// file path or OS access: all capabilities are functions on a host-created
// `host` table, gated by the permissions parsed from plugin.json.  The VM uses
// the vendored Lua 5.4.8 with the inline instruction-budget patch (see
// patches/lua-5.4.8-instruction-budget.patch).  No dlopen and no JIT are
// involved, so script plugins work on iPadOS/iOS.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct lua_State;

namespace casioemu::plugin {

inline constexpr const char* kScriptEngineLua54 = "lua54";
inline constexpr int kScriptApiVersion = 1;
inline constexpr unsigned long long kDefaultScriptBudget = 20000000ull;
inline constexpr unsigned long long kMaxScriptBudget = 200000000ull;
inline constexpr std::size_t kDefaultScriptMemoryLimit = 16ull * 1024ull * 1024ull;

// Permission bits checked by the C functions bound into the `host` table.
// These mirror the manifest permissions used by the declarative loaders.
enum class ScriptPermission : unsigned {
	None = 0,
	Read = 1u << 0,  // emulator.read
	Write = 1u << 1, // emulator.write
	Inject = 1u << 2, // emulator.inject
	Shell = 1u << 3,  // ui.shell
};

inline constexpr unsigned operator|(ScriptPermission a, ScriptPermission b) {
	return static_cast<unsigned>(a) | static_cast<unsigned>(b);
}

// Host services available to a script.  Empty std::functions are safe: the
// corresponding host API returns nil / an empty value.  The registry fills
// these from the existing PluginApi facades; headless tests supply their own.
struct ScriptHostApi {
	std::function<void(const std::string&)> log;
	std::function<std::uint64_t()> tick;
	std::function<std::string()> model;
	// width, height, pitch (framebuffer metadata only -- never a pointer).
	std::function<bool(int&, int&, int&)> framebufferSize;
	// Register name -> value + bit width.
	std::function<bool(const std::string&, std::uint32_t&, unsigned&)> readRegister;
	std::function<bool(std::uint32_t, std::uint8_t&)> readMemory8;
	std::function<void(std::uint32_t, std::uint8_t)> writeMemory8;
	std::function<bool(int, int)> keyIsDown;
	std::function<void(int, int, bool)> key;
	std::function<void(const std::string&)> addWindow;
};

struct ScriptRuntimeConfig {
	std::string pluginId;
	std::string engine = kScriptEngineLua54;
	unsigned permissions = 0;
	unsigned long long budget = kDefaultScriptBudget;
	std::size_t memoryLimit = kDefaultScriptMemoryLimit;
	ScriptHostApi host;
};

struct ScriptRuntimeImpl; // internal state; owned by ScriptRuntime

class ScriptRuntime {
public:
	explicit ScriptRuntime(ScriptRuntimeConfig config);
	~ScriptRuntime();

	ScriptRuntime(const ScriptRuntime&) = delete;
	ScriptRuntime& operator=(const ScriptRuntime&) = delete;

	// Compiles in TEXT-ONLY mode (bytecode dumps are rejected) and runs the
	// top-level chunk.  Errors are copied to `error` and LastError().
	bool Load(const std::string& source, const std::string& chunkName, std::string& error);

	// Optional lifecycle callbacks.  Missing callbacks are a successful no-op.
	// A fresh instruction budget is installed before every invocation.
	bool OnFrame(double dt, std::string& error);
	bool OnKey(int ki, int ko, bool down, std::string& error);
	bool OnUnload(std::string& error);

	void SetBudget(unsigned long long budget);
	[[nodiscard]] unsigned long long Budget() const;
	[[nodiscard]] std::size_t MemoryUsed() const;
	[[nodiscard]] bool HasCallbacks() const;
	[[nodiscard]] unsigned DeniedCount() const;
	[[nodiscard]] const std::string& LastError() const;

private:
	// The callback function and its arguments must already be on the stack.
	[[nodiscard]] bool InvokePrepared(int argumentCount, std::string& error);
	std::unique_ptr<ScriptRuntimeImpl> m_impl;
};

} // namespace casioemu::plugin
