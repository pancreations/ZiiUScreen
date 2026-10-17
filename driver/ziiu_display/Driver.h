// SPDX-License-Identifier: MS-PL
// Copyright (c) Microsoft Corporation
// Modifications copyright (c) 2026 pancreations
// See LICENSE in this directory.
// ZiiUScreen virtual monitor: an IddCx (UMDF) indirect display driver that gives
// Windows one 59.94 Hz monitor (the GamePad; 1920x1080 by default, also 1280x720,
// 1708x960 and the native 854x480), scales every frame Windows composes for it to
// 854x480 on the GPU and hands it to the GamePad streamer through shared memory
// (ziiu_display_shared.h).
//
// Based on Microsoft's IddCx sample (Windows-driver-samples video/IndirectDisplay,
// commit 2dc3fd3a, Microsoft Public License; see docs/REFERENCES.md). Changes: one
// monitor with a generated EDID and 59.94 Hz 16:9 modes, GPU downscale and frame copy
// to shared memory, no WPP tracing.
#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <vector>

#include "ziiu_display_shared.h"

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace ZiiU
{
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

    // The shared section and event (created once per driver host).
    class FrameSink
    {
    public:
        bool Open();
        void Publish(ID3D11DeviceContext* Context, ID3D11Texture2D* Staging, UINT Width, UINT Height,
                     UINT DirtyRects, UINT64 PresentFrame, LONGLONG QpcAcquired);
        void CountDropped();

    private:
        HANDLE m_Section = nullptr;
        HANDLE m_Event = nullptr;
        ZIIU_FRAME_HEADER* m_Header = nullptr;
        BYTE* m_Base = nullptr;
        LONGLONG m_Seq = 0;
    };

    class SwapChainProcessor
    {
    public:
        SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
        ~SwapChainProcessor();

    private:
        static DWORD CALLBACK RunThread(LPVOID Argument);

        void Run();
        void RunCore();
        void CopyFrame(IDXGIResource* Surface, const IDARG_OUT_RELEASEANDACQUIREBUFFER& Buffer);
        ID3D11Texture2D* Scale(ID3D11Texture2D* Source, const D3D11_TEXTURE2D_DESC& Desc);

        IDDCX_SWAPCHAIN m_hSwapChain;
        std::shared_ptr<Direct3DDevice> m_Device;
        HANDLE m_hAvailableBufferEvent;
        Microsoft::WRL::Wrappers::Thread m_hThread;
        Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Staging;
        D3D11_TEXTURE2D_DESC m_StagingDesc = {};
        // GPU downscale to the GamePad's 854x480 (D3D11 video processor).
        Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_VideoDevice;
        Microsoft::WRL::ComPtr<ID3D11VideoContext> m_VideoContext;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_Enum;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_Processor;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Scaled;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_OutView;
        UINT m_ScaleFromW = 0, m_ScaleFromH = 0;
    };

    class IndirectDeviceContext
    {
    public:
        IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
        virtual ~IndirectDeviceContext();

        void InitAdapter();
        void FinishInit();

    protected:
        WDFDEVICE m_WdfDevice;
        IDDCX_ADAPTER m_Adapter;
    };

    class IndirectMonitorContext
    {
    public:
        IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor);
        virtual ~IndirectMonitorContext();

        void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
        void UnassignSwapChain();

    private:
        IDDCX_MONITOR m_Monitor;
        std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
    };
}
