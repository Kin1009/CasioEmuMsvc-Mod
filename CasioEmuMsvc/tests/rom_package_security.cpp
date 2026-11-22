// Fail-closed parsing regression tests for RomPackage::Read() (V_PKG2).
// Builds synthetic packages in memory: no ROMs, no hardware, no exam material.
#include "Ext/RomPackage.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
	if (!condition) {
		std::cerr << "FAIL: " << what << "\n";
		++g_failures;
	}
}

uint32_t Crc32(const std::vector<unsigned char>& data) {
	uint32_t crc = 0xFFFFFFFF;
	for (unsigned char c : data) {
		crc ^= c;
		for (int i = 0; i < 8; ++i)
			crc = (crc >> 1) ^ (0xEDB88320u * (crc & 1));
	}
	return ~crc;
}

RomPackage MakePackage() {
	RomPackage package;
	package.RomData = { 'R', 'O', 'M', 1, 2, 3 };
	package.FlashData = { 'F', 'L', 'A', 'S', 'H' };
	package.InterfaceData = { 'I', 'F' };
	package.Crc32 = Crc32(package.RomData) ^ Crc32(package.FlashData) ^
		Crc32(package.InterfaceData);
	package.ModelInfo.rom_path = "rom.bin";
	package.ModelInfo.flash_path = "flash.bin";
	package.ModelInfo.interface_path = "interface.png";
	return package;
}

std::string Serialize(const RomPackage& package) {
	std::stringstream stream;
	package.Write(stream);
	return stream.str();
}

bool ReadRejected(const std::string& bytes, const std::string& expected) {
	RomPackage parsed;
	std::stringstream stream(bytes);
	try {
		parsed.Read(stream);
	}
	catch (const std::exception& error) {
		return std::string(error.what()).find(expected) != std::string::npos;
	}
	return false;
}

// Offsets in the Binary layout: three u64-prefixed vectors, then encVersion.
size_t EncVersionOffset(const RomPackage& package) {
	return 8 + package.RomData.size() + 8 + package.FlashData.size() +
		8 + package.InterfaceData.size();
}

} // namespace

int main() {
	const RomPackage original = MakePackage();
	const std::string valid = Serialize(original);

	RomPackage parsed;
	std::stringstream good(valid);
	try {
		parsed.Read(good);
		Check(true, "valid v0 package is accepted");
		Check(parsed.RomData == original.RomData && parsed.Crc32 == original.Crc32,
			"valid package round-trips with its CRC");
	}
	catch (const std::exception& error) {
		Check(false, std::string("valid v0 package is accepted: ") + error.what());
	}

	// V2-01: every truncation point must be refused, including the last byte.
	for (size_t keep : { size_t(0), size_t(4), size_t(8), valid.size() / 2,
			valid.size() - 2, valid.size() - 1 }) {
		Check(ReadRejected(valid.substr(0, keep), "truncated"),
			"truncated package is rejected at byte " + std::to_string(keep));
	}

	// V2-02: a payload byte change invalidates the stored CRC.
	std::string bad_crc = valid;
	bad_crc[9] ^= 0x01;
	Check(ReadRejected(bad_crc, "CRC"), "corrupted payload is rejected by CRC at read time");

	// V2-03: unknown encryption versions are refused by the parser itself.
	for (uint8_t version : { uint8_t(3), uint8_t(255) }) {
		std::string bad_version = valid;
		bad_version[EncVersionOffset(original)] = static_cast<char>(version);
		Check(ReadRejected(bad_version, "unsupported encryption version"),
			"unsupported encryption version " + std::to_string(version) + " is rejected");
	}

	// V2-04: a 2^48 length prefix (the old strict-greater off-by-one) must
	// become a controlled parse error, never std::bad_alloc.
	std::string huge = valid;
	const uint64_t hostile_size = 1ULL << 48;
	std::memcpy(huge.data(), &hostile_size, sizeof(hostile_size));
	Check(ReadRejected(huge, "container size"),
		"hostile size prefix is a controlled parse error");

	// V2-05: path validation is inherited by every Read() consumer.
	RomPackage traversal = MakePackage();
	traversal.ModelInfo.rom_path = "../ESCAPE.bin";
	Check(ReadRejected(Serialize(traversal), "unsafe entry path"),
		"parent-directory entry path is rejected by Read()");
	RomPackage windows_path = MakePackage();
	windows_path.ModelInfo.flash_path = "..\\ESCAPE.bin";
	Check(ReadRejected(Serialize(windows_path), "unsafe entry path"),
		"Windows-style parent path is rejected by Read()");

	if (g_failures != 0) {
		std::cerr << g_failures << " ROM package check(s) failed\n";
		return 1;
	}
	std::cout << "rom package security test passed\n";
	return 0;
}
