// CalcScreenWindow.cpp - the complete calculator inside the debugger.
//
// The desktop build is single-window: the debugger owns the only OS window, so
// there is no separate calculator window any more. Emulator::Frame() renders
// the whole model (face, key labels, LCD and status sprites) into an offscreen
// texture, and this window presents exactly that image - the same picture the
// old external window showed. The LCD-only path is kept as a fallback for
// builds or models without a model face.
#include "WizToolchain.h"

#include "Chipset/Chipset.hpp"
#include "DiagScreenDetector.h"
#include "Peripheral/Screen.hpp"
#include "Shell/SessionHost.h"
#include "ToolUi.hpp"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr const char* kScreenWindowName = "Calculator";

struct Placement {
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	bool hovered = false;
};

// Draw a texture centred in the available area (minus the controls strip).
// In Fit mode the calculator grows until one pair of edges is reached.  Use the
// exact fractional scale (not floor) so a 1.34x panel really fills the panel;
// the texture keeps nearest-neighbour filtering, so the keys stay crisp.
Placement DrawCentred(ImTextureID image, int width, int height, float footer, bool fit, float zoom) {
	Placement place;
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	float area_w = avail.x;
	float area_h = avail.y - footer;
	if (area_w < 1.0f || area_h < 1.0f)
		return place;

	float raw = std::min(area_w / static_cast<float>(width), area_h / static_cast<float>(height));
	float scale = fit ? raw : zoom;
	if (scale <= 0.0f)
		scale = 1.0f;
	place.w = width * scale;
	place.h = height * scale;

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	place.x = origin.x + (area_w - place.w) * 0.5f;
	place.y = origin.y + (area_h - place.h) * 0.5f;
	ImGui::SetCursorScreenPos(ImVec2(place.x, place.y));
	ImGui::Image(image, ImVec2(place.w, place.h));
	place.hovered = ImGui::IsItemHovered();
	// Leave the cursor below the picture so the controls line up at the bottom.
	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + area_h));
	return place;
}

}  // namespace

class CalcScreenWindow : public UIWindow {
public:
	CalcScreenWindow() : UIWindow(kScreenWindowName) {
		inital_size = ImVec2(420, 700);
		flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	}

	~CalcScreenWindow() override {
		if (texture_)
			SDL_DestroyTexture(texture_);
	}

	void RenderCore() override {
		if (!m_emu) {
			ImGui::TextDisabled("The calculator is not running.");
			return;
		}

		// The calculator panel keeps only ONE zoom control row. Reserve its real
		// height (44 pt minimum touch target on iPadOS) plus one gap so a fitted
		// 128x64 or 396x224 face never grows underneath the controls.
		const float footer = std::max(ImGui::GetFrameHeightWithSpacing(), 44.0f) +
		                     ImGui::GetStyle().ItemSpacing.y;

#ifdef SINGLE_WINDOW
		// The whole calculator, as the external window used to render it.  Use
		// the session mirror: a secondary emulator's texture belongs to its own
		// hidden SDL renderer and cannot be drawn by the main renderer.
		int image_w = 0;
		int image_h = 0;
		if (SDL_Texture* composite = casioemu::session::ActiveTexture(&image_w, &image_h)) {
			if (image_w > 0 && image_h > 0) {
				Placement place = DrawCentred(reinterpret_cast<ImTextureID>(composite), image_w,
											  image_h, footer, fit_, zoom_);
				ForwardClicks(place, image_w, image_h);
				DrawPressedKeyHighlight(place, image_w, image_h);
				DrawControls(image_w, image_h);
				return;
			}
		}
#endif

		using casioemu::IScreenFrameProvider;
		IScreenFrameProvider* provider = m_emu->chipset.QueryInterface<IScreenFrameProvider>();
		if (!provider) {
			ImGui::TextDisabled("This model has no pixel screen.");
			return;
		}

		// Fallback: only the LCD pixels (models without a model face, or builds
		// that still use a separate calculator window).
		provider->UpdateFrameAlpha();
		const int width = provider->GetFrameWidth();
		const int height = provider->GetFrameHeight();
		if (width <= 0 || height <= 0)
			return;
		if (!texture_ || width != tex_width_ || height != tex_height_)
			RecreateTexture(width, height);
		if (!texture_)
			return;

		const casioemu::ColourInfo& ink = m_emu->ModelDefinition.ink_color;
		frame_.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
		provider->WriteFrameRgba(frame_.data(), ink.r, ink.g, ink.b);
		SDL_UpdateTexture(texture_, nullptr, frame_.data(), width * 4);

		Placement place = DrawCentred(reinterpret_cast<ImTextureID>(texture_), width, height,
									  footer, fit_, zoom_);
		ForwardClicks(place, width, height);
		DrawControls(width, height);
	}

private:
	// Clicking the picture presses the calculator key under the cursor. The old
	// external window did this through the window rectangle; the docked image is
	// scaled, so map the mouse position back to model coordinates here.
	void ForwardClicks(const Placement& place, int width, int height) {
		if (!m_emu || !place.hovered || place.w <= 0.0f || place.h <= 0.0f)
			return;
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		int mx = static_cast<int>((mouse.x - place.x) / place.w * width);
		int my = static_cast<int>((mouse.y - place.y) / place.h * height);
		mx = std::max(0, std::min(width - 1, mx));
		my = std::max(0, std::min(height - 1, my));
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			m_emu->ModelMouseEvent(mx, my, true);
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			m_emu->ModelMouseEvent(mx, my, false);
	}

