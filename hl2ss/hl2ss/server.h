
#pragma once

#include <winsock2.h>

typedef void(*FrameSentCallback)(const DWORD);

bool   InitializeSockets();
SOCKET CreateSocket(char const* port);
void   CleanupSockets();

bool recv_u8(SOCKET socket, uint8_t& byte);
bool recv_u16(SOCKET socket, uint16_t& word);
bool recv_u32(SOCKET socket, uint32_t& dword);
bool recv_u32_reconnect(SOCKET socket, uint32_t& dword, bool reconnect);
bool recv(SOCKET clientsocket, char* buf, int bytes);
bool recv_reconnect(SOCKET s, char* buf, int len, int flags, bool reconnect);

bool send_multiple(SOCKET s, LPWSABUF buffers, DWORD dwBufferCount, FrameSentCallback callback = nullptr);
bool send_multiple_udp(SOCKET s, LPWSABUF buffers, DWORD dwBufferCount, sockaddr_in *to, FrameSentCallback callback = nullptr);
void pack_buffer(WSABUF* dst, int index, void const* buffer, ULONG length);
