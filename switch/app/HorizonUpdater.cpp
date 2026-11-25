// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "HorizonUpdater.h"
#include "HorizonHost.h"

#include "common/Console.h"
#include "common/FileSystem.h"

#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/VMManager.h"

#include "common/Horizon/Horizon.h"

#include <curl/curl.h>
#include "fmt/format.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

// Set by switch/app/CMakeLists.txt (the CI workflow passes the commit, repository and release tag).
#ifndef ARMSX2_BUILD_SHA
#define ARMSX2_BUILD_SHA ""
#endif
#ifndef ARMSX2_UPDATE_REPO
#define ARMSX2_UPDATE_REPO ""
#endif
#ifndef ARMSX2_UPDATE_TAG
#define ARMSX2_UPDATE_TAG ""
#endif

namespace HorizonUpdater
{
namespace
{
	constexpr const char* SETTINGS_SECTION = "Horizon";
	constexpr const char* NRO_ASSET = "armsx2nx.nro";
	constexpr const char* VERSION_ASSET = "version.txt";
	constexpr const char* DEFAULT_NRO_PATH = "sdmc:/switch/armsx2nx.nro";
	constexpr const char* PROGRESS_ID = "horizon_updater";
	constexpr size_t VERSION_MAX_SIZE = 4096;

	struct RemoteVersion
	{
		std::string sha;
		std::string date;
		std::string title;
	};

	std::string s_nro_path;

	std::mutex s_thread_mutex;
	std::thread s_thread;
	std::atomic_bool s_busy{false};
	std::atomic_bool s_abort{false};

	// Set once a validated download is waiting at DownloadPath().
	std::atomic_bool s_install_pending{false};
	std::atomic_bool s_restart_after_install{false};

	std::once_flag s_curl_once;
	bool s_curl_ok = false;

	std::string DownloadPath() { return s_nro_path + ".download"; }
	std::string BackupPath() { return s_nro_path + ".bak"; }

	std::string ShortSha(std::string_view sha) { return std::string(sha.substr(0, 7)); }

	std::string Repository()
	{
		return Host::GetBaseStringSettingValue(SETTINGS_SECTION, "UpdateRepository", ARMSX2_UPDATE_REPO);
	}

	std::string Channel()
	{
		return Host::GetBaseStringSettingValue(SETTINGS_SECTION, "UpdateChannel", ARMSX2_UPDATE_TAG);
	}

	std::string AssetUrl(const std::string& repo, const std::string& channel, const char* asset)
	{
		return fmt::format("https://github.com/{}/releases/download/{}/{}", repo, channel, asset);
	}

	// FullscreenUI lives on the GS thread, and MTGS::RunOnGSThread may only be called from the CPU thread.
	void RunOnUIThread(std::function<void()> fn)
	{
		Host::RunOnCPUThread([fn = std::move(fn)]() { MTGS::RunOnGSThread(fn); }, false);
	}

	void ShowInfo(std::string title, std::string message)
	{
		INFO_LOG("Updater: {}: {}", title, message);
		RunOnUIThread([title = std::move(title), message = std::move(message)]() {
			if (ImGuiManager::InitializeFullscreenUI())
				ImGuiFullscreen::OpenInfoMessageDialog(title, message);
		});
	}

	void ShowConfirm(std::string title, std::string message, std::function<void(bool)> callback, std::string yes,
		std::string no)
	{
		RunOnUIThread([title = std::move(title), message = std::move(message), callback = std::move(callback),
						  yes = std::move(yes), no = std::move(no)]() {
			if (ImGuiManager::InitializeFullscreenUI())
				ImGuiFullscreen::OpenConfirmMessageDialog(title, message, callback, yes, no);
		});
	}

	void ShowToast(std::string title, std::string message)
	{
		INFO_LOG("Updater: {}: {}", title, message);
		RunOnUIThread([title = std::move(title), message = std::move(message)]() {
			if (FullscreenUI::IsInitialized())
				ImGuiFullscreen::ShowToast(title, message, 10.0f);
		});
	}

	//------------------------------------------------------------------------------------------------
	// HTTP

	struct Transfer
	{
		std::FILE* file = nullptr; // download target, or
		std::string* text = nullptr; // small in-memory response
		bool report_progress = false;
		u64 last_report_kb = ~0ull;
	};

