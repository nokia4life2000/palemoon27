/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TextureD3D9.h"
#include "CompositorD3D9.h"
#include "gfxContext.h"
#include "gfxImageSurface.h"
#include "Effects.h"
#include "mozilla/layers/YCbCrImageDataSerializer.h"
#include "gfxWindowsPlatform.h"
#include "gfx2DGlue.h"
#include "gfxUtils.h"
#include "mozilla/gfx/2D.h"

using namespace mozilla::gfx;

namespace mozilla {
namespace layers {

static uint32_t GetRequiredTilesD3D9(uint32_t aSize, uint32_t aMaxSize)
{
  uint32_t requiredTiles = aSize / aMaxSize;
  if (aSize % aMaxSize) {
    requiredTiles++;
  }
  return requiredTiles;
}

TextureSourceD3D9::~TextureSourceD3D9()
{
  MOZ_ASSERT(!mCreatingDeviceManager ||
             mCreatingDeviceManager->IsInTextureHostList(this),
             "Inconsistency in list of texture hosts.");
  // Remove ourselves from the list of d3d9 texture hosts.
  if (mPreviousHost) {
    MOZ_ASSERT(mPreviousHost->mNextHost == this);
    mPreviousHost->mNextHost = mNextHost;
  } else if (mCreatingDeviceManager) {
    mCreatingDeviceManager->RemoveTextureListHead(this);
  }
  if (mNextHost) {
    MOZ_ASSERT(mNextHost->mPreviousHost == this);
    mNextHost->mPreviousHost = mPreviousHost;
  }
}

already_AddRefed<TextureHost>
CreateTextureHostD3D9(const SurfaceDescriptor& aDesc,
                      ISurfaceAllocator* aDeallocator,
                      TextureFlags aFlags)
{
  RefPtr<TextureHost> result;
  switch (aDesc.type()) {
    case SurfaceDescriptor::TSurfaceDescriptorShmem:
    case SurfaceDescriptor::TSurfaceDescriptorMemory: {
      result = CreateBackendIndependentTextureHost(aDesc, aDeallocator, aFlags);
      break;
    }
    case SurfaceDescriptor::TSurfaceDescriptorD3D9: {
      result = new TextureHostD3D9(aFlags, aDesc);
      break;
    }
    case SurfaceDescriptor::TSurfaceDescriptorD3D10: {
      result = new DXGITextureHostD3D9(aFlags, aDesc);
      break;
    }
    case SurfaceDescriptor::TSurfaceDescriptorDXGIYCbCr: {
      result = new DXGIYCbCrTextureHostD3D9(aFlags, aDesc.get_SurfaceDescriptorDXGIYCbCr());
      break;
    }
    default: {
      NS_WARNING("Unsupported SurfaceDescriptor type");
    }
  }
  return result.forget();
}

static SurfaceFormat
D3D9FormatToSurfaceFormat(_D3DFORMAT format)
{
  switch (format) {
  case D3DFMT_X8R8G8B8:
    return SurfaceFormat::B8G8R8X8;
  case D3DFMT_A8R8G8B8:
    return SurfaceFormat::B8G8R8A8;
  case D3DFMT_A8:
    return SurfaceFormat::A8;
  default:
    NS_ERROR("Bad texture format");
  }
  return SurfaceFormat::UNKNOWN;
}

static _D3DFORMAT
SurfaceFormatToD3D9Format(SurfaceFormat format)
{
  switch (format) {
  case SurfaceFormat::B8G8R8X8:
    return D3DFMT_X8R8G8B8;
  case SurfaceFormat::B8G8R8A8:
    return D3DFMT_A8R8G8B8;
  case SurfaceFormat::A8:
    return D3DFMT_A8;
  default:
    NS_ERROR("Bad texture format");
  }
  return D3DFMT_A8R8G8B8;
}

CompositingRenderTargetD3D9::CompositingRenderTargetD3D9(IDirect3DTexture9* aTexture,
                                                         SurfaceInitMode aInit,
                                                         const gfx::IntRect& aRect)
  : CompositingRenderTarget(aRect.TopLeft())
  , mInitMode(aInit)
  , mInitialized(false)
{
  MOZ_COUNT_CTOR(CompositingRenderTargetD3D9);
  MOZ_ASSERT(aTexture);

  mTexture = aTexture;
  HRESULT hr = mTexture->GetSurfaceLevel(0, getter_AddRefs(mSurface));
  NS_ASSERTION(mSurface, "Couldn't create surface for texture");
  TextureSourceD3D9::SetSize(aRect.Size());
}

CompositingRenderTargetD3D9::CompositingRenderTargetD3D9(IDirect3DSurface9* aSurface,
                                                         SurfaceInitMode aInit,
                                                         const gfx::IntRect& aRect)
  : CompositingRenderTarget(aRect.TopLeft())
  , mSurface(aSurface)
  , mInitMode(aInit)
  , mInitialized(false)
{
  MOZ_COUNT_CTOR(CompositingRenderTargetD3D9);
  MOZ_ASSERT(mSurface);
  TextureSourceD3D9::SetSize(aRect.Size());
}

CompositingRenderTargetD3D9::~CompositingRenderTargetD3D9()
{
  MOZ_COUNT_DTOR(CompositingRenderTargetD3D9);
}

void
CompositingRenderTargetD3D9::BindRenderTarget(IDirect3DDevice9* aDevice)
{
  aDevice->SetRenderTarget(0, mSurface);
  if (!mInitialized &&
      mInitMode == INIT_MODE_CLEAR) {
    mInitialized = true;
    aDevice->Clear(0, 0, D3DCLEAR_TARGET, D3DCOLOR_RGBA(0, 0, 0, 0), 0, 0);
  }
}

IntSize
CompositingRenderTargetD3D9::GetSize() const
{
  return TextureSourceD3D9::GetSize();
}

/**
 * Helper method for DataToTexture.
 * The last three params are out params.
 * Returns the created texture, or null if we fail.
 */
already_AddRefed<IDirect3DTexture9>
TextureSourceD3D9::InitTextures(DeviceManagerD3D9* aDeviceManager,
                                const IntSize &aSize,
                                _D3DFORMAT aFormat,
                                RefPtr<IDirect3DSurface9>& aSurface,
                                D3DLOCKED_RECT& aLockedRect)
{
  if (!aDeviceManager) {
    return nullptr;
  }
  RefPtr<IDirect3DTexture9> result;
  // D3D9Ex doesn't support managed textures and we don't want the hassle even
  // if we don't have Ex. We could use dynamic textures
  // here but since Images are immutable that probably isn't such a great
  // idea.
  result = aDeviceManager->CreateTexture(aSize, aFormat, D3DPOOL_DEFAULT, this);
  if (!result) {
    return nullptr;
  }

  RefPtr<IDirect3DTexture9> tmpTexture =
    aDeviceManager->CreateTexture(aSize, aFormat, D3DPOOL_SYSTEMMEM, this);
  if (!tmpTexture) {
    return nullptr;
  }

  tmpTexture->GetSurfaceLevel(0, getter_AddRefs(aSurface));
  
  HRESULT hr = aSurface->LockRect(&aLockedRect, nullptr, 0);
  if (FAILED(hr) || !aLockedRect.pBits) {
    gfxCriticalError() << "Failed to lock rect initialize texture in D3D9 " << hexa(hr);
    return nullptr;
  }

  return result.forget();
}

/**
 * Helper method for DataToTexture.
 */
static void
FinishTextures(DeviceManagerD3D9* aDeviceManager,
               IDirect3DTexture9* aTexture,
               IDirect3DSurface9* aSurface)
{
  if (!aDeviceManager) {
    return;
  }

  aSurface->UnlockRect();
  RefPtr<IDirect3DSurface9> dstSurface;
  aTexture->GetSurfaceLevel(0, getter_AddRefs(dstSurface));
  aDeviceManager->device()->UpdateSurface(aSurface, nullptr, dstSurface,
                                          nullptr);
}

already_AddRefed<IDirect3DTexture9>
TextureSourceD3D9::DataToTexture(DeviceManagerD3D9* aDeviceManager,
                                 unsigned char *aData,
                                 int aStride,
                                 const IntSize &aSize,
                                 _D3DFORMAT aFormat,
                                 uint32_t aBPP)
{
  RefPtr<IDirect3DSurface9> surface;
  D3DLOCKED_RECT lockedRect;
  RefPtr<IDirect3DTexture9> texture = InitTextures(aDeviceManager, aSize, aFormat,
                                                   surface, lockedRect);
  if (!texture) {
    return nullptr;
  }

  uint32_t width = aSize.width * aBPP;

  for (int y = 0; y < aSize.height; y++) {
    memcpy((char*)lockedRect.pBits + lockedRect.Pitch * y,
            aData + aStride * y,
            width);
  }

  FinishTextures(aDeviceManager, texture, surface);

  return texture.forget();
}

already_AddRefed<IDirect3DTexture9>
TextureSourceD3D9::TextureToTexture(DeviceManagerD3D9* aDeviceManager,
                                    IDirect3DTexture9* aTexture,
                                    const IntSize& aSize,
                                    _D3DFORMAT aFormat)
{
  if (!aDeviceManager) {
    return nullptr;
  }

  RefPtr<IDirect3DTexture9> texture =
    aDeviceManager->CreateTexture(aSize, aFormat, D3DPOOL_DEFAULT, this);
  if (!texture) {
    return nullptr;
  }

  HRESULT hr = aDeviceManager->device()->UpdateTexture(aTexture, texture);
  if (FAILED(hr)) {
    return nullptr;
  }

  return texture.forget();
}

DataTextureSourceD3D9::DataTextureSourceD3D9(gfx::SurfaceFormat aFormat,
                                             CompositorD3D9* aCompositor,
                                             TextureFlags aFlags,
                                             StereoMode aStereoMode)
  : mFormat(aFormat)
  , mCompositor(aCompositor)
  , mCurrentTile(0)
  , mFlags(aFlags)
  , mIsTiled(false)
  , mIterating(false)
{
  mStereoMode = aStereoMode;
  MOZ_COUNT_CTOR(DataTextureSourceD3D9);
}

DataTextureSourceD3D9::DataTextureSourceD3D9(gfx::SurfaceFormat aFormat,
                                             gfx::IntSize aSize,
                                             CompositorD3D9* aCompositor,
                                             IDirect3DTexture9* aTexture,
                                             TextureFlags aFlags)
  : mFormat(aFormat)
  , mCompositor(aCompositor)
  , mCurrentTile(0)
  , mFlags(aFlags)
  , mIsTiled(false)
  , mIterating(false)
{
  mSize = aSize;
  mTexture = aTexture;
  mStereoMode = StereoMode::MONO;
  MOZ_COUNT_CTOR(DataTextureSourceD3D9);
}

DataTextureSourceD3D9::~DataTextureSourceD3D9()
{
  MOZ_COUNT_DTOR(DataTextureSourceD3D9);
}

IDirect3DTexture9*
DataTextureSourceD3D9::GetD3D9Texture()
{
  return mIterating ? mTileTextures[mCurrentTile]
                    : mTexture;
}

bool
DataTextureSourceD3D9::Update(gfx::DataSourceSurface* aSurface,
                              nsIntRegion* aDestRegion,
                              gfx::IntPoint* aSrcOffset)
{
  // Right now we only support full surface update. If aDestRegion is provided,
  // It will be ignored. Incremental update with a source offset is only used
  // on Mac so it is not clear that we ever will need to support it for D3D.
  MOZ_ASSERT(!aSrcOffset);

  if (!mCompositor || !mCompositor->device()) {
    NS_WARNING("No D3D device to update the texture.");
    return false;
  }
  mSize = aSurface->GetSize();

  uint32_t bpp = 0;

  _D3DFORMAT format = D3DFMT_A8R8G8B8;
  mFormat = aSurface->GetFormat();
  switch (mFormat) {
  case SurfaceFormat::B8G8R8X8:
    format = D3DFMT_X8R8G8B8;
    bpp = 4;
    break;
  case SurfaceFormat::B8G8R8A8:
    format = D3DFMT_A8R8G8B8;
    bpp = 4;
    break;
  case SurfaceFormat::A8:
    format = D3DFMT_A8;
    bpp = 1;
    break;
  default:
    NS_WARNING("Bad image format");
    return false;
  }

  int32_t maxSize = mCompositor->GetMaxTextureSize();
  DeviceManagerD3D9* deviceManager = gfxWindowsPlatform::GetPlatform()->GetD3D9DeviceManager();
  if ((mSize.width <= maxSize && mSize.height <= maxSize) ||
      (mFlags & TextureFlags::DISALLOW_BIGIMAGE)) {
    mTexture = DataToTexture(deviceManager,
                             aSurface->GetData(), aSurface->Stride(),
                             IntSize(mSize), format, bpp);
    if (!mTexture) {
      NS_WARNING("Could not upload texture");
      Reset();
      return false;
    }
    mIsTiled = false;
  } else {
    mIsTiled = true;
    uint32_t tileCount = GetRequiredTilesD3D9(mSize.width, maxSize) *
                         GetRequiredTilesD3D9(mSize.height, maxSize);
    mTileTextures.resize(tileCount);
    mTexture = nullptr;

    for (uint32_t i = 0; i < tileCount; i++) {
      IntRect tileRect = GetTileRect(i);
      unsigned char* data = aSurface->GetData() +
                            tileRect.y * aSurface->Stride() +
                            tileRect.x * bpp;
      mTileTextures[i] = DataToTexture(deviceManager,
                                       data,
                                       aSurface->Stride(),
                                       IntSize(tileRect.width, tileRect.height),
                                       format,
                                       bpp);
      if (!mTileTextures[i]) {
        NS_WARNING("Could not upload texture");
        Reset();
        return false;
      }
    }
  }

  return true;
}

void
DataTextureSourceD3D9::SetCompositor(Compositor* aCompositor)
{
  MOZ_ASSERT(aCompositor);
  CompositorD3D9* d3dCompositor = static_cast<CompositorD3D9*>(aCompositor);
  if (mCompositor && mCompositor != d3dCompositor) {
    Reset();
  }
  mCompositor = d3dCompositor;
}

void
DataTextureSourceD3D9::Reset()
{
  mSize.width = 0;
  mSize.height = 0;
  mIsTiled = false;
  mTexture = nullptr;
  mTileTextures.clear();
}

IntRect
DataTextureSourceD3D9::GetTileRect(uint32_t aTileIndex) const
{
  uint32_t maxSize = mCompositor->GetMaxTextureSize();
  uint32_t horizontalTiles = GetRequiredTilesD3D9(mSize.width, maxSize);
  uint32_t verticalTiles = GetRequiredTilesD3D9(mSize.height, maxSize);

  uint32_t verticalTile = aTileIndex / horizontalTiles;
  uint32_t horizontalTile = aTileIndex % horizontalTiles;

  return IntRect(horizontalTile * maxSize,
                 verticalTile * maxSize,
                 horizontalTile < (horizontalTiles - 1) ? maxSize : mSize.width % maxSize,
                 verticalTile < (verticalTiles - 1) ? maxSize : mSize.height % maxSize);
}

IntRect
DataTextureSourceD3D9::GetTileRect()
{
  return GetTileRect(mCurrentTile);
}

TextureClientD3D9::TextureClientD3D9(ISurfaceAllocator* aAllocator,
                                     gfx::SurfaceFormat aFormat,
                                     TextureFlags aFlags)
  : TextureClient(aAllocator, aFlags)
  , mFormat(aFormat)
  , mIsLocked(false)
  , mNeedsClear(false)
  , mNeedsClearWhite(false)
  , mLockRect(false)
{
  MOZ_COUNT_CTOR(TextureClientD3D9);
}

TextureClientD3D9::~TextureClientD3D9()
{
  MOZ_COUNT_DTOR(TextureClientD3D9);
}

already_AddRefed<TextureClient>
TextureClientD3D9::CreateSimilar(TextureFlags aFlags, TextureAllocationFlags aAllocFlags) const
{
  RefPtr<TextureClient> tex = new TextureClientD3D9(mAllocator, mFormat,
                                                    mFlags | aFlags);

  if (!tex->AllocateForSurface(mSize, aAllocFlags)) {
    return nullptr;
  }

  return tex.forget();
}

bool
TextureClientD3D9::Lock(OpenMode aMode)
{
  MOZ_ASSERT(!mIsLocked);
  if (!IsValid() || !IsAllocated()) {
    return false;
  }

  if (!gfxWindowsPlatform::GetPlatform()->GetD3D9Device()) {
    // If the device has failed then we should not lock the surface,
    // even if we could.
    mD3D9Surface = nullptr;
    return false;
  }

  if (!mD3D9Surface) {
    HRESULT hr = mTexture->GetSurfaceLevel(0, getter_AddRefs(mD3D9Surface));
    if (FAILED(hr)) {
      NS_WARNING("Failed to get texture surface level.");
      return false;
    }
  }

  mIsLocked = true;

  return true;
}

void
TextureClientD3D9::Unlock()
{
  MOZ_ASSERT(mIsLocked, "Unlocked called while the texture is not locked!");
  if (!mIsLocked) {
    return;
  }

  if (mDrawTarget) {
    mDrawTarget->Flush();
    mDrawTarget = nullptr;
  }

  if (mLockRect) {
    mD3D9Surface->UnlockRect();
    mLockRect = false;
  }

  mIsLocked = false;
}

bool
TextureClientD3D9::ToSurfaceDescriptor(SurfaceDescriptor& aOutDescriptor)
{
  MOZ_ASSERT(IsValid());
  if (!IsAllocated()) {
    return false;
  }

  mTexture->AddRef(); // Release in TextureHostD3D9::TextureHostD3D9
  aOutDescriptor = SurfaceDescriptorD3D9(reinterpret_cast<uintptr_t>(mTexture.get()));
  return true;
}

gfx::DrawTarget*
TextureClientD3D9::BorrowDrawTarget()
{
  MOZ_ASSERT(mIsLocked && mD3D9Surface);
  if (!mIsLocked || !mD3D9Surface) {
    NS_WARNING("Calling BorrowDrawTarget on an Unlocked TextureClient");
    return nullptr;
  }

  if (mDrawTarget) {
    return mDrawTarget;
  }

  if (ContentForFormat(mFormat) == gfxContentType::COLOR) {
    RefPtr<gfxASurface> surface = new gfxWindowsSurface(mD3D9Surface);
    if (!surface || surface->CairoStatus()) {
      NS_WARNING("Could not create surface for d3d9 surface");
      return nullptr;
    }
    mDrawTarget =
      gfxPlatform::GetPlatform()->CreateDrawTargetForSurface(surface, mSize);
  } else {
    // gfxWindowsSurface don't support transparency so we can't use the d3d9
    // windows surface optimization.
    // Instead we have to use a gfxImageSurface and fallback for font drawing.
    D3DLOCKED_RECT rect;
    HRESULT hr = mD3D9Surface->LockRect(&rect, nullptr, 0);
    if (FAILED(hr) || !rect.pBits) {
      gfxCriticalError() << "Failed to lock rect borrowing the target in D3D9 " << hexa(hr);
      return nullptr;
    }
    mDrawTarget =
     gfxPlatform::GetPlatform()->CreateDrawTargetForData((uint8_t*)rect.pBits, mSize,
                                                         rect.Pitch, mFormat);
    mLockRect = true;
  }

  if (mNeedsClear) {
    mDrawTarget->ClearRect(Rect(0, 0, GetSize().width, GetSize().height));
    mNeedsClear = false;
  }
  if (mNeedsClearWhite) {
    mDrawTarget->FillRect(Rect(0, 0, GetSize().width, GetSize().height), ColorPattern(Color(1.0, 1.0, 1.0, 1.0)));
    mNeedsClearWhite = false;
  }

  return mDrawTarget;
}

void
TextureClientD3D9::UpdateFromSurface(gfx::SourceSurface* aSurface)
{
  MOZ_ASSERT(mIsLocked && mD3D9Surface);

  // gfxWindowsSurface don't support transparency so we can't use the d3d9
  // windows surface optimization.
  // Instead we have to use a gfxImageSurface and fallback for font drawing.
  D3DLOCKED_RECT rect;
  HRESULT hr = mD3D9Surface->LockRect(&rect, nullptr, 0);
  if (FAILED(hr) || !rect.pBits) {
    gfxCriticalError() << "Failed to lock rect borrowing the target in D3D9 " << hexa(hr);
    return;
  }

  RefPtr<DataSourceSurface> srcSurf = aSurface->GetDataSurface();

  if (!srcSurf) {
    gfxCriticalError() << "Failed to GetDataSurface in UpdateFromSurface.";
    return;
  }

  if (mSize != srcSurf->GetSize() || mFormat != srcSurf->GetFormat()) {
    gfxCriticalError() << "Attempt to update texture client from a surface with a different size or format! This: " << mSize << " " << mFormat << " Other: " << srcSurf->GetSize() << " " << srcSurf->GetFormat();
    return;
  }

  DataSourceSurface::MappedSurface sourceMap;
  if (!srcSurf->Map(DataSourceSurface::READ, &sourceMap)) {
    gfxCriticalError() << "Failed to map source surface for UpdateFromSurface.";
    return;
  }

  for (int y = 0; y < srcSurf->GetSize().height; y++) {
    memcpy((uint8_t*)rect.pBits + rect.Pitch * y,
           sourceMap.mData + sourceMap.mStride * y,
           srcSurf->GetSize().width * BytesPerPixel(srcSurf->GetFormat()));
  }

  srcSurf->Unmap();
  mD3D9Surface->UnlockRect();
}

bool
TextureClientD3D9::AllocateForSurface(gfx::IntSize aSize, TextureAllocationFlags aFlags)
{
  MOZ_ASSERT(!IsAllocated());
  mSize = aSize;
  _D3DFORMAT format = SurfaceFormatToD3D9Format(mFormat);

  DeviceManagerD3D9* deviceManager = gfxWindowsPlatform::GetPlatform()->GetD3D9DeviceManager();
  if (!deviceManager ||
      !(mTexture = deviceManager->CreateTexture(mSize, format, D3DPOOL_SYSTEMMEM, nullptr))) {
    NS_WARNING("Could not create d3d9 texture");
    return false;
  }

  mNeedsClear = aFlags & ALLOC_CLEAR_BUFFER;
  mNeedsClearWhite = aFlags & ALLOC_CLEAR_BUFFER_WHITE;

  MOZ_ASSERT(mTexture);
  return true;
}

SharedTextureClientD3D9::SharedTextureClientD3D9(ISurfaceAllocator* aAllocator,
                                                 gfx::SurfaceFormat aFormat,
                                                 TextureFlags aFlags)
  : TextureClient(aAllocator, aFlags)
  , mFormat(aFormat)
  , mHandle(0)
  , mIsLocked(false)
{
  MOZ_COUNT_CTOR(SharedTextureClientD3D9);
}

SharedTextureClientD3D9::~SharedTextureClientD3D9()
{
  MOZ_COUNT_DTOR(SharedTextureClientD3D9);
}

void
SharedTextureClientD3D9::FinalizeOnIPDLThread()
{
  if (mTexture && mActor) {
    KeepUntilFullDeallocation(MakeUnique<TKeepAlive<IDirect3DTexture9>>(mTexture));
  }
  if (mTexture) {
    gfxWindowsPlatform::sD3D9SharedTextureUsed -= mDesc.Width * mDesc.Height * 4;
  }
}

// static
already_AddRefed<SharedTextureClientD3D9>
SharedTextureClientD3D9::Create(ISurfaceAllocator* aAllocator,
                                gfx::SurfaceFormat aFormat,
                                TextureFlags aFlags,
                                IDirect3DDevice9* aDevice,
                                const gfx::IntSize& aSize)
{
  MOZ_ASSERT(aFormat == gfx::SurfaceFormat::B8G8R8X8);

  RefPtr<IDirect3DTexture9> texture;
  HANDLE shareHandle = nullptr;
  HRESULT hr = aDevice->CreateTexture(aSize.width,
                                      aSize.height,
                                      1,
                                      D3DUSAGE_RENDERTARGET,
                                      D3DFMT_X8R8G8B8,
                                      D3DPOOL_DEFAULT,
                                      getter_AddRefs(texture),
                                      &shareHandle);
  NS_ENSURE_TRUE(SUCCEEDED(hr) && shareHandle, nullptr);

  RefPtr<SharedTextureClientD3D9> client =
    new SharedTextureClientD3D9(aAllocator,
                                aFormat,
                                aFlags);

  client->mDevice = aDevice;
  client->mTexture = texture;
  client->mHandle = shareHandle;
  texture->GetLevelDesc(0, &client->mDesc);

  gfxWindowsPlatform::sD3D9SharedTextureUsed += aSize.width * aSize.height * 4;
  return client.forget();
}

already_AddRefed<IDirect3DSurface9>
SharedTextureClientD3D9::GetD3D9Surface() const
{
  RefPtr<IDirect3DSurface9> textureSurface;
  HRESULT hr = mTexture->GetSurfaceLevel(0, getter_AddRefs(textureSurface));
  NS_ENSURE_TRUE(SUCCEEDED(hr), nullptr);

  return textureSurface.forget();
}

bool
SharedTextureClientD3D9::Lock(OpenMode)
{
  MOZ_ASSERT(!mIsLocked);
  if (!IsValid()) {
    return false;
  }
  mIsLocked = true;
  return true;
}

void
SharedTextureClientD3D9::Unlock()
{
  MOZ_ASSERT(mIsLocked, "Unlock called while the texture is not locked!");
}

bool
SharedTextureClientD3D9::ToSurfaceDescriptor(SurfaceDescriptor& aOutDescriptor)
{
  MOZ_ASSERT(IsValid());
  if (!IsAllocated()) {
    return false;
  }

  aOutDescriptor = SurfaceDescriptorD3D10((WindowsHandle)(mHandle), mFormat, GetSize());
  return true;
}

TextureHostD3D9::TextureHostD3D9(TextureFlags aFlags,
                                 const SurfaceDescriptorD3D9& aDescriptor)
  : TextureHost(aFlags)
  , mFormat(SurfaceFormat::UNKNOWN)
  , mIsLocked(false)
{
  mTexture = reinterpret_cast<IDirect3DTexture9*>(aDescriptor.texture());
  MOZ_ASSERT(mTexture);
  mTexture->Release(); // see AddRef in TextureClientD3D9::ToSurfaceDescriptor
  MOZ_ASSERT(mTexture);
  D3DSURFACE_DESC desc;
  HRESULT hr = mTexture->GetLevelDesc(0, &desc);
  if (!FAILED(hr)) {
    mFormat = D3D9FormatToSurfaceFormat(desc.Format);
    mSize.width = desc.Width;
    mSize.height = desc.Height;
  }
}

bool
DataTextureSourceD3D9::UpdateFromTexture(IDirect3DTexture9* aTexture,
                                         const nsIntRegion* aRegion)
{
  MOZ_ASSERT(aTexture);

  D3DSURFACE_DESC desc;
  HRESULT hr = aTexture->GetLevelDesc(0, &desc);
  if (FAILED(hr)) {
    return false;
  } else {
    // If we changed the compositor, the size might have been reset to zero
    // Otherwise the texture size must not change.
    MOZ_ASSERT(mFormat == D3D9FormatToSurfaceFormat(desc.Format));
    MOZ_ASSERT(!mSize.width || mSize.width == desc.Width);
    MOZ_ASSERT(!mSize.height || mSize.height == desc.Height);
    mSize = IntSize(desc.Width, desc.Height);
  }

  DeviceManagerD3D9* dm = gfxWindowsPlatform::GetPlatform()->GetD3D9DeviceManager();
  if (!dm || !dm->device()) {
    return false;
  }

  if (!mTexture) {
    mTexture = dm->CreateTexture(mSize, SurfaceFormatToD3D9Format(mFormat),
                                 D3DPOOL_DEFAULT, this);
    if (!mTexture) {
      NS_WARNING("Failed to create a texture");
      return false;
    }
  }

  RefPtr<IDirect3DSurface9> srcSurface;
  RefPtr<IDirect3DSurface9> dstSurface;

  hr = aTexture->GetSurfaceLevel(0, getter_AddRefs(srcSurface));
  if (FAILED(hr)) {
    return false;
  }
  hr = mTexture->GetSurfaceLevel(0, getter_AddRefs(dstSurface));
  if (FAILED(hr)) {
    return false;
  }

  if (aRegion) {
    nsIntRegionRectIterator iter(*aRegion);
    const IntRect *iterRect;
    while ((iterRect = iter.Next())) {
      RECT rect;
      rect.left = iterRect->x;
      rect.top = iterRect->y;
      rect.right = iterRect->XMost();
      rect.bottom = iterRect->YMost();

      POINT point;
      point.x = iterRect->x;
      point.y = iterRect->y;
      hr = dm->device()->UpdateSurface(srcSurface, &rect, dstSurface, &point);
      if (FAILED(hr)) {
        NS_WARNING("Failed Update the surface");
        return false;
      }
    }
  } else {
    hr = dm->device()->UpdateSurface(srcSurface, nullptr, dstSurface, nullptr);
    if (FAILED(hr)) {
      NS_WARNING("Failed Update the surface");
      return false;
    }
  }
  mIsTiled = false;
  return true;
}

void
TextureHostD3D9::UpdatedInternal(const nsIntRegion* aRegion)
{
  MOZ_ASSERT(mTexture);
  if (!mTexture) {
    return;
  }

  if (!mTextureSource) {
    mTextureSource = new DataTextureSourceD3D9(mFormat, mSize, mCompositor,
                                               nullptr, mFlags);
  }

  if (!mTextureSource->UpdateFromTexture(mTexture, aRegion)) {
    gfxCriticalError() << "[D3D9] DataTextureSourceD3D9::UpdateFromTexture failed";
  }
}

IDirect3DDevice9*
TextureHostD3D9::GetDevice()
{
  return mCompositor ? mCompositor->device() : nullptr;
}

void
TextureHostD3D9::SetCompositor(Compositor* aCompositor)
{
  mCompositor = static_cast<CompositorD3D9*>(aCompositor);
  if (mTextureSource) {
    mTextureSource->SetCompositor(aCompositor);
  }
}

bool
TextureHostD3D9::BindTextureSource(CompositableTextureSourceRef& aTexture)
{
  MOZ_ASSERT(mIsLocked);
  MOZ_ASSERT(mTextureSource);
  aTexture = mTextureSource;
  return !!aTexture;
}

bool
TextureHostD3D9::Lock()
{
  MOZ_ASSERT(!mIsLocked);
  // XXX - Currently if a TextureHostD3D9 is created but Update is never called,
  // it will not have a TextureSource although it could since it has a valid
  // D3D9 texture.
  mIsLocked = !!mTextureSource;
  return mIsLocked;
}

void
TextureHostD3D9::Unlock()
{
  MOZ_ASSERT(mIsLocked);
  mIsLocked = false;
}

void
TextureHostD3D9::DeallocateDeviceData()
{
  mTextureSource = nullptr;
}

DXGITextureHostD3D9::DXGITextureHostD3D9(TextureFlags aFlags,
  const SurfaceDescriptorD3D10& aDescriptor)
  : TextureHost(aFlags)
  , mHandle(aDescriptor.handle())
  , mFormat(aDescriptor.format())
  , mSize(aDescriptor.size())
  , mIsLocked(false)
{
  MOZ_ASSERT(mHandle);
  OpenSharedHandle();
}

IDirect3DDevice9*
DXGITextureHostD3D9::GetDevice()
{
  return mCompositor ? mCompositor->device() : nullptr;
}

void
DXGITextureHostD3D9::OpenSharedHandle()
{
  MOZ_ASSERT(!mTextureSource);

  if (!GetDevice()) {
    return;
  }

  RefPtr<IDirect3DTexture9> texture;
  HRESULT hr = GetDevice()->CreateTexture(mSize.width, mSize.height, 1,
                                          D3DUSAGE_RENDERTARGET,
                                          SurfaceFormatToD3D9Format(mFormat),
                                          D3DPOOL_DEFAULT,
                                          getter_AddRefs(texture),
                                          (HANDLE*)&mHandle);
  if (FAILED(hr)) {
    NS_WARNING("Failed to open shared texture");
    return;
  }

  mTextureSource = new DataTextureSourceD3D9(mFormat, mSize, mCompositor, texture);

  return;
}

bool
DXGITextureHostD3D9::BindTextureSource(CompositableTextureSourceRef& aTexture)
{
  MOZ_ASSERT(mIsLocked);
  MOZ_ASSERT(mTextureSource);
  aTexture = mTextureSource;
  return !!aTexture;
}

bool
DXGITextureHostD3D9::Lock()
{
  MOZ_ASSERT(!mIsLocked);

  if (!GetDevice()) {
    return false;
  }

  if (!mTextureSource) {
    OpenSharedHandle();
  }
  mIsLocked = !!mTextureSource;
  return mIsLocked;
}

void
DXGITextureHostD3D9::Unlock()
{
  MOZ_ASSERT(mIsLocked);
  mIsLocked = false;
}

void
DXGITextureHostD3D9::SetCompositor(Compositor* aCompositor)
{
  MOZ_ASSERT(aCompositor);
  mCompositor = static_cast<CompositorD3D9*>(aCompositor);
}

void
DXGITextureHostD3D9::DeallocateDeviceData()
{
  mTextureSource = nullptr;
}

DXGIYCbCrTextureHostD3D9::DXGIYCbCrTextureHostD3D9(TextureFlags aFlags,
                                                   const SurfaceDescriptorDXGIYCbCr& aDescriptor)
 : TextureHost(aFlags)
 , mSize(aDescriptor.size())
 , mSizeY(aDescriptor.sizeY())
 , mSizeCbCr(aDescriptor.sizeCbCr())
 , mIsLocked(false)
{
  mHandles[0] = reinterpret_cast<HANDLE>(aDescriptor.handleY());
  mHandles[1] = reinterpret_cast<HANDLE>(aDescriptor.handleCb());
  mHandles[2] = reinterpret_cast<HANDLE>(aDescriptor.handleCr());
}

IDirect3DDevice9*
DXGIYCbCrTextureHostD3D9::GetDevice()
{
  return mCompositor ? mCompositor->device() : nullptr;
}

void
DXGIYCbCrTextureHostD3D9::SetCompositor(Compositor* aCompositor)
{
  MOZ_ASSERT(aCompositor);
  mCompositor = static_cast<CompositorD3D9*>(aCompositor);
}

bool
DXGIYCbCrTextureHostD3D9::Lock()
{
  if (!GetDevice()) {
    NS_WARNING("trying to lock a TextureHost without a D3D device");
    return false;
  }
  if (!mTextureSources[0]) {
    if (!mHandles[0]) {
      return false;
    }

    if (FAILED(GetDevice()->CreateTexture(mSizeY.width, mSizeY.height,
                                          1, 0, D3DFMT_A8, D3DPOOL_DEFAULT,
                                          getter_AddRefs(mTextures[0]), &mHandles[0]))) {
      return false;
    }

    if (FAILED(GetDevice()->CreateTexture(mSizeCbCr.width, mSizeCbCr.height,
                                          1, 0, D3DFMT_A8, D3DPOOL_DEFAULT,
                                          getter_AddRefs(mTextures[1]), &mHandles[1]))) {
      return false;
    }

    if (FAILED(GetDevice()->CreateTexture(mSizeCbCr.width, mSizeCbCr.height,
                                          1, 0, D3DFMT_A8, D3DPOOL_DEFAULT,
                                          getter_AddRefs(mTextures[2]), &mHandles[2]))) {
      return false;
    }

    mTextureSources[0] = new DataTextureSourceD3D9(SurfaceFormat::A8, mSize, mCompositor, mTextures[0]);
    mTextureSources[1] = new DataTextureSourceD3D9(SurfaceFormat::A8, mSize, mCompositor, mTextures[1]);
    mTextureSources[2] = new DataTextureSourceD3D9(SurfaceFormat::A8, mSize, mCompositor, mTextures[2]);
    mTextureSources[0]->SetNextSibling(mTextureSources[1]);
    mTextureSources[1]->SetNextSibling(mTextureSources[2]);
  }

  mIsLocked = true;
  return mIsLocked;
}

void
DXGIYCbCrTextureHostD3D9::Unlock()
{
  MOZ_ASSERT(mIsLocked);
  mIsLocked = false;
}

bool
DXGIYCbCrTextureHostD3D9::BindTextureSource(CompositableTextureSourceRef& aTexture)
{
  MOZ_ASSERT(mIsLocked);
  // If Lock was successful we must have a valid TextureSource.
  MOZ_ASSERT(mTextureSources[0] && mTextureSources[1] && mTextureSources[2]);
  aTexture = mTextureSources[0].get();
  return !!aTexture;
}

}
}
