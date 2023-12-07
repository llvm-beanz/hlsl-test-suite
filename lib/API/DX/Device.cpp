//===- DX/Device.cpp - HLSL API DirectX Device API ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include <atlbase.h>
#include <combaseapi.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <dxgi1_4.h>
#include <dxgiformat.h>

// The windows headers define these macros which conflict with the C++ standard
// library. Undefining them before including any LLVM C++ code prevents errors.
#undef max
#undef min

#include "HLSLTest/API/Capabilities.h"
#include "HLSLTest/API/Device.h"
#include "HLSLTest/API/Pipeline.h"
#include "HLSLTest/WinError.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include <codecvt>
#include <locale>

using namespace hlsltest;
namespace {

std::string StringFromWString(const std::wstring &In) {
  using convert_type = std::codecvt_utf8<wchar_t>;
  std::wstring_convert<convert_type, wchar_t> Converter;
  return Converter.to_bytes(In);
}

class DXDevice : public hlsltest::Device {
private:
  CComPtr<IDXGIAdapter1> Adapter;
  CComPtr<ID3D12Device> Device;
  Capabilities Caps;

  struct InvocationState {
    CComPtr<ID3D12RootSignature> RootSig;
    CComPtr<ID3D12DescriptorHeap> DescHeap;
    CComPtr<ID3D12PipelineState> PSO;
    CComPtr<ID3D12CommandQueue> Queue;
    CComPtr<ID3D12CommandAllocator> Allocator;
    CComPtr<ID3D12GraphicsCommandList> CmdList;
    CComPtr<ID3D12Fence> Fence;
    HANDLE Event;

    llvm::SmallVector<CComPtr<ID3D12Resource>> Resources;
  };

public:
  DXDevice(CComPtr<IDXGIAdapter1> A, CComPtr<ID3D12Device> D,
           DXGI_ADAPTER_DESC1 Desc)
      : Adapter(A), Device(D) {
    Description = StringFromWString(std::wstring(Desc.Description, 128));
  }
  DXDevice(const DXDevice &) = default;

  ~DXDevice() override = default;

  llvm::StringRef getAPIName() const override { return "DirectX"; }
  GPUAPI getAPI() const override { return GPUAPI::DirectX; }

  static llvm::Expected<DXDevice> Create(CComPtr<IDXGIAdapter1> Adapter) {
    CComPtr<ID3D12Device> Device;
    if (auto Err =
            HR::toError(D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_11_0,
                                          IID_PPV_ARGS(&Device)),
                        "Failed to create D3D device"))
      return Err;
    DXGI_ADAPTER_DESC1 Desc;
    if (auto Err = HR::toError(Adapter->GetDesc1(&Desc),
                               "Failed to get device description"))
      return Err;
    return DXDevice(Adapter, Device, Desc);
  }

  const Capabilities &getCapabilities() override {
    if (Caps.empty())
      queryCapabilities();
    return Caps;
  }

