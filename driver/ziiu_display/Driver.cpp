// SPDX-License-Identifier: MS-PL
// Copyright (c) Microsoft Corporation
// Modifications copyright (c) 2026 pancreations
// See LICENSE in this directory.
/*++

ZiiUScreen virtual monitor (IddCx, UMDF). See Driver.h.

Based on Microsoft's IddCx sample driver (Windows-driver-samples
video/IndirectDisplay/IddSampleDriver, Copyright (c) Microsoft Corporation,
Microsoft Public License).

--*/

#include "Driver.h"

#include <sddl.h>

using namespace std;
using namespace ZiiU;
using namespace Microsoft::WRL;

#pragma region Monitor

// The GamePad's panel is 854x480 (the H.264 stream is 864x480 with the SPS cropping
// it), 59.94 Hz like the video stream, about 137 x 77 mm (6.2 in, 16:9). Windows is
// cramped at that size, so larger 16:9 modes are offered too; every frame is scaled
// to 854x480 on the GPU before it leaves the driver. 1920x1080 is the default (owner's choice).
static constexpr DWORD kWidth = 854;      // GamePad panel (output size)
static constexpr DWORD kHeight = 480;
static constexpr DWORD kRefreshNum = 60000;
static constexpr DWORD kRefreshDen = 1001;
struct ModeSize { DWORD Width, Height; };
static const ModeSize kModes[] = { { 1920, 1080 }, { 1280, 720 }, { 1708, 960 }, { 854, 480 } };
static constexpr DWORD kModeCount = ARRAYSIZE(kModes);

// Stable container id: Windows remembers this monitor's arrangement and scaling.
// {6b1f9a3e-2c4d-4f7a-9b21-5d7e0c8a4f13}
static const GUID kMonitorContainerId =
    { 0x6b1f9a3e, 0x2c4d, 0x4f7a, { 0x9b, 0x21, 0x5d, 0x7e, 0x0c, 0x8a, 0x4f, 0x13 } };

static constexpr size_t kEdidSize = 128;

// EDID 1.4 built at runtime so the checksum and timing fields stay consistent.
static const BYTE* MonitorEdid()
{
    static BYTE e[kEdidSize];
    static bool built = false;
    if (built) return e;

    static const BYTE header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    memcpy(e, header, 8);
    // Manufacturer "ZUS" (local, unregistered), product 0x5550, serial 1, week 42 / 2026.
    const WORD mfg = (WORD)((('Z' - '@') << 10) | (('U' - '@') << 5) | ('S' - '@'));
    e[8] = (BYTE)(mfg >> 8); e[9] = (BYTE)(mfg & 0xff);
    e[10] = 0x50; e[11] = 0x55;
    e[12] = 1;
    e[16] = 42; e[17] = 2026 - 1990;
    e[18] = 1; e[19] = 4;                     // EDID 1.4
    e[20] = 0x80;                             // digital input
    e[21] = 14; e[22] = 8;                    // cm
    e[23] = 120;                              // gamma 2.2
    e[24] = 0x06;                             // sRGB default, preferred timing in DTD 1
    static const BYTE srgb[10] = { 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54 };
    memcpy(e + 25, srgb, 10);
    for (int i = 38; i < 54; i++) e[i] = 0x01; // no standard timings

    // DTD 1 (preferred): 1920x1080, blanking 160/22 -> 2080x1102, 59.94 Hz -> 137.4 MHz.
    BYTE* d = e + 54;
    const DWORD w = kModes[0].Width, h = kModes[0].Height;
    const DWORD hBlank = 160, vBlank = 22, hFront = 48, hSync = 32, vFront = 3, vSync = 5;
    const DWORD clock10k = (DWORD)((UINT64)(w + hBlank) * (h + vBlank) * kRefreshNum / kRefreshDen / 10000);
    d[0] = (BYTE)clock10k; d[1] = (BYTE)(clock10k >> 8);
    d[2] = (BYTE)w; d[3] = (BYTE)hBlank; d[4] = (BYTE)(((w >> 8) << 4) | (hBlank >> 8));
    d[5] = (BYTE)h; d[6] = (BYTE)vBlank; d[7] = (BYTE)(((h >> 8) << 4) | (vBlank >> 8));
    d[8] = (BYTE)hFront; d[9] = (BYTE)hSync; d[10] = (BYTE)((vFront << 4) | vSync); d[11] = 0;
    d[12] = 137; d[13] = 77; d[14] = 0;      // mm
    d[17] = 0x1e;                            // digital separate sync, +h +v

    // Descriptor 2: monitor name (exactly 13 characters, so no terminator).
    BYTE* n = e + 72;
    n[3] = 0xfc;
    memcpy(n + 5, "Wii U GamePad", 13);
    // Descriptor 3: range limits 50-75 Hz, 20-80 kHz, 150 MHz (covers 1920x1080).
    BYTE* r = e + 90;
    r[3] = 0xfd;
    r[5] = 50; r[6] = 75; r[7] = 20; r[8] = 80; r[9] = 15; r[10] = 0;
    r[11] = 0x0a; for (int i = 12; i < 18; i++) r[i] = 0x20;
    // Descriptor 4: serial string.
    BYTE* s = e + 108;
    s[3] = 0xff;
    memcpy(s + 5, "ZIIUSCREEN01\n", 13);

    BYTE sum = 0;
    for (size_t i = 0; i < kEdidSize - 1; i++) sum = (BYTE)(sum + e[i]);
    e[127] = (BYTE)(0x100 - sum);
    built = true;
    return e;
}

