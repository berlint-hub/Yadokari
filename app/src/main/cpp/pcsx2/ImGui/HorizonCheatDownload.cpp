// SPDX-License-Identifier: GPL-3.0+
#include "HorizonCheatDownload.h"

#include "common/FileSystem.h"
#include "common/HTTPDownloader.h"
#include "common/Path.h"

#include <curl/curl.h>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace HorizonCheatDownload
{
namespace
{
constexpr size_t MAX_FILE_SIZE = 512 * 1024;
constexpr const char* BASE_URL = "https://raw.githubusercontent.com/xs1l3n7x/pcsx2_cheats_collection/main/cheats/";

struct Transfer
{
	std::string data;
	std::atomic_bool* cancel;
	bool too_large = false;
};

size_t WriteData(char* ptr, size_t size, size_t count, void* userdata)
{
	auto& transfer = *static_cast<Transfer*>(userdata);
	if (size && count > (MAX_FILE_SIZE - transfer.data.size()) / size)
	{
		transfer.too_large = true;
		return 0;
	}
	const size_t length = size * count;
	transfer.data.append(ptr, length);
	return length;
}

int Progress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return static_cast<Transfer*>(userdata)->cancel->load() ? 1 : 0;
}

struct Response
{
	long status = 0;
	CURLcode error = CURLE_FAILED_INIT;
	bool too_large = false;
	std::string data;
};

Response Fetch(const std::string& url, std::atomic_bool& cancel)
{
	Response response;
	CURL* curl = curl_easy_init();
	if (!curl)
		return response;
	Transfer transfer{{}, &cancel};
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "ARMSX2-NX cheat downloader");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Progress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
	response.error = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
	curl_easy_cleanup(curl);
	response.too_large = transfer.too_large;
	response.data = std::move(transfer.data);
	return response;
}

Result Download(std::string serial, uint32_t crc, const std::string& filename, std::atomic_bool& cancel)
{
	Result result{serial, crc, false, {}};
	if (!IsSafeSerial(serial) || crc == 0)
	{
		result.message = "This game has no valid serial and CRC. Start it once and rescan the game list.";
		return result;
	}
	if (FileSystem::FileExists(filename.c_str()))
	{
		result.message = "A cheat file already exists for this game. It was not overwritten.";
		return result;
	}

	// Reuse the core's curl initialization; this instance makes no requests itself.
	auto initializer = HTTPDownloader::Create("ARMSX2-NX");
	if (!initializer)
	{
		result.message = "Could not initialize the network downloader.";
		return result;
	}
	char crc_text[9];
	std::snprintf(crc_text, sizeof(crc_text), "%08X", static_cast<unsigned>(crc));
	Response response = Fetch(std::string(BASE_URL) + serial + "_" + crc_text + ".pnach", cancel);
	if (response.error == CURLE_OK && response.status == 404 && !cancel.load())
		response = Fetch(std::string(BASE_URL) + crc_text + ".pnach", cancel);

	if (cancel.load())
		result.message = "Download cancelled.";
	else if (response.too_large)
		result.message = "Cheat file is too large; download rejected.";
	else if (response.error != CURLE_OK)
		result.message = std::string("Download failed: ") + curl_easy_strerror(response.error);
	else if (response.status == 404)
		result.message = "No cheats found for this exact game version.";
	else if (response.status != 200)
		result.message = "Cheat server returned HTTP " + std::to_string(response.status) + ".";
	else if (!IsSafeCheatFile(response.data))
		result.message = "Cheat file is invalid or contains unlabelled codes; download rejected.";
	else if (!FileSystem::CreateDirectoryPath(std::string(Path::GetDirectory(filename)).c_str(), true))
		result.message = "Could not create the cheats directory.";
	else
	{
		// O_EXCL prevents replacing user files, including ones created during the download.
		const int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd < 0)
			result.message = (errno == EEXIST) ? "A cheat file already exists; it was not overwritten." : "Could not create the cheat file.";
		else
		{
			size_t written = 0;
			while (written < response.data.size())
			{
				const ssize_t count = write(fd, response.data.data() + written, response.data.size() - written);
				if (count <= 0)
					break;
				written += static_cast<size_t>(count);
			}
			const bool closed = (close(fd) == 0);
			if (written != response.data.size() || !closed)
			{
				unlink(filename.c_str());
				result.message = "Could not write the cheat file; the incomplete file was removed.";
			}
			else
			{
				result.success = true;
				result.message = "Cheats downloaded. Enable cheats and select the codes you want to use.";
			}
		}
	}
	return result;
}
} // namespace

Downloader::~Downloader() { Shutdown(); }

bool Downloader::Start(std::string serial, uint32_t crc, std::string filename)
{
	if (m_busy)
		return false;
	m_cancel.store(false);
	m_busy = true;
	m_thread = std::thread([this, serial = std::move(serial), crc, filename = std::move(filename)]() {
		Result result = Download(serial, crc, filename, m_cancel);
		std::lock_guard lock(m_mutex);
		m_result = std::move(result);
	});
	return true;
}

std::optional<Result> Downloader::Poll()
{
	std::optional<Result> result;
	{
		std::lock_guard lock(m_mutex);
		if (!m_result)
			return {};
		result = std::move(m_result);
		m_result.reset();
	}
	m_thread.join();
	m_busy = false;
	return result;
}

void Downloader::Shutdown()
{
	m_cancel.store(true);
	if (m_thread.joinable())
		m_thread.join();
	std::lock_guard lock(m_mutex);
	m_result.reset();
	m_busy = false;
}
} // namespace HorizonCheatDownload
