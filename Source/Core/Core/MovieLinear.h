// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <string>

#include "Core/MovieInterface.h"
#include "InputCommon/GCPadStatus.h"

namespace Movie
{
	enum class InputType : u8
	{
		GCPAD,
		WIIMOTE,
		RESET
	};

	struct LinearInput
	{
		InputType type;
		u8 pad_number;
		union
		{
			GCPadStatus gcpad;
		};
	};

	struct LinearFormat
	{
		// TODO Info, contains misc info like author and description
		std::map<std::string,std::string> info;

		// TODO Settings, contains emulator settings
		std::map<std::string,std::string> settings;

		// Map of frame number and inputs.
		// Frames with default values are omitted.
		std::multimap<u64, LinearInput> inputs;

	};

	class LinearPlayback : public PlaybackInterface
	{
	public:
		LinearPlayback(const std::string& filename);

		void PlayController(GCPadStatus* pad_status, int controller_id) override;
		void PlayWiimote(int wiimote_id, u8* data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key) override;
		void FrameAdvance() override;
		bool IsFinished() override;

	private:
		LinearFormat m_data = {};
		bool m_finished = false;
	};

	class LinearRecording : public RecordingInterface
	{
	public:
		void RecordController(const GCPadStatus* pad_status, int controller_id) override;
		void RecordWiimote(int wiimote_id, const u8* data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key) override;
		void FrameAdvance() override;
		void SaveRecording(const std::string& filename) override;

	private:
		LinearFormat m_data = {};
	};

}