static void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;
    Mode.vSyncFreq.Numerator = kRefreshNum;
    Mode.vSyncFreq.Denominator = kRefreshDen;
    Mode.hSyncFreq.Numerator = kRefreshNum * Height;
    Mode.hSyncFreq.Denominator = kRefreshDen;
    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    Mode.pixelRate = (UINT64)kRefreshNum * Width * Height / kRefreshDen;
}

static IDDCX_MONITOR_MODE MonitorMode(DWORD Index, IDDCX_MONITOR_MODE_ORIGIN Origin)
{
    IDDCX_MONITOR_MODE Mode = {};
    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, kModes[Index].Width, kModes[Index].Height, true);
    return Mode;
}

static IDDCX_TARGET_MODE TargetMode(DWORD Index)
{
    IDDCX_TARGET_MODE Mode = {};
    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, kModes[Index].Width, kModes[Index].Height, false);
    return Mode;
}

#pragma endregion

#pragma region Diagnostics

// Progress of each stage, so ziiu-monitor can show where a start stops: a line in
// C:\\ProgramData\\ZiiUScreen\\display.log and a value in the device's writable UMDF
// subkey (the hardware key itself is read-only for UMDF). Written on state changes
// only, never per frame.
static WDFDEVICE g_Device;

static void Diag(PCWSTR Name, ULONG Value)
{
    WCHAR line[160];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = swprintf_s(line, L"%02u:%02u:%02u.%03u %s=0x%lx\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       Name, Value);
    CreateDirectoryW(L"C:\\ProgramData\\ZiiUScreen", nullptr);
    HANDLE f = CreateFileW(L"C:\\ProgramData\\ZiiUScreen\\display.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char a[160];
        int m = WideCharToMultiByte(CP_UTF8, 0, line, n, a, sizeof(a), nullptr, nullptr);
        DWORD w = 0;
        if (m > 0) WriteFile(f, a, (DWORD)m, &w, nullptr);
        CloseHandle(f);
    }
    if (!g_Device) return;
    WDFKEY key;
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(g_Device, PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY,
                                             KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) return;
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, Name);
    WdfRegistryAssignULong(key, &name, Value);
    WdfRegistryClose(key);
}

static void DiagCount(PCWSTR Name)
{
    static volatile LONG counts[8];
    static PCWSTR names[8];
    for (int i = 0; i < 8; i++) {
        if (!names[i]) names[i] = Name;
        if (names[i] == Name) {
            Diag(Name, (ULONG)InterlockedIncrement(&counts[i]));
            return;
        }
    }
}

#pragma endregion

#pragma region FrameSink

static FrameSink g_Sink;

static HANDLE CreateWithSddl(PCWSTR Sddl, bool Section)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, SDDL_REVISION_1, &sd, nullptr)) return nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    HANDLE h = Section
        ? CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, ZIIU_FRAME_SECTION_BYTES, ZIIU_FRAME_SECTION)
        : CreateEventW(&sa, FALSE, FALSE, ZIIU_FRAME_EVENT);
    LocalFree(sd);
    return h;
}

