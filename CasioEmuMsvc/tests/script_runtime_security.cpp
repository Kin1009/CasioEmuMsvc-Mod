// Headless regression tests for the plugin ScriptRuntime sandbox escapes.
//
// 1. A to-be-closed variable whose __close metamethod loops must NOT bypass the
//    instruction budget when the budget error unwinds the stack.
// 2. A running script must NOT be able to use the global load() to compile raw
//    binary bytecode into the sandboxed lua_State (the plugin chunk itself is
//    compiled text-only by ScriptRuntime::Load).
// A small positive test keeps the legitimate host/callback features covered.
#include "ScriptRuntime.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace casioemu::plugin;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string& name) {
	++g_checks;
	std::cout << (condition ? "  PASS " : "  FAIL ") << name << "\n";
	if (!condition)
		++g_failures;
}

bool Contains(const std::vector<std::string>& logs, const std::string& needle) {
	for (const auto& value : logs)
		if (value.find(needle) != std::string::npos)
			return true;
	return false;
}

ScriptRuntime MakeRuntime(std::vector<std::string>* logs, unsigned long long budget) {
	ScriptRuntimeConfig config;
	config.pluginId = "security.regression";
	config.budget = budget;
	config.permissions = static_cast<unsigned>(ScriptPermission::Read) |
		static_cast<unsigned>(ScriptPermission::Write) |
		static_cast<unsigned>(ScriptPermission::Inject) |
		static_cast<unsigned>(ScriptPermission::Shell);
	config.host.log = [logs](const std::string& text) {
		if (logs)
			logs->push_back(text);
	};
	config.host.tick = []() -> std::uint64_t { return 7; };
	config.host.model = []() -> std::string { return "CG-100"; };
	config.host.framebufferSize = [](int& w, int& h, int& pitch) {
		w = 192; h = 64; pitch = 192; return true;
	};
	return ScriptRuntime(std::move(config));
}

// Escape 1: budget-exhaustion __close bypass (used to hang forever).
void TestCloseEscape() {
	std::cout << "== regression: __close budget bypass ==\n";
	const std::string source = R"(
function on_frame(dt) host.log('frame') end
local guard = setmetatable({}, {
  __close = function() while true do end end
})
local x <close> = guard
host.log('before loop')
while true do end
)";
	std::vector<std::string> logs;
	ScriptRuntime runtime = MakeRuntime(&logs, 20000ull);
	std::string error;
	// The CTest fixture sets a 20s TIMEOUT: if __close were budget-exempt this
	// call would never return and the test would be killed instead of passing.
	const bool loaded = runtime.Load(source, "close_escape.lua", error);
	Check(!loaded, "budget error from __close escape returns (no hang)");
	Check(error.find("script.budget:") == 0 &&
		error.find("instruction budget exhausted") != std::string::npos,
		"exhaustion reported as script.budget (not a hang/abort)");
	Check(Contains(logs, "before loop"), "script ran up to the budget-exhausting loop");

	// A fresh budget must clear the fail-closed state and restore normal calls.
	runtime.SetBudget(20000000ull);
	std::string cb_error;
	Check(runtime.OnFrame(0.016, cb_error) && cb_error.empty(),
		"budget refilled after exhaustion; on_frame works again");
	Check(Contains(logs, "frame"), "on_frame callback still runs after refill");
}

// Escape 2: raw bytecode load() from inside the sandbox.
void TestBytecodeEscape() {
	std::cout << "== regression: load() binary bytecode escape ==\n";
	const std::string source = R"(
local bc = string.dump(function() return 42 end)
local ok, res = pcall(load, bc)
host.log('load_type=' .. type(load))
host.log('loadstring_type=' .. type(loadstring))
host.log('bytecode_pcall_ok=' .. tostring(ok))
host.log('bytecode_pcall_err=' .. tostring(res))
)";
	std::vector<std::string> logs;
	ScriptRuntime runtime = MakeRuntime(&logs, 20000000ull);
	std::string error;
	const bool loaded = runtime.Load(source, "bytecode_escape.lua", error);
	Check(loaded, "probe script itself runs to completion");
	Check(Contains(logs, "load_type=nil"), "global load removed");
	Check(Contains(logs, "loadstring_type=nil"), "legacy loadstring removed");
	Check(Contains(logs, "bytecode_pcall_ok=false"), "binary bytecode load rejected");
	Check(Contains(logs, "bytecode_pcall_err=attempt to call a nil value"),
		"rejection is an ordinary Lua error, not a VM escape");
}

// Guard against collateral damage to the documented script API.
void TestLegitFeatures() {
	std::cout << "== positive: legitimate script API ==\n";
	const std::string source = R"(
host.log('hello')
local model = host.get_model()
local w, h, pitch = host.get_framebuffer_size()
host.log('fb=' .. tostring(w) .. 'x' .. tostring(h) .. ' p=' .. tostring(pitch))
host.log('model=' .. tostring(model))
function on_frame(dt) host.log('frame=' .. tostring(dt > 0)) end
function on_key(ki, ko, down) host.log('key=' .. ki .. ',' .. ko .. ',' .. tostring(down)) end
function on_unload() host.log('unload') end
)";
	std::vector<std::string> logs;
	ScriptRuntime runtime = MakeRuntime(&logs, 20000000ull);
	std::string error;
	Check(runtime.Load(source, "legit.lua", error), "top-level chunk runs");
	Check(Contains(logs, "hello"), "host.log works");
	Check(Contains(logs, "fb=192x64 p=192"), "arch/framebuffer API works");
	Check(Contains(logs, "model=CG-100"), "host.get_model works");
	std::string cb_error;
	Check(runtime.OnFrame(0.016, cb_error) && Contains(logs, "frame=true"), "on_frame works");
	Check(runtime.OnKey(1, 2, true, cb_error) && Contains(logs, "key=1,2,true"), "on_key works");
	Check(runtime.OnUnload(cb_error) && Contains(logs, "unload"), "on_unload works");
	Check(runtime.DeniedCount() == 0, "no permission denials");
}

} // namespace

int main() {
	TestCloseEscape();
	TestBytecodeEscape();
	TestLegitFeatures();
	std::cout << (g_failures == 0 ? "RESULT: PASS" : "RESULT: FAIL")
		<< " (" << (g_checks - g_failures) << "/" << g_checks << ")\n";
	return g_failures == 0 ? 0 : 1;
}
