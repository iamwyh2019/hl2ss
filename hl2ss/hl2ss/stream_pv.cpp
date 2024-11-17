
#include <mfapi.h>
#include "custom_media_sink.h"
#include "custom_media_buffers.h"
#include "personal_video.h"
#include "locator.h"
#include "stream_pv.h"
#include "log.h"
#include "ports.h"
#include "timestamps.h"
#include "ipc_sc.h"
#include "research_mode.h"
#include "extended_execution.h"
#include "nfo.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.Devices.Core.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Perception.Spatial.h>

#include <chrono>

// disable winsock deprecated warnings
#pragma warning(disable: 4996)

using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Capture::Frames;
using namespace winrt::Windows::Media::Devices::Core;
using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Perception::Spatial;
using namespace std::chrono;

struct uint64x2
{
    uint64_t val[2];
};

struct float7
{
    float val[7];
};

struct PV_Projection
{
    float2   f;
    float2   c;
    uint64_t exposure_time;
    uint64x2 exposure_compensation;
    uint32_t lens_position;
    uint32_t focus_state;
    uint32_t iso_speed;
    uint32_t white_balance;
    float2   iso_gains;
    float3   white_balance_gains;
    uint64_t _reserved;
    uint64_t timestamp;
    float4x4 pose;
};

struct PV_Mode2
{
    float2   f;
    float2   c;
    float3   r;
    float2   t;
    float4x4 p;
    float4x4 extrinsics;
    float4   intrinsics_mf;
    float7   extrinsics_mf;
};

//-----------------------------------------------------------------------------
// Global Variables
//-----------------------------------------------------------------------------

static HANDLE g_event_quit = NULL; // CloseHandle
static HANDLE g_thread = NULL; // CloseHandle
static SRWLOCK g_lock;

// Mode: 0, 1
static IMFSinkWriter* g_pSinkWriter = NULL; // Release
static DWORD g_dwVideoIndex = 0;
static uint32_t g_counter = 0;
static uint32_t g_divisor = 1;
static PV_Projection g_pvp_sh;

// Mode: 2
static HANDLE g_event_intrinsic = NULL; // alias
static PV_Mode2 g_calibration;

// Custom callback
FrameCallback g_frameCallBack = nullptr;
FrameSentCallback g_frameSentCallBack = nullptr;

// Generic UDP socket to send data
SOCKET g_UDP_socket = INVALID_SOCKET;

// Function to get system boot time
int64_t GetSystemBootTime()
{
	auto current_time = system_clock::now();
	auto uptime_ms = GetTickCount64();
	auto boot_time_ms = duration_cast<milliseconds>(current_time.time_since_epoch()).count() - uptime_ms;
	return static_cast<int64_t>(boot_time_ms);
}
int64_t g_system_boot_time = GetSystemBootTime(); // in milliseconds

int64_t GetFrameUTCTimestamp(int64_t timestamp)
{
	int64_t relative_time_ms = timestamp / 10000; // convert to milliseconds
	return g_system_boot_time + relative_time_ms;
}


//-----------------------------------------------------------------------------
// Delay measurement-related definitions
//-----------------------------------------------------------------------------
static HANDLE g_delay_thread;

DelayCallback g_system_delay_callback = nullptr;
DelayCallback g_network_delay_callback = nullptr;

void PV_SetSystemDelayCallback(DelayCallback callback)
{
	g_system_delay_callback = callback;
}

void PV_SetNetworkDelayCallback(DelayCallback callback)
{
    g_network_delay_callback = callback;
}

void onDelayDataReceived(const char* data, int length, sockaddr_in* clientAddr)
{
	// if too long, discard it
    if (length > 4096)
    {
		return;
	}

    // get the timestamp from the first 8 bytes
    //int64_t timestamp = *(int64_t*)data;
    // convert to milliseconds
    //timestamp /= 10000;

    // get the time stamp from the second 8 bytes
    int64_t timestamp = *(int64_t*)(data + 8);

    // get the current time
    uint64_t current_time = GetTickCount64();

    // calculate the delay
    int64_t delay = current_time - timestamp;
    // round trip delay, so divide by 2
    delay /= 2;

    if (g_network_delay_callback != nullptr)
    {
        g_network_delay_callback(delay);
	}
}

struct ThreadParams
{
	SOCKET socket;
	DataReceivedCallback callback;
};

char g_delay_buffer[65535];

