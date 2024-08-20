
#pragma once

#include <winrt/Windows.Media.Capture.Frames.h>
#include "server.h"
using namespace winrt::Windows::Media::Capture::Frames;

typedef void(*FrameCallback)(const MediaFrameReference*);

void PV_Initialize();
void PV_Quit();
void PV_Cleanup();
void PV_SetCustomFrameCallback(FrameCallback callback);
void PV_SetCustomFrameSentCallback(FrameSentCallback callback);