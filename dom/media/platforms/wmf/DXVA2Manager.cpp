/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DXVA2Manager.h"
#include <d3d11.h>
#include "nsThreadUtils.h"
#include "ImageContainer.h"
#include "gfxWindowsPlatform.h"
#include "D3D9SurfaceImage.h"
#include "mozilla/layers/D3D11ShareHandleImage.h"
#include "mozilla/layers/ImageBridgeChild.h"
#include "mozilla/Preferences.h"
#include "mfapi.h"
#include "MFTDecoder.h"
#include "DriverCrashGuard.h"
#include "nsPrintfCString.h"

const CLSID CLSID_VideoProcessorMFT =
{
  0x88753b26,
  0x5b24,
  0x49bd,
  { 0xb2, 0xe7, 0xc, 0x44, 0x5c, 0x78, 0xc9, 0x82 }
};

const GUID MF_XVP_PLAYBACK_MODE =
{
  0x3c5d293f,
  0xad67,
  0x4e29,
  { 0xaf, 0x12, 0xcf, 0x3e, 0x23, 0x8a, 0xcc, 0xe9 }
};

DEFINE_GUID(MF_LOW_LATENCY,
  0x9c27891a, 0xed7a, 0x40e1, 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee);

namespace mozilla {

using layers::Image;
using layers::ImageContainer;
using layers::D3D9SurfaceImage;
using layers::D3D9RecycleAllocator;
using layers::D3D11ShareHandleImage;
using layers::D3D11RecycleAllocator;

class D3D9DXVA2Manager : public DXVA2Manager
{
public:
  D3D9DXVA2Manager();
  virtual ~D3D9DXVA2Manager();

  HRESULT Init(nsACString& aFailureReason);

  IUnknown* GetDXVADeviceManager() override;

  // Copies a region (aRegion) of the video frame stored in aVideoSample
  // into an image which is returned by aOutImage.
  HRESULT CopyToImage(IMFSample* aVideoSample,
                      const nsIntRect& aRegion,
                      ImageContainer* aContainer,
                      Image** aOutImage) override;

  virtual bool SupportsConfig(IMFMediaType* aType) override;

private:
  RefPtr<IDirect3D9Ex> mD3D9;
  RefPtr<IDirect3DDevice9Ex> mDevice;
  RefPtr<IDirect3DDeviceManager9> mDeviceManager;
  RefPtr<D3D9RecycleAllocator> mTextureClientAllocator;
  RefPtr<IDirectXVideoDecoderService> mDecoderService;
  UINT32 mResetToken;
};

void GetDXVA2ExtendedFormatFromMFMediaType(IMFMediaType *pType,
                                           DXVA2_ExtendedFormat *pFormat)
{
  // Get the interlace mode.
  MFVideoInterlaceMode interlace =
    (MFVideoInterlaceMode)MFGetAttributeUINT32(pType, MF_MT_INTERLACE_MODE, MFVideoInterlace_Unknown);

  if (interlace == MFVideoInterlace_MixedInterlaceOrProgressive) {
    pFormat->SampleFormat = DXVA2_SampleFieldInterleavedEvenFirst;
  } else {
    pFormat->SampleFormat = (UINT)interlace;
  }

  pFormat->VideoChromaSubsampling =
    MFGetAttributeUINT32(pType, MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_Unknown);
  pFormat->NominalRange =
    MFGetAttributeUINT32(pType, MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Unknown);
  pFormat->VideoTransferMatrix =
    MFGetAttributeUINT32(pType, MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown);
  pFormat->VideoLighting =
    MFGetAttributeUINT32(pType, MF_MT_VIDEO_LIGHTING, MFVideoLighting_Unknown);
  pFormat->VideoPrimaries =
    MFGetAttributeUINT32(pType, MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_Unknown);
  pFormat->VideoTransferFunction =
    MFGetAttributeUINT32(pType, MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_Unknown);
}

HRESULT ConvertMFTypeToDXVAType(IMFMediaType *pType, DXVA2_VideoDesc *pDesc)
{
  ZeroMemory(pDesc, sizeof(*pDesc));

  // The D3D format is the first DWORD of the subtype GUID.
  GUID subtype = GUID_NULL;
  HRESULT hr = pType->GetGUID(MF_MT_SUBTYPE, &subtype);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);
  pDesc->Format = (D3DFORMAT)subtype.Data1;

