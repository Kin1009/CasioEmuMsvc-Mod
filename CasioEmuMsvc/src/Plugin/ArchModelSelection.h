#pragma once
// Descriptor-driven model selection (portable, host-testable).
//
// The architecture dispatch in casioemu.cpp already picks a core from the
// model's descriptor.  Startup model selection used to be hardcoded to one
// bundled pack, which left iPadOS with no data-driven way to choose a board.
// This helper selects among already-parsed model packs using only their
// descriptors and the set of registered providers:
//
//   1. an explicitly preferred path wins (deliberate user choice);
//   2. a board model whose provider is available wins (descriptor-driven);
//   3. otherwise a built-in (non-board) model keeps the interpreter path;
//   4. otherwise the first board model is returned with provider_missing=true
//      and an honest reason, so the caller can log the degraded state.
//
// No hardware id or board name is consulted. Header-only so the host-side
// test needs no SDL/iOS SDK and the port trees need no CMake reconfigure.

#include "ArchDescriptor.h"
#include "ModelInfo.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace casioemu {
namespace arch {

struct ModelCandidate {
	std::filesystem::path path;
	ModelInfo model;
};

/// True when the model's `arch_provider` descriptor key has a usable provider.
using ProviderAvailableFn = std::function<bool(const Descriptor&)>;

struct ModelSelection {
	std::filesystem::path path;
	Descriptor descriptor;
	std::string name;
	bool found = false;
	bool board = false;
	bool provider_missing = false;
	std::string reason;
};

inline ModelSelection MakeSelection(const ModelCandidate& candidate, const Descriptor& descriptor,
	bool board, bool provider_missing, const char* reason) {
	ModelSelection selection;
	selection.path = candidate.path;
	selection.descriptor = descriptor;
	selection.name = candidate.model.model_name;
	selection.found = true;
	selection.board = board;
	selection.provider_missing = provider_missing;
	selection.reason = reason;
	return selection;
}

inline ModelSelection SelectModel(const std::vector<ModelCandidate>& candidates,
	const ProviderAvailableFn& available, const std::string& preferred = {}) {
	ModelSelection none;
	none.reason = "no model candidates";

	// 1. Explicit path (env/argv on desktop, bundled default on iPadOS).
	if (!preferred.empty()) {
		for (const auto& candidate : candidates) {
			if (candidate.path.string() == preferred) {
				const Descriptor descriptor = FromModel(candidate.model);
				const bool board = IsBoardModel(candidate.model);
				const bool missing = board && available && !available(descriptor);
				return MakeSelection(candidate, descriptor, board, missing,
					missing ? "explicit board model has no registered provider"
						: "explicitly selected model");
			}
		}
	}

	// 2. Board model with a registered provider for its descriptor id.
	for (const auto& candidate : candidates) {
		const Descriptor descriptor = FromModel(candidate.model);
		if (IsBoardModel(candidate.model) && available && available(descriptor))
			return MakeSelection(candidate, descriptor, true, false,
				"board model selected by arch_provider descriptor");
	}

	// 3. Built-in model: the interpreter path, always safe.
	for (const auto& candidate : candidates) {
		if (!IsBoardModel(candidate.model))
			return MakeSelection(candidate, FromModel(candidate.model), false, false,
				"built-in model selected (interpreter)");
	}

	// 4. Only board models with unavailable providers. Report honestly.
	for (const auto& candidate : candidates) {
		if (IsBoardModel(candidate.model))
			return MakeSelection(candidate, FromModel(candidate.model), true, true,
				"board model selected but no provider is registered; falling back to built-in core");
	}
	return none;
}

/// One-line log description of a selection decision.
inline std::string DescribeSelection(const ModelSelection& selection) {
	if (!selection.found)
		return "[ArchModel] no model selected: " + selection.reason;
	return "[ArchModel] selected '" + selection.path.string() + "' (" +
		(selection.name.empty() ? std::string("unnamed") : selection.name) + "): " +
		selection.reason;
}

} // namespace arch
} // namespace casioemu
