#pragma once

#include <vector>
#include <string>

namespace Sqex::CommandLine {
	extern const char ChecksumTable[16];
	extern const char ObfuscationHead[12];
	extern const char ObfuscationTail[4];

	std::vector<std::pair<std::string, std::string>> FromString(std::string source, bool* wasObfuscated = nullptr);
	std::string ToString(const std::vector<std::pair<std::string, std::string>>& map, bool obfuscate);

	void ReverseEvery4Bytes(std::string& s);
	std::vector<std::string> Split(const std::string& source, char delimiter, size_t maxCount);
}