bool FrameSink::Open()
{
    if (m_Header) return true;
    // SYSTEM, Administrators and LocalService (this host) full access; interactive
    // users may read frames and wait on the event.
    m_Section = CreateWithSddl(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GR;;;IU)", true);
    m_Event = CreateWithSddl(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;0x00100001;;;IU)", false);
    if (!m_Section || !m_Event) {
        Diag(L"SectionError", GetLastError() ? GetLastError() : 1);
        if (m_Section) CloseHandle(m_Section);
        if (m_Event) CloseHandle(m_Event);
        m_Section = m_Event = nullptr;
        return false;
    }
    m_Base = (BYTE*)MapViewOfFile(m_Section, FILE_MAP_ALL_ACCESS, 0, 0, ZIIU_FRAME_SECTION_BYTES);
    if (!m_Base) {
        Diag(L"SectionError", GetLastError());
        return false;
    }
    Diag(L"SectionError", 0);
    m_Header = (ZIIU_FRAME_HEADER*)m_Base;
    memset(m_Header, 0, sizeof(*m_Header));
    m_Header->Slots = ZIIU_FRAME_SLOTS;
    m_Header->SlotBytes = ZIIU_FRAME_SLOT_BYTES;
    m_Header->DataOffset = ZIIU_FRAME_DATA_OFFSET;
    m_Header->Version = ZIIU_FRAME_VERSION;
    MemoryBarrier();
    m_Header->Magic = ZIIU_FRAME_MAGIC;
    return true;
}

void FrameSink::CountDropped()
{
    if (m_Header) InterlockedIncrement64(&m_Header->Dropped);
}

void FrameSink::Publish(ID3D11DeviceContext* Context, ID3D11Texture2D* Staging, UINT Width, UINT Height,
                        UINT DirtyRects, UINT64 PresentFrame, LONGLONG QpcAcquired)
{
    if (!Open()) return;
    if (Width > ZIIU_FRAME_MAX_WIDTH || Height > ZIIU_FRAME_MAX_HEIGHT) {
        CountDropped();
        return;
    }
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(Context->Map(Staging, 0, D3D11_MAP_READ, 0, &map))) {
        CountDropped();
        return;
    }
    LONGLONG seq = ++m_Seq;
    UINT slot = (UINT)(seq % ZIIU_FRAME_SLOTS);
    ZIIU_FRAME_SLOT_INFO& info = m_Header->Slot[slot];
    InterlockedExchange64(&info.Seq, 0);       // readers of this slot must not accept it now
    BYTE* dst = m_Base + ZIIU_FRAME_DATA_OFFSET + (SIZE_T)slot * ZIIU_FRAME_SLOT_BYTES;
    const BYTE* src = (const BYTE*)map.pData;
    for (UINT y = 0; y < Height; y++) memcpy(dst + (SIZE_T)y * Width * 4, src + (SIZE_T)y * map.RowPitch, Width * 4);
    Context->Unmap(Staging, 0);
    info.Width = Width;
    info.Height = Height;
    info.Stride = Width * 4;
    info.DirtyRects = DirtyRects;
    info.QpcAcquired = QpcAcquired;
    info.PresentFrame = PresentFrame;
    InterlockedExchange64(&info.Seq, seq);
    InterlockedExchange64(&m_Header->Latest, seq);
    SetEvent(m_Event);
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD ZiiuDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY ZiiuDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED ZiiuAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES ZiiuAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION ZiiuParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES ZiiuMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES ZiiuMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN ZiiuMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN ZiiuMonitorUnassignSwapChain;

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;
    void Cleanup() { delete pContext; pContext = nullptr; }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;
    void Cleanup() { delete pContext; pContext = nullptr; }
};

WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE hInstance, _In_ UINT dwReason, _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);
    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    WDF_DRIVER_CONFIG_INIT(&Config, ZiiuDeviceAdd);
    return WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS ZiiuDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = ZiiuDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);
    IddConfig.EvtIddCxAdapterInitFinished = ZiiuAdapterInitFinished;
    IddConfig.EvtIddCxParseMonitorDescription = ZiiuParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = ZiiuMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = ZiiuMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = ZiiuAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = ZiiuMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = ZiiuMonitorUnassignSwapChain;

    NTSTATUS Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status)) return Status;

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext) pContext->Cleanup();
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status)) return Status;
    g_Device = Device;
    Diag(L"Stage", 1);          // device added

    Status = IddCxDeviceInitialize(Device);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);
    return Status;
}

_Use_decl_annotations_
NTSTATUS ZiiuDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();
    return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

HRESULT Direct3DDevice::Init()
{
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr)) return hr;
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr)) return hr;
    return D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                             D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
    : m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    SetEvent(m_hTerminateEvent.Get());
    if (m_hThread.Get()) WaitForSingleObject(m_hThread.Get(), INFINITE);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);
    RunCore();
    // Deleting the swap-chain makes the OS provide a new one if needed.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;
    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

