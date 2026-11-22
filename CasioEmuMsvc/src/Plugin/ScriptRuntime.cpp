#include "ScriptRuntime.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace casioemu::plugin {
namespace {

struct AllocState {
	std::size_t used = 0;
	std::size_t limit = kDefaultScriptMemoryLimit;
};

// Enforces the per-plugin Lua heap cap.  Lua treats a null return as OOM and
// turns it into a catchable Lua error; no other Lua code sees raw memory.
void* ScriptAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
	auto* alloc = static_cast<AllocState*>(ud);
	if (nsize == 0) {
		if (ptr) {
			alloc->used -= osize;
			std::free(ptr);
		}
		return nullptr;
	}
	if (nsize > alloc->limit)
		return nullptr;
	if (nsize > osize && nsize - osize > alloc->limit - alloc->used)
		return nullptr;
	void* result = std::realloc(ptr, nsize);
	if (!result)
		return nullptr;
	alloc->used += nsize - osize;
	return result;
}

} // namespace

// State shared with button callbacks handed to the host.  ScriptRuntimeImpl is
// destroyed before lua_close, so callbacks must not keep a raw pointer to it:
// a weak_ptr makes a tap after script unload a safe no-op.
struct ScriptButtonState {
	lua_State* L = nullptr;
	bool alive = true;
	unsigned long long budget = 0;
	std::function<void(const std::string&)> log;
	std::recursive_mutex callMutex;
	std::vector<int> refs;
};

// Internal per-plugin state.  Also serves as the light-userdata upvalue of
// every host API closure so permission checks cannot be forged from Lua.
struct ScriptRuntimeImpl {
	lua_State* L = nullptr;
	ScriptRuntimeConfig config;
	AllocState alloc;
	std::string lastError;
	unsigned deniedCalls = 0;
	unsigned logLines = 0;
	std::shared_ptr<ScriptButtonState> buttons = std::make_shared<ScriptButtonState>();
	std::set<std::string> loggedOnce;
};

namespace {

int Deny(lua_State* L, ScriptRuntimeImpl* impl, const char* api, const char* permission) {
	++impl->deniedCalls;
	return luaL_error(L, "permission denied: '%s' requires '%s'", api, permission);
}

bool May(ScriptRuntimeImpl* impl, ScriptPermission permission) {
	return (impl->config.permissions & static_cast<unsigned>(permission)) != 0;
}

// One warning per missing/unsupported host facility: a script polls
// arch_status on every frame, so an unbound callback must not flood the log.
void LogOnce(ScriptRuntimeImpl* impl, const std::string& key, const std::string& message) {
	if (!impl->loggedOnce.insert(key).second)
		return;
	if (impl->config.host.log)
		impl->config.host.log(message);
}

// ---------------------------------------------------------------- host table
// Every function captures the Impl as a light userdata upvalue.  Lua code
// cannot forge or replace the upvalue, so permissions cannot be escalated.

int HLog(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	const char* text = luaL_checkstring(L, 1);
	// Rate limit: a script must not be able to flood the host log.
	if (impl->logLines++ < 200 && impl->config.host.log)
		impl->config.host.log(text);
	return 0;
}

int HTick(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	const std::uint64_t tick = impl->config.host.tick ? impl->config.host.tick() : 0;
	lua_pushinteger(L, static_cast<lua_Integer>(tick));
	return 1;
}

int HGetModel(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Read))
		return Deny(L, impl, "get_model", "emulator.read");
	const std::string model = impl->config.host.model ? impl->config.host.model() : std::string();
	lua_pushlstring(L, model.data(), model.size());
	return 1;
}

int HGetFramebufferSize(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Read))
		return Deny(L, impl, "get_framebuffer_size", "emulator.read");
	int width = 0, height = 0, pitch = 0;
	if (!impl->config.host.framebufferSize || !impl->config.host.framebufferSize(width, height, pitch)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, width);
	lua_pushinteger(L, height);
	lua_pushinteger(L, pitch);
	return 3;
}

