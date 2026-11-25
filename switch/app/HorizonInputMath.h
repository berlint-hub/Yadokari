// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include <algorithm>
#include <cmath>

namespace HorizonInputMath
{
// Rescale a normalized axis outside the deadzone to preserve the full range.
inline float ApplyDeadzone(float value, float deadzone)
{
	value = std::clamp(value, -1.0f, 1.0f);
	deadzone = std::clamp(deadzone, 0.0f, 0.5f);
	const float magnitude = std::fabs(value);
	if (magnitude <= deadzone)
		return 0.0f;
	const float scaled = (magnitude - deadzone) / (1.0f - deadzone);
	return value < 0.0f ? -scaled : scaled;
}
} // namespace HorizonInputMath
