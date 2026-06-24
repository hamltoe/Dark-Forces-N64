#include "platformMain.h"

#if defined(N64)

#include <debug.h>
#include <dragonfs.h>
#include <joypad.h>
#include <timer.h>

#include <TFE_Input/input.h>

namespace
{
	static f32 normalizeSignedAxis(s32 value, s32 range, s32 deadzone)
	{
		if (range <= 0)
		{
			return 0.0f;
		}

		s32 absValue = value < 0 ? -value : value;
		if (absValue <= deadzone)
		{
			return 0.0f;
		}

		f32 normalized = (f32)value / (f32)range;
		if (normalized < -1.0f) { normalized = -1.0f; }
		if (normalized > 1.0f) { normalized = 1.0f; }
		return normalized;
	}

	static void applyButtonEdge(bool pressed, bool released, Button button)
	{
		if (pressed)
		{
			TFE_Input::setButtonDown(button);
		}
		if (released)
		{
			TFE_Input::setButtonUp(button);
		}
	}
}

namespace TFE_Platform
{
	bool initDesktopRuntime()
	{
		debug_init(DEBUG_FEATURE_LOG_USB | DEBUG_FEATURE_LOG_EMU);
		timer_init();
		joypad_init();

			const int dfsResult = dfs_init(DFS_DEFAULT_LOCATION);
			if (dfsResult != DFS_ESUCCESS)
			{
				debugf("n64 lane: dfs_init failed (%d)\n", dfsResult);
				return false;
			}

		return true;
	}

	void shutdownDesktopRuntime()
	{
		joypad_close();
		timer_close();
	}

	void setApplicationName(const char* name)
	{
		(void)name;
	}

	bool queryDesktopDisplayMode(s32 displayIndex, s32* outWidth, s32* outHeight, f32* outRefreshRate)
	{
		(void)displayIndex;
		if (!outWidth || !outHeight || !outRefreshRate)
		{
			return false;
		}

		*outWidth = 320;
		*outHeight = 240;
		*outRefreshRate = 60.0f;
		return true;
	}

	bool setRelativeMouseMode(bool enable)
	{
		(void)enable;
		return true;
	}

	bool dispatchPlatformEvent(const void* eventData)
	{
		(void)eventData;

		joypad_poll();
		if (!joypad_is_connected(JOYPAD_PORT_1))
		{
			TFE_Input::setAxis(AXIS_LEFT_X, 0.0f);
			TFE_Input::setAxis(AXIS_LEFT_Y, 0.0f);
			TFE_Input::setAxis(AXIS_RIGHT_X, 0.0f);
			TFE_Input::setAxis(AXIS_RIGHT_Y, 0.0f);
			TFE_Input::setAxis(AXIS_LEFT_TRIGGER, 0.0f);
			TFE_Input::setAxis(AXIS_RIGHT_TRIGGER, 0.0f);
			return false;
		}

		const joypad_inputs_t inputs = joypad_get_inputs(JOYPAD_PORT_1);
		const joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
		const joypad_buttons_t released = joypad_get_buttons_released(JOYPAD_PORT_1);
		const joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);

		const s32 stickDeadzone = 8;
		const s32 cstickDeadzone = 8;
		TFE_Input::setAxis(AXIS_LEFT_X, normalizeSignedAxis(inputs.stick_x, JOYPAD_RANGE_N64_STICK_MAX, stickDeadzone));
		TFE_Input::setAxis(AXIS_LEFT_Y, normalizeSignedAxis(inputs.stick_y, JOYPAD_RANGE_N64_STICK_MAX, stickDeadzone));
		TFE_Input::setAxis(AXIS_RIGHT_X, normalizeSignedAxis(inputs.cstick_x, JOYPAD_RANGE_GCN_CSTICK_MAX, cstickDeadzone));
		TFE_Input::setAxis(AXIS_RIGHT_Y, normalizeSignedAxis(inputs.cstick_y, JOYPAD_RANGE_GCN_CSTICK_MAX, cstickDeadzone));