int HFbGetInfo(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Read))
		return Deny(L, impl, "fb_get_info", "emulator.read");
	int width = 0, height = 0, pitch = 0;
	if (!impl->config.host.framebufferSize || !impl->config.host.framebufferSize(width, height, pitch)) {
		lua_pushnil(L);
		return 1;
	}
	lua_createtable(L, 0, 3);
	lua_pushinteger(L, width);
	lua_setfield(L, -2, "width");
	lua_pushinteger(L, height);
	lua_setfield(L, -2, "height");
	lua_pushinteger(L, pitch);
	lua_setfield(L, -2, "pitch");
	return 1;
}

int HRegRead(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Read))
		return Deny(L, impl, "reg_read", "emulator.read");
	const char* name = luaL_checkstring(L, 1);
	std::uint32_t value = 0;
	unsigned bits = 0;
	if (!impl->config.host.readRegister || !impl->config.host.readRegister(name, value, bits)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, static_cast<lua_Integer>(value));
	lua_pushinteger(L, static_cast<lua_Integer>(bits));
	return 2;
}

int HMemRead8(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Read))
		return Deny(L, impl, "mem_read8", "emulator.read");
	const auto address = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
	std::uint8_t value = 0;
	if (!impl->config.host.readMemory8 || !impl->config.host.readMemory8(address, value)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, value);
	return 1;
}

int HMemWrite8(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Write))
		return Deny(L, impl, "mem_write8", "emulator.write");
	const auto address = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
	const auto value = static_cast<std::uint8_t>(luaL_checkinteger(L, 2));
	if (impl->config.host.writeMemory8)
		impl->config.host.writeMemory8(address, value);
	return 0;
}

int HKeyIsDown(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Read))
		return Deny(L, impl, "key_is_down", "emulator.read");
	const int ki = static_cast<int>(luaL_checkinteger(L, 1));
	const int ko = static_cast<int>(luaL_checkinteger(L, 2));
	const bool down = impl->config.host.keyIsDown ? impl->config.host.keyIsDown(ki, ko) : false;
	lua_pushboolean(L, down ? 1 : 0);
	return 1;
}

int HAddWindow(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Shell))
		return Deny(L, impl, "ui_add_window", "ui.shell");
	if (impl->config.host.addWindow)
		impl->config.host.addWindow(luaL_checkstring(L, 1));
	return 0;
}

// host.arch_control(cmd [, arg]) [emulator.inject | ui.shell] -----------------
// Dispatches to the host's architecture control bridge.  Commands mirror the
// C ABI: "reboot", "reset", "power_on", "enter_diag" (arg = diag mode).
// "enter_diag(1)" is also accepted for callers that encode the mode in cmd.
int HArchControl(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Inject) && !May(impl, ScriptPermission::Shell))
		return Deny(L, impl, "arch_control", "emulator.inject");

	std::string command = luaL_checkstring(L, 1);
	int arg = static_cast<int>(luaL_optinteger(L, 2, 0));
	const std::string prefix = "enter_diag(";
	if (command.rfind(prefix, 0) == 0 && !command.empty() && command.back() == ')') {
		const std::string digits = command.substr(prefix.size(), command.size() - prefix.size() - 1);
		char* end = nullptr;
		const long mode = std::strtol(digits.c_str(), &end, 10);
		if (end && *end == '\0') {
			arg = static_cast<int>(mode);
			command = "enter_diag";
		}
	}

	const bool known = command == "reboot" || command == "reset" ||
		command == "power_on" || command == "enter_diag";
	if (!known) {
		LogOnce(impl, "arch_control:unknown:" + command,
			"[script] arch_control('" + command + "') is not a recognised command");
		lua_pushboolean(L, 0);
		return 1;
	}
	if (!impl->config.host.archControl) {
		LogOnce(impl, "arch_control:unavailable",
			"[script] host.arch_control() unavailable (no architecture core bound)");
		lua_pushboolean(L, 0);
		return 1;
	}
	const bool accepted = impl->config.host.archControl(command, arg);
	if (!accepted)
		LogOnce(impl, "arch_control:rejected:" + command,
			"[script] arch_control('" + command + "') rejected by host (unsupported or no core)");
	lua_pushboolean(L, accepted ? 1 : 0);
	return 1;
}

