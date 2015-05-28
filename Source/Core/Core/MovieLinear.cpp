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
	/* // making an array
	template <class Archive>
	void serialize(Archive& ar, std::array<int,5>& data)
	{
		size_t size = data.size();
		ar(cereal::make_size_tag(size));
		for (auto& it : data)
			ar(it);
	}
	*/

	// std::map string -> value specializations
	template <class Archive, class T, class C, class A>
	typename std::enable_if<cereal::traits::is_text_archive<Archive>::value, void>::type
	save(Archive& ar, const std::map<std::string, T, C, A>& map)
	{
		for (const auto& it : map)
			ar(cereal::make_nvp(it.first, it.second));
	}
	template <class Archive, class T, class C, class A>
	typename std::enable_if<cereal::traits::is_text_archive<Archive>::value, void>::type
	load(Archive& ar, std::map<std::string, T, C, A>& map)
	{
		map.clear();

		const char* node_name;
		while ((node_name = ar.getNodeName()) != nullptr)
		{
			std::string key(node_name);
			std::string value;
			ar(value);
			map.emplace(key, value);
		}
	}

	// std::multimap u64 -> [objects] specialization (frame key)
	template <class Archive, class T, class C, class A>
	typename std::enable_if<cereal::traits::is_text_archive<Archive>::value, void>::type
	save(Archive& ar, const std::multimap<u64, T, C, A>& map)
	{
		std::vector<T> data;
		u64 current_key = 0;
		for (const auto& it : map)
		{
			if (current_key != it.first)
			{
				if (!data.empty())
				{
					ar(cereal::make_nvp(std::to_string(current_key), data));
					data.clear();
				}
				current_key = it.first;
			}

			if (current_key == it.first)
			{
				data.emplace_back(it.second);
			}
		}
		if (!data.empty())
			ar(cereal::make_nvp(std::to_string(current_key), data));
	}
	template <class Archive, class T, class C, class A>
	typename std::enable_if<cereal::traits::is_text_archive<Archive>::value, void>::type
	load(Archive& ar, std::multimap<u64, T, C, A>& map)
	{
		map.clear();

		const char* node_name;
		while ((node_name = ar.getNodeName()) != nullptr)
		{
			std::string key_str(node_name);
			u64 key = std::stoull(key_str);

			std::vector<T> values;
			ar(values);

			for (const auto& v : values)
				map.emplace(key, v);
		}
	}

}


namespace Movie
{
	static const std::array<std::string, 16> s_button_strings = {
		"LEFT",
		"RIGHT",
		"DOWN",
		"UP",
		"Z",
		"R",
		"L",
		"UNK1",
		"A",
		"B",
		"X",
		"Y",
		"START",
		"UNK2",
		"UNK3",
		"UNK4"
	};
	static const GCPadStatus s_neutral_pad = {
		0, GCPadStatus::MAIN_STICK_CENTER_X, GCPadStatus::MAIN_STICK_CENTER_Y,
		GCPadStatus::C_STICK_CENTER_X, GCPadStatus::C_STICK_CENTER_Y };