  UINT32 width = 0;
  UINT32 height = 0;
  hr = MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &width, &height);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);
  pDesc->SampleWidth = width;
  pDesc->SampleHeight = height;

  UINT32 fpsNumerator = 0;
  UINT32 fpsDenominator = 0;
  hr = MFGetAttributeRatio(pType, MF_MT_FRAME_RATE, &fpsNumerator, &fpsDenominator);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);
  pDesc->InputSampleFreq.Numerator = fpsNumerator;
  pDesc->InputSampleFreq.Denominator = fpsDenominator;

  GetDXVA2ExtendedFormatFromMFMediaType(pType, &pDesc->SampleFormat);
  pDesc->OutputFrameFreq = pDesc->InputSampleFreq;
  if ((pDesc->SampleFormat.SampleFormat == DXVA2_SampleFieldInterleavedEvenFirst) ||
      (pDesc->SampleFormat.SampleFormat == DXVA2_SampleFieldInterleavedOddFirst)) {
    pDesc->OutputFrameFreq.Numerator *= 2;
  }

  return S_OK;
}

static const GUID DXVA2_ModeH264_E = {
  0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 }
};

// This tests if a DXVA video decoder can be created for the given media type/resolution.
// It uses the same decoder device (DXVA2_ModeH264_E - DXVA2_ModeH264_VLD_NoFGT) as the H264
// decoder MFT provided by windows (CLSID_CMSH264DecoderMFT) uses, so we can use it to determine
// if the MFT will use software fallback or not.
bool
D3D9DXVA2Manager::SupportsConfig(IMFMediaType* aType)
{
  DXVA2_VideoDesc desc;
  HRESULT hr = ConvertMFTypeToDXVAType(aType, &desc);
  NS_ENSURE_TRUE(SUCCEEDED(hr), false);

  UINT configCount;
  DXVA2_ConfigPictureDecode* configs = nullptr;
  hr = mDecoderService->GetDecoderConfigurations(DXVA2_ModeH264_E, &desc, nullptr, &configCount, &configs);
  NS_ENSURE_TRUE(SUCCEEDED(hr), false);

  RefPtr<IDirect3DSurface9> surface;
  hr = mDecoderService->CreateSurface(desc.SampleWidth, desc.SampleHeight, 0, (D3DFORMAT)MAKEFOURCC('N', 'V', '1', '2'),
  D3DPOOL_DEFAULT, 0, DXVA2_VideoDecoderRenderTarget,
  surface.StartAssignment(), NULL);
  if (!SUCCEEDED(hr)) {
    CoTaskMemFree(configs);
    return false;
  }

  for (UINT i = 0; i < configCount; i++) {
    RefPtr<IDirectXVideoDecoder> decoder;
    IDirect3DSurface9* surfaces = surface;
    hr = mDecoderService->CreateVideoDecoder(DXVA2_ModeH264_E, &desc, &configs[i], &surfaces, 1, decoder.StartAssignment());
    if (SUCCEEDED(hr) && decoder) {
      CoTaskMemFree(configs);
      return true;
    }
  }
  CoTaskMemFree(configs);
  return false;
}

D3D9DXVA2Manager::D3D9DXVA2Manager()
  : mResetToken(0)
{
  MOZ_COUNT_CTOR(D3D9DXVA2Manager);
  MOZ_ASSERT(NS_IsMainThread());
}

D3D9DXVA2Manager::~D3D9DXVA2Manager()
{
  MOZ_COUNT_DTOR(D3D9DXVA2Manager);
  MOZ_ASSERT(NS_IsMainThread());
}

IUnknown*
D3D9DXVA2Manager::GetDXVADeviceManager()
{
  MutexAutoLock lock(mLock);
  return mDeviceManager;
}