	// Board models (e.g. the SH4 CG100) bypass the built-in Keyboard peripheral,
	// so its Frame() never draws the dark pressed-key overlay that Graph Light
	// and the other models show (Keyboard.cpp: SDL_RenderFillRect black, alpha
	// 127). Reproduce that look here with ImGui: map each pressed button rect
	// from face/model pixels into the placed (possibly scaled) skin texture.
	void DrawPressedKeyHighlight(const Placement& place, int width, int height) {
		if (!m_emu || place.w <= 0.0f || place.h <= 0.0f || width <= 0 || height <= 0)
			return;
		const auto rects = m_emu->GetPressedButtonRects();
		if (rects.empty())
			return;
		const float sx = place.w / static_cast<float>(width);
		const float sy = place.h / static_cast<float>(height);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		for (const auto& r : rects) {
			if (r.w <= 0 || r.h <= 0)
				continue;
			const ImVec2 p_min(place.x + r.x * sx, place.y + r.y * sy);
			const ImVec2 p_max(place.x + (r.x + r.w) * sx, place.y + (r.y + r.h) * sy);
			const float span = std::min(p_max.x - p_min.x, p_max.y - p_min.y);
			const float rounding = std::min(12.0f * std::min(sx, sy), span * 0.35f);
			draw->AddRectFilled(p_min, p_max, IM_COL32(0, 0, 0, 127), rounding);
		}
	}

	// Zoom is the only state this panel owns now. CG100 / SH4 diagnostics and
	// reset live in the session's Hardware tab (see HwController.cpp) so a touch
	// user never has to hit a long, multi-button row underneath the calculator.
	void StepZoom(float delta) {
		fit_ = false;
		zoom_ = std::clamp(std::round((zoom_ + delta) * 100.0f) / 100.0f, 0.25f, 4.0f);
	}

	void DrawControls(int width, int height) {
		// iPadOS touch audit: a 0.01x step, a 110 px slider and SmallButton()
		// targets are not usable with a finger. Every control here is at least
		// 44 pt tall, steps a visible 0.25x, and wraps instead of overflowing a
		// narrow Stage Manager pane. The CG100 diag/reboot buttons were moved to
		// the session Hardware tab; this panel is zoom-only on every platform.
		const float kTouch = 44.0f;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
			ImVec2(ImGui::GetStyle().FramePadding.x,
			       std::max(8.0f, (kTouch - ImGui::GetTextLineHeight()) * 0.5f)));

		if (ImGui::Button(fit_ ? "Fit (on)" : "Fit", ImVec2(kTouch * 1.9f, kTouch)))
			fit_ = true;
		ToolUi::SameLineOrWrap(kTouch * 1.9f);
		const bool one_to_one = !fit_ && std::abs(zoom_ - 1.0f) < 0.001f;
		if (ImGui::Button(one_to_one ? "1:1 (on)" : "1:1", ImVec2(kTouch * 1.9f, kTouch))) {
			fit_ = false;
			zoom_ = 1.0f;
		}
		ToolUi::SameLineOrWrap(kTouch);
		if (ImGui::Button("-", ImVec2(kTouch, kTouch)))
			StepZoom(-0.25f);
		ToolUi::SameLineOrWrap(220.0f);
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::SliderFloat("##zoom", &zoom_, 0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp)) {
			fit_ = false;
			zoom_ = std::round(zoom_ * 100.0f) / 100.0f;
		}
		ToolUi::SameLineOrWrap(kTouch);
		if (ImGui::Button("+", ImVec2(kTouch, kTouch)))
			StepZoom(0.25f);
		ToolUi::SameLineOrWrap(110.0f);
		ImGui::TextDisabled("%dx%d", width, height);
		if (m_emu && m_emu->GetPaused()) {
			ToolUi::SameLineOrWrap(70.0f);
			ImGui::TextDisabled("(paused)");
		}
		ImGui::PopStyleVar();
	}

	void RecreateTexture(int width, int height) {
		if (texture_)
			SDL_DestroyTexture(texture_);
		texture_ = nullptr;
		if (!renderer)
			return;
		texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
									 SDL_TEXTUREACCESS_STREAMING, width, height);
		if (!texture_)
			return;
		SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
		tex_width_ = width;
		tex_height_ = height;
	}

	SDL_Texture* texture_ = nullptr;
	int tex_width_ = 0;
	int tex_height_ = 0;
	bool fit_ = true;
	float zoom_ = 2.0f;
	std::vector<uint8_t> frame_;
};

UIWindow* CreateCalcScreenWindow() {
	return new CalcScreenWindow();
}
