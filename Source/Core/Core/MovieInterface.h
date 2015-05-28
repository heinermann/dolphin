// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>

#include "Common/CommonTypes.h"

struct GCPadStatus;
struct wiimote_key;

namespace WiimoteEmu
{
struct ReportFeatures;
}

namespace Movie
{
	class PlaybackInterface;
	class RecordingInterface;

	using PlaybackInterfacePtr = std::unique_ptr<PlaybackInterface>;
	using RecordingInterfacePtr = std::unique_ptr<RecordingInterface>;

	// An interface used for creating playback modules.
	//
	// For example, if you want to add python scripting,
	// you would extend this interface, implement all
	// the necessary functions (including constructor/destructor),
	// and add the file extension to the factory function.
	class PlaybackInterface
	{
	public:
		virtual ~PlaybackInterface() {};

		virtual void PlayController(GCPadStatus* pad_status, int controller_id) = 0;
		virtual void PlayWiimote(int wiimote_id, u8* data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key) = 0;
		virtual void FrameAdvance() = 0;
		virtual bool IsFinished() = 0;

		// Factory
		static PlaybackInterfacePtr CreateInterface(const std::string& filename);
	};

	// An interface for creating input recording modules.
	class RecordingInterface
	{
	public:
		virtual ~RecordingInterface() {};

		virtual void RecordController(const GCPadStatus* pad_status, int controller_id) = 0;
		virtual void RecordWiimote(int wiimote_id, const u8* data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key) = 0;
		virtual void FrameAdvance() = 0;
		virtual void SaveRecording(const std::string& filename) = 0;
	};


}

