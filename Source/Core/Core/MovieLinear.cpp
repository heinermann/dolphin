// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// Overriding to disable exceptions (right now do nothing)
#ifndef RAPIDJSON_ASSERT
#define RAPIDJSON_ASSERT(x)
#endif

#include <cereal/archives/json.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <bitset>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>

#include "Common/MsgHandler.h"
#include "Core/Movie.h"
#include "Core/MovieLinear.h"

namespace cereal
{
	/* making an array
	template <class Archive>
	void serialize(Archive& ar, std::array<int,5>& data)
	{
		size_t size = data.size();
		ar(cereal::make_size_tag(size));
		for (auto& it : data)
			ar(it);
	}
	*/
}


namespace Movie
{
	static bool IsNeutralPad(const GCPadStatus* pad)
	{
		return pad->button == 0 &&
			pad->stickX == GCPadStatus::MAIN_STICK_CENTER_X &&
			pad->stickY == GCPadStatus::MAIN_STICK_CENTER_Y &&
			pad->substickX == GCPadStatus::C_STICK_CENTER_X &&
			pad->substickY == GCPadStatus::C_STICK_CENTER_Y &&
			pad->triggerLeft == 0 &&
			pad->triggerRight == 0;
	}

	//
	// Serialization helpers
	//
	
	// Bitset serialization, depends on if the archive is a text archive or not.
	template <class Archive, size_t N>
	typename std::enable_if<!cereal::traits::is_text_archive<Archive>::value, void>::type
	serialize_bitset(Archive& ar, const std::string& name, std::bitset<N>& value)
	{
		unsigned long long value_raw = value.to_ullong();
		ar(cereal::make_nvp(name, value_raw));
		value = std::bitset<N>(value_raw);
	}
	template <class Archive, size_t N>
	typename std::enable_if<cereal::traits::is_text_archive<Archive>::value, void>::type
	serialize_bitset(Archive& ar, const std::string& name, std::bitset<N>& value)
	{
		std::string str = value.to_string();
		ar(cereal::make_nvp(name, str));
		value = std::bitset<N>(str);
	}

	//
	// Serialization functions
	//

	enum InputMask
	{
		MASK_PAD_NUMBER,
		MASK_BUTTON,
		MASK_ANALOG_X,
		MASK_ANALOG_Y,
		MASK_CSTICK_X,
		MASK_CSTICK_Y,
		MASK_TRIGGER_L,
		MASK_TRIGGER_R,
		MAX
	};
	template <class Archive>
	void serialize(Archive& ar, LinearInput& data)
	{
		std::bitset<InputMask::MAX> input_mask;

		if (data.pad_number != 0)
			input_mask.set(MASK_PAD_NUMBER);
		if (data.input.button != 0)
			input_mask.set(MASK_BUTTON);
		if (data.input.stickX != 128)
			input_mask.set(MASK_ANALOG_X);
		if (data.input.stickY != 128)
			input_mask.set(MASK_ANALOG_Y);
		if (data.input.substickX != 128)
			input_mask.set(MASK_CSTICK_X);
		if (data.input.substickY != 128)
			input_mask.set(MASK_CSTICK_Y);
		if (data.input.triggerLeft != 0)
			input_mask.set(MASK_TRIGGER_L);
		if (data.input.triggerRight != 0)
			input_mask.set(MASK_TRIGGER_R);

		serialize_bitset(ar, "mask", input_mask);

		if (input_mask.test(MASK_PAD_NUMBER))
			ar(cereal::make_nvp("padNumber", data.pad_number));

		// Serialize buttons
		if (input_mask.test(MASK_BUTTON))
		{
			std::bitset<16> buttons(data.input.button);
			serialize_bitset(ar, "buttons", buttons);
			data.input.button = static_cast<u16>(buttons.to_ulong());
		}

		if (input_mask.test(MASK_ANALOG_X))
			ar(cereal::make_nvp("analogX", data.input.stickX));

		if (input_mask.test(MASK_ANALOG_Y))
			ar(cereal::make_nvp("analogY", data.input.stickY));

		if (input_mask.test(MASK_CSTICK_X))
			ar(cereal::make_nvp("cstickX", data.input.substickX));

		if (input_mask.test(MASK_CSTICK_Y))
			ar(cereal::make_nvp("cstickY", data.input.substickY));

		if (input_mask.test(MASK_TRIGGER_L))
			ar(cereal::make_nvp("triggerL", data.input.triggerLeft));

		if (input_mask.test(MASK_TRIGGER_R))
			ar(cereal::make_nvp("triggerR", data.input.triggerRight));
	}

	template <class Archive>
	void serialize(Archive& ar, LinearFormat& data)
	{
		ar(cereal::make_nvp("info", data.info));
		ar(cereal::make_nvp("settings", data.settings));
		ar(cereal::make_nvp("inputs", data.inputs));
	}


	//
	// Linear Playback interface
	//

	LinearPlayback::LinearPlayback(const std::string& filename)
	{
		const std::string extension = filename.substr(std::min(filename.size(), filename.rfind('.')));

		if (extension == ".dijson")
		{
			std::ifstream input_file(filename);
			cereal::JSONInputArchive archive(input_file);
			serialize(archive, m_data);
		}
		else if (extension == ".dibin")
		{
			std::ifstream input_file(filename, std::ios::binary);
			cereal::PortableBinaryInputArchive archive(input_file);
			serialize(archive, m_data);
		}
	}

	void LinearPlayback::PlayController(GCPadStatus* pad_status, int controller_id)
	{
		auto range = m_data.inputs.equal_range(g_currentFrame);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second.pad_number == controller_id)
			{
				*pad_status = it->second.input;
				return;
			}
		}
	}

	void LinearPlayback::PlayWiimote(int wiimote_id, u8* data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key)
	{
		// TODO *shrug*
	}

	void LinearPlayback::FrameAdvance()
	{
	}

	//
	// Linear Recording interface
	//

	void LinearRecording::RecordController(const GCPadStatus* pad_status, int controller_id)
	{
		if (!IsNeutralPad(pad_status))
			m_data.inputs.insert(std::make_pair(g_currentFrame, LinearInput{controller_id, *pad_status}));
	}

	void LinearRecording::RecordWiimote(int wiimote_id, const u8* data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key)
	{
		// TODO *shrug*
	}

	void LinearRecording::FrameAdvance()
	{
	}

	void LinearRecording::SaveRecording(const std::string& filename)
	{
		const std::string extension = filename.substr(std::min(filename.size(), filename.rfind('.')));

		if (extension == ".dijson")
		{
			std::ofstream output_file(filename);

			// Use tab instead of spaces, saves bytes
			cereal::JSONOutputArchive::Options opts(
					std::numeric_limits<double>::max_digits10,
					cereal::JSONOutputArchive::Options::IndentChar::tab, 1);
			cereal::JSONOutputArchive archive(output_file, opts);

			serialize(archive, m_data);
		}
		else if (extension == ".dibin")
		{
			std::ofstream output_file(filename, std::ios::binary);
			cereal::PortableBinaryOutputArchive archive(output_file);
			serialize(archive, m_data);
		}
	}
}

