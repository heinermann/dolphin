// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <memory>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/DSP/DSPCore.h"
#include "Core/HW/DVDInterface.h"
#include "Core/HW/EXI_Device.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteEmu/WiimoteHid.h"
#include "Core/HW/WiimoteEmu/Attachment/Classic.h"
#include "Core/HW/WiimoteEmu/Attachment/Nunchuk.h"
#include "Core/IPC_HLE/WII_IPC_HLE_Device_usb.h"
#include "Core/IPC_HLE/WII_IPC_HLE_WiiMote.h"
#include "Core/PowerPC/PowerPC.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/VideoConfig.h"

static std::mutex cs_frameSkip;

namespace Movie {

static bool s_bFrameStep = false;
static bool s_bReadOnly = true;

enum PlayMode
{
	MODE_NONE = 0,
	MODE_RECORDING,
	MODE_PLAYING
};
static PlayMode s_playMode = MODE_NONE;

static u32 s_framesToSkip = 0, s_frameSkipCounter = 0;

static u8 s_numPads = 0;
u64 g_currentFrame = 0; // VI
static u64 s_currentLagCount = 0;
static u64 s_recordingStartTime; // seconds since 1970 that recording started

// config stuff
static bool s_bSaveConfig = false;
static bool s_bClearSave = false;
static bool s_bDiscChange = false;

bool g_bReset = false;
static std::string s_discChange = "";
static u64 s_titleID = 0;
static u8 s_bongos, s_memcards;

static bool s_bRecordingFromSaveState = false;
static bool s_bPolled = false;

static std::string s_InputDisplay[8];

static GCManipFunction gcmfunc = nullptr;
static WiiManipFunction wiimfunc = nullptr;

void FrameSkipping();
void RecordWiimote(int wiimote, u8 *data, u8 size);
void GetSettings();

static std::string GetInputDisplay()
{
	if (!IsMovieActive())
	{
		s_numPads = 0;
		for (int i = 0; i < 4; ++i)
		{
			if (SerialInterface::GetDeviceType(i) != SIDEVICE_NONE)
				s_numPads |= (1 << i);
			if (g_wiimote_sources[i] != WIIMOTE_SRC_NONE)
				s_numPads |= (1 << (i + 4));
		}
	}

	std::string inputDisplay = "";
	for (int i = 0; i < 8; ++i)
		if ((s_numPads & (1 << i)) != 0)
			inputDisplay.append(s_InputDisplay[i]);

	return inputDisplay;
}

void FrameUpdate()
{
	g_currentFrame++;
	if (!s_bPolled)
		s_currentLagCount++;

	if (s_bFrameStep)
	{
		Core::SetState(Core::CORE_PAUSE);
		s_bFrameStep = false;
	}

	if (s_framesToSkip)
		FrameSkipping();

	s_bPolled = false;
}

// called when game is booting up, even if no movie is active,
// but potentially after BeginRecordingInput or PlayInput has been called.
void Init()
{
	s_bPolled = false;
	s_bFrameStep = false;

	if (IsRecordingInput())
	{
		GetSettings();
	}

	s_frameSkipCounter = s_framesToSkip;
	if (!IsPlayingInput())
		Core::SetStateFileName("");

	for (auto& disp : s_InputDisplay)
		disp.clear();

	if (!IsMovieActive())
	{
		s_bRecordingFromSaveState = false;
		g_currentFrame = 0;
		s_currentLagCount = 0;
	}
}

void InputUpdate()
{
	if (IsPlayingInput() && SConfig::GetInstance().m_PauseMovie)
		Core::SetState(Core::CORE_PAUSE);
}

void SetFrameSkipping(unsigned int framesToSkip)
{
	std::lock_guard<std::mutex> lk(cs_frameSkip);

	s_framesToSkip = framesToSkip;
	s_frameSkipCounter = 0;

	// Don't forget to re-enable rendering in case it wasn't...
	// as this won't be changed anymore when frameskip is turned off
	if (framesToSkip == 0)
		g_video_backend->Video_SetRendering(true);
}

void DoFrameStep()
{
	if (Core::GetState() == Core::CORE_PAUSE)
	{
		// if already paused, frame advance for 1 frame
		Core::SetState(Core::CORE_RUN);
		Core::RequestRefreshInfo();
		s_bFrameStep = true;
	}
	else if (!s_bFrameStep)
	{
		// if not paused yet, pause immediately instead
		Core::SetState(Core::CORE_PAUSE);
	}
}

void SetReadOnly(bool bEnabled)
{
	if (s_bReadOnly != bEnabled)
		Core::DisplayMessage(bEnabled ? "Read-only mode." :  "Read+Write mode.", 1000);

	s_bReadOnly = bEnabled;
}

void FrameSkipping()
{
	// Frameskipping will desync movie playback
	if (!IsMovieActive() || NetPlay::IsNetPlayRunning())
	{
		std::lock_guard<std::mutex> lk(cs_frameSkip);

		s_frameSkipCounter++;
		if (s_frameSkipCounter > s_framesToSkip || Core::ShouldSkipFrame(s_frameSkipCounter) == false)
			s_frameSkipCounter = 0;

		g_video_backend->Video_SetRendering(!s_frameSkipCounter);
	}
}

static void CallGCInputManip(GCPadStatus* PadStatus, int controllerID)
{
	if (gcmfunc)
		(*gcmfunc)(PadStatus, controllerID);
}

static void CallWiiInputManip(u8* data, WiimoteEmu::ReportFeatures rptf, int controllerID, int ext, const wiimote_key key)
{
	if (wiimfunc)
		(*wiimfunc)(data, rptf, controllerID, ext, key);
}

bool IsRecordingInput()
{
	return (s_playMode == MODE_RECORDING);
}

static bool IsRecordingInputFromSaveState()
{
	return s_bRecordingFromSaveState;
}

bool IsJustStartingRecordingInputFromSaveState()
{
	return IsRecordingInputFromSaveState() && g_currentFrame == 0;
}

bool IsJustStartingPlayingInputFromSaveState()
{
	return IsRecordingInputFromSaveState() && g_currentFrame == 1 && IsPlayingInput();
}

bool IsPlayingInput()
{
	return (s_playMode == MODE_PLAYING);
}

bool IsMovieActive()
{
	return s_playMode != MODE_NONE;
}

bool IsReadOnly()
{
	return s_bReadOnly;
}

u64 GetRecordingStartTime()
{
	return s_recordingStartTime;
}

bool IsUsingPad(int controller)
{
	return ((s_numPads & (1 << controller)) != 0);
}

bool IsUsingBongo(int controller)
{
	return ((s_bongos & (1 << controller)) != 0);
}

static bool IsUsingWiimote(int wiimote)
{
	return ((s_numPads & (1 << (wiimote + 4))) != 0);
}

bool IsConfigSaved()
{
	return s_bSaveConfig;
}

bool IsStartingFromClearSave()
{
	return s_bClearSave;
}

bool IsUsingMemcard(int memcard)
{
	return (s_memcards & (1 << memcard)) != 0;
}

static void ChangePads(bool instantly)
{
	if (!Core::IsRunning())
		return;

	int controllers = 0;

	for (int i = 0; i < MAX_SI_CHANNELS; ++i)
		if (SConfig::GetInstance().m_SIDevice[i] == SIDEVICE_GC_CONTROLLER || SConfig::GetInstance().m_SIDevice[i] == SIDEVICE_GC_TARUKONGA)
			controllers |= (1 << i);

	if (instantly && (s_numPads & 0x0F) == controllers)
		return;

	for (int i = 0; i < MAX_SI_CHANNELS; ++i)
		if (instantly) // Changes from savestates need to be instantaneous
			SerialInterface::AddDevice(IsUsingPad(i) ? (IsUsingBongo(i) ? SIDEVICE_GC_TARUKONGA : SIDEVICE_GC_CONTROLLER) : SIDEVICE_NONE, i);
		else
			SerialInterface::ChangeDevice(IsUsingPad(i) ? (IsUsingBongo(i) ? SIDEVICE_GC_TARUKONGA : SIDEVICE_GC_CONTROLLER) : SIDEVICE_NONE, i);
}

void ChangeWiiPads(bool instantly)
{
	int controllers = 0;

	for (int i = 0; i < MAX_WIIMOTES; ++i)
		if (g_wiimote_sources[i] != WIIMOTE_SRC_NONE)
			controllers |= (1 << i);

	// This is important for Wiimotes, because they can desync easily if they get re-activated
	if (instantly && (s_numPads >> 4) == controllers)
		return;

	for (int i = 0; i < MAX_WIIMOTES; ++i)
	{
		g_wiimote_sources[i] = IsUsingWiimote(i) ? WIIMOTE_SRC_EMU : WIIMOTE_SRC_NONE;
		GetUsbPointer()->AccessWiiMote(i | 0x100)->Activate(IsUsingWiimote(i));
	}
}

bool BeginRecordingInput(int controllers)
{
	if (s_playMode != MODE_NONE || controllers == 0)
		return false;

	bool was_unpaused = Core::PauseAndLock(true);

	s_numPads = controllers;
	g_currentFrame = 0;
	s_currentLagCount = 0;
	s_bongos = 0;
	s_memcards = 0;
	if (NetPlay::IsNetPlayRunning())
	{
		s_recordingStartTime = g_netplay_initial_gctime;
	}
	else
	{
		s_recordingStartTime = Common::Timer::GetLocalTimeSinceJan1970();
	}

	for (int i = 0; i < MAX_SI_CHANNELS; ++i)
		if (SConfig::GetInstance().m_SIDevice[i] == SIDEVICE_GC_TARUKONGA)
			s_bongos |= (1 << i);

	if (Core::IsRunningAndStarted())
	{
		const std::string save_path = File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav";
		if (File::Exists(save_path))
			File::Delete(save_path);

		State::SaveAs(save_path);
		s_bRecordingFromSaveState = true;

		// This is only done here if starting from save state because otherwise we won't have the titleid. Otherwise it's set in WII_IPC_HLE_Device_es.cpp.
		// TODO: find a way to GetTitleDataPath() from Init()
		if (SConfig::GetInstance().m_LocalCoreStartupParameter.bWii)
		{
			if (File::Exists(Common::GetTitleDataPath(s_titleID) + "banner.bin"))
				s_bClearSave = false;
			else
				s_bClearSave = true;
		}
		GetSettings();
	}
	s_playMode = MODE_RECORDING;

	Core::UpdateWantDeterminism();

	Core::PauseAndLock(false, was_unpaused);

	Core::DisplayMessage("Starting movie recording", 2000);
	return true;
}

static std::string AnalogValueToString(u8 v, const std::string& lowest, const std::string& highest)
{
	if (v <= 1)
		return lowest;
	else if (v >= 255)
		return highest;
	return StringFromFormat("%d", v);
}

static std::string Analog2DToString(u8 x, u8 y, const std::string& prefix, u8 range = 255)
{
	std::string result;
	u8 center = range / 2 + 1;
	if (x != center || y != center)
	{
		result += prefix;
		result += ":";

		if (x != center)
			result += AnalogValueToString(x, "LEFT", "RIGHT");

		if (x != center && y != center)
			result += ",";

		if (y != center)
			result += AnalogValueToString(y, "DOWN", "UP");
	}
	return result;
}

static std::string Analog1DToString(u8 v, const std::string& prefix, u8 range = 255)
{
	std::string result;
	if (v > 0)
	{
		result += prefix;
		if (v != range)
			result += StringFromFormat(":%d", v);
	}
	return result;
}

static void SetInputDisplayString(const GCPadStatus* padStatus, int controllerID)
{
	s_InputDisplay[controllerID] = StringFromFormat("P%d:", controllerID + 1);

	if (padStatus->button & PAD_BUTTON_A)
		s_InputDisplay[controllerID].append(" A");
	if (padStatus->button & PAD_BUTTON_B)
		s_InputDisplay[controllerID].append(" B");
	if (padStatus->button & PAD_BUTTON_X)
		s_InputDisplay[controllerID].append(" X");
	if (padStatus->button & PAD_BUTTON_Y)
		s_InputDisplay[controllerID].append(" Y");
	if (padStatus->button & PAD_TRIGGER_Z)
		s_InputDisplay[controllerID].append(" Z");
	if (padStatus->button & PAD_BUTTON_START)
		s_InputDisplay[controllerID].append(" START");

	if (padStatus->button & PAD_BUTTON_UP)
		s_InputDisplay[controllerID].append(" UP");
	if (padStatus->button & PAD_BUTTON_DOWN)
		s_InputDisplay[controllerID].append(" DOWN");
	if (padStatus->button & PAD_BUTTON_LEFT)
		s_InputDisplay[controllerID].append(" LEFT");
	if (padStatus->button & PAD_BUTTON_RIGHT)
		s_InputDisplay[controllerID].append(" RIGHT");

	s_InputDisplay[controllerID].append(Analog1DToString(padStatus->triggerLeft, " L"));
	s_InputDisplay[controllerID].append(Analog1DToString(padStatus->triggerRight, " R"));
	s_InputDisplay[controllerID].append(Analog2DToString(padStatus->stickX, padStatus->stickY, " ANA"));
	s_InputDisplay[controllerID].append(Analog2DToString(padStatus->substickX, padStatus->substickY, " C"));
	s_InputDisplay[controllerID].append("\n");
}

static void SetWiiInputDisplayString(int remoteID, u8* const data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key)
{
	int controllerID = remoteID + 4;

	s_InputDisplay[controllerID] = StringFromFormat("R%d:", remoteID + 1);

	u8* const coreData = rptf.core ? (data + rptf.core) : nullptr;
	u8* const accelData = rptf.accel ? (data + rptf.accel) : nullptr;
	u8* const irData = rptf.ir ? (data + rptf.ir) : nullptr;
	u8* const extData = rptf.ext ? (data + rptf.ext) : nullptr;

	if (coreData)
	{
		wm_buttons buttons = *(wm_buttons*)coreData;
		if(buttons.left)
			s_InputDisplay[controllerID].append(" LEFT");
		if(buttons.right)
			s_InputDisplay[controllerID].append(" RIGHT");
		if(buttons.down)
			s_InputDisplay[controllerID].append(" DOWN");
		if(buttons.up)
			s_InputDisplay[controllerID].append(" UP");
		if(buttons.a)
			s_InputDisplay[controllerID].append(" A");
		if(buttons.b)
			s_InputDisplay[controllerID].append(" B");
		if(buttons.plus)
			s_InputDisplay[controllerID].append(" +");
		if(buttons.minus)
			s_InputDisplay[controllerID].append(" -");
		if(buttons.one)
			s_InputDisplay[controllerID].append(" 1");
		if(buttons.two)
			s_InputDisplay[controllerID].append(" 2");
		if(buttons.home)
			s_InputDisplay[controllerID].append(" HOME");
	}

	if (accelData)
	{
		wm_accel* dt = (wm_accel*)accelData;
		std::string accel = StringFromFormat(" ACC:%d,%d,%d",
			dt->x << 2 | ((wm_buttons*)coreData)->acc_x_lsb, dt->y << 2 | ((wm_buttons*)coreData)->acc_y_lsb << 1, dt->z << 2 | ((wm_buttons*)coreData)->acc_z_lsb << 1);
		s_InputDisplay[controllerID].append(accel);
	}

	if (irData)
	{
		u16 x = irData[0] | ((irData[2] >> 4 & 0x3) << 8);
		u16 y = irData[1] | ((irData[2] >> 6 & 0x3) << 8);
		std::string ir = StringFromFormat(" IR:%d,%d", x,y);
		s_InputDisplay[controllerID].append(ir);
	}

	// Nunchuk
	if (extData && ext == 1)
	{
		wm_nc nunchuk;
		memcpy(&nunchuk, extData, sizeof(wm_nc));
		WiimoteDecrypt(&key, (u8*)&nunchuk, 0, sizeof(wm_nc));
		nunchuk.bt.hex = nunchuk.bt.hex ^ 0x3;

		std::string accel = StringFromFormat(" N-ACC:%d,%d,%d",
			(nunchuk.ax << 2) | nunchuk.bt.acc_x_lsb, (nunchuk.ay << 2) | nunchuk.bt.acc_y_lsb, (nunchuk.az << 2) | nunchuk.bt.acc_z_lsb);

		if (nunchuk.bt.c)
			s_InputDisplay[controllerID].append(" C");
		if (nunchuk.bt.z)
			s_InputDisplay[controllerID].append(" Z");
		s_InputDisplay[controllerID].append(accel);
		s_InputDisplay[controllerID].append(Analog2DToString(nunchuk.jx, nunchuk.jy, " ANA"));
	}

	// Classic controller
	if (extData && ext == 2)
	{
		wm_classic_extension cc;
		memcpy(&cc, extData, sizeof(wm_classic_extension));
		WiimoteDecrypt(&key, (u8*)&cc, 0, sizeof(wm_classic_extension));
		cc.bt.hex = cc.bt.hex ^ 0xFFFF;

		if (cc.bt.regular_data.dpad_left)
			s_InputDisplay[controllerID].append(" LEFT");
		if (cc.bt.dpad_right)
			s_InputDisplay[controllerID].append(" RIGHT");
		if (cc.bt.dpad_down)
			s_InputDisplay[controllerID].append(" DOWN");
		if (cc.bt.regular_data.dpad_up)
			s_InputDisplay[controllerID].append(" UP");
		if (cc.bt.a)
			s_InputDisplay[controllerID].append(" A");
		if (cc.bt.b)
			s_InputDisplay[controllerID].append(" B");
		if (cc.bt.x)
			s_InputDisplay[controllerID].append(" X");
		if (cc.bt.y)
			s_InputDisplay[controllerID].append(" Y");
		if (cc.bt.zl)
			s_InputDisplay[controllerID].append(" ZL");
		if (cc.bt.zr)
			s_InputDisplay[controllerID].append(" ZR");
		if (cc.bt.plus)
			s_InputDisplay[controllerID].append(" +");
		if (cc.bt.minus)
			s_InputDisplay[controllerID].append(" -");
		if (cc.bt.home)
			s_InputDisplay[controllerID].append(" HOME");

		s_InputDisplay[controllerID].append(Analog1DToString(cc.lt1 | (cc.lt2 << 3), " L", 31));
		s_InputDisplay[controllerID].append(Analog1DToString(cc.rt, " R", 31));
		s_InputDisplay[controllerID].append(Analog2DToString(cc.regular_data.lx, cc.regular_data.ly, " ANA", 63));
		s_InputDisplay[controllerID].append(Analog2DToString(cc.rx1 | (cc.rx2 << 1) | (cc.rx3 << 3), cc.ry, " R-ANA", 31));
	}

	s_InputDisplay[controllerID].append("\n");
}

static void RecordInput(GCPadStatus* PadStatus, int controllerID)
{
	if (!IsRecordingInput() || !IsUsingPad(controllerID))
		return;

	// FUNCTION: Takes the given pad status and writes it to file.
	// Effectively recording the pad's input
}

void RecordWiimote(int wiimote, u8 *data, u8 size)
{
	if (!IsRecordingInput() || !IsUsingWiimote(wiimote))
		return;

	// FUNCTION: Takes the given wiimote status and writes it to file.
	// Effectively recording the wiimote's input
}

bool PlayInput(const std::string& filename)
{
	if (s_playMode != MODE_NONE || !File::Exists(filename))
		return false;

	g_currentFrame = 0;
	s_currentLagCount = 0;

	// Loads the input file and readies everything for input playback.
	// If loading is successful, then we continue...

	s_playMode = MODE_PLAYING;

	Core::UpdateWantDeterminism();

	// In this block, you would load the savestate if that's where it starts at.

	return true;
}

void DoState(PointerWrap &p)
{
	u64 s_currentByte = 0, s_tickCountAtLastInput = 0, s_currentInputCount = 0;

	// many of these could be useful to save even when no movie is active,
	// and the data is tiny, so let's just save it regardless of movie state.
	p.Do(g_currentFrame);
	p.Do(s_currentByte);
	p.Do(s_currentLagCount);
	p.Do(s_currentInputCount);
	p.Do(s_bPolled);
	p.Do(s_tickCountAtLastInput);
	// other variables (such as s_totalBytes and s_totalFrames) are set in LoadInput
}

void LoadInput(const std::string& filename)
{
	// This block reads an input file for continuing a recording or something.

	ChangePads(true);
	if (SConfig::GetInstance().m_LocalCoreStartupParameter.bWii)
		ChangeWiiPads(true);

	// Then decides which playback state to go into
}

// Called when the GCController polls for input (should be renamed?)
static void PlayController(GCPadStatus* PadStatus, int controllerID)
{
	// Correct playback is entirely dependent on the emulator polling the controllers
	// in the same order done during recording
	if (!IsPlayingInput() || !IsUsingPad(controllerID))
		return;

	// PadStatus is then set to whatever the movie playback should have.

	/*
	// How old Movie disc changing happened. Seems like more of a hack?
	{
		// This implementation assumes the disc change will only happen once. Trying to change more than that will cause
		// it to load the last disc every time. As far as i know though, there are no 3+ disc games, so this should be fine.
		Core::SetState(Core::CORE_PAUSE);
		bool found = false;
		std::string path;
		for (size_t i = 0; i < SConfig::GetInstance().m_ISOFolder.size(); ++i)
		{
			path = SConfig::GetInstance().m_ISOFolder[i];
			if (File::Exists(path + '/' + s_discChange))
			{
				found = true;
				break;
			}
		}
		if (found)
		{
			DVDInterface::ChangeDisc(path + '/' + s_discChange);
			Core::SetState(Core::CORE_RUN);
		}
		else
		{
			PanicAlertT("Change the disc to %s", s_discChange.c_str());
		}
	}*/

	// How resetting happens. Much easier.
	// ProcessorInterface::ResetButton_Tap();

	// Optionally end input playback
}

// Callback when GCPad gets pad status
// Note sure about netplay.
void GetPadStatus(GCPadStatus* pad_status, int controller_id)
{
	s_bPolled = true;
	CallGCInputManip(pad_status, controller_id);

	if (IsPlayingInput() && IsUsingPad(controller_id))
	{
		PlayController(pad_status, controller_id);
	}

	// This is where netplay update would take place

	if (IsRecordingInput() && IsUsingPad(controller_id))
	{
		RecordInput(pad_status, controller_id);
	}

	InputUpdate();
	SetInputDisplayString(pad_status, controller_id);
}

static bool PlayWiimote(int wiimote, u8 *data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key key)
{
	if (!IsPlayingInput() || !IsUsingWiimote(wiimote))
		return false;

	// Same as GCPad input

	// Optionally end input playback
	return true;
}

// Callback when Wiimote is updated
void UpdateWiimote(int wiimote, u8* data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key)
{
	s_bPolled = true;
	CallWiiInputManip(data, rptf, wiimote, ext, key);

	if (IsPlayingInput() && IsUsingWiimote(wiimote))
	{
		PlayWiimote(wiimote, data, rptf, ext, key);
	}

	// This is where netplay update and misc stuff would take place

	if (IsRecordingInput() && IsUsingWiimote(wiimote))
	{
		RecordWiimote(wiimote, data, rptf.size);
	}

	InputUpdate();
	SetWiiInputDisplayString(wiimote, data, rptf, ext, key);
}

void EndPlayInput()
{
	if (s_playMode != MODE_NONE)
	{
		s_playMode = MODE_NONE;
		Core::UpdateWantDeterminism();
		Core::DisplayMessage("Movie End.", 2000);
		s_bRecordingFromSaveState = false;
	}
}

void SaveRecording(const std::string& filename)
{
	// Saves playback file.

	// Additionally, save state file with this
	// if (success && s_bRecordingFromSaveState)
	// {
	// 	std::string stateFilename = filename + ".sav";
	// 	success = File::Copy(File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav", stateFilename);
	// }

	// Success message
	// if (success)
	// 	Core::DisplayMessage(StringFromFormat("DTM %s saved", filename.c_str()), 2000);
	// else
	// 	Core::DisplayMessage(StringFromFormat("Failed to save %s", filename.c_str()), 2000);
}

void SetGCInputManip(GCManipFunction func)
{
	gcmfunc = func;
}
void SetWiiInputManip(WiiManipFunction func)
{
	wiimfunc = func;
}

// Sets g_Config from VideoConfig just before it becomes the active configuration
// Can potentially eliminate this.
void SetGraphicsConfig()
{
}

void GetSettings()
{
	// Copies SConfig::GetInstance().m_LocalCoreStartupParameter to local variables.
	s_bSaveConfig = true;
	/*
	s_bSkipIdle = SConfig::GetInstance().m_LocalCoreStartupParameter.bSkipIdle;
	s_bDualCore = SConfig::GetInstance().m_LocalCoreStartupParameter.bCPUThread;
	s_bProgressive = SConfig::GetInstance().m_LocalCoreStartupParameter.bProgressive;
	s_bDSPHLE = SConfig::GetInstance().m_LocalCoreStartupParameter.bDSPHLE;
	s_bFastDiscSpeed = SConfig::GetInstance().m_LocalCoreStartupParameter.bFastDiscSpeed;
	s_videoBackend = g_video_backend->GetName();
	s_bSyncGPU = SConfig::GetInstance().m_LocalCoreStartupParameter.bSyncGPU;
	s_iCPUCore = SConfig::GetInstance().m_LocalCoreStartupParameter.iCPUCore;
	if (!SConfig::GetInstance().m_LocalCoreStartupParameter.bWii)
		s_bClearSave = !File::Exists(SConfig::GetInstance().m_strMemoryCardA);
	s_memcards |= (SConfig::GetInstance().m_EXIDevice[0] == EXIDEVICE_MEMORYCARD) << 0;
	s_memcards |= (SConfig::GetInstance().m_EXIDevice[1] == EXIDEVICE_MEMORYCARD) << 1;
	*/
}

void Shutdown()
{
	// Cleanup
}

void ChangeDiscCallback(const std::string& newFileName)
{
	if (IsRecordingInput())
	{
		s_bDiscChange = true;
		std::string fileName = newFileName;
		auto sizeofpath = fileName.find_last_of("/\\") + 1;
		if (fileName.substr(sizeofpath).length() > 40)
		{
			PanicAlert("Saving iso filename to .dtm failed; max file name length is 40 characters.");
		}
		s_discChange = fileName.substr(sizeofpath);
	}
}

void SaveClearCallback(u64 tmdTitleID)
{
	s_titleID = tmdTitleID;
	std::string savePath = Common::GetTitleDataPath(tmdTitleID);
	if (IsRecordingInput())
	{
		// TODO: Check for the actual save data
		if (File::Exists(savePath + "banner.bin"))
			s_bClearSave = false;
		else
			s_bClearSave = true;
	}

	// TODO: Force the game to save to another location, instead of moving the user's save.
	if (IsPlayingInput() && IsConfigSaved() && IsStartingFromClearSave())
	{
		if (File::Exists(savePath + "banner.bin"))
		{
			if (File::Exists(savePath + "../backup/"))
			{
				// The last run of this game must have been to play back a movie, so their save is already backed up.
				File::DeleteDirRecursively(savePath);
			}
			else
			{
				#ifdef _WIN32
					MoveFile(UTF8ToTStr(savePath).c_str(), UTF8ToTStr(savePath + "../backup/").c_str());
				#else
					File::CopyDir(savePath, savePath + "../backup/");
					File::DeleteDirRecursively(savePath);
				#endif
			}
		}
	}
	else if (File::Exists(savePath + "../backup/"))
	{
		// Delete the save made by a previous movie, and copy back the user's save.
		if (File::Exists(savePath + "banner.bin"))
			File::DeleteDirRecursively(savePath);
		#ifdef _WIN32
			MoveFile(UTF8ToTStr(savePath + "../backup/").c_str(), UTF8ToTStr(savePath).c_str());
		#else
			File::CopyDir(savePath + "../backup/", savePath);
			File::DeleteDirRecursively(savePath + "../backup/");
		#endif
	}
}

std::string GetDebugInfo()
{
	std::string info;
	if (SConfig::GetInstance().m_ShowFrameCount)
	{
		info += StringFromFormat("Frame: %lu", g_currentFrame);
		info += "\n";
	}

	if (SConfig::GetInstance().m_ShowLag)
	{
		info += StringFromFormat("Lag: %lu\n", s_currentLagCount);
	}

	if (SConfig::GetInstance().m_ShowInputDisplay)
	{
		info += GetInputDisplay();
	}
	return info;
}

void SetStartupOptions(SCoreStartupParameter* startUp)
{
	if (IsPlayingInput() && IsConfigSaved())
	{
		/*
		startUp->bCPUThread = s_bDualCore;
		startUp->bSkipIdle = s_bSkipIdle;
		startUp->bDSPHLE = s_bDSPHLE;
		startUp->bProgressive = s_bProgressive;
		startUp->bFastDiscSpeed = s_bFastDiscSpeed;
		startUp->iCPUCore = s_iCPUCore;
		startUp->bSyncGPU = s_bSyncGPU;
		*/
		for (int i = 0; i < 2; ++i)
		{
			if (IsUsingMemcard(i) && IsStartingFromClearSave() && !startUp->bWii)
			{
				if (File::Exists(File::GetUserPath(D_GCUSER_IDX) + StringFromFormat("Movie%s.raw", (i == 0) ? "A" : "B")))
					File::Delete(File::GetUserPath(D_GCUSER_IDX) + StringFromFormat("Movie%s.raw", (i == 0) ? "A" : "B"));
			}
		}
	}
}

};