HRESULT
D3D9DXVA2Manager::Init(nsACString& aFailureReason)
{
  MOZ_ASSERT(NS_IsMainThread());

  gfx::D3D9VideoCrashGuard crashGuard;
  if (crashGuard.Crashed()) {
    NS_WARNING("DXVA2D3D9 crash detected");
    aFailureReason.AssignLiteral("DXVA2D3D9 crashes detected in the past");
    return E_FAIL;
  }

  // Create D3D9Ex.
  HMODULE d3d9lib = LoadLibraryW(L"d3d9.dll");
  NS_ENSURE_TRUE(d3d9lib, E_FAIL);
  decltype(Direct3DCreate9Ex)* d3d9Create =
    (decltype(Direct3DCreate9Ex)*) GetProcAddress(d3d9lib, "Direct3DCreate9Ex");
  RefPtr<IDirect3D9Ex> d3d9Ex;
  HRESULT hr = d3d9Create(D3D_SDK_VERSION, getter_AddRefs(d3d9Ex));
  if (!d3d9Ex) {
    NS_WARNING("Direct3DCreate9 failed");
    aFailureReason.AssignLiteral("Direct3DCreate9 failed");
    return E_FAIL;
  }

  // Ensure we can do the YCbCr->RGB conversion in StretchRect.
  // Fail if we can't.
  hr = d3d9Ex->CheckDeviceFormatConversion(D3DADAPTER_DEFAULT,
                                           D3DDEVTYPE_HAL,
                                           (D3DFORMAT)MAKEFOURCC('N','V','1','2'),
                                           D3DFMT_X8R8G8B8);
  if (!SUCCEEDED(hr)) {
    aFailureReason = nsPrintfCString("CheckDeviceFormatConversion failed with error %X", hr);
    return hr;
  }

  // Create D3D9DeviceEx.
  D3DPRESENT_PARAMETERS params = {0};
  params.BackBufferWidth = 1;
  params.BackBufferHeight = 1;
  params.BackBufferFormat = D3DFMT_UNKNOWN;
  params.BackBufferCount = 1;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = ::GetShellWindow();
  params.Windowed = TRUE;
  params.Flags = D3DPRESENTFLAG_VIDEO;

  RefPtr<IDirect3DDevice9Ex> device;
  hr = d3d9Ex->CreateDeviceEx(D3DADAPTER_DEFAULT,
                              D3DDEVTYPE_HAL,
                              ::GetShellWindow(),
                              D3DCREATE_FPU_PRESERVE |
                              D3DCREATE_MULTITHREADED |
                              D3DCREATE_MIXED_VERTEXPROCESSING,
                              &params,
                              nullptr,
                              getter_AddRefs(device));
  if (!SUCCEEDED(hr)) {
    aFailureReason = nsPrintfCString("CreateDeviceEx failed with error %X", hr);
    return hr;
  }

  // Ensure we can create queries to synchronize operations between devices.
  // Without this, when we make a copy of the frame in order to share it with
  // another device, we can't be sure that the copy has finished before the
  // other device starts using it.
  RefPtr<IDirect3DQuery9> query;

  hr = device->CreateQuery(D3DQUERYTYPE_EVENT, getter_AddRefs(query));
  if (!SUCCEEDED(hr)) {
    aFailureReason = nsPrintfCString("CreateQuery failed with error %X", hr);
    return hr;
  }

  // Create and initialize IDirect3DDeviceManager9.
  UINT resetToken = 0;
  RefPtr<IDirect3DDeviceManager9> deviceManager;

  hr = wmf::DXVA2CreateDirect3DDeviceManager9(&resetToken,
                                              getter_AddRefs(deviceManager));
  if (!SUCCEEDED(hr)) {
    aFailureReason = nsPrintfCString("DXVA2CreateDirect3DDeviceManager9 failed with error %X", hr);
    return hr;
  }
  hr = deviceManager->ResetDevice(device, resetToken);
  if (!SUCCEEDED(hr)) {
    aFailureReason = nsPrintfCString("IDirect3DDeviceManager9::ResetDevice failed with error %X", hr);
    return hr;
  }

  HANDLE deviceHandle;
  RefPtr<IDirectXVideoDecoderService> decoderService;
  hr = deviceManager->OpenDeviceHandle(&deviceHandle);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = deviceManager->GetVideoService(deviceHandle, IID_PPV_ARGS(decoderService.StartAssignment()));
  deviceManager->CloseDeviceHandle(deviceHandle);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  UINT deviceCount;
  GUID* decoderDevices = nullptr;
  hr = decoderService->GetDecoderDeviceGuids(&deviceCount, &decoderDevices);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  bool found = false;
  for (UINT i = 0; i < deviceCount; i++) {
    if (decoderDevices[i] == DXVA2_ModeH264_E) {
      found = true;
      break;
    }
  }
  CoTaskMemFree(decoderDevices);

  if (!found) {
    return E_FAIL;
  }

  mDecoderService = decoderService;

  mResetToken = resetToken;
  mD3D9 = d3d9Ex;
  mDevice = device;
  mDeviceManager = deviceManager;

  mTextureClientAllocator = new D3D9RecycleAllocator(layers::ImageBridgeChild::GetSingleton(),
                                                     mDevice);
  mTextureClientAllocator->SetMaxPoolSize(5);

  return S_OK;
}