	static bool IsNeutralPad(const GCPadStatus* pad)
	{
		return pad->button == s_neutral_pad.button &&
			pad->stickX == s_neutral_pad.stickX &&
			pad->stickY == s_neutral_pad.stickY &&
			pad->substickX == s_neutral_pad.substickX &&
			pad->substickY == s_neutral_pad.substickY &&
			pad->triggerLeft == s_neutral_pad.triggerLeft &&
			pad->triggerRight == s_neutral_pad.triggerRight;
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
	
	template <class Archive, size_t N>
	typename std::enable_if<!cereal::traits::is_text_archive<Archive>::value, void>::type
	serialize_bitset_str(Archive& ar, const std::string& name, std::bitset<N>& value, const std::array<std::string,N>&)
	{
		unsigned long long value_raw = value.to_ullong();
		ar(cereal::make_nvp(name, value_raw));
		value = std::bitset<N>(value_raw);
	}
	template <class Archive, size_t N>
	typename std::enable_if<cereal::traits::is_text_archive<Archive>::value, void>::type
	serialize_bitset_str(Archive& ar, const std::string& name, std::bitset<N>& value, const std::array<std::string,N>& drop_ins)
	{
		std::string str;
		for (size_t i = 0; i < N; ++i)
		{
			if (value.test(i))
			{
				if (!str.empty()) str += ' ';
				str += drop_ins[i];
			}
		}

		ar(cereal::make_nvp(name, str));

		value.reset();
		std::istringstream ss(str);
		while (ss >> str)
		{
			auto it = std::find(drop_ins.begin(), drop_ins.end(), str);
			if (it != drop_ins.end())
				value.set(it - drop_ins.begin());
		}
	}
	

	//
	// Serialization functions
	//

	enum GCInputMask
	{
		MASK_PADNUM,
		MASK_BUTTON,
		MASK_ANALOG_X,
		MASK_ANALOG_Y,
		MASK_CSTICK_X,
		MASK_CSTICK_Y,
		MASK_TRIGGER_L,
		MASK_TRIGGER_R,
		MASK_MAX
	};
	template <class Archive>
	void serialize(Archive& ar, LinearInput& data)
	{
		ar(cereal::make_nvp("type", data.type));

		if (data.type == InputType::GCPAD)
		{
			// Create input mask
			std::bitset<MASK_MAX> input_mask;
			if (data.pad_number != 0)
				input_mask.set(MASK_PADNUM);
			if (data.gcpad.button != s_neutral_pad.button)
				input_mask.set(MASK_BUTTON);
			if (data.gcpad.stickX != s_neutral_pad.stickX)
				input_mask.set(MASK_ANALOG_X);
			if (data.gcpad.stickY != s_neutral_pad.stickY)
				input_mask.set(MASK_ANALOG_Y);
			if (data.gcpad.substickX != s_neutral_pad.substickX)
				input_mask.set(MASK_CSTICK_X);
			if (data.gcpad.substickY != s_neutral_pad.substickY)
				input_mask.set(MASK_CSTICK_Y);
			if (data.gcpad.triggerLeft != s_neutral_pad.triggerLeft)
				input_mask.set(MASK_TRIGGER_L);
			if (data.gcpad.triggerRight != s_neutral_pad.triggerRight)
				input_mask.set(MASK_TRIGGER_R);

			serialize_bitset(ar, "mask", input_mask);

			if (input_mask.test(MASK_PADNUM))
				ar(cereal::make_nvp("padNumber", data.pad_number));
			else
				data.pad_number = 0;

			// Serialize buttons
			if (input_mask.test(MASK_BUTTON))
			{
				std::bitset<16> buttons(data.gcpad.button);
				serialize_bitset_str(ar, "buttons", buttons, s_button_strings);
				data.gcpad.button = static_cast<u16>(buttons.to_ulong());
			}
			else
			{
				data.gcpad.button = s_neutral_pad.button;
			}

			if (input_mask.test(MASK_ANALOG_X))
				ar(cereal::make_nvp("analogX", data.gcpad.stickX));
			else
				data.gcpad.stickX = s_neutral_pad.stickX;

			if (input_mask.test(MASK_ANALOG_Y))
				ar(cereal::make_nvp("analogY", data.gcpad.stickY));
			else
				data.gcpad.stickY = s_neutral_pad.stickY;

			if (input_mask.test(MASK_CSTICK_X))
				ar(cereal::make_nvp("cstickX", data.gcpad.substickX));
			else
				data.gcpad.substickX = s_neutral_pad.substickX;

			if (input_mask.test(MASK_CSTICK_Y))
				ar(cereal::make_nvp("cstickY", data.gcpad.substickY));
			else
				data.gcpad.substickY = s_neutral_pad.substickX;

			if (input_mask.test(MASK_TRIGGER_L))
				ar(cereal::make_nvp("triggerL", data.gcpad.triggerLeft));
			else
				data.gcpad.triggerLeft = s_neutral_pad.triggerLeft;

			if (input_mask.test(MASK_TRIGGER_R))
				ar(cereal::make_nvp("triggerR", data.gcpad.triggerRight));
			else
				data.gcpad.triggerRight = s_neutral_pad.triggerRight;
		}
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

		SetRecordingStartTime(std::stoull(m_data.info["seed"]));
	}

	void LinearPlayback::PlayController(GCPadStatus* pad_status, int controller_id)
	{
		auto range = m_data.inputs.equal_range(g_currentFrame);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second.type == InputType::GCPAD && it->second.pad_number == controller_id)
			{
				*pad_status = it->second.gcpad;
				return;
			}
		}

		*pad_status = s_neutral_pad;
	}

	void LinearPlayback::PlayWiimote(int wiimote_id, u8* data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key& key)
	{
		// TODO *shrug*
	}

	void LinearPlayback::FrameAdvance()
	{
		if (m_data.inputs.empty() || g_currentFrame > m_data.inputs.rbegin()->first)
			m_finished = true;
	}

	bool LinearPlayback::IsFinished()
	{
		return m_finished;
	}

	//
	// Linear Recording interface
	//

	void LinearRecording::RecordController(const GCPadStatus* pad_status, int controller_id)
	{
		// Find if there is already an entry for this info
		auto found_it = m_data.inputs.end();
		auto range = m_data.inputs.equal_range(g_currentFrame);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second.type == InputType::GCPAD && it->second.pad_number == controller_id)
				found_it = it;
		}

		// Modify this entry or insert a new one
		if (found_it != m_data.inputs.end())
		{
			if (IsNeutralPad(pad_status))
				m_data.inputs.erase(found_it);
			else
				found_it->second.gcpad = *pad_status;
		}
		else if (!IsNeutralPad(pad_status))
		{
			m_data.inputs.emplace(g_currentFrame, LinearInput{InputType::GCPAD, static_cast<u8>(controller_id), *pad_status});
		}
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
		m_data.info["seed"] = std::to_string(GetRecordingStartTime());

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