// host.arch_status() [emulator.read] -----------------------------------------
// Returns a plain table: { running, paused, cycles, model, hw_id }.
// Unavailable host state yields nil (never an error).
int HArchStatus(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Read))
		return Deny(L, impl, "arch_status", "emulator.read");
	if (!impl->config.host.archStatus) {
		LogOnce(impl, "arch_status:unavailable",
			"[script] host.arch_status() unavailable (no emulator state bound)");
		lua_pushnil(L);
		return 1;
	}
	ScriptArchStatus status;
	if (!impl->config.host.archStatus(status)) {
		LogOnce(impl, "arch_status:unavailable",
			"[script] host.arch_status() unavailable (no emulator state bound)");
		lua_pushnil(L);
		return 1;
	}
	lua_createtable(L, 0, 6);
	lua_pushboolean(L, status.running ? 1 : 0);
	lua_setfield(L, -2, "running");
	lua_pushboolean(L, status.paused ? 1 : 0);
	lua_setfield(L, -2, "paused");
	lua_pushinteger(L, static_cast<lua_Integer>(status.cycles));
	lua_setfield(L, -2, "cycles");
	lua_pushlstring(L, status.model.data(), status.model.size());
	lua_setfield(L, -2, "model");
	lua_pushinteger(L, status.hardwareId);
	lua_setfield(L, -2, "hw_id");
	lua_pushinteger(L, status.hardwareId);
	lua_setfield(L, -2, "hardware_id");
	return 1;
}

// host.ui_add_button(win, label, callback) [ui.shell] -------------------------
// Keeps the Lua callback in the registry and hands the host a std::function
// that re-enters the script with a fresh instruction budget when tapped.
int HAddButton(lua_State* L) {
	auto* impl = static_cast<ScriptRuntimeImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
	if (!May(impl, ScriptPermission::Shell))
		return Deny(L, impl, "ui_add_button", "ui.shell");
	const char* window = lua_tostring(L, 1); // may be nil in Phase 1 (no real windows)
	if (!window)
		window = "";
	const char* label = luaL_checkstring(L, 2);
	luaL_checktype(L, 3, LUA_TFUNCTION);

	lua_pushvalue(L, 3);
	const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	const auto state = impl->buttons;
	std::function<void()> invoke = [weak = std::weak_ptr<ScriptButtonState>(state), ref]() {
		const auto shared = weak.lock();
		if (!shared || !shared->alive || !shared->L)
			return; // script unloaded: a late tap is a no-op
		std::lock_guard<std::recursive_mutex> guard(shared->callMutex);
		lua_State* thread = shared->L;
		lua_settop(thread, 0);
		lua_setinstrbudget(thread, shared->budget);
		lua_rawgeti(thread, LUA_REGISTRYINDEX, ref);
		if (lua_pcall(thread, 0, 0, 0) != LUA_OK) {
			const char* message = lua_tostring(thread, -1);
			if (shared->log)
				shared->log(std::string("[script] ui button callback error: ") + (message ? message : "unknown"));
		}
		lua_settop(thread, 0);
	};

	if (!impl->config.host.addButton) {
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		LogOnce(impl, "ui_add_button:unavailable",
			"[script] host.ui_add_button() unavailable (script window UI is not bound)");
		lua_pushboolean(L, 0);
		return 1;
	}
	const int id = impl->config.host.addButton(window, label, std::move(invoke));
	if (id <= 0) {
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		LogOnce(impl, "ui_add_button:unavailable",
			"[script] host.ui_add_button() unavailable (script window UI is not bound)");
		lua_pushboolean(L, 0);
		return 1;
	}
	state->refs.push_back(ref);
	lua_pushinteger(L, id);
	return 1;
}

void Bind(lua_State* L, ScriptRuntimeImpl* impl, const char* name, lua_CFunction fn) {
	lua_pushlightuserdata(L, impl);
	lua_pushcclosure(L, fn, 1);
	lua_setfield(L, -2, name);
}

