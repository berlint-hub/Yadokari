// SPDX-License-Identifier: GPL-3.0+
#pragma once

// Stable INI values shared by the Switch input loop and FullscreenUI.
namespace HorizonInputSettings
{
inline constexpr const char* SECTION = "HorizonInput";
inline constexpr const char* DEADZONE = "StickDeadzonePercent";
inline constexpr int DEFAULT_DEADZONE = 15;
inline constexpr int MIN_DEADZONE = 0;
inline constexpr int MAX_DEADZONE = 50;

enum Button
{
	Disabled, R, L, ZR, ZL, DPadLeft, DPadRight, DPadUp, DPadDown,
	A, B, X, Y, LeftStick, RightStick, Count
};

struct Binding
{
	const char* key;
	const char* label;
	int default_button;
};
inline constexpr Binding BINDINGS[] = {
	{"QuickSave", "Quick Save", R},
	{"QuickLoad", "Quick Load", L},
	{"PreviousSlot", "Previous Save Slot", DPadLeft},
	{"NextSlot", "Next Save Slot", DPadRight},
	{"Screenshot", "Screenshot", DPadUp},
};
inline constexpr const char* BUTTON_NAMES[] = {
	"Disabled", "R", "L", "ZR", "ZL", "D-pad Left", "D-pad Right", "D-pad Up", "D-pad Down",
	"A", "B", "X", "Y", "Left Stick Click", "Right Stick Click"
};
static_assert(sizeof(BUTTON_NAMES) / sizeof(BUTTON_NAMES[0]) == Count);
} // namespace HorizonInputSettings
