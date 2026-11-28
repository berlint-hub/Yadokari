// SPDX-License-Identifier: GPL-3.0+
#include "HorizonCheatDownload.h"

#include <algorithm>
#include <sstream>
#include <string_view>

namespace HorizonCheatDownload
{
bool IsSafeSerial(const std::string& serial)
{
	return !serial.empty() && serial.size() <= 32 &&
		std::all_of(serial.begin(), serial.end(), [](unsigned char c) {
			return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
		});
}

bool IsSafeCheatFile(const std::string& data)
{
	// Reject binary/HTML responses and unlabelled patches: PCSX2 can enable those
	// automatically when cheats are on, without an individual user selection.
	if (data.empty() || data.find('\0') != std::string::npos || data.find('<') != std::string::npos)
		return false;
	std::istringstream stream(data);
	std::string line;
	bool labelled = false;
	bool found_patch = false;
	while (std::getline(stream, line))
	{
		// Mirror Patch::TrimPatchLine: whitespace is stripped before comments,
		// not after them. This matters for whether a section header is valid.
		const size_t first = line.find_first_not_of(" \t\r\n\v\f");
		if (first == std::string::npos)
			continue;
		const size_t last = line.find_last_not_of(" \t\r\n\v\f");
		std::string_view text(line.data() + first, last - first + 1);
		const size_t comment = text.find("//");
		if (comment != std::string_view::npos)
			text = text.substr(0, comment);
		if (text.empty())
			continue;
		if (text.front() == '[' && text.back() == ']')
			labelled = text.size() > 2;
		else
		{
			const size_t equals = text.find('=');
			const std::string_view key = text.substr(0, equals);
			const size_t key_end = key.find_last_not_of(" \t\r\n\v\f");
			if (equals != std::string_view::npos && key_end != std::string_view::npos)
			{
				const std::string_view command = key.substr(0, key_end + 1);
				if (command == "patch" || command == "dpatch" || command == "gsaspectratio" || command == "gsinterlacemode")
				{
					if (!labelled)
						return false;
					if (command == "patch" || command == "dpatch")
						found_patch = true;
				}
			}
		}
	}
	return found_patch;
}
} // namespace HorizonCheatDownload
