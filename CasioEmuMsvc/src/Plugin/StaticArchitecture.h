#pragma once
// Static architecture-provider bootstrap.
//
// iPadOS forbids dlopen and JIT, so an architecture core compiled into the app
// must register itself at startup.  This header provides the missing glue:
//
//   * a process-local table of zero-argument factory functions;
//   * a macro that adds a factory from a static initializer (no host edit);
//   * one idempotent bootstrap call that creates each provider and routes it
//     through RegisterStaticArchitectureProvider() -- the single generic hook;
//   * an honest report (attempted/registered/duplicate/failed) for logging.
//
// Nothing here knows a board name or a hardware id; dispatch stays descriptor
// driven (see ArchDescriptor.h).  Header-only on purpose: the port trees glob
// Plugin/*.cpp without CONFIGURE_DEPENDS, so a new .cpp would need a reconfigure.

#include "IArchitectureProvider.h"
#include "PluginMan.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace casioemu {
namespace arch {

using StaticProviderFactory = casioemu::IArchitectureProvider* (*)();
using StaticProviderLog = std::function<void(const std::string&)>;

/// Factories registered by CASIOEMU_REGISTER_STATIC_ARCH_PROVIDER.
inline std::vector<StaticProviderFactory>& StaticProviderFactories() {
	static std::vector<StaticProviderFactory> factories;
	return factories;
}

/// Add a factory. Duplicates (same function pointer) are ignored.
inline bool RegisterStaticProviderFactory(StaticProviderFactory factory) {
	if (!factory)
		return false;
	auto& factories = StaticProviderFactories();
	for (StaticProviderFactory existing : factories)
		if (existing == factory)
			return false;
	factories.push_back(factory);
	return true;
}

struct StaticBootstrapReport {
	std::size_t factories = 0;
	std::size_t registered = 0;
	std::size_t duplicates = 0;
	std::size_t failed = 0;
	std::vector<std::string> ids;
};

inline StaticBootstrapReport& StaticReport() {
	static StaticBootstrapReport report;
	return report;
}

/// Create every registered provider once and register it through the generic
/// hook. Idempotent: later calls return the first call's report unchanged.
/// Ownership passes to the host registry; providers live for the process
/// lifetime (the registry intentionally never deletes them).
inline const StaticBootstrapReport& RegisterStaticArchitectureProviders(const StaticProviderLog& log = {}) {
	static bool done = false;
	StaticBootstrapReport& report = StaticReport();
	if (done)
		return report;
	done = true;

	const auto emit = [&log](const std::string& line) {
		if (log)
			log(line);
	};

	for (StaticProviderFactory factory : StaticProviderFactories()) {
		++report.factories;
		casioemu::IArchitectureProvider* provider = factory ? factory() : nullptr;
		if (!provider) {
			++report.failed;
			emit("[ARCH] static provider factory returned null");
			continue;
		}
		const char* id = provider->GetId();
		const std::string id_string = id ? id : "";
		if (FindArchitectureProviderById(id_string.c_str())) {
			++report.duplicates;
			emit("[ARCH] static provider '" + id_string + "' already registered; skipped");
			continue;
		}
		if (RegisterStaticArchitectureProvider(provider)) {
			++report.registered;
			report.ids.push_back(id_string);
			emit("[ARCH] static provider '" + id_string + "' registered at build time (no dlopen)");
		}
		else {
			++report.failed;
			emit("[ARCH] static provider '" + id_string + "' failed to register");
		}
	}
	if (report.factories == 0)
		emit("[ARCH] no static architecture providers compiled in; board models will use the built-in interpreter");
	return report;
}

} // namespace arch
} // namespace casioemu

/// Two-level token pasting. `##` suppresses expansion of its operands, so
/// `name ## __COUNTER__` pastes the literal token `__COUNTER__` and every
/// registration in a translation unit declares the SAME identifier. Expanding
/// `__COUNTER__` in an intermediate macro first gives each registration a
/// unique symbol (FA-01 regression).
#define CASIOEMU_ARCH_JOIN_IMPL(a, b) a##b
#define CASIOEMU_ARCH_JOIN(a, b) CASIOEMU_ARCH_JOIN_IMPL(a, b)

/// Declare a statically linked architecture provider. `factory` must be a
/// zero-argument function returning IArchitectureProvider*; call this once per
/// core in that core's translation unit. Multiple registrations in one TU are
/// supported (each gets a distinct `__COUNTER__`-suffixed symbol). Example:
///
///   static casioemu::IArchitectureProvider* MakeMyCore() { return new MyCore(); }
///   CASIOEMU_REGISTER_STATIC_ARCH_PROVIDER(MakeMyCore);
#define CASIOEMU_REGISTER_STATIC_ARCH_PROVIDER(factory)                                  \
	namespace {                                                                            \
	[[maybe_unused]] const bool CASIOEMU_ARCH_JOIN(casioemu_static_arch_reg_, __COUNTER__) = \
		::casioemu::arch::RegisterStaticProviderFactory(&(factory));                      \
	}