	size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		Transfer* const t = static_cast<Transfer*>(userdata);
		const size_t bytes = size * nmemb;
		if (t->file)
			return std::fwrite(ptr, 1, bytes, t->file);
		if (t->text->size() + bytes > VERSION_MAX_SIZE)
			return 0; // not a version file, abort
		t->text->append(ptr, bytes);
		return bytes;
	}

	int ProgressCallback(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
	{
		if (s_abort.load(std::memory_order_relaxed))
			return 1;

		Transfer* const t = static_cast<Transfer*>(userdata);
		if (!t->report_progress || dltotal <= 0)
			return 0;

		// About ten updates per MB is plenty.
		const u64 now_kb = static_cast<u64>(dlnow) / 1024;
		if (t->last_report_kb != ~0ull && now_kb < t->last_report_kb + 100 && dlnow != dltotal)
			return 0;
		t->last_report_kb = now_kb;

		const s32 total_kb = static_cast<s32>(dltotal / 1024);
		ImGuiFullscreen::UpdateBackgroundProgressDialog(PROGRESS_ID,
			fmt::format("Downloading update: {:.1f} / {:.1f} MB", dlnow / 1048576.0, dltotal / 1048576.0), 0,
			total_kb, static_cast<s32>(now_kb));
		return 0;
	}

	bool Fetch(const std::string& url, Transfer& transfer, std::string* error)
	{
		std::call_once(s_curl_once, []() { s_curl_ok = (curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK); });
		if (!s_curl_ok)
		{
			*error = "curl_global_init() failed";
			return false;
		}

		CURL* const curl = curl_easy_init();
		if (!curl)
		{
			*error = "curl_easy_init() failed";
			return false;
		}

		char errbuf[CURL_ERROR_SIZE] = {};
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "ARMSX2-NX-Updater");
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // release assets redirect to GitHub's CDN
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
		curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L); // give up below 1 KB/s ...
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L); // ... for 30 seconds
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &ProgressCallback);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

		const CURLcode result = curl_easy_perform(curl);
		long status = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_cleanup(curl);

		if (result != CURLE_OK)
		{
			if (result == CURLE_HTTP_RETURNED_ERROR && status == 404)
				*error = "No build has been published for this update channel yet.";
			else
				*error = fmt::format("{} ({})", errbuf[0] ? errbuf : curl_easy_strerror(result), static_cast<int>(result));
			return false;
		}
		return true;
	}

	std::optional<RemoteVersion> ParseVersion(std::string_view text)
	{
		RemoteVersion version;
		while (!text.empty())
		{
			const size_t eol = text.find('\n');
			std::string_view line = text.substr(0, eol);
			text = (eol == std::string_view::npos) ? std::string_view() : text.substr(eol + 1);
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);

			const size_t eq = line.find('=');
			if (eq == std::string_view::npos)
				continue;
			const std::string_view key = line.substr(0, eq);
			const std::string value(line.substr(eq + 1));
			if (key == "sha")
				version.sha = value;
			else if (key == "date")
				version.date = value;
			else if (key == "title")
				version.title = value;
		}

		if (version.sha.size() < 7)
			return std::nullopt;
		return version;
	}

	//------------------------------------------------------------------------------------------------
	// NRO handling

	bool ValidateNro(const std::string& path, std::string* error)
	{
		std::FILE* const fp = std::fopen(path.c_str(), "rb");
		if (!fp)
		{
			*error = "Could not open the downloaded file.";
			return false;
		}

		struct
		{
			NroStart start;
			NroHeader header;
		} nro = {};
		const bool have_header = std::fread(&nro, sizeof(nro), 1, fp) == 1;

		std::fseek(fp, 0, SEEK_END);
		const long file_size = std::ftell(fp);

		// The romfs (fonts, UI, GameIndex) lives in the asset section right after the NRO image.
		u32 asset_magic = 0;
		const bool have_assets = have_header && file_size > static_cast<long>(nro.header.size) &&
								 std::fseek(fp, static_cast<long>(nro.header.size), SEEK_SET) == 0 &&
								 std::fread(&asset_magic, sizeof(asset_magic), 1, fp) == 1;
		std::fclose(fp);

		if (!have_header || nro.header.magic != NROHEADER_MAGIC)
		{
			*error = "The downloaded file is not a Switch homebrew (.nro).";
			return false;
		}
		if (!have_assets || asset_magic != NROASSETHEADER_MAGIC)
		{
			*error = "The downloaded .nro is incomplete.";
			return false;
		}
		return true;
	}

	//------------------------------------------------------------------------------------------------
	// Worker

	void DownloadAndStage(const RemoteVersion& remote, const std::string& repo, const std::string& channel)
	{
		const std::string target = DownloadPath();
		std::remove(target.c_str());

		std::FILE* const fp = std::fopen(target.c_str(), "wb");
		if (!fp)
		{
			ShowInfo("Update failed", fmt::format("Could not create {}.", target));
			return;
		}

		ImGuiFullscreen::OpenBackgroundProgressDialog(PROGRESS_ID, "Downloading update...", 0, 1, 0);

		Transfer transfer;
		transfer.file = fp;
		transfer.report_progress = true;
		std::string error;
		const bool downloaded = Fetch(AssetUrl(repo, channel, NRO_ASSET), transfer, &error);
		const bool closed = (std::fclose(fp) == 0);

		ImGuiFullscreen::CloseBackgroundProgressDialog(PROGRESS_ID);

		if (s_abort.load())
		{
			std::remove(target.c_str());
			return;
		}
		if (!downloaded || !closed || !ValidateNro(target, &error))
		{
			std::remove(target.c_str());
			ShowInfo("Update failed", error.empty() ? std::string("Could not write the update to the SD card.") : error);
			return;
		}

		s_install_pending.store(true, std::memory_order_release);
		INFO_LOG("Updater: build {} staged at {}", ShortSha(remote.sha), target);

		ShowConfirm("Update downloaded",
			fmt::format("Build {} is ready. It is installed when the app closes.\n\nRestart now to start using it?",
				ShortSha(remote.sha)),
			[](bool restart) {
				if (!restart)
					return;
				s_restart_after_install.store(true, std::memory_order_release);
				HorizonHost::RequestExit();
			},
			"Restart now", "Later");
	}

	struct Offer
	{
		RemoteVersion remote;
		std::string repo;
		std::string channel;
		std::string summary;
	};

	void StartDownload(Offer offer)
	{
		std::lock_guard lock(s_thread_mutex);
		if (s_busy.exchange(true))
			return;
		if (s_thread.joinable())
			s_thread.join(); // the check thread, which has finished by now
		s_thread = std::thread([offer = std::move(offer)]() {
			DownloadAndStage(offer.remote, offer.repo, offer.channel);
			s_busy.store(false);
		});
	}

	void OfferUpdate(Offer offer)
	{
		std::string message = offer.summary + "\n\nDownload and install it now?";
		ShowConfirm("Update available", std::move(message),
			[offer = std::move(offer)](bool yes) {
				if (yes)
					StartDownload(offer);
			},
			"Download", "Not now");
	}

	// Runs on the worker thread. Returns the update to offer, if there is one.
	std::optional<Offer> CheckWorker(bool interactive)
	{
		const std::string repo = Repository();
		const std::string channel = Channel();
		const std::string installed = ARMSX2_BUILD_SHA;

		if (repo.empty() || channel.empty())
		{
			if (interactive)
				ShowInfo("Updates", "No update source is configured. Set [Horizon] UpdateRepository and UpdateChannel in armsx2.ini.");
			return std::nullopt;
		}

		std::string text, error;
		Transfer transfer;
		transfer.text = &text;
		if (!Fetch(AssetUrl(repo, channel, VERSION_ASSET), transfer, &error))
		{
			if (interactive && !s_abort.load())
				ShowInfo("Could not check for updates", fmt::format("{}\n\n{} ({})", error, repo, channel));
			else
				WARNING_LOG("Updater: check failed: {}", error);
			return std::nullopt;
		}

		const std::optional<RemoteVersion> remote = ParseVersion(text);
		if (!remote.has_value())
		{
			if (interactive)
				ShowInfo("Could not check for updates", "The published version information is invalid.");
			return std::nullopt;
		}

		if (!installed.empty() && remote->sha == installed)
		{
			if (interactive)
				ShowInfo("Up to date", fmt::format("You are running the latest build ({}).", ShortSha(installed)));
			return std::nullopt;
		}

		if (s_install_pending.load())
		{
			if (interactive)
				ShowInfo("Update ready", "An update has already been downloaded. Close the app to install it.");
			return std::nullopt;
		}

		// Don't pop a dialog over a running game from the startup check.
		if (!interactive && VMManager::HasValidVM())
		{
			ShowToast("Update available", "Open Update in the main menu to install it.");
			return std::nullopt;
		}

		std::string summary = fmt::format("Installed: {}\nAvailable: {}{}{}",
			installed.empty() ? std::string("unknown") : ShortSha(installed), ShortSha(remote->sha),
			remote->date.empty() ? std::string() : fmt::format(" ({})", remote->date),
			remote->title.empty() ? std::string() : fmt::format("\n{}", remote->title));
		return Offer{*remote, repo, channel, std::move(summary)};
	}
} // namespace