HRESULT
D3D9DXVA2Manager::CopyToImage(IMFSample* aSample,
                              const nsIntRect& aRegion,
                              ImageContainer* aImageContainer,
                              Image** aOutImage)
{
  RefPtr<IMFMediaBuffer> buffer;
  HRESULT hr = aSample->GetBufferByIndex(0, getter_AddRefs(buffer));
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  RefPtr<IDirect3DSurface9> surface;
  hr = wmf::MFGetService(buffer,
                         MR_BUFFER_SERVICE,
                         IID_IDirect3DSurface9,
                         getter_AddRefs(surface));
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  RefPtr<Image> image = aImageContainer->CreateImage(ImageFormat::D3D9_RGB32_TEXTURE);
  NS_ENSURE_TRUE(image, E_FAIL);
  NS_ASSERTION(image->GetFormat() == ImageFormat::D3D9_RGB32_TEXTURE,
               "Wrong format?");

  D3D9SurfaceImage* videoImage = static_cast<D3D9SurfaceImage*>(image.get());
  hr = videoImage->SetData(D3D9SurfaceImage::Data(surface, aRegion, mTextureClientAllocator));

  image.forget(aOutImage);

  return S_OK;
}

// Count of the number of DXVAManager's we've created. This is also the
// number of videos we're decoding with DXVA. Use on main thread only.
static uint32_t sDXVAVideosCount = 0;

/* static */
DXVA2Manager*
DXVA2Manager::CreateD3D9DXVA(nsACString& aFailureReason)
{
  MOZ_ASSERT(NS_IsMainThread());
  HRESULT hr;

  // DXVA processing takes up a lot of GPU resources, so limit the number of
  // videos we use DXVA with at any one time.
  const uint32_t dxvaLimit =
    Preferences::GetInt("media.windows-media-foundation.max-dxva-videos", 8);
  if (sDXVAVideosCount == dxvaLimit) {
    aFailureReason.AssignLiteral("Too many DXVA videos playing");
    return nullptr;
  }

  nsAutoPtr<D3D9DXVA2Manager> d3d9Manager(new D3D9DXVA2Manager());
  hr = d3d9Manager->Init(aFailureReason);
  if (SUCCEEDED(hr)) {
    return d3d9Manager.forget();
  }

  // No hardware accelerated video decoding. :(
  return nullptr;
}

class D3D11DXVA2Manager : public DXVA2Manager
{
public:
  D3D11DXVA2Manager();
  virtual ~D3D11DXVA2Manager();

  HRESULT Init(nsACString& aFailureReason);

  IUnknown* GetDXVADeviceManager() override;

  // Copies a region (aRegion) of the video frame stored in aVideoSample
  // into an image which is returned by aOutImage.
  HRESULT CopyToImage(IMFSample* aVideoSample,
                      const nsIntRect& aRegion,
                      ImageContainer* aContainer,
                      Image** aOutImage) override;

  HRESULT ConfigureForSize(uint32_t aWidth, uint32_t aHeight) override;

  bool IsD3D11() override { return true; }

private:
  HRESULT CreateFormatConverter();

