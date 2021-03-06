// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

namespace DiscIO
{
class IVolume
{
public:
	// Increment CACHE_REVISION if the enums below are modified (ISOFile.cpp & GameFile.cpp)
	enum ECountry
	{
		COUNTRY_EUROPE = 0,
		COUNTRY_JAPAN,
		COUNTRY_USA,
		COUNTRY_AUSTRALIA,
		COUNTRY_FRANCE,
		COUNTRY_GERMANY,
		COUNTRY_ITALY,
		COUNTRY_KOREA,
		COUNTRY_NETHERLANDS,
		COUNTRY_RUSSIA,
		COUNTRY_SPAIN,
		COUNTRY_TAIWAN,
		COUNTRY_WORLD,
		COUNTRY_UNKNOWN,
		NUMBER_OF_COUNTRIES
	};

	// Languages 0 - 9 match the official Wii language numbering.
	// Languages 1 - 6 match the official GC PAL languages 0 - 5.
	enum ELanguage
	{
		LANGUAGE_JAPANESE = 0,
		LANGUAGE_ENGLISH = 1,
		LANGUAGE_GERMAN = 2,
		LANGUAGE_FRENCH = 3,
		LANGUAGE_SPANISH = 4,
		LANGUAGE_ITALIAN = 5,
		LANGUAGE_DUTCH = 6,
		LANGUAGE_SIMPLIFIED_CHINESE = 7,
		LANGUAGE_TRADITIONAL_CHINESE = 8,
		LANGUAGE_KOREAN = 9,
		LANGUAGE_UNKNOWN
	};

	IVolume() {}
	virtual ~IVolume() {}

	// decrypt parameter must be false if not reading a Wii disc
	virtual bool Read(u64 _Offset, u64 _Length, u8* _pBuffer, bool decrypt) const = 0;
	virtual u32 Read32(u64 _Offset, bool decrypt) const
	{
		u32 temp;
		Read(_Offset, sizeof(u32), (u8*)&temp, decrypt);
		return Common::swap32(temp);
	}

	virtual bool GetTitleID(u8*) const { return false; }
	virtual std::unique_ptr<u8[]> GetTMD(u32 *_sz) const
	{
		*_sz = 0;
		return std::unique_ptr<u8[]>();
	}
	virtual std::string GetUniqueID() const = 0;
	virtual std::string GetMakerID() const = 0;
	virtual int GetRevision() const = 0;
	virtual std::string GetInternalName() const = 0;
	virtual std::map<ELanguage, std::string> GetNames() const = 0;
	virtual std::map<ELanguage, std::string> GetDescriptions() const { return std::map<ELanguage, std::string>(); }
	virtual std::string GetCompany() const { return std::string(); }
	virtual std::vector<u32> GetBanner(int* width, int* height) const;
	virtual u32 GetFSTSize() const = 0;
	virtual std::string GetApploaderDate() const = 0;

	virtual bool IsDiscTwo() const { return false; }
	virtual bool IsWiiDisc() const { return false; }
	virtual bool IsWadFile() const { return false; }
	virtual bool SupportsIntegrityCheck() const { return false; }
	virtual bool CheckIntegrity() const { return false; }
	virtual bool ChangePartition(u64 offset) { return false; }

	virtual ECountry GetCountry() const = 0;
	virtual u64 GetSize() const = 0;

	// Size on disc (compressed size)
	virtual u64 GetRawSize() const = 0;

protected:
	template <u32 N>
	std::string DecodeString(const char(&data)[N]) const
	{
		// strnlen to trim NULLs
		std::string string(data, strnlen(data, sizeof(data)));

		// There don't seem to be any GC discs with the country set to Taiwan...
		// But maybe they would use Shift_JIS if they existed? Not sure
		bool use_shift_jis = (COUNTRY_JAPAN == GetCountry() || COUNTRY_TAIWAN == GetCountry());

		if (use_shift_jis)
			return SHIFTJISToUTF8(string);
		else
			return CP1252ToUTF8(string);
	}

	static std::map<IVolume::ELanguage, std::string> ReadWiiNames(std::vector<u8>& data);

	static const size_t NUMBER_OF_LANGUAGES = 10;
	static const size_t NAME_STRING_LENGTH = 42;
	static const size_t NAME_BYTES_LENGTH = NAME_STRING_LENGTH * sizeof(u16);
	static const size_t NAMES_TOTAL_BYTES = NAME_BYTES_LENGTH * NUMBER_OF_LANGUAGES;
};

// Generic Switch function for all volumes
IVolume::ECountry CountrySwitch(u8 country_code);
u8 GetSysMenuRegion(u16 _TitleVersion);

} // namespace