// Scale the frame to the GamePad's 854x480 with the D3D11 video processor (the GPU's
// video scaler) unless it already is that size. Returns the texture to stage.
ID3D11Texture2D* SwapChainProcessor::Scale(ID3D11Texture2D* Source, const D3D11_TEXTURE2D_DESC& Desc)
{
    if (Desc.Width == kWidth && Desc.Height == kHeight) return Source;
    if (!m_Processor || Desc.Width != m_ScaleFromW || Desc.Height != m_ScaleFromH) {
        m_Processor.Reset();
        m_Enum.Reset();
        m_OutView.Reset();
        if (!m_VideoDevice && (FAILED(m_Device->Device.As(&m_VideoDevice)) ||
                               FAILED(m_Device->DeviceContext.As(&m_VideoContext)))) return nullptr;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = { kRefreshNum, kRefreshDen };
        cd.InputWidth = Desc.Width;
        cd.InputHeight = Desc.Height;
        cd.OutputFrameRate = { kRefreshNum, kRefreshDen };
        cd.OutputWidth = kWidth;
        cd.OutputHeight = kHeight;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
        if (FAILED(m_VideoDevice->CreateVideoProcessorEnumerator(&cd, &m_Enum)) ||
            FAILED(m_VideoDevice->CreateVideoProcessor(m_Enum.Get(), 0, &m_Processor))) return nullptr;
        if (!m_Scaled) {
            D3D11_TEXTURE2D_DESC od = {};
            od.Width = kWidth;
            od.Height = kHeight;
            od.MipLevels = 1;
            od.ArraySize = 1;
            od.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            od.SampleDesc.Count = 1;
            od.Usage = D3D11_USAGE_DEFAULT;
            od.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(m_Device->Device->CreateTexture2D(&od, nullptr, &m_Scaled))) return nullptr;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = {};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(m_VideoDevice->CreateVideoProcessorOutputView(m_Scaled.Get(), m_Enum.Get(), &ov, &m_OutView)))
            return nullptr;
        RECT dst = { 0, 0, (LONG)kWidth, (LONG)kHeight };
        m_VideoContext->VideoProcessorSetStreamFrameFormat(m_Processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_VideoContext->VideoProcessorSetOutputTargetRect(m_Processor.Get(), TRUE, &dst);
        m_VideoContext->VideoProcessorSetStreamDestRect(m_Processor.Get(), 0, TRUE, &dst);
        m_VideoContext->VideoProcessorSetStreamAutoProcessingMode(m_Processor.Get(), 0, FALSE);
        m_ScaleFromW = Desc.Width;
        m_ScaleFromH = Desc.Height;
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> in;
    if (FAILED(m_VideoDevice->CreateVideoProcessorInputView(Source, m_Enum.Get(), &iv, &in))) return nullptr;
    RECT src = { 0, 0, (LONG)Desc.Width, (LONG)Desc.Height };
    m_VideoContext->VideoProcessorSetStreamSourceRect(m_Processor.Get(), 0, TRUE, &src);
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = in.Get();
    if (FAILED(m_VideoContext->VideoProcessorBlt(m_Processor.Get(), m_OutView.Get(), 0, 1, &stream))) return nullptr;
    return m_Scaled.Get();
}

// Scale on the GPU, copy into a CPU-readable staging texture, then into the section.
void SwapChainProcessor::CopyFrame(IDXGIResource* Surface, const IDARG_OUT_RELEASEANDACQUIREBUFFER& Buffer)
{
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(Surface->QueryInterface(IID_PPV_ARGS(&tex)))) {
        g_Sink.CountDropped();
        return;
    }
    D3D11_TEXTURE2D_DESC src;
    tex->GetDesc(&src);
    ID3D11Texture2D* frame = Scale(tex.Get(), src);
    if (!frame) {
        g_Sink.CountDropped();
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    frame->GetDesc(&desc);
    if (!m_Staging || desc.Width != m_StagingDesc.Width || desc.Height != m_StagingDesc.Height ||
        desc.Format != m_StagingDesc.Format) {
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        m_Staging.Reset();
        if (FAILED(m_Device->Device->CreateTexture2D(&sd, nullptr, &m_Staging))) {
            g_Sink.CountDropped();
            return;
        }
        m_StagingDesc = desc;
    }
    m_Device->DeviceContext->CopyResource(m_Staging.Get(), frame);
    g_Sink.Publish(m_Device->DeviceContext.Get(), m_Staging.Get(), desc.Width, desc.Height,
                   Buffer.MetaData.DirtyRectCount, Buffer.MetaData.PresentationFrameNumber, qpc.QuadPart);
}

void SwapChainProcessor::RunCore()
{
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr)) return;

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();
    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    Diag(L"SetDeviceHr", (ULONG)hr);
    if (FAILED(hr)) return;

    g_Sink.Open();
    bool first = true;
    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        if (hr == E_PENDING)
        {
            HANDLE WaitHandles[] = { m_hAvailableBufferEvent, m_hTerminateEvent.Get() };
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT) continue;
            break;   // terminate, or an unexpected wait result
        }
        else if (SUCCEEDED(hr))
        {
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);
            if (first) {
                Diag(L"FirstFrame", 1);
                first = false;
            }
            CopyFrame(AcquiredBuffer.Get(), Buffer);
            AcquiredBuffer.Reset();
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr)) break;
        }
        else
        {
            Diag(L"AcquireExitHr", (ULONG)hr);
            break;   // swap-chain abandoned (e.g. DXGI_ERROR_ACCESS_LOST)
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) : m_WdfDevice(WdfDevice)
{
    m_Adapter = {};
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);
    AdapterCaps.MaxMonitorsSupported = 1;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    // WIRED_OTHER as in Microsoft's sample and the Virtual Display Driver: with WIRELESS_OTHER
    // Windows kept the monitor connected but never activated it (not even from Settings).
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"ZiiUScreen GamePad display";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"ZiiUScreen";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"Wii U GamePad (virtual monitor)";

    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);
    Diag(L"AdapterInitStatus", (ULONG)Status);
    if (NT_SUCCESS(Status))
    {
        m_Adapter = AdapterInitOut.AdapterObject;
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
}