  HRESULT CreateOutputSample(RefPtr<IMFSample>& aSample,
                             ID3D11Texture2D* aTexture);

  RefPtr<ID3D11Device> mDevice;
  RefPtr<ID3D11DeviceContext> mContext;
  RefPtr<IMFDXGIDeviceManager> mDXGIDeviceManager;
  RefPtr<MFTDecoder> mTransform;
  RefPtr<D3D11RecycleAllocator> mTextureClientAllocator;
  uint32_t mWidth;
  uint32_t mHeight;
  UINT mDeviceManagerToken;
};

D3D11DXVA2Manager::D3D11DXVA2Manager()
  : mWidth(0)
  , mHeight(0)
  , mDeviceManagerToken(0)
{
}

D3D11DXVA2Manager::~D3D11DXVA2Manager()
{
}

IUnknown*
D3D11DXVA2Manager::GetDXVADeviceManager()
{
  MutexAutoLock lock(mLock);
  return mDXGIDeviceManager;
}

HRESULT
D3D11DXVA2Manager::Init(nsACString& aFailureReason)
{
  HRESULT hr;

  mDevice = gfxWindowsPlatform::GetPlatform()->CreateD3D11DecoderDevice();
  if (!mDevice) {
    aFailureReason.AssignLiteral("Failed to create D3D11 device for decoder");
    return E_FAIL;
  }

  mDevice->GetImmediateContext(getter_AddRefs(mContext));
  if (!mContext) {
    aFailureReason.AssignLiteral("Failed to get immediate context for d3d11 device");
    return E_FAIL;
  }

  hr = wmf::MFCreateDXGIDeviceManager(&mDeviceManagerToken, getter_AddRefs(mDXGIDeviceManager));
  if (!SUCCEEDED(hr)) {
    aFailureReason = nsPrintfCString("MFCreateDXGIDeviceManager failed with code %X", hr);
    return hr;
  }

  hr = mDXGIDeviceManager->ResetDevice(mDevice, mDeviceManagerToken);
  if (!SUCCEEDED(hr)) {
    aFailureReason = nsPrintfCString("IMFDXGIDeviceManager::ResetDevice failed with code %X", hr);
    return hr;
  }

  mTransform = new MFTDecoder();
  hr = mTransform->Create(CLSID_VideoProcessorMFT);
  if (!SUCCEEDED(hr)) {
    aFailureReason = nsPrintfCString("MFTDecoder::Create(CLSID_VideoProcessorMFT) failed with code %X", hr);
    return hr;
  }

  hr = mTransform->SendMFTMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(mDXGIDeviceManager.get()));
  if (!SUCCEEDED(hr)) {
    aFailureReason = nsPrintfCString("MFTDecoder::SendMFTMessage(MFT_MESSAGE_SET_D3D_MANAGER) failed with code %X", hr);
    return hr;
  }

  mTextureClientAllocator = new D3D11RecycleAllocator(layers::ImageBridgeChild::GetSingleton(),
                                                      mDevice);
  mTextureClientAllocator->SetMaxPoolSize(5);

  return S_OK;
}

HRESULT
D3D11DXVA2Manager::CreateOutputSample(RefPtr<IMFSample>& aSample, ID3D11Texture2D* aTexture)
{
  RefPtr<IMFSample> sample;
  HRESULT hr = wmf::MFCreateSample(getter_AddRefs(sample));
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  RefPtr<IMFMediaBuffer> buffer;
  hr = wmf::MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), aTexture, 0, FALSE, getter_AddRefs(buffer));
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  sample->AddBuffer(buffer);

  aSample = sample;
  return S_OK;
}