		// Default controller bindings use trigger axes for fire actions.
		TFE_Input::setAxis(AXIS_RIGHT_TRIGGER, held.z ? 1.0f : 0.0f);
		TFE_Input::setAxis(AXIS_LEFT_TRIGGER, held.l ? 1.0f : 0.0f);

		applyButtonEdge(pressed.a, released.a, CONTROLLER_BUTTON_A);
		applyButtonEdge(pressed.b, released.b, CONTROLLER_BUTTON_B);
		applyButtonEdge(pressed.c_down, released.c_down, CONTROLLER_BUTTON_X);
		applyButtonEdge(pressed.c_up, released.c_up, CONTROLLER_BUTTON_Y);

		applyButtonEdge(pressed.start, released.start, CONTROLLER_BUTTON_START);
		applyButtonEdge(pressed.start, released.start, CONTROLLER_BUTTON_GUIDE);
		applyButtonEdge(pressed.r, released.r, CONTROLLER_BUTTON_RIGHTSTICK);

		applyButtonEdge(pressed.c_left, released.c_left, CONTROLLER_BUTTON_LEFTSHOULDER);
		applyButtonEdge(pressed.c_right, released.c_right, CONTROLLER_BUTTON_RIGHTSHOULDER);

		applyButtonEdge(pressed.d_up, released.d_up, CONTROLLER_BUTTON_DPAD_UP);
		applyButtonEdge(pressed.d_down, released.d_down, CONTROLLER_BUTTON_DPAD_DOWN);
		applyButtonEdge(pressed.d_left, released.d_left, CONTROLLER_BUTTON_DPAD_LEFT);
		applyButtonEdge(pressed.d_right, released.d_right, CONTROLLER_BUTTON_DPAD_RIGHT);

		return false;
	}

	void pumpEvents(EventCallback callback, void* userData)
	{
		if (callback)
		{
			callback(nullptr, userData);
		}
	}
}

#else

#include <TFE_Input/input.h>
#include <TFE_Settings/settings.h>
#include <TFE_RenderBackend/renderBackend.h>
#include <TFE_Ui/ui.h>

#include <SDL.h>

