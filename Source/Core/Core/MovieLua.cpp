// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <array>
#include <lua.hpp>

#include "Common/MathUtil.h"
#include "Common/MsgHandler.h"
#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI.h"
#include "Core/Movie.h"
#include "Core/MovieLua.h"
#include "Core/PowerPC/PowerPC.h"
#include "InputCommon/GCPadStatus.h"

enum PadButtonExtension
{
	PADX_ANALOG_LEFT  = 0x00010000,
	PADX_ANALOG_RIGHT = 0x00020000,
	PADX_ANALOG_DOWN  = 0x00040000,
	PADX_ANALOG_UP    = 0x00080000,
	PADX_CSTICK_LEFT  = 0x00100000,
	PADX_CSTICK_RIGHT = 0x00200000,
	PADX_CSTICK_DOWN  = 0x00400000,
	PADX_CSTICK_UP    = 0x00800000,
};

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
			int Advance(lua_State* L);
			int Panic(lua_State* L);
			int Reset(lua_State* L);
			int Pause(lua_State* L);

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
				PanicAlert("%s", str);
				return 0;
			}

			// Lua function: core.reset()
			//
			// Like pressing the reset button on the console.
			int Reset(lua_State* L)
			{
				ProcessorInterface::ResetButton_Tap();
				return 0;
			}

			// Lua function: core.pause()
			//
			// Pauses emulation.
			int Pause(lua_State* L)
			{
				::Core::SetState(::Core::CORE_PAUSE);
				return 0;
			}
			
			static const luaL_Reg s_core_lib[] = {
				{ "advance", &Advance },
				{ "panic", &Panic },
				{ "reset", &Reset },
				{ "pause", &Pause },
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
					GCPadStatus& pad = s_controller_states[PadNumber];
					int num_args = lua_gettop(L);
					for (int i = 1; i <= num_args; ++i)
					{
						int button_id = luaL_checkinteger(L, i);
						if (button_id & PADX_CSTICK_UP)
							pad.substickY = 255;
						if (button_id & PADX_CSTICK_DOWN)
							pad.substickY = 0;
						if (button_id & PADX_CSTICK_LEFT)
							pad.substickX = 0;
						if (button_id & PADX_CSTICK_RIGHT)
							pad.substickX = 255;
						if (button_id & PADX_ANALOG_UP)
							pad.stickY = 255;
						if (button_id & PADX_ANALOG_DOWN)
							pad.stickY = 0;
						if (button_id & PADX_ANALOG_LEFT)
							pad.stickX = 0;
						if (button_id & PADX_ANALOG_RIGHT)
							pad.stickX = 255;

						pad.button |= static_cast<u16>(button_id);
					}
					return 0;
				}

				// Lua function: gcpad#.release(button, ...)
				//
				// Releases the given buttons if they were held or pressed.
				static int Release(lua_State* L)
				{
					GCPadStatus& pad = s_controller_states[PadNumber];
					int num_args = lua_gettop(L);
					for (int i = 1; i <= num_args; ++i)
					{
						int button_id = luaL_checkinteger(L, i);

						if (button_id & (PADX_CSTICK_UP | PADX_CSTICK_DOWN))
							pad.substickY = GCPadStatus::C_STICK_CENTER_Y;
						if (button_id & (PADX_CSTICK_LEFT | PADX_CSTICK_RIGHT))
							pad.substickX = GCPadStatus::C_STICK_CENTER_X;
						if (button_id & (PADX_ANALOG_UP | PADX_ANALOG_DOWN))
							pad.stickY = GCPadStatus::MAIN_STICK_CENTER_Y;
						if (button_id & (PADX_ANALOG_LEFT | PADX_ANALOG_RIGHT))
							pad.stickX = GCPadStatus::MAIN_STICK_CENTER_X;

						pad.button &= ~static_cast<u16>(button_id);
						s_hold_controller_states[PadNumber].button &= ~static_cast<u16>(button_id);
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
						s_hold_controller_states[PadNumber].button |= static_cast<u16>(button_id);
					}
					return 0;
				}

				// Lua function: gcpad#.trigger(button, amount)
				//
				// Partially presses the given button (if controller supported)
				// amount will be clamped between 0 and 255
				static int Trigger(lua_State* L)
				{
					int button_id = luaL_checkinteger(L, 1);
					int amount = luaL_checkinteger(L, 2);

					MathUtil::Clamp(&amount, 0, 255);

					GCPadStatus& pad = s_controller_states[PadNumber];
					if (button_id & PAD_TRIGGER_R)
						pad.triggerRight = amount;
					if (button_id & PAD_TRIGGER_L)
						pad.triggerLeft = amount;
					if (button_id & PAD_BUTTON_A)
						pad.analogA = amount;
					if (button_id & PAD_BUTTON_B)
						pad.analogB = amount;

					return 0;
				}

				// Lua function: gcpad#.analog(x, y)
				//
				// Sets the position of the analog stick,
				// between -127 and 127 for x and y. The neutral value is 0.
				// These values will be clamped if out of range.
				static int Analog(lua_State* L)
				{
					int x = luaL_checkinteger(L, 1);
					int y = luaL_checkinteger(L, 2);

					MathUtil::Clamp(&x, -127, 127);
					MathUtil::Clamp(&y, -127, 127);

					s_controller_states[PadNumber].stickX = x + GCPadStatus::MAIN_STICK_CENTER_X;
					s_controller_states[PadNumber].stickY = y + GCPadStatus::MAIN_STICK_CENTER_Y;
					return 0;
				}

				// Lua function: gcpad#.cstick(x, y)
				//
				// Sets the position of the C-stick,
				// between -127 and 127 for x and y. The neutral value is 0.
				// These values will be clamped if out of range.
				static int CStick(lua_State* L)
				{
					int x = luaL_checkinteger(L, 1);
					int y = luaL_checkinteger(L, 2);

					MathUtil::Clamp(&x, -127, 127);
					MathUtil::Clamp(&y, -127, 127);

					s_controller_states[PadNumber].substickX = x + GCPadStatus::MAIN_STICK_CENTER_X;
					s_controller_states[PadNumber].substickY = y + GCPadStatus::MAIN_STICK_CENTER_Y;
					return 0;
				}
			};
			template <int PadNum>
			const luaL_Reg* GetPadLib()
			{
				static const luaL_Reg s_gcpad_lib[] = {
					{ "press", &GCPadLib<PadNum>::Press },
					{ "release", &GCPadLib<PadNum>::Release },
					{ "hold", &GCPadLib<PadNum>::Hold },
					{ "trigger", &GCPadLib<PadNum>::Trigger },
					{ "analog", &GCPadLib<PadNum>::Analog },
					{ "cstick", &GCPadLib<PadNum>::CStick },
					{ nullptr, nullptr}
				};
				return s_gcpad_lib;
			}
		}

		// Lua module: mem
		namespace MemoryLib
		{
			int ReadU8(lua_State* L);
			int ReadS8(lua_State* L);
			int ReadU16(lua_State* L);
			int ReadS16(lua_State* L);
			int ReadU32(lua_State* L);
			int ReadS32(lua_State* L);
			int ReadU64(lua_State* L);
			int ReadS64(lua_State* L);
			int ReadString(lua_State* L);
			int ReadF32(lua_State* L);
			int ReadF64(lua_State* L);

			int ReadU8(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushinteger(L, PowerPC::Read_U8(address));
				return 1;
			}
			int ReadS8(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushinteger(L, static_cast<s8>(PowerPC::Read_U8(address)));
				return 1;
			}
			int ReadU16(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushinteger(L, PowerPC::Read_U16(address));
				return 1;
			}
			int ReadS16(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushinteger(L, static_cast<s16>(PowerPC::Read_U16(address)));
				return 1;
			}
			int ReadU32(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushinteger(L, PowerPC::Read_U32(address));
				return 1;
			}
			int ReadS32(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushinteger(L, static_cast<s32>(PowerPC::Read_U32(address)));
				return 1;
			}
			int ReadU64(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushinteger(L, PowerPC::Read_U64(address));
				return 1;
			}
			int ReadS64(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushinteger(L, static_cast<s64>(PowerPC::Read_U64(address)));
				return 1;
			}
			int ReadString(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				size_t size = luaL_optinteger(L, 2, 0);
				lua_pushstring(L, PowerPC::HostGetString(address, size).c_str());
				return 1;
			}
			int ReadF32(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushnumber(L, PowerPC::Read_F32(address));
				return 1;
			}
			int ReadF64(lua_State* L)
			{
				u32 address = luaL_checkinteger(L, 1);
				lua_pushnumber(L, PowerPC::Read_F64(address));
				return 1;
			}

			static const luaL_Reg s_memory_lib[] = {
				{ "readu8", &ReadU8 },
				{ "reads8", &ReadS8 },
				{ "readu16", &ReadU16 },
				{ "reads16", &ReadS16 },
				{ "readu32", &ReadU32 },
				{ "reads32", &ReadS32 },
				{ "readu64", &ReadU64 },
				{ "reads64", &ReadS64 },
				{ "getstring", &ReadString},
				{ "readf32", &ReadF32 },
				{ "readf64", &ReadF64 },
				{ nullptr, nullptr}
			};
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
		RegisterLuaTable("mem", Callbacks::MemoryLib::s_memory_lib);

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
		RegisterLuaConstant("CStickUp", PADX_CSTICK_UP);
		RegisterLuaConstant("CStickDown", PADX_CSTICK_DOWN);
		RegisterLuaConstant("CStickLeft", PADX_CSTICK_LEFT);
		RegisterLuaConstant("CStickRight", PADX_CSTICK_RIGHT);
		RegisterLuaConstant("Up", PADX_ANALOG_UP);
		RegisterLuaConstant("Down", PADX_ANALOG_DOWN);
		RegisterLuaConstant("Left", PADX_ANALOG_LEFT);
		RegisterLuaConstant("Right", PADX_ANALOG_RIGHT);

		if (luaL_dofile(s_lua, filename.c_str()))
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

	void PlayControllerLua(GCPadStatus* padStatus, int controllerID)
	{
		if (!IsLuaControllerValid(controllerID))
		{
			PanicAlertT("Controller ID %d out of supported range.", controllerID);
			return;
		}

		*padStatus = s_controller_states[controllerID];
		padStatus->button |= s_hold_controller_states[controllerID].button;

		s_controller_states[controllerID] = s_neutral_pad;
	}

};

