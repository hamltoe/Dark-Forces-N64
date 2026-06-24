#pragma once

#include <TFE_System/types.h>

namespace TFE_Platform
{
	typedef void (*EventCallback)(const void* eventData, void* userData);

	bool initDesktopRuntime();
	void shutdownDesktopRuntime();

	void setApplicationName(const char* name);
	bool queryDesktopDisplayMode(s32 displayIndex, s32* outWidth, s32* outHeight, f32* outRefreshRate);
	bool setRelativeMouseMode(bool enable);
	bool dispatchPlatformEvent(const void* eventData);

	void pumpEvents(EventCallback callback, void* userData);
}
