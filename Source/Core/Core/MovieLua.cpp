// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Core/Core.h"
#include "Core/HW/SI.h"
#include "Core/Movie.h"
#include "Core/MovieLua.h"
#include "Common/MsgHandler.h"
#include "InputCommon/GCPadStatus.h"

#include "lua.hpp"

#include <type_traits>
#include <array>

namespace Movie
{
	static lua_State* s_lua = nullptr;
	static int s_wait_amount = 0;

	static const GCPadStatus s_neutral_pad = {
		0, 
		GCPadStatus::MAIN_STICK_CENTER_X, GCPadStatus::MAIN_STICK_CENTER_Y,
		GCPadStatus::C_STICK_CENTER_X, GCPadStatus::C_STICK_CENTER_Y,
		0, 0, 0, 0, 0
	};

	static std::array<GCPadStatus, MAX_SI_CHANNELS> s_controller_states;
	static std::array<GCPadStatus, MAX_SI_CHANNELS> s_hold_controller_states;

	namespace Callbacks
	{
		// Lua module: core
		namespace Core
		{
			// Lua function: core.advance(n = 0)
			//
			// Yields the lua thread and advances the game by n frames.
			int Advance(lua_State* L)
			{
				s_wait_amount = luaL_optinteger(L, 1, 0);
				if (s_wait_amount >= 0)
				{
					return lua_yield(L, 0);
				}
				s_wait_amount = 0;
				return 0;
			}

			// Lua function: core.panic(str)
			//
			// Creates a panic window with the given string.
			int Panic(lua_State* L)
			{
				const char* str = luaL_optstring(L, 1, "Lua panic!");
				PanicAlertT("%s", str);
				return 0;
			}

			static const luaL_Reg s_core_lib[] = {
				{ "advance", &Advance },
				{ "panic", &Panic },
				{ nullptr, nullptr}
			};
		}

		// Lua modules: gcpad1, gcpad2, gcpad3, gcpad4
		namespace GCPad
		{
			template <int PadNumber>
			struct GCPadLib
			{
				// Lua function: gcpad#.press(button, ...)
				//
				// Presses the given buttons for the current frame only.
				static int Press(lua_State* L)
				{
					int num_args = lua_gettop(L);
					for (int i = 1; i <= num_args; ++i)
					{
						int button_id = luaL_checkinteger(L, i);
						s_controller_states[PadNumber].button |= button_id;
					}
					return 0;
				}
				// Lua function: gcpad#.release(button, ...)
				//
				// Releases the given buttons if they were held or pressed.
				static int Release(lua_State* L)
				{
					int num_args = lua_gettop(L);
					for (int i = 1; i <= num_args; ++i)
					{
						int button_id = luaL_checkinteger(L, i);
						s_controller_states[PadNumber].button &= ~button_id;
						s_hold_controller_states[PadNumber].button &= ~button_id;
					}
					return 0;
				}
				// Lua function: gcpad#.hold(button, ...)
				//
				// Holds the given buttons until release is called for them.
				static int Hold(lua_State* L)
				{
					int num_args = lua_gettop(L);
					for (int i = 1; i <= num_args; ++i)
					{
						int button_id = luaL_checkinteger(L, i);
						s_hold_controller_states[PadNumber].button |= button_id;
					}
					return 0;
				}

			};
			template <int PadNum>
			const luaL_Reg* GetPadLib()
			{
				static const luaL_Reg s_gcpad_lib[] = {
					{ "press", &GCPadLib<0>::Press },
					{ "release", &GCPadLib<0>::Release },
					{ "hold", &GCPadLib<0>::Hold },
					{ nullptr, nullptr}
				};
				return s_gcpad_lib;
			}
		}
	}

	// Used since luaL_register is deprecated
	void RegisterLuaTable(const char* name, const luaL_Reg* table)
	{
		lua_newtable(s_lua);
		luaL_setfuncs(s_lua, table, 0);
		lua_pushvalue(s_lua, -1);
		lua_setglobal(s_lua, name);
	}

	// Helper for setting global constants
	void RegisterLuaConstant(const char* name, const int value)
	{
		lua_pushinteger(s_lua, value);
		lua_setglobal(s_lua, name);
	}

