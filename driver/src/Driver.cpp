/*++

TwinScreen - virtual display driver (IddCx).

Presents one (or more) virtual monitors to Windows. Real frame content is
captured in user mode by twinhost via Desktop Duplication on the virtual
monitor, so this driver never renders and never touches the network - it
simply consumes (ACKs and drops) every swap-chain frame to keep the OS
presenter happy.

Architecture and API usage are grounded against the MIT-licensed
VirtualDrivers/Virtual-Display-Driver (itsmikethetech/IddSampleDriver fork)
which is a derivative of the Microsoft IddSampleDriver.

Environment: User Mode, UMDF2, IddCx.

--*/

#include "Driver.h"

#include <string>
#include <vector>
#include <tuple>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdlib>

using namespace TwinScreen::IndirectDisp;
using namespace Microsoft::WRL;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD VirtualDisplayDriverDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY VirtualDisplayDriverDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED VirtualDisplayDriverAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES VirtualDisplayDriverAdapterCommitModes;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES VirtualDisplayDriverMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES VirtualDisplayDriverMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN VirtualDisplayDriverMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN VirtualDisplayDriverMonitorUnassignSwapChain;

// ---------------------------------------------------------------------------
// WDF context types
// ---------------------------------------------------------------------------

// Declared before any use so WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE and
// WdfObjectGet_IndirectDeviceContextWrapper compile (WDF_DECLARE_CONTEXT_TYPE
// emits the _ContextTypeInfo symbol this file references).
struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;
};
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper)

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

namespace
{
    const wchar_t* k_OptionsPath = L"C:\\ProgramData\\TwinScreen\\option.txt";
    const UINT k_DefaultDisplayCount = 1;

    // Verbatim EDID used by VirtualDrivers/Virtual-Display-Driver (MIT):
    // a 128-byte base block (1920x1080 DTD, range limits, display name) plus a
    // CEA-861 extension block advertising additional video modes. The full
    // selectable mode list lives in option.txt and is served via
    // EvtIddCxMonitorQueryTargetModes; the EDID is primarily for monitor
    // identification. Checksum byte [127] is recomputed at load time.
    const BYTE k_TwinScreenEdid[256] =
    {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x36, 0x94, 0x37, 0x13, 0xe7, 0x1e, 0xe7, 0x1e,
        0x1c, 0x22, 0x01, 0x03, 0x80, 0x32, 0x1f, 0x78, 0x07, 0xee, 0x95, 0xa3, 0x54, 0x4c, 0x99, 0x26,
        0x0f, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
        0x45, 0x00, 0x63, 0xc8, 0x10, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x17, 0xf0, 0x0f,
        0xff, 0x37, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc,
        0x00, 0x56, 0x44, 0x44, 0x20, 0x62, 0x79, 0x20, 0x4d, 0x54, 0x54, 0x0a, 0x20, 0x20, 0x01, 0xc2,
        0x02, 0x03, 0x20, 0x40, 0xe6, 0x06, 0x0d, 0x01, 0xa2, 0xa2, 0x10, 0xe3, 0x05, 0xd8, 0x00, 0x67,
        0xd8, 0x5d, 0xc4, 0x01, 0x6e, 0x80, 0x00, 0x68, 0x03, 0x0c, 0x00, 0x00, 0x00, 0x30, 0x00, 0x0b,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8c
    };
}

// ---------------------------------------------------------------------------
// Static members
// ---------------------------------------------------------------------------

std::vector<DISPLAYCONFIG_VIDEO_SIGNAL_INFO> IndirectDeviceContext::s_KnownMonitorModes;
std::vector<BYTE> IndirectDeviceContext::s_KnownMonitorEdid;

std::map<LUID, std::shared_ptr<Direct3DDevice>, LuidComparator> IndirectDeviceContext::s_DeviceCache;
std::mutex IndirectDeviceContext::s_DeviceCacheMutex;

// ---------------------------------------------------------------------------
// Mode / EDID helpers
// ---------------------------------------------------------------------------

namespace
{
    std::vector<std::string> split(const std::string& input, char delimiter)
    {
        std::vector<std::string> parts;
        std::stringstream ss(input);
        std::string item;
        while (std::getline(ss, item, delimiter))
        {
            parts.push_back(item);
        }
        return parts;
    }

