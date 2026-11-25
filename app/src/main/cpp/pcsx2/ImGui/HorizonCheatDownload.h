// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace HorizonCheatDownload
{
struct Result
{
	std::string serial;
	uint32_t crc = 0;
	bool success = false;
	std::string message;
};

// One request at a time. Start, Poll and Shutdown are called on the GS thread.
class Downloader
{
public:
	~Downloader();
	bool Start(std::string serial, uint32_t crc, std::string filename);
	std::optional<Result> Poll();
	void Shutdown();
	bool Busy() const { return m_busy; }
private:
	std::thread m_thread;
	std::mutex m_mutex;
	std::optional<Result> m_result;
	std::atomic_bool m_cancel{false};
	bool m_busy = false;
};

// Exposed separately for host-side tests.
bool IsSafeSerial(const std::string& serial);
bool IsSafeCheatFile(const std::string& data);
} // namespace HorizonCheatDownload
