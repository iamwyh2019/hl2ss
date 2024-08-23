
#pragma once
typedef void (*UnityDebugCallback)(const char*);

void ShowMessage(const char* format, ...);
void ShowMessage(const wchar_t* format, ...);
void UnityShowMessage(const char* format, ...);
void SetUnityDebugCallback(UnityDebugCallback callback);
