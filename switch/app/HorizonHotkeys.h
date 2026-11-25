// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <cstdint>

// Platform-independent chord detection, kept separate from the HID polling loop.
namespace HorizonHotkeys
{
enum class Action { None, Save, Load, PreviousSlot, NextSlot, Screenshot };

struct Buttons
{
	uint64_t modifier, menu, save, load, previous, next, screenshot;
};

struct Result
{
	Action action = Action::None;
	uint64_t consumed = 0;
};

class Chords
{
public:
	explicit Chords(Buttons buttons) : m_buttons(buttons) {}

	Result Update(uint64_t held, bool enabled)
	{
		const uint64_t actions = m_buttons.save | m_buttons.load | m_buttons.previous |
			m_buttons.next | m_buttons.screenshot;
		// Keep suppressing the other half of a chord until all its buttons are released.
		m_suppressed &= held;
		if (!(held & m_buttons.modifier))
			m_blocked = false;
		if (!enabled || (held & m_buttons.menu))
			m_blocked = (held & m_buttons.modifier) != 0;
		if (!enabled || m_blocked || !(held & m_buttons.modifier))
		{
			m_previous_chord = 0;
			m_ambiguous = false;
			return {Action::None, m_suppressed};
		}

		const uint64_t pressed = held & actions;
		if (!pressed)
		{
			m_previous_chord = 0;
			m_ambiguous = false;
			return {Action::None, m_suppressed};
		}

		m_suppressed |= m_buttons.modifier | pressed;
		Action action = Action::None;
		// Ambiguous multi-button chords stay blocked until the action buttons are released.
		if ((pressed & (pressed - 1)) != 0)
			m_ambiguous = true;
		if (!m_ambiguous && pressed != m_previous_chord)
		{
			if (pressed == m_buttons.save) action = Action::Save;
			else if (pressed == m_buttons.load) action = Action::Load;
			else if (pressed == m_buttons.previous) action = Action::PreviousSlot;
			else if (pressed == m_buttons.next) action = Action::NextSlot;
			else if (pressed == m_buttons.screenshot) action = Action::Screenshot;
		}
		m_previous_chord = pressed;
		return {action, m_suppressed};
	}

private:
	Buttons m_buttons;
	uint64_t m_previous_chord = 0;
	uint64_t m_suppressed = 0;
	bool m_ambiguous = false;
	bool m_blocked = false;
};
} // namespace HorizonHotkeys
