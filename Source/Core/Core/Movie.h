// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "Common/CommonTypes.h"

struct GCPadStatus;
class PointerWrap;
struct wiimote_key;
struct SCoreStartupParameter;

namespace WiimoteEmu
{
struct ReportFeatures;
}

// De-coupling notes (TODOs):
//  - VideoConfig
//    - calls SetGraphicsConfig() when retrieving active config
//  - HW/EXI.cpp:
//    - Can set SConfig::GetInstance().m_EXIDevice[0/1] to memory card
//  - HW/EXI_DeviceMemoryCard.cpp
//    - Can set SConfig::GetInstance().m_strMemoryCardA/B to string for movie file
//  - HW/Wiimote.cpp
//    - Not sure but we can probably do this elsewhere
//  - BootManager
//    - Need to experiment, but may not need to rely on this stuff calling Movie.
//  - HW/SI.cpp
//    - Same as memory card, can use SConfig::GetInstance().m_SIDevice[i]
//
//
// TODO: Don't forget to test netplay (coop recording)

namespace Movie
{
	// Global declarations
	extern bool g_bReset;
	extern u64 g_currentFrame;

	void FrameUpdate();
	void InputUpdate();
	void Init();

	bool IsRecordingInput();
	bool IsJustStartingRecordingInputFromSaveState();
	bool IsJustStartingPlayingInputFromSaveState();
	bool IsPlayingInput();
	bool IsMovieActive();
	u64  GetRecordingStartTime();
	void SetRecordingStartTime(u64 time);

	bool IsStartingFromClearSave();
	bool IsUsingMemcard(int memcard);
	void SetGraphicsConfig();

	bool IsUsingPad(int controller);
	void ChangeWiiPads(bool instantly = false);

	void DoFrameStep();

	void SetFrameSkipping(unsigned int framesToSkip);

	bool BeginRecordingInput(int controllers);

	bool PlayInput(const std::string& filename);
	void LoadInput(const std::string& filename);
	void EndPlayInput();
	void SaveRecording(const std::string& filename);
	void DoState(PointerWrap &p);
	void Shutdown();

	// Done this way to avoid mixing of core and gui code
	typedef void(*GCManipFunction)(GCPadStatus*, int);
	typedef void(*WiiManipFunction)(u8*, WiimoteEmu::ReportFeatures, int, int, wiimote_key);

	// Callback to have the TAS dialog write pad status
	void SetGCInputManip(GCManipFunction);
	void SetWiiInputManip(WiiManipFunction);

	void ChangeDiscCallback(const std::string& newFileName);
	void SaveClearCallback(u64 tmdTitleId);
	std::string GetDebugInfo();
	void SetStartupOptions(SCoreStartupParameter* StartUp);

	void GetPadStatus(GCPadStatus* pad_status, int controller_id);
	void UpdateWiimote(int wiimote, u8* data, const struct WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key);

}
