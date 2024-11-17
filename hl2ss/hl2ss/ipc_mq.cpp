
#include <queue>
#include <malloc.h>

#include "server.h"
#include "ports.h"
#include "lock.h"
#include "log.h"
#include "ipc_mq.h"

struct MQ_MSG
{
	uint32_t command;
	uint32_t size;
	void* data;
};

//-----------------------------------------------------------------------------
// Const Variables
//-----------------------------------------------------------------------------
static const uint32_t MQ_COMMAND_HEARTBEAT = 0;
static const uint32_t MQ_COMMAND_GLOBAL_QUIT = 1;

//-----------------------------------------------------------------------------
// Global Variables
//-----------------------------------------------------------------------------

static HANDLE g_thread; // CloseHandle
static HANDLE g_thread_cx; // CloseHandle
static HANDLE g_event_restart; // CloseHandle
static HANDLE g_event_restart_cx; // CloseHandle
static HANDLE g_semaphore_so; // CloseHandle
static HANDLE g_event_error; // CloseHandle
static HANDLE g_event_quit; // CloseHandle
static HANDLE g_event_quit_cx; // CloseHandle
static CRITICAL_SECTION g_lock_si; // DeleteCriticalSection
static CRITICAL_SECTION g_lock_ci; // DeleteCriticalSection
static CRITICAL_SECTION g_lock_so; // DeleteCriticalSection
static CRITICAL_SECTION g_lock_co; // DeleteCriticalSection
static std::queue<MQ_MSG> g_queue_si;
static std::queue<MQ_MSG> g_queue_ci;
static std::queue<uint32_t> g_queue_so;
static std::queue<uint32_t> g_queue_co;

//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

char g_udp_buffer[65535];
HANDLE g_udp_thread = NULL;
HANDLE g_udp_event_quit = NULL;

const int SOCKET_TIMEOUT = 3000;