void Initialize(const char* argv0)
{
	std::string path = (argv0 && argv0[0]) ? argv0 : "";
	if (!path.empty() && path.front() == '/')
		path = "sdmc:" + path;
	if (path.size() < 4 || path.compare(path.size() - 4, 4, ".nro") != 0 || !FileSystem::FileExists(path.c_str()))
		path = DEFAULT_NRO_PATH;
	s_nro_path = std::move(path);

	// A download that never got installed (e.g. the app crashed) is not trusted; fetch it again.
	std::remove(DownloadPath().c_str());

	INFO_LOG("Updater: build {} ({} / {}), installed at {}", ARMSX2_BUILD_SHA[0] ? ARMSX2_BUILD_SHA : "unknown",
		ARMSX2_UPDATE_REPO, ARMSX2_UPDATE_TAG, s_nro_path);
}

void CheckForUpdates(bool interactive)
{
	// Development builds don't know their own commit, so they only check on request.
	if (!interactive && (ARMSX2_BUILD_SHA[0] == '\0' ||
							!Host::GetBaseBoolSettingValue(SETTINGS_SECTION, "CheckForUpdatesOnStartup", true)))
		return;

	std::lock_guard lock(s_thread_mutex);
	if (s_busy.exchange(true))
	{
		if (interactive)
			ShowToast("Updates", "An update check or download is already running.");
		return;
	}
	if (s_thread.joinable())
		s_thread.join();

	s_thread = std::thread([interactive]() {
		std::optional<Offer> offer = CheckWorker(interactive);
		// Clear busy before offering, so answering the dialog can start the download right away.
		s_busy.store(false);
		if (offer.has_value() && !s_abort.load())
			OfferUpdate(std::move(*offer));
	});
}

