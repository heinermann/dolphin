// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <string>

struct luaL_Reg;
struct GCPadStatus;

// LUA callbacks for movie playback
namespace Movie
{
	void RegisterLuaTable(const char* name, const luaL_Reg* table);
	void RegisterLuaConstant(const char* name, const int value);
	bool PlayInputLua(const std::string& filename);
	void StopLua();
	void AdvanceLua();
	bool IsLuaControllerValid(int controllerID);
	void PlayControllerLua(GCPadStatus* padStatus, int controllerID);
}