  void queryCapabilities() {
    CD3DX12FeatureSupport Features;
    Features.Init(Device);

#define D3D_FEATURE_BOOL(Name)                                                 \
  Caps.insert(                                                                 \
      std::make_pair(#Name, make_capability<bool>(#Name, Features.Name())));

#define D3D_FEATURE_UINT(Name)                                                 \
  Caps.insert(std::make_pair(                                                  \
      #Name, make_capability<uint32_t>(#Name, Features.Name())));

#include "DXFeatures.def"
  }

  llvm::Error createRootSignature(Pipeline &P, InvocationState &State) {
    std::vector<D3D12_ROOT_PARAMETER> RootParams;
    uint32_t DescriptorCount = P.getDescriptorCount();
    std::unique_ptr<D3D12_DESCRIPTOR_RANGE[]> Ranges =
        std::unique_ptr<D3D12_DESCRIPTOR_RANGE[]>(
            new D3D12_DESCRIPTOR_RANGE[DescriptorCount]);

    uint32_t RangeIdx = 0;
    for (const auto &D : P.Sets) {
      uint32_t DescriptorIdx = 0;
      uint32_t StartRangeIdx = RangeIdx;
      for (const auto &R : D.Resources) {
        switch (R.Access) {
        case DataAccess::ReadOnly:
          Ranges.get()[RangeIdx].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
          break;
        case DataAccess::ReadWrite:
          Ranges.get()[RangeIdx].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
          break;
        case DataAccess::Constant:
          Ranges.get()[RangeIdx].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
          break;
        }
        Ranges.get()[RangeIdx].NumDescriptors = 1;
        Ranges.get()[RangeIdx].BaseShaderRegister = R.DXBinding.Register;
        Ranges.get()[RangeIdx].RegisterSpace = R.DXBinding.Space;
        Ranges.get()[RangeIdx].OffsetInDescriptorsFromTableStart =
            DescriptorIdx;
        RangeIdx++;
      }
      if (D.Resources.size() > 0)
        RootParams.push_back(
            D3D12_ROOT_PARAMETER{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                                 {D3D12_ROOT_DESCRIPTOR_TABLE{
                                     static_cast<uint32_t>(D.Resources.size()),
                                     &Ranges[StartRangeIdx]}},
                                 D3D12_SHADER_VISIBILITY_ALL});
    }

    D3D12_ROOT_SIGNATURE_DESC Desc = D3D12_ROOT_SIGNATURE_DESC{
        static_cast<uint32_t>(RootParams.size()), RootParams.data(), 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_NONE};

    CComPtr<ID3DBlob> Signature;
    CComPtr<ID3DBlob> Error;
    if (auto Err = HR::toError(
            D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                        &Signature, &Error),
            "Failed to seialize root signature.")) {
      std::string Msg =
          std::string(reinterpret_cast<char *>(Error->GetBufferPointer()),
                      Error->GetBufferSize() / sizeof(char));
      return joinErrors(
          std::move(Err),
          llvm::createStringError(std::errc::protocol_error, Msg.c_str()));
    }

    if (auto Err = HR::toError(
            Device->CreateRootSignature(0, Signature->GetBufferPointer(),
                                        Signature->GetBufferSize(),
                                        IID_PPV_ARGS(&State.RootSig)),
            "Failed to create root signature."))
      return Err;

    return llvm::Error::success();
  }

  llvm::Error createDescriptorHeap(Pipeline &P, InvocationState &State) {
    const D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, P.getDescriptorCount(),
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (auto Err = HR::toError(Device->CreateDescriptorHeap(
                                   &HeapDesc, IID_PPV_ARGS(&State.DescHeap)),
                               "Failed to create descriptor heap."))
      return Err;
    Device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return llvm::Error::success();
  }

  llvm::Error createPSO(Pipeline &P, llvm::StringRef DXIL,
                        InvocationState &State) {
    const D3D12_COMPUTE_PIPELINE_STATE_DESC Desc = {
        State.RootSig,
        {DXIL.data(), DXIL.size()},
        0,
        {
            nullptr,
            0,
        },
        D3D12_PIPELINE_STATE_FLAG_NONE};
    if (auto Err = HR::toError(
            Device->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&State.PSO)),
            "Failed to create PSO."))
      return Err;
    return llvm::Error::success();
  }

  llvm::Error createCommandStructures(InvocationState &IS) {
    const D3D12_COMMAND_QUEUE_DESC Desc = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0,
                                           D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (auto Err = HR::toError(
            Device->CreateCommandQueue(&Desc, IID_PPV_ARGS(&IS.Queue)),
            "Failed to create command queue."))
      return Err;
    if (auto Err = HR::toError(
            Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&IS.Allocator)),
            "Failed to create command allocator."))
      return Err;
    if (auto Err =
            HR::toError(Device->CreateCommandList(
                            0, D3D12_COMMAND_LIST_TYPE_DIRECT, IS.Allocator,
                            nullptr, IID_PPV_ARGS(&IS.CmdList)),
                        "Failed to create command list."))
      return Err;
    return llvm::Error::success();
  }

  void addResourceUploadCommands(Resource &R, InvocationState &IS,
                                 CComPtr<ID3D12Resource> Destination,
                                 CComPtr<ID3D12Resource> Source) {
    addUploadBeginBarrier(IS, Destination);
    IS.CmdList->CopyBufferRegion(Destination, 0, Source, 0, R.Size);
    addUploadEndBarrier(IS, Destination, R.Access == DataAccess::ReadOnly);
  }

  llvm::Error createSRV(Resource &R, InvocationState &IS,
                        const uint32_t HeapIdx) {
    return llvm::createStringError(std::errc::not_supported,
                                   "DXDevice::createSRV not supported.");
  }

  llvm::Error createUAV(Resource &R, InvocationState &IS,
                        const uint32_t HeapIdx) {
    CComPtr<ID3D12Resource> Buffer;
    CComPtr<ID3D12Resource> UploadBuffer;

    const D3D12_HEAP_PROPERTIES HeapProp = {D3D12_HEAP_TYPE_DEFAULT,
                                            D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                            D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC ResDesc = {
        D3D12_RESOURCE_DIMENSION_BUFFER,
        0,
        R.Size,
        1,
        1,
        1,
        DXGI_FORMAT_UNKNOWN,
        {1, 0},
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};

    if (auto Err = HR::toError(Device->CreateCommittedResource(
                                   &HeapProp, D3D12_HEAP_FLAG_NONE, &ResDesc,
                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                   IID_PPV_ARGS(&Buffer)),
                               "Failed to create committed resource (buffer)."))
      return Err;

    const D3D12_HEAP_PROPERTIES UploadHeapProp = {
        D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const D3D12_RESOURCE_DESC UploadResDesc = {D3D12_RESOURCE_DIMENSION_BUFFER,
                                               0,
                                               R.Size,
                                               1,
                                               1,
                                               1,
                                               DXGI_FORMAT_UNKNOWN,
                                               {1, 0},
                                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                               D3D12_RESOURCE_FLAG_NONE};

    if (auto Err =
            HR::toError(Device->CreateCommittedResource(
                            &UploadHeapProp, D3D12_HEAP_FLAG_NONE,
                            &UploadResDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                            nullptr, IID_PPV_ARGS(&UploadBuffer)),
                        "Failed to create committed resource (upload buffer)."))
      return Err;

    // Initialize the UAV data
    void *ResDataPtr = nullptr;
    if (auto Err = HR::toError(UploadBuffer->Map(0, nullptr, &ResDataPtr),
                               "Failed to acquire UAV data pointer."))
      return Err;
    memcpy(ResDataPtr, R.Data.get(), R.Size);
    UploadBuffer->Unmap(0, nullptr);

    addResourceUploadCommands(R, IS, Buffer, UploadBuffer);

    const uint32_t EltSize = R.getElementSize();
    const uint32_t NumElts = R.Size / EltSize;
    const D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {
        DXGI_FORMAT_UNKNOWN,
        D3D12_UAV_DIMENSION_BUFFER,
        {D3D12_BUFFER_UAV{0, NumElts, EltSize, 0, D3D12_BUFFER_UAV_FLAG_NONE}}};

    D3D12_CPU_DESCRIPTOR_HANDLE UAVHandle =
        IS.DescHeap->GetCPUDescriptorHandleForHeapStart();
    UAVHandle.ptr += HeapIdx * Device->GetDescriptorHandleIncrementSize(
                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Device->CreateUnorderedAccessView(Buffer, nullptr, &UAVDesc, UAVHandle);

    IS.Resources.push_back(Buffer);
    IS.Resources.push_back(UploadBuffer);
    return llvm::Error::success();
  }

  llvm::Error createCBV(Resource &R, InvocationState &IS,
                        const uint32_t HeapIdx) {
    return llvm::createStringError(std::errc::not_supported,
                                   "DXDevice::createSRV not supported.");
  }

  llvm::Error createBuffers(Pipeline &P, InvocationState &IS) {
    uint32_t HeapIndex = 0;
    for (auto &D : P.Sets) {
      for (auto &R : D.Resources) {
        switch (R.Access) {
        case DataAccess::ReadOnly:
          if (auto Err = createSRV(R, IS, HeapIndex++))
            return Err;
          break;
        case DataAccess::ReadWrite:
          if (auto Err = createUAV(R, IS, HeapIndex++))
            return Err;
          break;
        case DataAccess::Constant:
          if (auto Err = createCBV(R, IS, HeapIndex++))
            return Err;
          break;
        }
      }
    }
    return llvm::Error::success();
  }

  void addUploadBeginBarrier(InvocationState &IS, CComPtr<ID3D12Resource> R) {
    const D3D12_RESOURCE_BARRIER BeginBarrier = {
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        D3D12_RESOURCE_BARRIER_FLAG_NONE,
        {D3D12_RESOURCE_TRANSITION_BARRIER{
            R, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST}}};
    IS.CmdList->ResourceBarrier(1, &BeginBarrier);
  }

  void addUploadEndBarrier(InvocationState &IS, CComPtr<ID3D12Resource> R,
                           bool IsUAV) {
    const D3D12_RESOURCE_BARRIER BeginBarrier = {
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        D3D12_RESOURCE_BARRIER_FLAG_NONE,
        {D3D12_RESOURCE_TRANSITION_BARRIER{
            R, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON,
            IsUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                  : D3D12_RESOURCE_STATE_GENERIC_READ}}};
    IS.CmdList->ResourceBarrier(1, &BeginBarrier);
  }

  llvm::Error createEvent(InvocationState &IS) {
    if (auto Err = HR::toError(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                   IID_PPV_ARGS(&IS.Fence)),
                               "Failed to create fence."))
      return Err;
    IS.Event = CreateEventA(nullptr, false, false, nullptr);
    if (!IS.Event)
      return llvm::createStringError(std::errc::device_or_resource_busy,
                                     "Failed to create event.");
    return llvm::Error::success();
  }

  llvm::Error executeCommandList(InvocationState &IS) {
    if (auto Err = HR::toError(IS.CmdList->Close(), "Failed to close command list."))
      return Err;
    ID3D12CommandList *CmdLists[] = { IS.CmdList};
    IS.Queue->ExecuteCommandLists(1, CmdLists);

    if(auto Err = HR::toError(IS.Queue->Signal(IS.Fence, 1), "Failed to add signal."))
      return Err;

    if (auto Err = HR::toError(Fence->SetEventOnCompletion(1, IS.Event), "Failed to register end event."))
      return Err;

    WaitForSingleObject(IS.Event, INFINITE);
    return llvm::Error::success();
  }

  llvm::Error executeProgram(llvm::StringRef Program, Pipeline &P) override {
    InvocationState State;
    llvm::outs() << "Configuring execution on device: " << Description << "\n";
    if (auto Err = createRootSignature(P, State))
      return Err;
    llvm::outs() << "RootSignature created.\n";
    if (auto Err = createDescriptorHeap(P, State))
      return Err;
    llvm::outs() << "Descriptor heap created.\n";
    if (auto Err = createPSO(P, Program, State))
      return Err;
    llvm::outs() << "PSO created.\n";
    if (auto Err = createCommandStructures(State))
      return Err;
    llvm::outs() << "Command structures created.\n";
    if (auto Err = createBuffers(P, State))
      return Err;
    llvm::outs() << "Buffers created.\n";
    if (auto Err = createEvent(State))
      return Err;
    llvm::outs() << "Event prepared.\n";
    if (auto Err = executeCommandList(State))
      return Err;
    llvm::outs() << "Commands executed\n";

    return llvm::Error::success();
  }
};

class DirectXContext {
private:
  CComPtr<IDXGIFactory2> Factory;
  llvm::SmallVector<std::shared_ptr<DXDevice>> Devices;

  DirectXContext() = default;
  ~DirectXContext() = default;

public:
  llvm::Error initialize() {
    if (auto Err = HR::toError(CreateDXGIFactory2(0, IID_PPV_ARGS(&Factory)),
                               "Failed to create DXGI Factory")) {
      return Err;
    }
    CComPtr<IDXGIAdapter1> Adapter;
    unsigned AdapterIndex = 0;
    while (SUCCEEDED(Factory->EnumAdapters1(AdapterIndex++, &Adapter))) {
      auto ExDevice = DXDevice::Create(Adapter);
      if (!ExDevice)
        return ExDevice.takeError();
      auto ShPtr = std::make_shared<DXDevice>(*ExDevice);
      Devices.push_back(ShPtr);
      Device::registerDevice(std::static_pointer_cast<Device>(ShPtr));
    }
    return llvm::Error::success();
  }

  using iterator = llvm::SmallVector<std::shared_ptr<DXDevice>>::iterator;

  iterator begin() { return Devices.begin(); }
  iterator end() { return Devices.end(); }

  static DirectXContext &Instance() {
    static DirectXContext Ctx;
    return Ctx;
  }
};
} // namespace

llvm::Error InitializeDXDevices() {
  return DirectXContext::Instance().initialize();
}
