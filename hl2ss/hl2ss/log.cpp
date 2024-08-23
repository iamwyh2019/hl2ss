
#include <Windows.h>
#include <stdio.h>
#include <malloc.h>
#include "log.h"

//-----------------------------------------------------------------------------
// Functions 
//-----------------------------------------------------------------------------

// OK
void ShowMessage(const char* format, ...)
{
	char* text;
	int len;
	va_list arg_list;
	va_start(arg_list, format);
	len = _vscprintf(format, arg_list) + 2;
	text = (char*)malloc(len);
	if (!text) { return; }
	vsprintf_s(text, len, format, arg_list);
	va_end(arg_list);
	text[len - 2] = '\n';
	text[len - 1] = '\0';
	OutputDebugStringA(text);
	free(text);
}

// OK
void ShowMessage(const wchar_t* format, ...)
{
	wchar_t* text;
	int len;
	va_list arg_list;
	va_start(arg_list, format);
	len = _vscwprintf(format, arg_list) + 2;
	text = (wchar_t*)malloc(len * sizeof(wchar_t));
	if (!text) { return; }
	vswprintf_s(text, len, format, arg_list);
	va_end(arg_list);
	text[len - 2] = L'\n';
	text[len - 1] = L'\0';
	OutputDebugStringW(text);
	free(text);
}

// Global variable to store the callback function pointer
UnityDebugCallback unityDebugCallback = nullptr;

void SetUnityDebugCallback(UnityDebugCallback callback) {
	unityDebugCallback = callback;
}

void UnityShowMessage(const char* format, ...) {
	if (unityDebugCallback != nullptr) {
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		// Call the Unity callback with the formatted message
		unityDebugCallback(buffer);
	}
	else {
		ShowMessage("UnityDebugCallback is not set. Cannot send message to Unity.");
	}
}