    void float_to_vsync(float refresh_rate, int& num, int& den)
    {
        den = 10000;
        num = static_cast<int>(std::lround(refresh_rate * den));
        int divisor = std::gcd(num, den);
        num /= divisor;
        den /= divisor;
    }

    void CreateTargetMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, UINT Width, UINT Height, UINT VSyncNum, UINT VSyncDen)
    {
        Mode.totalSize.cx = Mode.activeSize.cx = Width;
        Mode.totalSize.cy = Mode.activeSize.cy = Height;
        Mode.AdditionalSignalInfo.vSyncFreqDivider = 1;
        Mode.AdditionalSignalInfo.videoStandard = 255;
        Mode.vSyncFreq.Numerator = VSyncNum;
        Mode.vSyncFreq.Denominator = VSyncDen;
        Mode.hSyncFreq.Numerator = VSyncNum * Height;
        Mode.hSyncFreq.Denominator = VSyncDen;
        Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
        Mode.pixelRate = static_cast<UINT64>(VSyncNum) * Width * Height / VSyncDen;
    }

    BYTE CalculateEdidChecksum(const std::vector<BYTE>& edid)
    {
        int sum = 0;
        for (int i = 0; i < 127; ++i)
        {
            sum += edid[i];
        }
        sum %= 256;
        if (sum != 0)
        {
            sum = 256 - sum;
        }
        return static_cast<BYTE>(sum);
    }

    struct ModeSpec
    {
        UINT Width;
        UINT Height;
        float RefreshHz;
    };

    void GetFallbackModes(std::vector<std::tuple<int, int, int, int>>& outModes)
    {
        static const ModeSpec specs[] =
        {
            { 1920, 1080, 60.f },
            { 2560, 1440, 60.f },
            { 3840, 2160, 60.f },
            { 1920, 1200, 60.f },
            { 2560, 1600, 60.f },
            { 2732, 2048, 60.f },
            { 1600, 2560, 60.f },
            { 1280, 800,  60.f },
            { 1920, 1080, 120.f },
            { 2560, 1440, 120.f },
            { 3840, 2160, 30.f },
        };

        for (const auto& spec : specs)
        {
            int num, den;
            float_to_vsync(spec.RefreshHz, num, den);
            outModes.push_back(std::make_tuple(spec.Width, spec.Height, num, den));
        }
    }

    bool LoadOptions(std::vector<std::tuple<int, int, int, int>>& outModes, UINT& outDisplayCount)
    {
        outDisplayCount = k_DefaultDisplayCount;

        std::ifstream ifs(k_OptionsPath);
        if (!ifs.is_open())
        {
            return false;
        }

        std::string line;
        if (!std::getline(ifs, line))
        {
            return false;
        }

        int count = std::atoi(line.c_str());
        outDisplayCount = (UINT)std::max(1, count);

        std::vector<std::tuple<int, int, int, int>> modes;
        while (std::getline(ifs, line))
        {
            // strip leading whitespace
            size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                continue;
            }
            line = line.substr(first);
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            std::vector<std::string> parts = split(line, ',');
            if (parts.size() != 3)
            {
                continue;
            }

            int vsync_num, vsync_den;
            float_to_vsync((float)std::atof(parts[2].c_str()), vsync_num, vsync_den);
            modes.push_back(std::make_tuple(std::atoi(parts[0].c_str()), std::atoi(parts[1].c_str()), vsync_num, vsync_den));
        }

        if (modes.empty())
        {
            return false;
        }

        outModes = std::move(modes);
        return true;
    }
}

// ---------------------------------------------------------------------------
// IndirectDeviceContext
// ---------------------------------------------------------------------------

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice)
    : m_WdfDevice(WdfDevice), m_Adapter(nullptr), m_DisplayCount(k_DefaultDisplayCount)
{
}

IndirectDeviceContext::~IndirectDeviceContext()
{
    // Stop every swap-chain consumer thread before tearing down the device.
    for (auto& pair : m_ProcessingThreads)
    {
        pair.second.reset();
    }
    m_ProcessingThreads.clear();
}

void IndirectDeviceContext::LoadConfiguration()
{
    std::vector<std::tuple<int, int, int, int>> modes;
    UINT displayCount = k_DefaultDisplayCount;

    if (LoadOptions(modes, displayCount))
    {
        TS_TRACE("Loaded %zu modes from %ls", modes.size(), k_OptionsPath);
    }
    else
    {
        displayCount = k_DefaultDisplayCount;
        GetFallbackModes(modes);
        TS_TRACE("Using built-in fallback modes (%zu)", modes.size());
    }

    m_DisplayCount = displayCount;

    s_KnownMonitorModes.clear();
    s_KnownMonitorModes.reserve(modes.size());
    for (const auto& m : modes)
    {
        DISPLAYCONFIG_VIDEO_SIGNAL_INFO info = {};
        CreateTargetMode(info, std::get<0>(m), std::get<1>(m), std::get<2>(m), std::get<3>(m));
        s_KnownMonitorModes.push_back(info);
    }

    s_KnownMonitorEdid.assign(k_TwinScreenEdid, k_TwinScreenEdid + sizeof(k_TwinScreenEdid));
    s_KnownMonitorEdid[127] = CalculateEdidChecksum(s_KnownMonitorEdid);
}

void IndirectDeviceContext::InitAdapter()
{
    LoadConfiguration();

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);
    AdapterCaps.MaxMonitorsSupported = m_DisplayCount;

    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"TwinScreen Virtual Display";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"TwinScreen";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"TwinScreen Virtual Display Model";

    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.ParentObject = m_WdfDevice;

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);
    if (!NT_SUCCESS(Status))
    {
        TS_ERROR("InitAdapter: IddCxAdapterInitAsync failed 0x%08X", Status);
        return;
    }

    m_Adapter = AdapterInitOut.AdapterObject;

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(m_Adapter);
    if (pContext)
    {
        pContext->pContext = this;
    }

    TS_TRACE("InitAdapter: adapter created (%u virtual display(s))", m_DisplayCount);
}

void IndirectDeviceContext::FinishInit()
{
    for (UINT i = 0; i < m_DisplayCount; ++i)
    {
        CreateMonitor(i);
    }
}

void IndirectDeviceContext::CreateMonitor(unsigned int index)
{
    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = index;
    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = static_cast<UINT>(s_KnownMonitorEdid.size());
    MonitorInfo.MonitorDescription.pData = s_KnownMonitorEdid.data();

    CoCreateGuid(&MonitorInfo.MonitorContainerId);

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.ParentObject = m_Adapter;

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (!NT_SUCCESS(Status))
    {
        TS_ERROR("CreateMonitor: IddCxMonitorCreate failed 0x%08X", Status);
        return;
    }

    IDDCX_MONITOR Monitor = MonitorCreateOut.MonitorObject;
    m_Monitors.push_back(Monitor);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Monitor);
    if (pContext)
    {
        pContext->pContext = this;
    }

    IDARG_OUT_MONITORARRIVAL ArrivalOut = {};
    Status = IddCxMonitorArrival(Monitor, &ArrivalOut);
    if (!NT_SUCCESS(Status))
    {
        TS_ERROR("CreateMonitor: IddCxMonitorArrival failed 0x%08X", Status);
        return;
    }

    TS_TRACE("CreateMonitor: monitor %u arrived", index);
}

void IndirectDeviceContext::AssignSwapChain(IDDCX_MONITOR Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    auto Device = GetOrCreateDevice(RenderAdapter);
    if (!Device)
    {
        TS_ERROR("AssignSwapChain: failed to get or create D3D device for render adapter %08x%08x",
                 RenderAdapter.HighPart, RenderAdapter.LowPart);
        WdfObjectDelete(SwapChain);
        return;
    }

    std::unique_ptr<SwapChainProcessor> previousProcessor;
    auto newProcessor = std::make_unique<SwapChainProcessor>(SwapChain, Device, NewFrameEvent);

    {
        std::lock_guard<std::mutex> lock(m_ProcessingThreadsMutex);
        auto& processorSlot = m_ProcessingThreads[Monitor];
        previousProcessor = std::move(processorSlot);
        processorSlot = std::move(newProcessor);
    }

    TS_TRACE("AssignSwapChain: monitor=%p swapchain=%p", Monitor, SwapChain);
}

void IndirectDeviceContext::UnassignSwapChain(IDDCX_MONITOR Monitor)
{
    std::unique_ptr<SwapChainProcessor> processorToStop;
    {
        std::lock_guard<std::mutex> lock(m_ProcessingThreadsMutex);
        auto it = m_ProcessingThreads.find(Monitor);
        if (it != m_ProcessingThreads.end())
        {
            processorToStop = std::move(it->second);
            m_ProcessingThreads.erase(it);
        }
    }

    // Destructor signals the terminate event and joins the consumer thread.
    TS_TRACE("UnassignSwapChain: monitor=%p", Monitor);
}

std::shared_ptr<Direct3DDevice> IndirectDeviceContext::GetOrCreateDevice(LUID RenderAdapter)
{
    std::lock_guard<std::mutex> lock(s_DeviceCacheMutex);

    auto it = s_DeviceCache.find(RenderAdapter);
    if (it != s_DeviceCache.end())
    {
        return it->second;
    }

    auto Device = std::make_shared<Direct3DDevice>(RenderAdapter);
    HRESULT hr = Device->Init();
    if (FAILED(hr))
    {
        TS_ERROR("GetOrCreateDevice: D3D init failed 0x%08X", hr);
        return nullptr;
    }

    s_DeviceCache[RenderAdapter] = Device;
    return Device;
}

// ---------------------------------------------------------------------------
// Direct3DDevice
// ---------------------------------------------------------------------------

Direct3DDevice::Direct3DDevice(LUID AdapterLuid)
    : AdapterLuid(AdapterLuid)
{
}

HRESULT Direct3DDevice::Init()
{
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        return hr;
    }

    D3D_FEATURE_LEVEL FeatureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    hr = D3D11CreateDevice(
        Adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        FeatureLevels,
        ARRAYSIZE(FeatureLevels),
        D3D11_SDK_VERSION,
        &Device,
        nullptr,
        &DeviceContext);

    return hr;
}

// ---------------------------------------------------------------------------
// SwapChainProcessor
// ---------------------------------------------------------------------------

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
    : m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    if (m_hTerminateEvent.Get())
    {
        SetEvent(m_hTerminateEvent.Get());
    }

    if (m_hThread.Get())
    {
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    TS_TRACE("SwapChainProcessor::Run() started");

    // Multimedia Class Scheduler Service: keep frame consumption on a
    // real-time scheduling class so the OS presenter stays healthy.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

    RunCore();

    if (m_hSwapChain)
    {
        WdfObjectDelete((WDFOBJECT)m_hSwapChain);
        m_hSwapChain = nullptr;
    }

    if (AvTaskHandle)
    {
        AvRevertMmThreadCharacteristics(AvTaskHandle);
    }

    TS_TRACE("SwapChainProcessor::Run() exited");
}