void RegisterHostTable(lua_State* L, ScriptRuntimeImpl* impl) {
	lua_newtable(L);
	Bind(L, impl, "log", HLog);
	Bind(L, impl, "get_tick", HTick);
	Bind(L, impl, "get_model", HGetModel);
	Bind(L, impl, "get_framebuffer_size", HGetFramebufferSize);
	Bind(L, impl, "fb_get_info", HFbGetInfo);
	Bind(L, impl, "reg_read", HRegRead);
	Bind(L, impl, "mem_read8", HMemRead8);
	Bind(L, impl, "mem_write8", HMemWrite8);
	Bind(L, impl, "key_is_down", HKeyIsDown);
	Bind(L, impl, "ui_add_window", HAddWindow);
	Bind(L, impl, "arch_control", HArchControl);
	Bind(L, impl, "arch_status", HArchStatus);
	Bind(L, impl, "ui_add_button", HAddButton);
	lua_setglobal(L, "host");

	lua_pushinteger(L, kScriptApiVersion);
	lua_setglobal(L, "HOST_API_VERSION");
}

// Only side-effect-free libraries are registered.  `io`, `os`, `package` and
// `debug` are never opened; require/dofile/loadfile/collectgarbage are removed
// explicitly so the globals cannot be used as capability escapes.
void RegisterSafeLibraries(lua_State* L) {
	static const luaL_Reg libraries[] = {
		{ "_G", luaopen_base },
		{ LUA_TABLIBNAME, luaopen_table },
		{ LUA_STRLIBNAME, luaopen_string },
		{ LUA_MATHLIBNAME, luaopen_math },
		{ LUA_UTF8LIBNAME, luaopen_utf8 },
		{ nullptr, nullptr },
	};
	for (const luaL_Reg* lib = libraries; lib->func; ++lib) {
		luaL_requiref(L, lib->name, lib->func, 1);
		lua_pop(L, 1);
	}
	static const char* removed[] = { "require", "dofile", "loadfile", "collectgarbage", "io", "os", "package", "debug" };
	for (const char* name : removed) {
		lua_pushnil(L);
		lua_setglobal(L, name);
	}
}

} // namespace

ScriptRuntime::ScriptRuntime(ScriptRuntimeConfig config)
	: m_impl(std::make_unique<ScriptRuntimeImpl>()) {
	m_impl->config = std::move(config);
	m_impl->alloc.limit = m_impl->config.memoryLimit;
	m_impl->L = lua_newstate(ScriptAlloc, &m_impl->alloc);
	if (!m_impl->L) {
		m_impl->lastError = "script.runtime: cannot create Lua state (memory limit)";
		return;
	}
	m_impl->buttons->L = m_impl->L;
	m_impl->buttons->budget = m_impl->config.budget;
	m_impl->buttons->log = m_impl->config.host.log;
	RegisterSafeLibraries(m_impl->L);
	RegisterHostTable(m_impl->L, m_impl.get());
	// Start deterministic: no budget active outside a host-invoked call.
	lua_setinstrbudget(m_impl->L, 0);
}

ScriptRuntime::~ScriptRuntime() {
	if (m_impl && m_impl->L) {
		const auto state = m_impl->buttons;
		std::lock_guard<std::recursive_mutex> guard(state->callMutex);
		state->alive = false;
		for (int ref : state->refs)
			luaL_unref(m_impl->L, LUA_REGISTRYINDEX, ref);
		state->refs.clear();
		state->L = nullptr;
		lua_close(m_impl->L);
		m_impl->L = nullptr;
	}
}

bool ScriptRuntime::Load(const std::string& source, const std::string& chunkName, std::string& error) {
	if (!m_impl->L) {
		error = m_impl->lastError.empty() ? "script.runtime: no Lua state" : m_impl->lastError;
		return false;
	}
	std::lock_guard<std::recursive_mutex> guard(m_impl->buttons->callMutex);
	lua_settop(m_impl->L, 0);
	// Text only: a plugin cannot ship precompiled bytecode that exploits the loader.
	const int status = luaL_loadbufferx(m_impl->L, source.data(), source.size(), chunkName.c_str(), "t");
	if (status != LUA_OK) {
		error = "script.compile: " + std::string(lua_tostring(m_impl->L, -1) ? lua_tostring(m_impl->L, -1) : "compile failed");
		m_impl->lastError = error;
		lua_settop(m_impl->L, 0);
		return false;
	}
	lua_setinstrbudget(m_impl->L, m_impl->config.budget);
	const int call = lua_pcall(m_impl->L, 0, 0, 0);
	if (call != LUA_OK) {
		const char* message = lua_tostring(m_impl->L, -1);
		const std::string detail = message ? message : "runtime error";
		error = std::string(detail.find("instruction budget exhausted") != std::string::npos
			? "script.budget: " : "script.runtime: ") + detail;
		m_impl->lastError = error;
		lua_settop(m_impl->L, 0);
		return false;
	}
	lua_settop(m_impl->L, 0);
	m_impl->lastError.clear();
	return true;
}