static DWORD WINAPI udpListenerThread(LPVOID lpParam)
{
	ThreadParams* params = (ThreadParams*)lpParam;
	SOCKET socket = params->socket;
	DataReceivedCallback callback = params->callback;

	sockaddr_in clientAddr;
	int clientAddrSize = sizeof(clientAddr);
	int bytesRead;

    do
    {
		bytesRead = recvfrom(socket, g_delay_buffer, sizeof(g_delay_buffer), 0, (sockaddr*)&clientAddr, &clientAddrSize);

        if (bytesRead == SOCKET_ERROR)
        {
			int error = WSAGetLastError();
            if (error == WSAETIMEDOUT)
            {
				// no data available
				continue;
			}
            else
            {
				UnityShowMessage("Error receiving delay data: %d", error);
				break;
			}
		}

		callback(g_delay_buffer, bytesRead, &clientAddr);
	}
    while (WaitForSingleObject(g_event_quit, 0) == WAIT_TIMEOUT);

    // cleanup
    closesocket(socket);
    delete params;

    return 0;
}

static void runUdpListener(uint16_t port, DataReceivedCallback callback)
{
	SOCKET udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET)
    {
		// error
		return;
	}

    int timeout = 5000; // 5 seconds
    if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR)
    {
		// error
		closesocket(udpSocket);
		return;
	}

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(udpSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        // error
        closesocket(udpSocket);
        return;
    }

    ThreadParams *params = new ThreadParams();
    params->socket = udpSocket;
    params->callback = callback;

    g_delay_thread = CreateThread(NULL, 0, udpListenerThread, params, 0, NULL);
}


//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

void PV_SetCustomFrameCallback(FrameCallback callback)
{
    g_frameCallBack = callback;
}

void PV_SetCustomFrameSentCallback(FrameSentCallback callback)
{
    g_frameSentCallBack = callback;
}

// OK
template<bool ENABLE_LOCATION>
void PV_OnVideoFrameArrived(MediaFrameReader const& sender, MediaFrameArrivedEventArgs const& args)
{
    (void)args;

    // get the QPC timestamp
    LARGE_INTEGER frequency, currentQPC;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&currentQPC);
    LONGLONG currentTimeIn100ns = currentQPC.QuadPart * 10000000 / frequency.QuadPart;
    
    IMFSample* pSample; // Release
    SoftwareBitmapBuffer* pBuffer; // Release
    PV_Projection pj;

    if (TryAcquireSRWLockShared(&g_lock) != 0)
    {
        auto const& frame = sender.TryAcquireLatestFrame();
        if (frame) 
        {
            if (g_counter == 0)
            {
                SoftwareBitmapBuffer::CreateInstance(&pBuffer, frame);

                MFCreateSample(&pSample);

                int64_t timestamp = frame.SystemRelativeTime().Value().count();

                // get the delay between the start of this function and the actual frame timestamp
                int64_t delay = currentTimeIn100ns - timestamp;
                // convert to milliseconds
                delay /= 10000;

                pSample->AddBuffer(pBuffer);
                pSample->SetSampleDuration(frame.Duration().count());
                pSample->SetSampleTime(timestamp);

                auto const& intrinsics = frame.VideoMediaFrame().CameraIntrinsics();
                auto const& metadata   = frame.Properties().Lookup(MFSampleExtension_CaptureMetadata).as<IMapView<winrt::guid, winrt::Windows::Foundation::IInspectable>>();

                //pj.timestamp = timestamp;
 
                // use the reserved field to store the delay
                pj._reserved = delay;
				// record current time tick to measure delay
                pj.timestamp = GetTickCount64();
                pj.f = intrinsics.FocalLength();
                pj.c = intrinsics.PrincipalPoint();

                pj.exposure_time         = metadata.Lookup(MF_CAPTURE_METADATA_EXPOSURE_TIME).as<uint64_t>();
                pj.exposure_compensation = *(uint64x2*)metadata.Lookup(MF_CAPTURE_METADATA_EXPOSURE_COMPENSATION).as<winrt::Windows::Foundation::IReferenceArray<uint8_t>>().Value().begin();
                pj.iso_speed             = metadata.Lookup(MF_CAPTURE_METADATA_ISO_SPEED).as<uint32_t>();
                pj.iso_gains             = *(float2*)metadata.Lookup(MF_CAPTURE_METADATA_ISO_GAINS).as<winrt::Windows::Foundation::IReferenceArray<uint8_t>>().Value().begin();
                pj.lens_position         = metadata.Lookup(MF_CAPTURE_METADATA_LENS_POSITION).as<uint32_t>();
                pj.focus_state           = metadata.Lookup(MF_CAPTURE_METADATA_FOCUSSTATE).as<uint32_t>();
                pj.white_balance         = metadata.Lookup(MF_CAPTURE_METADATA_WHITEBALANCE).as<uint32_t>();
                pj.white_balance_gains   = *(float3*)metadata.Lookup(MF_CAPTURE_METADATA_WHITEBALANCE_GAINS).as<winrt::Windows::Foundation::IReferenceArray<uint8_t>>().Value().begin();

                if constexpr (ENABLE_LOCATION)
                {
                    pj.pose = Locator_GetTransformTo(frame.CoordinateSystem(), Locator_GetWorldCoordinateSystem(QPCTimestampToPerceptionTimestamp(timestamp)));
                }

                if (g_frameCallBack != nullptr)
                {
                    g_frameCallBack(pj.timestamp, (float*)&pj.pose);
				}

                pSample->SetBlob(MF_USER_DATA_PAYLOAD, (UINT8*)&pj, sizeof(pj));

                g_pSinkWriter->WriteSample(g_dwVideoIndex, pSample);

                pSample->Release();
                pBuffer->Release();
            }
            g_counter = (g_counter + 1) % g_divisor;
        }
    ReleaseSRWLockShared(&g_lock);
    }
}