	bool PlayInputLua(const std::string& filename)
	{
		StopLua();
		s_lua = luaL_newstate();
		luaL_openlibs(s_lua);

		// Reset controllers
		s_controller_states.fill(s_neutral_pad);
		s_hold_controller_states.fill(s_neutral_pad);

		// Custom libraries
		RegisterLuaTable("core", Callbacks::Core::s_core_lib);
		RegisterLuaTable("gcpad1", Callbacks::GCPad::GetPadLib<0>());
		RegisterLuaTable("gcpad2", Callbacks::GCPad::GetPadLib<1>());
		RegisterLuaTable("gcpad3", Callbacks::GCPad::GetPadLib<2>());
		RegisterLuaTable("gcpad4", Callbacks::GCPad::GetPadLib<3>());

		// GCPad constants
		RegisterLuaConstant("Start", PAD_BUTTON_START);
		RegisterLuaConstant("A", PAD_BUTTON_A);
		RegisterLuaConstant("B", PAD_BUTTON_B);
		RegisterLuaConstant("X", PAD_BUTTON_X);
		RegisterLuaConstant("Y", PAD_BUTTON_Y);
		RegisterLuaConstant("Z", PAD_TRIGGER_Z);
		RegisterLuaConstant("L", PAD_TRIGGER_L);
		RegisterLuaConstant("R", PAD_TRIGGER_R);
		RegisterLuaConstant("DPadUp", PAD_BUTTON_UP);
		RegisterLuaConstant("DPadDown", PAD_BUTTON_DOWN);
		RegisterLuaConstant("DPadLeft", PAD_BUTTON_LEFT);
		RegisterLuaConstant("DPadRight", PAD_BUTTON_RIGHT);

		if(luaL_dofile(s_lua, filename.c_str()))
		{
			PanicAlertT("[LUA] Failed to load.\n %s", lua_tostring(s_lua, -1));
			EndPlayInput(false);
			return false;
		}

		lua_getglobal(s_lua, "main");
		if (!lua_isfunction(s_lua, -1))
		{
			PanicAlertT("[LUA] Missing 'main()' function.");
			EndPlayInput(false);
			return false;
		}

		return true;
	}

	void StopLua()
	{
		if (s_lua)
		{
			lua_close(s_lua);
			s_lua = nullptr;
		}
	}

	void AdvanceLua()
	{
		if (s_lua == nullptr)
			return;

		if (s_wait_amount > 0 )
		{
			--s_wait_amount;
			return;
		}

		int result = lua_resume(s_lua, nullptr, 0);
		if (result != 0 && result != LUA_YIELD)
		{
			PanicAlertT("[LUA] Failed to continue.\n %s", lua_tostring(s_lua, -1));
			EndPlayInput(false);
		}

		if (result == 0)
			EndPlayInput(false);
	}

	bool IsLuaControllerValid(int controllerID)
	{
		return static_cast<unsigned>(controllerID) < s_controller_states.size();
	}

	ControllerState GetLuaController(int controllerID)
	{
		ControllerState result = {};
		if (!IsLuaControllerValid(controllerID))
		{
			PanicAlertT("Controller ID %d out of supported range.", controllerID);
			return result;
		}

		GCPadStatus& pad = s_controller_states[controllerID];
		pad.button |= s_hold_controller_states[controllerID].button;

		// Copy pad info
		result.Start = (pad.button & PAD_BUTTON_START) != 0;
		result.A = (pad.button & PAD_BUTTON_A) != 0;
		result.B = (pad.button & PAD_BUTTON_B) != 0;
		result.X = (pad.button & PAD_BUTTON_X) != 0;
		result.Y = (pad.button & PAD_BUTTON_Y) != 0;
		result.Z = (pad.button & PAD_TRIGGER_Z) != 0;
		result.L = (pad.button & PAD_TRIGGER_L) != 0;
		result.R = (pad.button & PAD_TRIGGER_R) != 0;
		result.DPadUp = (pad.button & PAD_BUTTON_UP) != 0;
		result.DPadDown = (pad.button & PAD_BUTTON_DOWN) != 0;
		result.DPadLeft = (pad.button & PAD_BUTTON_LEFT) != 0;
		result.DPadRight = (pad.button & PAD_BUTTON_RIGHT) != 0;
		result.TriggerL = pad.triggerLeft;
		result.TriggerR = pad.triggerRight;
		result.AnalogStickX = pad.stickX;
		result.AnalogStickY = pad.stickY;
		result.CStickX = pad.substickX;
		result.CStickY = pad.substickY;

		// Clear states
		pad = s_neutral_pad;

		return result;
	}


};