static DWORD WINAPI MQ_UDP_EntryPoint_Receive(void* param)
{
	g_udp_event_quit = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (g_udp_event_quit == NULL)
	{
		UnityShowMessage("MQ_UDP: Error creating event: %d", WSAGetLastError());
		return WSAGetLastError();
	}

	uint16_t stream_port = *((uint16_t*)param);

	DWORD error;

	sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	serverAddr.sin_port = htons(stream_port);

	UnityShowMessage("MQ_UDP: Listening at port %d", stream_port);

	SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udpSocket == INVALID_SOCKET)
	{
		// error
		error = WSAGetLastError();
		UnityShowMessage("MQ_UDP: Error creating socket: %d", error);
		return error;
	}

	if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&SOCKET_TIMEOUT, sizeof(SOCKET_TIMEOUT)) == SOCKET_ERROR)
	{
		// error
		closesocket(udpSocket);
		error = WSAGetLastError();
		UnityShowMessage("MQ_UDP: Error creating socket: %d", error);
		return error;
	}

	if (bind(udpSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
	{
		// error
		closesocket(udpSocket);
		error = WSAGetLastError();
		UnityShowMessage("MQ_UDP: Error creating socket: %d", error);
		return error;
	}

	MQ_MSG msg;
	int serverAddrSize = sizeof(serverAddr);
	int bytesRead;
	do
	{
		bytesRead = recvfrom(udpSocket, g_udp_buffer, sizeof(g_udp_buffer), 0, (sockaddr*)&serverAddr, &serverAddrSize);

		if (bytesRead == SOCKET_ERROR)
		{
			error = WSAGetLastError();
			if (error == WSAETIMEDOUT)
			{
				// no data available
				continue;
			}
			else
			{
				UnityShowMessage("MQ_UDP Error receiving: %d", error);
				break;
			}
		}

		msg.command = *(uint32_t*)g_udp_buffer;
		msg.size = *(uint32_t*)(g_udp_buffer + sizeof(uint32_t));

		// assertion: bytesRead == sizeof(uint32_t) + sizeof(uint32_t) + msg.size
		if (bytesRead != sizeof(uint32_t) + sizeof(uint32_t) + msg.size)
		{
			UnityShowMessage("MQ_UDP Error receiving: invalid message size (expect %d, got %d)", sizeof(uint32_t) + sizeof(uint32_t) + msg.size, bytesRead);
			continue;
		}

		if (msg.size > 0)
		{
			msg.data = malloc(msg.size);
			if (msg.data == NULL)
			{
				UnityShowMessage("MQ_UDP Error allocating memory");
				break;
			}
			memcpy(msg.data, g_udp_buffer + sizeof(uint32_t) + sizeof(uint32_t), msg.size);
		}
		else
		{
			msg.data = NULL;
		}
		{
			CriticalSection cs(&g_lock_si);
			g_queue_si.push(msg);
		}
	} while (WaitForSingleObject(g_udp_event_quit, 0) == WAIT_TIMEOUT);

	SetEvent(g_udp_event_quit);
	closesocket(udpSocket);

	return 0;
}

// OK
static DWORD WINAPI MQ_EntryPoint_Receive(void *param)
{
	SOCKET clientsocket = *((SOCKET*)param);
	bool ok;
	MQ_MSG msg;

	do
	{
		ok = recv_u32_reconnect(clientsocket, msg.command, false); // in case of timeout, it will stop the thread to wait for the next client
		if (!ok) {
			int error = WSAGetLastError();
			UnityShowMessage("MQ: Error receiving command (%d)", error);
			break;
		}

		ok = recv_u32(clientsocket, msg.size);
		if (!ok) {
			int error = WSAGetLastError();
			UnityShowMessage("MQ: Error receiving size (%d)", error);
			break;
		}

		if (msg.size > 0)
		{
			msg.data = malloc(msg.size);
			ok = recv(clientsocket, (char*)msg.data, msg.size);
			if (!ok)
			{
				free(msg.data);
				int error = WSAGetLastError();
				UnityShowMessage("MQ: Error receiving data (%d)", error);
				break;
			}
		}
		else
		{
			msg.data = NULL;
		}

		if (msg.command == MQ_COMMAND_HEARTBEAT)
		{
			// heartbeat
			continue;
		}
		else if (msg.command == MQ_COMMAND_GLOBAL_QUIT)
		{
			// break, plus end UDP thread
			// stop the UDP thread
			UnityShowMessage("MQ: Global quit command received");
			if (g_udp_event_quit != NULL && g_udp_thread != NULL) {
				SetEvent(g_udp_event_quit);
				WaitForSingleObject(g_udp_thread, INFINITE);
				CloseHandle(g_udp_thread);
				CloseHandle(g_udp_event_quit);
				g_udp_thread = NULL;
				g_udp_event_quit = NULL;
			}
			break;
		}

		{
			CriticalSection cs(&g_lock_si);
			g_queue_si.push(msg);
		}
	}
	while (WaitForSingleObject(g_event_error, 0) == WAIT_TIMEOUT);

	SetEvent(g_event_error);

	return 0;
}

// OK
uint32_t MQ_SI_Peek()
{
	CriticalSection cs(&g_lock_si);
	return g_queue_si.empty() ? ~0UL : g_queue_si.front().size;
}

// OK
void MQ_SI_Pop(uint32_t& command, uint8_t* data)
{
	MQ_MSG msg;

	{
		CriticalSection cs(&g_lock_si);
		msg = g_queue_si.front();
		g_queue_si.pop();
	}

	command = msg.command;
	if (msg.size <= 0) { return; }
	memcpy(data, msg.data, msg.size);
	free(msg.data);
}

// OK
static bool MQ_SO_Wait()
{
	HANDLE events[2];

	events[0] = g_semaphore_so;
	events[1] = g_event_error;

	return WaitForMultipleObjects(sizeof(events) / sizeof(HANDLE), events, FALSE, INFINITE) == WAIT_OBJECT_0;
}

// OK
static DWORD WINAPI MQ_EntryPoint_Send(void *param)
{
	SOCKET clientsocket = *((SOCKET*)param);
	int status;
	uint32_t id;

	do
	{
		if (!MQ_SO_Wait()) { break; }

		{
			CriticalSection cs(&g_lock_so);
			id = g_queue_so.front();
			g_queue_so.pop();
		}

		status = send(clientsocket, (char*)&id, sizeof(id), 0);
		if (status != sizeof(id)) { break; }
	}
	while (WaitForSingleObject(g_event_error, 0) == WAIT_TIMEOUT);

	SetEvent(g_event_error);

	return 0;
}

// OK
void MQ_SO_Push(uint32_t id)
{
	try {
		CriticalSection cs(&g_lock_so);
		g_queue_so.push(id);
		ReleaseSemaphore(g_semaphore_so, 1, NULL);
	}
	catch (std::exception e) {
		UnityShowMessage("MQ_SO_Push: %s", e.what());
	}
}

// OK
void MQ_Restart()
{
	g_queue_so = {};
	CloseHandle(g_semaphore_so);
	ResetEvent(g_event_error);
	SetEvent(g_event_restart);
}

// OK
static void MQ_Procedure(SOCKET clientsocket)
{
	HANDLE threads[2];
	// MQ_MSG msg;

	g_semaphore_so = CreateSemaphore(NULL, 0, LONG_MAX, NULL);

	uint16_t stream_port;
	bool ok;

	// first, set the timeout for the socket
	if (setsockopt(clientsocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&SOCKET_TIMEOUT, sizeof(SOCKET_TIMEOUT)) == SOCKET_ERROR)
	{
		UnityShowMessage("MQ: Error setting socket timeout (%d)", WSAGetLastError());
	}

	if (g_udp_thread == NULL) {
		ok = recv_u16(clientsocket, stream_port);
		if (!ok) {
			UnityShowMessage("MQ: Error receiving stream port (%d)", WSAGetLastError());
		}
		g_udp_thread = CreateThread(NULL, 0, MQ_UDP_EntryPoint_Receive, &stream_port, 0, NULL);
	}
	else {
		UnityShowMessage("MQ: UDP thread already started");
	}
	
	threads[0] = CreateThread(NULL, 0, MQ_EntryPoint_Receive, &clientsocket, 0, NULL);
	if (threads[0] == NULL) {
		UnityShowMessage("MQ: Error creating thread (%d)", WSAGetLastError());
	}
	else {
		WaitForSingleObject(threads[0], INFINITE);
		CloseHandle(threads[0]);
	}

	// Yuheng @ 9/5/2024: No need to send data back to client, so comment out the following code
	//threads[1] = CreateThread(NULL, 0, MQ_EntryPoint_Send, &clientsocket, 0, NULL);

	//WaitForMultipleObjects(sizeof(threads) / sizeof(HANDLE), threads, TRUE, INFINITE);
	

	//CloseHandle(threads[1]);

	/*msg.command = ~0UL;
	msg.size = 0;
	msg.data = NULL;

	{
		CriticalSection cs(&g_lock_si);
		g_queue_si.push(msg);
	}

	ShowMessage("MQ: Waiting for restart signal");
	WaitForSingleObject(g_event_restart, INFINITE);
	ShowMessage("MQ: Restart signal received");*/

	// Yuheng @ 9/5/2024: simply restart
	MQ_Restart();
}

// OK
static DWORD WINAPI MQ_EntryPoint(void* param)
{
	(void)param;

	SOCKET listensocket = CreateSocket(PORT_NAME_MQ);
	SOCKET clientsocket;

	ShowMessage("MQ: Listening at port %s", PORT_NAME_MQ);

	do
	{
		UnityShowMessage("MQ: Waiting for client");
		
		clientsocket = accept(listensocket, NULL, NULL);
		if (clientsocket == INVALID_SOCKET) { break; }

		UnityShowMessage("MQ: Client connected");

		MQ_Procedure(clientsocket);

		closesocket(clientsocket);

		UnityShowMessage("MQ: Client disconnected");
	}
	while (WaitForSingleObject(g_event_quit, 0) == WAIT_TIMEOUT);

	UnityShowMessage("MQ: Closing");

	// stop the UDP thread
	if (g_udp_event_quit != NULL && g_udp_thread != NULL) {
		SetEvent(g_udp_event_quit);
		WaitForSingleObject(g_udp_thread, INFINITE);
		CloseHandle(g_udp_thread);
		CloseHandle(g_udp_event_quit);
		g_udp_thread = NULL;
		g_udp_event_quit = NULL;
	}

	closesocket(listensocket);

	UnityShowMessage("MQ: Closed");

	return 0;
}

// OK
void MQ_Initialize()
{
	InitializeCriticalSection(&g_lock_so);
	InitializeCriticalSection(&g_lock_si);

	g_event_quit    = CreateEvent(NULL, TRUE,  FALSE, NULL);
	g_event_error   = CreateEvent(NULL, TRUE,  FALSE, NULL);
	g_event_restart = CreateEvent(NULL, FALSE, FALSE, NULL);
	
	g_thread = CreateThread(NULL, 0, MQ_EntryPoint, NULL, 0, NULL);
}

// OK
void MQ_Quit()
{
	SetEvent(g_event_quit);
}

// OK
void MQ_Cleanup()
{
	WaitForSingleObject(g_thread, INFINITE);

	CloseHandle(g_thread);

	CloseHandle(g_event_restart);
	CloseHandle(g_event_error);
	CloseHandle(g_event_quit);

	DeleteCriticalSection(&g_lock_si);
	DeleteCriticalSection(&g_lock_so);
}

// OK
static DWORD WINAPI MQX_EntryPoint_Exchange(void* param)
{
	SOCKET clientsocket = *((SOCKET*)param);
	uint32_t c;
	bool ok;
	WSABUF wsabuf[2];
	MQ_MSG msg;

	do
	{
	ok = recv_u32(clientsocket, c);
	if (!ok) { break; }

	if (c != ~0UL)
	{
	CriticalSection cs(&g_lock_co);
	g_queue_co.push(c);
	continue;
	}

	{
	CriticalSection cs(&g_lock_ci);
	if (g_queue_ci.size() > 0)
	{
		msg = g_queue_ci.front();
		g_queue_ci.pop();
	}
	else
	{
		msg.command = ~0UL;
		msg.size = 0;
		msg.data = NULL;
	}
	}

	pack_buffer(wsabuf, 0, &msg, sizeof(MQ_MSG::command) + sizeof(MQ_MSG::size));
	pack_buffer(wsabuf, 1, msg.data, msg.size);

	ok = send_multiple(clientsocket, wsabuf, sizeof(wsabuf) / sizeof(WSABUF));
	if (msg.data) { free(msg.data); }
	if (!ok) { break; }
	}
	while (true);

	return 0;
}

// OK
uint32_t MQX_CO_Peek()
{
	CriticalSection cs(&g_lock_co);
	return g_queue_co.empty() ? ~0UL : sizeof(uint32_t);
}

// OK
void MQX_CO_Pop(uint32_t& id)
{
	CriticalSection cs(&g_lock_co);
	id = g_queue_co.front();
	g_queue_co.pop();
}

// OK
void MQX_CI_Push(uint32_t command, uint32_t size, uint8_t const* data)
{
	MQ_MSG msg;
	msg.command = command;
	msg.size = size;
	if (size > 0)
	{
		msg.data = malloc(size);
		memcpy(msg.data, data, size);
	}
	else
	{
		msg.data = NULL;
	}
	{
	CriticalSection cs(&g_lock_ci);
	g_queue_ci.push(msg);
	}
}

// OK
void MQX_Restart()
{
	while (g_queue_ci.size() > 0)
	{
		MQ_MSG msg = g_queue_ci.front();
		g_queue_ci.pop();
		if (msg.data) { free(msg.data); }
	}
	SetEvent(g_event_restart_cx);
}

// OK
static void MQX_Procedure(SOCKET clientsocket)
{
	HANDLE thread = CreateThread(NULL, 0, MQX_EntryPoint_Exchange, &clientsocket, 0, NULL);
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);

	{
		CriticalSection cs(&g_lock_co);
		g_queue_co.push(~0UL);
	}

	ShowMessage("MQX: Waiting for restart signal");
	WaitForSingleObject(g_event_restart_cx, INFINITE);
	ShowMessage("MQ: Restart signal received");
}

// OK
static DWORD WINAPI MQX_EntryPoint(void* param)
{
	(void)param;

	SOCKET listensocket = CreateSocket(PORT_NAME_MQX);
	SOCKET clientsocket;

	ShowMessage("MQX: Listening at port %s", PORT_NAME_MQX);

	do
	{
		ShowMessage("MQX: Waiting for client");

		clientsocket = accept(listensocket, NULL, NULL);
		if (clientsocket == INVALID_SOCKET) { break; }

		ShowMessage("MQX: Client connected");

		MQX_Procedure(clientsocket);

		closesocket(clientsocket);

		ShowMessage("MQX: Client disconnected");
	}
	while (WaitForSingleObject(g_event_quit_cx, 0) == WAIT_TIMEOUT);

	closesocket(listensocket);

	ShowMessage("MQX: Closed");

	return 0;
}

// OK
void MQX_Initialize()
{
	InitializeCriticalSection(&g_lock_co);
	InitializeCriticalSection(&g_lock_ci);

	g_event_quit_cx = CreateEvent(NULL, TRUE, FALSE, NULL);
	g_event_restart_cx = CreateEvent(NULL, FALSE, FALSE, NULL);

	g_thread_cx = CreateThread(NULL, 0, MQX_EntryPoint, NULL, 0, NULL);
}

// OK
void MQX_Quit()
{
	SetEvent(g_event_quit_cx);
}

// OK
void MQX_Cleanup()
{
	WaitForSingleObject(g_thread_cx, INFINITE);

	CloseHandle(g_thread_cx);

	CloseHandle(g_event_restart_cx);
	CloseHandle(g_event_quit_cx);

	DeleteCriticalSection(&g_lock_ci);
	DeleteCriticalSection(&g_lock_co);
}