void IndirectDeviceContext::FinishInit()
{
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = 0;
    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = kEdidSize;
    MonitorInfo.MonitorDescription.pData = const_cast<BYTE*>(MonitorEdid());
    MonitorInfo.MonitorContainerId = kMonitorContainerId;

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (NT_SUCCESS(Status))
    {
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
        pMonitorContextWrapper->pContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject);
        IDARG_OUT_MONITORARRIVAL ArrivalOut;
        Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
    }
    Diag(L"MonitorArrivalStatus", (ULONG)Status);
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor) : m_Monitor(Monitor)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();
    DiagCount(L"SwapChainsAssigned");
    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    HRESULT hr = Device->Init();
    Diag(L"D3DInitHr", (ULONG)hr);
    if (FAILED(hr))
    {
        WdfObjectDelete(SwapChain);   // the OS retries with a new swap-chain
    }
    else
    {
        m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    DiagCount(L"SwapChainsUnassigned");
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS ZiiuAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    Diag(L"AdapterInitFinished", (ULONG)pInArgs->AdapterInitStatus);
    if (NT_SUCCESS(pInArgs->AdapterInitStatus)) pDeviceContextWrapper->pContext->FinishInit();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ZiiuAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    ULONG active = 0;
    for (UINT i = 0; i < pInArgs->PathCount; i++)
        if (pInArgs->pPaths[i].Flags & IDDCX_PATH_FLAGS_ACTIVE) active++;
    Diag(L"CommitModesActivePaths", active);
    return STATUS_SUCCESS;   // the swap-chain is managed by IddCx
}

_Use_decl_annotations_
NTSTATUS ZiiuParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    Diag(L"ParseMonitorDescription", pInArgs->MonitorModeBufferInputCount);
    pOutArgs->MonitorModeBufferOutputCount = kModeCount;
    if (pInArgs->MonitorModeBufferInputCount < kModeCount)
        return pInArgs->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    if (pInArgs->MonitorDescription.DataSize != kEdidSize ||
        memcmp(pInArgs->MonitorDescription.pData, MonitorEdid(), kEdidSize) != 0)
        return STATUS_INVALID_PARAMETER;
    for (DWORD i = 0; i < kModeCount; i++)
        pInArgs->pMonitorModes[i] = MonitorMode(i, IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ZiiuMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
                                    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    pOutArgs->DefaultMonitorModeBufferOutputCount = kModeCount;
    if (pInArgs->DefaultMonitorModeBufferInputCount >= kModeCount)
    {
        for (DWORD i = 0; i < kModeCount; i++)
            pInArgs->pDefaultMonitorModes[i] = MonitorMode(i, IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
        pOutArgs->PreferredMonitorModeIdx = 0;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ZiiuMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    Diag(L"QueryTargetModes", pInArgs->TargetModeBufferInputCount);
    UNREFERENCED_PARAMETER(MonitorObject);
    pOutArgs->TargetModeBufferOutputCount = kModeCount;
    if (pInArgs->TargetModeBufferInputCount >= kModeCount)
        for (DWORD i = 0; i < kModeCount; i++) pInArgs->pTargetModes[i] = TargetMode(i);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ZiiuMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ZiiuMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion
