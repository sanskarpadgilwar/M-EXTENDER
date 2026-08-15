#pragma once

#define NOMINMAX
#include <windows.h>
#include <wdf.h>
#include <IddCx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <vector>
#include <map>
#include <mutex>

#include "Trace.h"

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // WRL has no thread-handle wrapper by default; add one.
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace TwinScreen
{
    namespace IndirectDisp
    {
        /// <summary>
        /// A Direct3D 11 device created for the render adapter that owns an
        /// indirect display swap chain. Needed by IddCxSwapChainSetDevice so the
        /// OS can hand us the swap chain's surface textures.
        /// </summary>
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        /// <summary>
        /// A thread that consumes frames from an indirect display swap chain.
        /// This driver presents a virtual monitor only; real frame content is
        /// captured in user mode by twinhost via Desktop Duplication, so the
        /// processor simply ACKs and drops every frame.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
            ~SwapChainProcessor();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();

        public:
            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            Microsoft::WRL::Wrappers::Thread m_hThread;
            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
        };

        /// <summary>
        /// Custom comparator for LUID to be usable as a std::map key.
        /// </summary>
        struct LuidComparator
        {
            bool operator()(const LUID& a, const LUID& b) const
            {
                if (a.HighPart != b.HighPart)
                    return a.HighPart < b.HighPart;
                return a.LowPart < b.LowPart;
            }
        };

        /// <summary>
        /// Per-device indirect display context: creates the adapter and one or
        /// more monitors, and owns the swap chain consumer threads.
        /// </summary>
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            virtual ~IndirectDeviceContext();

            void InitAdapter();
            void FinishInit();

            void CreateMonitor(unsigned int index);

            void AssignSwapChain(IDDCX_MONITOR Monitor, IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain(IDDCX_MONITOR Monitor);

        protected:
            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;
            std::vector<IDDCX_MONITOR> m_Monitors;
            UINT m_DisplayCount;

            std::map<IDDCX_MONITOR, std::unique_ptr<SwapChainProcessor>> m_ProcessingThreads;
            std::mutex m_ProcessingThreadsMutex;

        public:
            static std::vector<DISPLAYCONFIG_VIDEO_SIGNAL_INFO> s_KnownMonitorModes;
            static std::vector<BYTE> s_KnownMonitorEdid;

        private:
            void LoadConfiguration();

            static std::map<LUID, std::shared_ptr<Direct3DDevice>, LuidComparator> s_DeviceCache;
            static std::mutex s_DeviceCacheMutex;
            static std::shared_ptr<Direct3DDevice> GetOrCreateDevice(LUID RenderAdapter);
        };
    }
}