void Shutdown()
{
	s_abort.store(true);
	std::lock_guard lock(s_thread_mutex);
	if (s_thread.joinable())
		s_thread.join();
}

void FinishPendingInstall()
{
	if (!s_install_pending.load(std::memory_order_acquire))
		return;

	const std::string download = DownloadPath();
	const std::string backup = BackupPath();

	// The running .nro is held open for its romfs until romfsExit(), so the swap happens here.
	std::remove(backup.c_str());
	if (std::rename(s_nro_path.c_str(), backup.c_str()) != 0 && errno != ENOENT)
	{
		ERROR_LOG("Updater: could not move {} out of the way ({}), update not installed", s_nro_path, errno);
		std::remove(download.c_str());
		return;
	}
	if (std::rename(download.c_str(), s_nro_path.c_str()) != 0)
	{
		ERROR_LOG("Updater: could not install {} ({}), restoring the previous build", s_nro_path, errno);
		std::rename(backup.c_str(), s_nro_path.c_str());
		std::remove(download.c_str());
		return;
	}
	INFO_LOG("Updater: installed update at {} (previous build kept as {})", s_nro_path, backup);

	if (s_restart_after_install.load(std::memory_order_acquire) && envHasNextLoad())
	{
		if (R_SUCCEEDED(envSetNextLoad(s_nro_path.c_str(), s_nro_path.c_str())))
			INFO_LOG("Updater: restarting into the new build");
	}
}
} // namespace HorizonUpdater

void Host::OnUpdateCheckRequested()
{
	HorizonUpdater::CheckForUpdates(true);
}