namespace TFE_Platform
{
	bool initDesktopRuntime()
	{
		const int code = SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO);
		return code == 0;
	}

	void shutdownDesktopRuntime()
	{
		SDL_Quit();
	}

	void setApplicationName(const char* name)
	{
#ifdef SDL_HINT_APP_NAME
		if (name && name[0])
		{
			SDL_SetHint(SDL_HINT_APP_NAME, name);
		}
#else
		(void)name;
#endif
	}

	bool queryDesktopDisplayMode(s32 displayIndex, s32* outWidth, s32* outHeight, f32* outRefreshRate)
	{
		if (!outWidth || !outHeight || !outRefreshRate)
		{
			return false;
		}

		SDL_DisplayMode mode = {};
		if (SDL_GetDesktopDisplayMode(displayIndex, &mode) != 0)
		{
			return false;
		}

		*outWidth = mode.w;
		*outHeight = mode.h;
		*outRefreshRate = (f32)mode.refresh_rate;
		return true;
	}

	bool setRelativeMouseMode(bool enable)
	{
		return SDL_SetRelativeMouseMode(enable ? SDL_TRUE : SDL_FALSE) >= 0;
	}

	bool dispatchPlatformEvent(const void* eventData)
	{
		if (!eventData)
		{
			return false;
		}

		const SDL_Event& event = *reinterpret_cast<const SDL_Event*>(eventData);
		TFE_Ui::setUiInput(eventData);

		TFE_Settings_Window* windowSettings = TFE_Settings::getWindowSettings();

		switch (event.type)
		{
			case SDL_QUIT:
			{
				return true;
			}
			case SDL_WINDOWEVENT:
			{
				if (event.window.event == SDL_WINDOWEVENT_RESIZED || event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
				{
					TFE_RenderBackend::resize(event.window.data1, event.window.data2);
				}
			} break;
			case SDL_CONTROLLERDEVICEADDED:
			{
				const s32 cIdx = event.cdevice.which;
				if (SDL_IsGameController(cIdx))
				{
					SDL_GameController* controller = SDL_GameControllerOpen(cIdx);
					SDL_Joystick* j = SDL_GameControllerGetJoystick(controller);
					SDL_JoystickID joyId = SDL_JoystickInstanceID(j);
					(void)joyId;

					//Save the joystick id to used in the future events
					SDL_GameControllerOpen(0);
				}
			} break;
			case SDL_MOUSEBUTTONDOWN:
			{
				TFE_Input::setMouseButtonDown(MouseButton(event.button.button - SDL_BUTTON_LEFT));
			} break;
			case SDL_MOUSEBUTTONUP:
			{
				TFE_Input::setMouseButtonUp(MouseButton(event.button.button - SDL_BUTTON_LEFT));
			} break;
			case SDL_MOUSEWHEEL:
			{
				TFE_Input::setMouseWheel(event.wheel.x, event.wheel.y);
			} break;
			case SDL_KEYDOWN:
			{
				if (event.key.keysym.scancode)
				{
					TFE_Input::setKeyDown(KeyboardCode(event.key.keysym.scancode), event.key.repeat != 0);
				}

				if (event.key.keysym.scancode)
				{
					TFE_Input::setBufferedKey(KeyboardCode(event.key.keysym.scancode));
				}
			} break;
			case SDL_KEYUP:
			{
				if (event.key.keysym.scancode)
				{
					const KeyboardCode code = KeyboardCode(event.key.keysym.scancode);
					TFE_Input::setKeyUp(KeyboardCode(event.key.keysym.scancode));

					// Fullscreen toggle.
					bool altHeld = TFE_Input::keyDown(KEY_LALT) || TFE_Input::keyDown(KEY_RALT);
					if (code == KeyboardCode::KEY_F11 || (code == KeyboardCode::KEY_RETURN && altHeld))
					{
						windowSettings->fullscreen = !windowSettings->fullscreen;
						TFE_RenderBackend::enableFullscreen(windowSettings->fullscreen);
					}
				}
			} break;
			case SDL_TEXTINPUT:
			{
				TFE_Input::setBufferedInput(event.text.text);
			} break;
			case SDL_CONTROLLERAXISMOTION:
			{
				// Axis are now handled interally so the deadzone can be changed.
				if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
				{ TFE_Input::setAxis(AXIS_LEFT_X, f32(event.caxis.value) / 32768.0f); }
				else if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
				{ TFE_Input::setAxis(AXIS_LEFT_Y, -f32(event.caxis.value) / 32768.0f); }

				if (event.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX)
				{ TFE_Input::setAxis(AXIS_RIGHT_X, f32(event.caxis.value) / 32768.0f); }
				else if (event.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY)
				{ TFE_Input::setAxis(AXIS_RIGHT_Y, -f32(event.caxis.value) / 32768.0f); }

				const s32 deadzone = 3200;
				if ((event.caxis.value < -deadzone) || (event.caxis.value > deadzone))
				{
					if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
					{ TFE_Input::setAxis(AXIS_LEFT_TRIGGER, f32(event.caxis.value) / 32768.0f); }
					if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
					{ TFE_Input::setAxis(AXIS_RIGHT_TRIGGER, f32(event.caxis.value) / 32768.0f); }
				}
				else
				{
					if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
					{ TFE_Input::setAxis(AXIS_LEFT_TRIGGER, 0.0f); }
					if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
					{ TFE_Input::setAxis(AXIS_RIGHT_TRIGGER, 0.0f); }
				}
			} break;
			case SDL_CONTROLLERBUTTONDOWN:
			{
				if (event.cbutton.button < CONTROLLER_BUTTON_COUNT)
				{
					TFE_Input::setButtonDown(Button(event.cbutton.button));
				}
			} break;
			case SDL_CONTROLLERBUTTONUP:
			{
				if (event.cbutton.button < CONTROLLER_BUTTON_COUNT)
				{
					TFE_Input::setButtonUp(Button(event.cbutton.button));
				}
			} break;
			default:
			break;
		}

		return false;
	}

	void pumpEvents(EventCallback callback, void* userData)
	{
		if (!callback)
		{
			return;
		}

		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			callback(&event, userData);
		}
	}
}

#endif
