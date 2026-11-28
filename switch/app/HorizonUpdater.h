// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Self-updater: downloads the newest armsx2nx.nro from the GitHub release the CI workflow publishes
// (https://github.com/<repo>/releases/download/<channel>/armsx2nx.nro) and swaps it in on exit.
namespace HorizonUpdater
{
	// argv0 is the path of the running .nro, as passed by hbloader.
	void Initialize(const char* argv0);

	// Checks for a newer build in the background. Interactive checks report "up to date" and errors;
	// the automatic startup check only speaks up when an update is available.
	void CheckForUpdates(bool interactive);

	// Stops a running check/download (call before tearing down the GS thread).
	void Shutdown();

	// Call after romfsExit() on the way out: replaces the .nro with a downloaded update and, if the user
	// asked for it, tells hbloader to start the new build next.
	void FinishPendingInstall();
} // namespace HorizonUpdater