bool ScriptRuntime::InvokePrepared(int argumentCount, std::string& error) {
	if (!m_impl->L) {
		error = "script.runtime: no Lua state";
		return false;
	}
	std::lock_guard<std::recursive_mutex> guard(m_impl->buttons->callMutex);
	lua_setinstrbudget(m_impl->L, m_impl->config.budget);
	const int status = lua_pcall(m_impl->L, argumentCount, 0, 0);
	if (status != LUA_OK) {
		const char* message = lua_tostring(m_impl->L, -1);
		const std::string detail = std::string(message ? message : "callback failed");
		error = std::string(detail.find("instruction budget exhausted") != std::string::npos
			? "script.budget: " : "script.runtime: ") + detail;
		m_impl->lastError = error;
		lua_settop(m_impl->L, 0);
		return false;
	}
	lua_settop(m_impl->L, 0);
	return true;
}

bool ScriptRuntime::OnFrame(double dt, std::string& error) {
	lua_settop(m_impl->L, 0);
	lua_getglobal(m_impl->L, "on_frame");
	if (!lua_isfunction(m_impl->L, -1)) {
		lua_settop(m_impl->L, 0);
		return true; // optional callback
	}
	lua_pushnumber(m_impl->L, dt);
	return InvokePrepared(1, error);
}

bool ScriptRuntime::OnKey(int ki, int ko, bool down, std::string& error) {
	if (!May(m_impl.get(), ScriptPermission::Read))
		return true; // on_key requires emulator.read
	lua_settop(m_impl->L, 0);
	lua_getglobal(m_impl->L, "on_key");
	if (!lua_isfunction(m_impl->L, -1)) {
		lua_settop(m_impl->L, 0);
		return true;
	}
	lua_pushinteger(m_impl->L, ki);
	lua_pushinteger(m_impl->L, ko);
	lua_pushboolean(m_impl->L, down ? 1 : 0);
	return InvokePrepared(3, error);
}

bool ScriptRuntime::OnUnload(std::string& error) {
	lua_settop(m_impl->L, 0);
	lua_getglobal(m_impl->L, "on_unload");
	if (!lua_isfunction(m_impl->L, -1)) {
		lua_settop(m_impl->L, 0);
		return true;
	}
	return InvokePrepared(0, error);
}

void ScriptRuntime::SetBudget(unsigned long long budget) {
	m_impl->config.budget = budget;
	std::lock_guard<std::recursive_mutex> guard(m_impl->buttons->callMutex);
	m_impl->buttons->budget = budget;
	if (m_impl->L)
		lua_setinstrbudget(m_impl->L, 0); // refilled on next call
}

unsigned long long ScriptRuntime::Budget() const {
	return m_impl->config.budget;
}

std::size_t ScriptRuntime::MemoryUsed() const {
	return m_impl->alloc.used;
}

bool ScriptRuntime::HasCallbacks() const {
	if (!m_impl->L)
		return false;
	bool found = false;
	for (const char* name : { "on_load", "on_frame", "on_key", "on_unload" }) {
		lua_getglobal(m_impl->L, name);
		if (lua_isfunction(m_impl->L, -1))
			found = true;
		lua_pop(m_impl->L, 1);
	}
	return found;
}

unsigned ScriptRuntime::DeniedCount() const {
	return m_impl->deniedCalls;
}

const std::string& ScriptRuntime::LastError() const {
	return m_impl->lastError;
}

} // namespace casioemu::plugin
