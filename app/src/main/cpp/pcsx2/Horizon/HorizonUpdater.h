// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef __SWITCH__
namespace HorizonUpdater
{
	// The channel this build follows unless Horizon/UpdateChannel overrides it.
	const char* GetDefaultChannel();
} // namespace HorizonUpdater
#endif