HRESULT
D3D11DXVA2Manager::CopyToImage(IMFSample* aVideoSample,
                               const nsIntRect& aRegion,
                               ImageContainer* aContainer,
                               Image** aOutImage)
{
  NS_ENSURE_TRUE(aVideoSample, E_POINTER);
  NS_ENSURE_TRUE(aContainer, E_POINTER);
  NS_ENSURE_TRUE(aOutImage, E_POINTER);

  // Our video frame is stored in a non-sharable ID3D11Texture2D. We need
  // to create a copy of that frame as a sharable resource, save its share
  // handle, and put that handle into the rendering pipeline.

  ImageFormat format = ImageFormat::D3D11_SHARE_HANDLE_TEXTURE;
  RefPtr<Image> image(aContainer->CreateImage(format));
  NS_ENSURE_TRUE(image, E_FAIL);
  NS_ASSERTION(image->GetFormat() == ImageFormat::D3D11_SHARE_HANDLE_TEXTURE,
               "Wrong format?");

  D3D11ShareHandleImage* videoImage = static_cast<D3D11ShareHandleImage*>(image.get());
  HRESULT hr = videoImage->SetData(D3D11ShareHandleImage::Data(mTextureClientAllocator,
                                                               gfx::IntSize(mWidth, mHeight),
                                                               aRegion));
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = mTransform->Input(aVideoSample);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  RefPtr<IMFSample> sample;
  RefPtr<ID3D11Texture2D> texture = videoImage->GetTexture();
  hr = CreateOutputSample(sample, texture);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  RefPtr<IDXGIKeyedMutex> keyedMutex;
  hr = texture->QueryInterface(static_cast<IDXGIKeyedMutex**>(getter_AddRefs(keyedMutex)));
  NS_ENSURE_TRUE(SUCCEEDED(hr) && keyedMutex, hr);

  hr = keyedMutex->AcquireSync(0, INFINITE);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = mTransform->Output(&sample);

  keyedMutex->ReleaseSync(0);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  image.forget(aOutImage);

  return S_OK;
}

HRESULT ConfigureOutput(IMFMediaType* aOutput, void* aData)
{
  HRESULT hr = aOutput->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = aOutput->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  gfx::IntSize* size = reinterpret_cast<gfx::IntSize*>(aData);
  hr = MFSetAttributeSize(aOutput, MF_MT_FRAME_SIZE, size->width, size->height);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  return S_OK;
}

HRESULT
D3D11DXVA2Manager::ConfigureForSize(uint32_t aWidth, uint32_t aHeight)
{
  mWidth = aWidth;
  mHeight = aHeight;

  RefPtr<IMFMediaType> inputType;
  HRESULT hr = wmf::MFCreateMediaType(getter_AddRefs(inputType));
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  RefPtr<IMFAttributes> attr = mTransform->GetAttributes();

  hr = attr->SetUINT32(MF_XVP_PLAYBACK_MODE, TRUE);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = attr->SetUINT32(MF_LOW_LATENCY, FALSE);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, aWidth, aHeight);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  RefPtr<IMFMediaType> outputType;
  hr = wmf::MFCreateMediaType(getter_AddRefs(outputType));
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  gfx::IntSize size(mWidth, mHeight);
  hr = mTransform->SetMediaTypes(inputType, outputType, ConfigureOutput, &size);
  NS_ENSURE_TRUE(SUCCEEDED(hr), hr);

  return S_OK;
}

/* static */
DXVA2Manager*
DXVA2Manager::CreateD3D11DXVA(nsACString& aFailureReason)
{
  // DXVA processing takes up a lot of GPU resources, so limit the number of
  // videos we use DXVA with at any one time.
  const uint32_t dxvaLimit =
    Preferences::GetInt("media.windows-media-foundation.max-dxva-videos", 8);
  if (sDXVAVideosCount == dxvaLimit) {
    aFailureReason.AssignLiteral("Too many DXVA videos playing");
    return nullptr;
  }

  nsAutoPtr<D3D11DXVA2Manager> manager(new D3D11DXVA2Manager());
  HRESULT hr = manager->Init(aFailureReason);
  NS_ENSURE_TRUE(SUCCEEDED(hr), nullptr);

  return manager.forget();
}

DXVA2Manager::DXVA2Manager()
  : mLock("DXVA2Manager")
{
  MOZ_ASSERT(NS_IsMainThread());
  ++sDXVAVideosCount;
}

DXVA2Manager::~DXVA2Manager()
{
  MOZ_ASSERT(NS_IsMainThread());
  --sDXVAVideosCount;
}

} // namespace mozilla