void SwapChainProcessor::RunCore()
{
    // Bind a D3D11 device so the OS can hand us swap-chain surfaces.
    ComPtr<IDXGIDevice> DxgiDevice;
    if (m_Device && m_Device->Device)
    {
        HRESULT hr = m_Device->Device.As(&DxgiDevice);
        if (SUCCEEDED(hr))
        {
            IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
            SetDevice.pDevice = DxgiDevice.Get();
            hr = (HRESULT)IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
            if (FAILED(hr))
            {
                TS_ERROR("RunCore: IddCxSwapChainSetDevice failed 0x%08X", hr);
                return;
            }
        }
        else
        {
            TS_ERROR("RunCore: QI IDXGIDevice failed 0x%08X", hr);
            return;
        }
    }

    UINT retryCount = 0;

    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;
        IDXGIResource* pSurface = nullptr;
        HRESULT hr = E_PENDING;

        if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2))
        {
            IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
            BufferInArgs.Size = sizeof(BufferInArgs);

            IDARG_OUT_RELEASEANDACQUIREBUFFER2 BufferOutArgs = {};
            hr = (HRESULT)IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &BufferOutArgs);
            pSurface = BufferOutArgs.MetaData.pSurface;
        }
        else
        {
            IDARG_OUT_RELEASEANDACQUIREBUFFER BufferOutArgs = {};
            hr = (HRESULT)IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &BufferOutArgs);
            pSurface = BufferOutArgs.MetaData.pSurface;
        }

        if (hr == E_PENDING)
        {
            // No surface yet; wait for a new frame or teardown.
            HANDLE waitHandles[2] = {};
            DWORD waitHandleCount = 0;

            if (m_hAvailableBufferEvent != nullptr && m_hAvailableBufferEvent != INVALID_HANDLE_VALUE)
            {
                waitHandles[waitHandleCount++] = m_hAvailableBufferEvent;
            }
            if (m_hTerminateEvent.Get())
            {
                waitHandles[waitHandleCount++] = m_hTerminateEvent.Get();
            }

            DWORD WaitResult = WaitForMultipleObjects(waitHandleCount, waitHandles, FALSE, INFINITE);
            if (WaitResult == WAIT_OBJECT_0)
            {
                continue; // new frame available, try to acquire again
            }
            if (waitHandleCount > 1 && WaitResult == WAIT_OBJECT_0 + 1)
            {
                break; // terminate requested
            }

            Sleep(1);
            continue;
        }
        else if (SUCCEEDED(hr))
        {
            AcquiredBuffer.Attach(pSurface);

            // Ack-and-drop: twinhost captures the real pixels via Desktop
            // Duplication, so we have nothing to render here.
            AcquiredBuffer.Reset();

            hr = (HRESULT)IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                TS_ERROR("RunCore: IddCxSwapChainFinishedProcessingFrame failed 0x%08X", hr);
                break;
            }

            retryCount = 0;
        }
        else
        {
            // Typically DXGI_ERROR_ACCESS_LOST when the presenter restarts.
            if (hr == DXGI_ERROR_ACCESS_LOST && retryCount < 5)
            {
                ++retryCount;
                Sleep(1 + retryCount * 10);
                continue;
            }

            TS_ERROR("RunCore: acquire failed 0x%08X", hr);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// WDF / IddCx callbacks
// ---------------------------------------------------------------------------

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config, VirtualDisplayDriverDeviceAdd);

    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IddCxInitialize();
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    TS_TRACE("DriverEntry done");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = VirtualDisplayDriverDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    IddConfig.EvtIddCxAdapterInitFinished = VirtualDisplayDriverAdapterInitFinished;
    IddConfig.EvtIddCxAdapterCommitModes = VirtualDisplayDriverAdapterCommitModes;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = VirtualDisplayDriverMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = VirtualDisplayDriverMonitorQueryModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = VirtualDisplayDriverMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = VirtualDisplayDriverMonitorUnassignSwapChain;

    NTSTATUS Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext && pContext->pContext)
        {
            delete pContext->pContext;
            pContext->pContext = nullptr;
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    if (!pContext)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pContext->pContext = new IndirectDeviceContext(Device);
    if (!pContext->pContext)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    if (pContext && pContext->pContext)
    {
        pContext->pContext->InitAdapter();
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);

    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        if (pContext && pContext->pContext)
        {
            pContext->pContext->FinishInit();
        }
        TS_TRACE("Adapter init finished successfully");
    }
    else
    {
        TS_ERROR("Adapter init failed: 0x%08X", pInArgs->AdapterInitStatus);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    // All advertised modes are valid for the virtual display; nothing to do.
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    // Never called: every monitor is created with a full EDID description.
    // Modes are served by EvtIddCxMonitorQueryTargetModes.
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);
    UNREFERENCED_PARAMETER(pOutArgs);
    return STATUS_NOT_IMPLEMENTED;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    const auto& modes = IndirectDeviceContext::s_KnownMonitorModes;
    pOutArgs->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());

    if (pInArgs->TargetModeBufferInputCount < modes.size())
    {
        TS_WARN("QueryModes: buffer too small (%u < %zu)", pInArgs->TargetModeBufferInputCount, modes.size());
        return STATUS_SUCCESS;
    }

    for (size_t i = 0; i < modes.size(); ++i)
    {
        IDDCX_TARGET_MODE& targetMode = pInArgs->pTargetModes[i];
        targetMode.Size = sizeof(IDDCX_TARGET_MODE);
        targetMode.TargetVideoSignalInfo.targetVideoSignalInfo = modes[i];
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
    if (!pContext || !pContext->pContext)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    pContext->pContext->AssignSwapChain(
        MonitorObject,
        pInArgs->hSwapChain,
        pInArgs->RenderAdapterLuid,
        pInArgs->hNextSurfaceAvailable);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VirtualDisplayDriverMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(MonitorObject);
    if (!pContext || !pContext->pContext)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    pContext->pContext->UnassignSwapChain(MonitorObject);
    return STATUS_SUCCESS;
}