// OK
static void PV_OnVideoFrameArrived_Intrinsics(MediaFrameReader const& sender, MediaFrameArrivedEventArgs const& args)
{
    (void)args;

    if (TryAcquireSRWLockExclusive(&g_lock) != 0)
    {
        if (WaitForSingleObject(g_event_intrinsic, 0) == WAIT_TIMEOUT)
        {
            auto const& frame = sender.TryAcquireLatestFrame();
            if (frame) 
            {    
                auto const& intrinsics = frame.VideoMediaFrame().CameraIntrinsics();
                auto const& extrinsics = frame.Properties().Lookup(MFSampleExtension_CameraExtrinsics).as<winrt::Windows::Foundation::IReferenceArray<uint8_t>>().Value();
                auto const& additional = frame.Format().Properties().Lookup(winrt::guid("86b6adbb-3735-447d-bee5-6fc23cb58d4a")).as<winrt::Windows::Foundation::IReferenceArray<uint8_t>>().Value();

                g_calibration.f = intrinsics.FocalLength();
                g_calibration.c = intrinsics.PrincipalPoint();
                g_calibration.r = intrinsics.RadialDistortion();
                g_calibration.t = intrinsics.TangentialDistortion();
                g_calibration.p = intrinsics.UndistortedProjectionTransform();

                g_calibration.extrinsics = Locator_Locate(QPCTimestampToPerceptionTimestamp(frame.SystemRelativeTime().Value().count()), ResearchMode_GetLocator(), frame.CoordinateSystem());

                g_calibration.intrinsics_mf = *(float4*)&((float*)additional.begin())[3];
                g_calibration.extrinsics_mf = *(float7*)&((float*)extrinsics.begin())[5];

                SetEvent(g_event_intrinsic);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
}

// OK
template<bool ENABLE_LOCATION>
void PV_SendSample(IMFSample* pSample, void* param)
{
    IMFMediaBuffer* pBuffer; // Release
    LONGLONG sampletime;
    BYTE* pBytes;
    DWORD cbData;
    WSABUF wsaBuf[ENABLE_LOCATION ? 6 : 5];

    HookCallbackSocket* user = (HookCallbackSocket*)param;
    H26xFormat* format = (H26xFormat*)user->format;
    bool sh = format->profile != H26xProfile::H26xProfile_None;

    pSample->GetSampleTime(&sampletime);
    pSample->ConvertToContiguousBuffer(&pBuffer);
    
    if (!sh) { pSample->GetBlob(MF_USER_DATA_PAYLOAD, (UINT8*)&g_pvp_sh, sizeof(g_pvp_sh), NULL); }

    pBuffer->Lock(&pBytes, NULL, &cbData);

    int const metadata = sizeof(g_pvp_sh) - sizeof(g_pvp_sh.timestamp) - sizeof(g_pvp_sh.pose);
    DWORD cbDataEx = cbData + metadata;

    // get the delay
    int64_t currentTick = GetTickCount64(); // implicit conversion to int64_t
    int64_t delay = currentTick - g_pvp_sh.timestamp; // +g_pvp_sh._reserved;
    if (g_system_delay_callback != nullptr)
    {
		g_system_delay_callback(delay);
	}
    currentTick = GetTickCount64();

    pack_buffer(wsaBuf, 0, &g_pvp_sh.timestamp, sizeof(g_pvp_sh.timestamp));
    pack_buffer(wsaBuf, 1, &currentTick, sizeof(currentTick));
    pack_buffer(wsaBuf, 2, &cbDataEx, sizeof(cbDataEx));
    pack_buffer(wsaBuf, 3, pBytes, cbData);
    pack_buffer(wsaBuf, 4, &g_pvp_sh, metadata);

    if constexpr(ENABLE_LOCATION)
    {
        pack_buffer(wsaBuf, 5, &g_pvp_sh.pose, sizeof(g_pvp_sh.pose));
    }
    
    // bool ok = send_multiple(user->clientsocket, wsaBuf, sizeof(wsaBuf) / sizeof(WSABUF), g_frameSentCallBack);

    bool ok = send_multiple_udp(g_UDP_socket, wsaBuf, sizeof(wsaBuf) / sizeof(WSABUF), &user->udp_address, g_frameSentCallBack);
    if (!ok) {
        SetEvent(user->clientevent);
        // get the error code
        int error = WSAGetLastError();
        UnityShowMessage("PV: Error sending data: %d", error);
    }

    pBuffer->Unlock();
    pBuffer->Release();

    if (sh) { pSample->GetBlob(MF_USER_DATA_PAYLOAD, (UINT8*)&g_pvp_sh, sizeof(g_pvp_sh), NULL); }
}

// OK
template<bool ENABLE_LOCATION>
int PV_Stream(SOCKET clientsocket, HANDLE clientevent, MediaFrameReader const& reader, H26xFormat& format, uint8_t& mode)
{
    CustomMediaSink* pSink; // Release
    std::vector<uint64_t> options;
    HookCallbackSocket user;
    uint16_t stream_port;
    bool ok;

    ok = ReceiveH26xFormat_Divisor(clientsocket, format);
    if (!ok) {
        return WSAGetLastError();
    }

    ok = ReceiveH26xFormat_Profile(clientsocket, format);
    if (!ok) {
        return WSAGetLastError();
    }

    ok = ReceiveH26xEncoder_Options(clientsocket, options);
    if (!ok) {
        return WSAGetLastError();
    }

    ok = recv_u16(clientsocket, stream_port);
    if (!ok) {
		return WSAGetLastError();
	}
 
    // initialize the generic UDP socket
    g_UDP_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_UDP_socket == INVALID_SOCKET) {
		return WSAGetLastError();
	}

    // get the IP address of the client
    sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);
    if (getpeername(clientsocket, (sockaddr*)&client_addr, &client_addr_len) != 0) {
        return WSAGetLastError();
    }

    // set the user's udp address
    user.udp_address.sin_family = AF_INET;
    user.udp_address.sin_port = htons(stream_port);
    user.udp_address.sin_addr = client_addr.sin_addr;

    user.clientsocket = clientsocket; // set it but won't use it
    user.clientevent  = clientevent;
    user.format       = &format;

    UnityShowMessage("PV: Streaming to %s:%d", inet_ntoa(user.udp_address.sin_addr), ntohs(user.udp_address.sin_port));

    CreateSinkWriterVideo(&pSink, &g_pSinkWriter, &g_dwVideoIndex, VideoSubtype::VideoSubtype_NV12, format, options, PV_SendSample<ENABLE_LOCATION>, &user);

    reader.FrameArrived(PV_OnVideoFrameArrived<ENABLE_LOCATION>);

    g_counter = 0;
    g_divisor = format.divisor;    
    memset(&g_pvp_sh, 0, sizeof(g_pvp_sh));

    //runUdpListener(stream_port, onDelayDataReceived);

    ReleaseSRWLockExclusive(&g_lock);
    reader.StartAsync().get();

    // receive mode
    do
    {
        ok = recv_u8(clientsocket, mode);
        if (!ok) {
			// if it's timeout, continue
            if (WSAGetLastError() == WSAETIMEDOUT)
            {
				continue;
			}
            break;
		}
        if (mode & 8) {
            break;
        }
    }
    while (WaitForSingleObject(clientevent, 0) == WAIT_TIMEOUT);

    reader.StopAsync().get();
    AcquireSRWLockExclusive(&g_lock);
    
    g_pSinkWriter->Flush(g_dwVideoIndex);
    g_pSinkWriter->Release();
    pSink->Shutdown();
    pSink->Release();

    return 0;
}

// OK
static void PV_Intrinsics(SOCKET clientsocket, HANDLE clientevent, MediaFrameReader const& reader)
{
    WSABUF wsaBuf[1];

    g_event_intrinsic = clientevent;

    reader.FrameArrived(PV_OnVideoFrameArrived_Intrinsics);

    ReleaseSRWLockExclusive(&g_lock);
    reader.StartAsync().get();
    WaitForSingleObject(clientevent, INFINITE);
    reader.StopAsync().get();
    AcquireSRWLockExclusive(&g_lock);

    pack_buffer(wsaBuf, 0, &g_calibration, sizeof(g_calibration));

    send_multiple(clientsocket, wsaBuf, sizeof(wsaBuf) / sizeof(WSABUF));
}

// OK
static int PV_Stream(SOCKET clientsocket)
{
    HANDLE clientevent; // CloseHandle
    MRCVideoOptions options;
    H26xFormat format;
    uint8_t mode;    
    bool ok;

    ok = recv_u8(clientsocket, mode);
    if (!ok) {
        return WSAGetLastError();
    }

    ok = ReceiveH26xFormat_Video(clientsocket, format);
    if (!ok) {
        return WSAGetLastError();
    }

    if (mode & 4)
    {
        ok = ReceiveMRCVideoOptions(clientsocket, options);
        if (!ok) {
            return WSAGetLastError();
        }
        if (PersonalVideo_Status()) { PersonalVideo_Close(); }
        PersonalVideo_Open(options);
    }

    if (!PersonalVideo_Status()) {
        // not a socket issue
        return -1;
    }

    ok = PersonalVideo_SetFormat(format.width, format.height, format.framerate);
    if (!ok) { return -1; }
    
    clientevent = CreateEvent(NULL, TRUE, FALSE, NULL);

    PersonalVideo_RegisterEvent(clientevent);
    auto const& videoFrameReader = PersonalVideo_CreateFrameReader();
    // videoFrameReader.AcquisitionMode(MediaFrameReaderAcquisitionMode::Buffered);
    videoFrameReader.AcquisitionMode(MediaFrameReaderAcquisitionMode::Realtime);

    switch (mode & 3)
    {
        case 0: PV_Stream<false>(clientsocket, clientevent, videoFrameReader, format, mode); break;
        case 1: PV_Stream<true>(clientsocket, clientevent, videoFrameReader, format, mode); break;
        case 2: PV_Intrinsics(clientsocket, clientevent, videoFrameReader);         break;
    }

    videoFrameReader.Close();
    PersonalVideo_RegisterEvent(NULL);

    CloseHandle(clientevent);

    if (mode & 8) {
        UnityShowMessage("Closing PersonalVideo");
        PersonalVideo_Close();
    }

    return 0;
}

// OK
static DWORD WINAPI PV_EntryPoint(void *param)
{
    (void)param;

    SOCKET listensocket; // closesocket
    SOCKET clientsocket; // closesocket
    int base_priority;

    listensocket = CreateSocket(PORT_NAME_PV);

    AcquireSRWLockExclusive(&g_lock);

    base_priority = GetThreadPriority(GetCurrentThread());

    do
    {
        clientsocket = accept(listensocket, NULL, NULL); // block
        if (clientsocket == INVALID_SOCKET) {
            break;
        }

        SetThreadPriority(GetCurrentThread(), ExtendedExecution_GetInterfacePriority(PORT_NUMBER_PV - PORT_NUMBER_BASE));

        PV_Stream(clientsocket);

        SetThreadPriority(GetCurrentThread(), base_priority);

        closesocket(clientsocket);
    } 
    while (WaitForSingleObject(g_event_quit, 0) == WAIT_TIMEOUT);

    closesocket(listensocket);

    return 0;
}

// OK
void PV_Initialize()
{
    InitializeSRWLock(&g_lock);
    g_event_quit = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, PV_EntryPoint, NULL, 0, NULL);
}

// OK
void PV_Quit()
{
    SetEvent(g_event_quit);
}

// OK
void PV_Cleanup()
{
    WaitForSingleObject(g_thread, INFINITE);

    CloseHandle(g_thread);
    CloseHandle(g_event_quit);
    
    g_thread = NULL;
    g_event_quit = NULL;
}
