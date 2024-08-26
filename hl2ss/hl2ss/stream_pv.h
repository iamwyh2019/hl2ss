
#pragma once

#include <winrt/Windows.Media.Capture.Frames.h>
#include "server.h"
using namespace winrt::Windows::Media::Capture::Frames;

typedef void(*FrameCallback)(const MediaFrameReference*);
typedef void (*DataReceivedCallback)(const char* data, int length, sockaddr_in* clientAddr);
typedef void (*DelayCallback)(const int64_t);

void PV_Initialize();
void PV_Quit();
void PV_Cleanup();
void PV_SetCustomFrameCallback(FrameCallback callback);
void PV_SetCustomFrameSentCallback(FrameSentCallback callback);
void PV_SetNetworkDelayCallback(DelayCallback callback);
void PV_SetSystemDelayCallback(DelayCallback callback);