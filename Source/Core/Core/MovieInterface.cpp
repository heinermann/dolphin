// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>

#include "Core/MovieInterface.h"
#include "Core/MovieLinear.h"

namespace Movie
{
	PlaybackInterfacePtr PlaybackInterface::CreateInterface(const std::string& filename)
	{
		const std::string extension = filename.substr(std::min(filename.size(), filename.rfind('.')));
		if (extension == ".dijson" || extension == ".dibin")
		{
			return PlaybackInterfacePtr(new LinearPlayback(filename));
		}
		return PlaybackInterfacePtr(nullptr);
	}

}

