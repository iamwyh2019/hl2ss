
#include <mfapi.h>
#include "custom_media_sink.h"
#include "custom_media_buffers.h"
#include "personal_video.h"
#include "locator.h"
#include "log.h"
#include "ports.h"
#include "timestamps.h"
#include "ipc_sc.h"
#include "research_mode.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.Devices.Core.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Perception.Spatial.h>

using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Capture::Frames;
using namespace winrt::Windows::Media::Devices::Core;
using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::Perception::Spatial;

struct PV_Projection
{
    float2 f;
    float2 c;
    float4x4 pose;
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
static float g_intrinsics[2 + 2 + 3 + 2 + 16];
static float4x4 g_extrinsics;

//-----------------------------------------------------------------------------
// Functions
//-----------------------------------------------------------------------------

// OK
template<bool ENABLE_LOCATION>
void PV_OnVideoFrameArrived(MediaFrameReader const& sender, MediaFrameArrivedEventArgs const& args)
{
    (void)args;
    
    CameraIntrinsics intrinsics = nullptr;
    MediaFrameReference frame = nullptr;
    IMFSample* pSample; // Release
    SoftwareBitmapBuffer* pBuffer; // Release
    PV_Projection pj;
    int64_t timestamp;

    if (TryAcquireSRWLockShared(&g_lock) != 0)
    {
    frame = sender.TryAcquireLatestFrame();
    if (frame) 
    {
    if (g_counter == 0)
    {
    SoftwareBitmapBuffer::CreateInstance(&pBuffer, frame);

    MFCreateSample(&pSample);

    timestamp = frame.SystemRelativeTime().Value().count();

    pSample->AddBuffer(pBuffer);
    pSample->SetSampleDuration(frame.Duration().count());
    pSample->SetSampleTime(timestamp);

    intrinsics = frame.VideoMediaFrame().CameraIntrinsics();

    pj.f = intrinsics.FocalLength();
    pj.c = intrinsics.PrincipalPoint();

    if constexpr (ENABLE_LOCATION)
    {
    pj.pose = Locator_GetTransformTo(frame.CoordinateSystem(), Locator_GetWorldCoordinateSystem(QPCTimestampToPerceptionTimestamp(timestamp)));
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

    CameraIntrinsics intrinsics = nullptr;
    MediaFrameReference frame = nullptr;
    float2 f;
    float2 c;
    float3 r;
    float2 t;
    float4x4 p;
    DWORD status;

    frame = sender.TryAcquireLatestFrame();
    if (!frame) { return; }

    status = WaitForSingleObject(g_event_intrinsic, 0);
    if (status != WAIT_TIMEOUT) { return; }

    intrinsics = frame.VideoMediaFrame().CameraIntrinsics();

    f = intrinsics.FocalLength();
    c = intrinsics.PrincipalPoint();
    r = intrinsics.RadialDistortion();
    t = intrinsics.TangentialDistortion();
    p = intrinsics.UndistortedProjectionTransform();

    memcpy(&g_intrinsics[0], &f, sizeof(f));
    memcpy(&g_intrinsics[2], &c, sizeof(c));
    memcpy(&g_intrinsics[4], &r, sizeof(r));
    memcpy(&g_intrinsics[7], &t, sizeof(t));
    memcpy(&g_intrinsics[9], &p, sizeof(p));

    g_extrinsics = Locator_Locate(QPCTimestampToPerceptionTimestamp(frame.SystemRelativeTime().Value().count()), ResearchMode_GetLocator(), frame.CoordinateSystem());

    SetEvent(g_event_intrinsic);
}

// OK
template<bool ENABLE_LOCATION>
void PV_SendSample(IMFSample* pSample, void* param)
{
    IMFMediaBuffer* pBuffer; // Release
    LONGLONG sampletime;
    BYTE* pBytes;
    DWORD cbData;
    DWORD cbDataEx;
    WSABUF wsaBuf[ENABLE_LOCATION ? 6 : 5];
    HookCallbackSocket* user;
    H26xFormat* format;
    bool sh;
    bool ok;

    user = (HookCallbackSocket*)param;
    format = (H26xFormat*)user->format;
    sh = format->profile != H26xProfile::H26xProfile_None;

    pSample->GetSampleTime(&sampletime);
    pSample->ConvertToContiguousBuffer(&pBuffer);
    
    if (!sh) { pSample->GetBlob(MF_USER_DATA_PAYLOAD, (UINT8*)&g_pvp_sh, sizeof(g_pvp_sh), NULL); }

    pBuffer->Lock(&pBytes, NULL, &cbData);

    cbDataEx = cbData + sizeof(g_pvp_sh.f) + sizeof(g_pvp_sh.c);

    pack_buffer(wsaBuf, 0, &sampletime, sizeof(sampletime));
    pack_buffer(wsaBuf, 1, &cbDataEx, sizeof(cbDataEx));
    pack_buffer(wsaBuf, 2, pBytes, cbData);
    pack_buffer(wsaBuf, 3, &g_pvp_sh.f, sizeof(g_pvp_sh.f));
    pack_buffer(wsaBuf, 4, &g_pvp_sh.c, sizeof(g_pvp_sh.c));

    if constexpr(ENABLE_LOCATION)
    {
    pack_buffer(wsaBuf, 5, &g_pvp_sh.pose, sizeof(g_pvp_sh.pose));
    }
    
    ok = send_multiple(user->clientsocket, wsaBuf, sizeof(wsaBuf) / sizeof(WSABUF));
    if (!ok) { SetEvent(user->clientevent); }

    pBuffer->Unlock();
    pBuffer->Release();

    if (sh) { pSample->GetBlob(MF_USER_DATA_PAYLOAD, (UINT8*)&g_pvp_sh, sizeof(g_pvp_sh), NULL); }
}

// OK
template<bool ENABLE_LOCATION>
void PV_Stream(SOCKET clientsocket, HANDLE clientevent, MediaFrameReader const& reader, H26xFormat& format)
{
    CustomMediaSink* pSink; // Release
    std::vector<uint64_t> options;
    HookCallbackSocket user;
    bool ok;

    ok = ReceiveH26xFormat_Divisor(clientsocket, format);
    if (!ok) { return; }

    ok = ReceiveH26xFormat_Profile(clientsocket, format);
    if (!ok) { return; }

    ok = ReceiveH26xEncoder_Options(clientsocket, options);
    if (!ok) { return; }

    user.clientsocket = clientsocket;
    user.clientevent  = clientevent;
    user.format       = &format;

    CreateSinkWriterVideo(&pSink, &g_pSinkWriter, &g_dwVideoIndex, VideoSubtype::VideoSubtype_NV12, format, options, PV_SendSample<ENABLE_LOCATION>, &user);

    reader.FrameArrived(PV_OnVideoFrameArrived<ENABLE_LOCATION>);

    g_counter = 0;
    g_divisor = format.divisor;    
    memset(&g_pvp_sh, 0, sizeof(g_pvp_sh));

    ReleaseSRWLockExclusive(&g_lock);
    reader.StartAsync().get();
    WaitForSingleObject(clientevent, INFINITE);
    reader.StopAsync().get();
    AcquireSRWLockExclusive(&g_lock);
    
    g_pSinkWriter->Flush(g_dwVideoIndex);
    g_pSinkWriter->Release();
    pSink->Shutdown();
    pSink->Release();
}

// OK
static void PV_Intrinsics(SOCKET clientsocket, HANDLE clientevent, MediaFrameReader const& reader)
{
    WSABUF wsaBuf[2];

    g_event_intrinsic = clientevent;

    reader.FrameArrived(PV_OnVideoFrameArrived_Intrinsics);

    reader.StartAsync().get();
    WaitForSingleObject(g_event_intrinsic, INFINITE);
    reader.StopAsync().get();

    pack_buffer(wsaBuf, 0,  g_intrinsics, sizeof(g_intrinsics));
    pack_buffer(wsaBuf, 1, &g_extrinsics, sizeof(g_extrinsics));

    send_multiple(clientsocket, wsaBuf, sizeof(wsaBuf) / sizeof(WSABUF));
}

// OK
static void PV_Stream(SOCKET clientsocket)
{
    MediaFrameReader videoFrameReader = nullptr;
    HANDLE clientevent; // CloseHandle
    H26xFormat format;
    uint8_t mode;    
    bool ok;

    ok = recv_u8(clientsocket, mode);
    if (!ok) { return; }

    ok = ReceiveH26xFormat_Video(clientsocket, format);
    if (!ok) { return; }

    if (mode & 4)
    {
    MRCVideoOptions options;
    ok = ReceiveMRCVideoOptions(clientsocket, options);
    if (!ok) { return; }
    if (PersonalVideo_Status()) { PersonalVideo_Close(); }
    PersonalVideo_Open(options);
    }

    if (!PersonalVideo_Status()) { return; }

    ok = PersonalVideo_SetFormat(format.width, format.height, format.framerate);
    if (!ok) { return; }
    
    clientevent = CreateEvent(NULL, TRUE, FALSE, NULL);

    PersonalVideo_RegisterEvent(clientevent);
    videoFrameReader = PersonalVideo_CreateFrameReader();
    videoFrameReader.AcquisitionMode(MediaFrameReaderAcquisitionMode::Buffered);
    
    switch (mode & 3)
    {
    case 0: PV_Stream<false>(clientsocket, clientevent, videoFrameReader, format); break;
    case 1: PV_Stream<true>( clientsocket, clientevent, videoFrameReader, format); break;
    case 2: PV_Intrinsics(   clientsocket, clientevent, videoFrameReader);         break;
    }

    videoFrameReader.Close();
    PersonalVideo_RegisterEvent(NULL);

    CloseHandle(clientevent);

    if (mode & 8) { PersonalVideo_Close(); }
}

// OK
static DWORD WINAPI PV_EntryPoint(void *param)
{
    (void)param;

    SOCKET listensocket; // closesocket
    SOCKET clientsocket; // closesocket

    listensocket = CreateSocket(PORT_NAME_PV);

    ShowMessage("PV: Listening at port %s", PORT_NAME_PV);

    AcquireSRWLockExclusive(&g_lock);

    do
    {
    ShowMessage("PV: Waiting for client");

    clientsocket = accept(listensocket, NULL, NULL); // block
    if (clientsocket == INVALID_SOCKET) { break; }

    ShowMessage("PV: Client connected");

    PV_Stream(clientsocket);

    closesocket(clientsocket);

    ShowMessage("PV: Client disconnected");
    } 
    while (WaitForSingleObject(g_event_quit, 0) == WAIT_TIMEOUT);

    closesocket(listensocket);

    ShowMessage("PV: Closed");

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
