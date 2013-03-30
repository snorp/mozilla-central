/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/ContentClient.h"
#include "mozilla/gfx/2D.h"
#include "BasicThebesLayer.h"
#include "nsIWidget.h"
#include "gfxUtils.h"
#include "gfxPlatform.h"

namespace mozilla {

using namespace gfx;

namespace layers {

/* static */ TemporaryRef<ContentClient>
ContentClient::CreateContentClient(LayersBackend aParentBackend,
                                   CompositableForwarder* aForwarder)
{
  if (aParentBackend != LAYERS_OPENGL && aParentBackend != LAYERS_D3D11) {
    return nullptr;
  }
  if (ShadowLayerManager::SupportsDirectTexturing() ||
      PR_GetEnv("MOZ_FORCE_DOUBLE_BUFFERING")) {
    return new ContentClientDoubleBuffered(aForwarder);
  }
  return new ContentClientSingleBuffered(aForwarder);
}

ContentClientBasic::ContentClientBasic(CompositableForwarder* aForwarder,
                                       BasicLayerManager* aManager)
: ContentClient(aForwarder), mManager(aManager)
{}

already_AddRefed<gfxASurface>
ContentClientBasic::CreateBuffer(ContentType aType,
                                 const nsIntRect& aRect,
                                 uint32_t aFlags)
{
  nsRefPtr<gfxASurface> referenceSurface = GetBuffer();
  if (!referenceSurface) {
    gfxContext* defaultTarget = mManager->GetDefaultTarget();
    if (defaultTarget) {
      referenceSurface = defaultTarget->CurrentSurface();
    } else {
      nsIWidget* widget = mManager->GetRetainerWidget();
      if (!widget || !(referenceSurface = widget->GetThebesSurface())) {
        referenceSurface = mManager->GetTarget()->CurrentSurface();
      }
    }
  }
  return referenceSurface->CreateSimilarSurface(
    aType, gfxIntSize(aRect.width, aRect.height));
}

TemporaryRef<DrawTarget>
ContentClientBasic::CreateDTBuffer(ContentType aType,
                                 const nsIntRect& aRect,
                                 uint32_t aFlags)
{
  NS_RUNTIMEABORT("ContentClientBasic does not support Moz2D drawing yet!");
  // TODO[Bas] - Implement me!?
  return nullptr;
}

void
ContentClientRemote::DestroyBuffers()
{
  if (!mTextureClient) {
    return;
  }

  MOZ_ASSERT(mTextureClient->GetAccessMode() == TextureClient::ACCESS_READ_WRITE);
  // dont't call m*mTextureClient->Destroyed();
  mTextureClient = nullptr;

  DestroyFrontBuffer();

  mForwarder->DestroyThebesBuffer(this);
}

void
ContentClientRemote::BeginPaint()
{
  // XXX: So we might not have a TextureClient yet.. because it will
  // only be created by CreateBuffer.. which will deliver a locked surface!.
  if (mTextureClient) {
    SetTextureClientForBuffer(mTextureClient);
  }
}

void
ContentClientRemote::EndPaint()
{
  // XXX: We might still not have a texture client if PaintThebes
  // decided we didn't need one yet because the region to draw was empty.
  SetTextureClientForBuffer(nullptr);
  mOldTextures.Clear();

  if (mTextureClient) {
    mTextureClient->Unlock();
  }
}

TemporaryRef<DrawTarget>
ContentClientRemote::CreateDTBuffer(ContentType aType,
                                    const nsIntRect& aRect,
                                    uint32_t aFlags)
{
  NS_ABORT_IF_FALSE(!mIsNewBuffer,
                    "Bad! Did we create a buffer twice without painting?");

  mIsNewBuffer = true;

  if (mTextureClient) {
    mOldTextures.AppendElement(mTextureClient);
    DestroyBuffers();
  }
  mTextureClient = CreateTextureClient(TEXTURE_CONTENT, aFlags | HostRelease);

  mContentType = aType;
  mSize = gfx::IntSize(aRect.width, aRect.height);
  mTextureClient->EnsureAllocated(mSize, mContentType);
  // note that LockSurfaceDescriptor doesn't actually lock anything
  MOZ_ASSERT(IsSurfaceDescriptorValid(*mTextureClient->LockSurfaceDescriptor()));

  CreateFrontBufferAndNotify(aRect, aFlags | HostRelease);

  RefPtr<DrawTarget> ret = mTextureClient->LockDrawTarget();
  return ret.forget();
}

already_AddRefed<gfxASurface>
ContentClientRemote::CreateBuffer(ContentType aType,
                                  const nsIntRect& aRect,
                                  uint32_t aFlags)
{
  NS_ABORT_IF_FALSE(!mIsNewBuffer,
                    "Bad! Did we create a buffer twice without painting?");

  mIsNewBuffer = true;

  if (mTextureClient) {
    mOldTextures.AppendElement(mTextureClient);
    DestroyBuffers();
  }
  mTextureClient = CreateTextureClient(TEXTURE_CONTENT, aFlags | HostRelease);

  mContentType = aType;
  mSize = gfx::IntSize(aRect.width, aRect.height);
  mTextureClient->EnsureAllocated(mSize, mContentType);
  // note that LockSurfaceDescriptor doesn't actually lock anything
  MOZ_ASSERT(IsSurfaceDescriptorValid(*mTextureClient->LockSurfaceDescriptor()));

  CreateFrontBufferAndNotify(aRect, aFlags | HostRelease);

  nsRefPtr<gfxASurface> ret = mTextureClient->LockSurface();
  return ret.forget();
}

nsIntRegion 
ContentClientRemote::GetUpdatedRegion(const nsIntRegion& aRegionToDraw,
                                      const nsIntRegion& aVisibleRegion,
                                      bool aDidSelfCopy)
{
  nsIntRegion updatedRegion;
  if (mIsNewBuffer || aDidSelfCopy) {
    // A buffer reallocation clears both buffers. The front buffer has all the
    // content by now, but the back buffer is still clear. Here, in effect, we
    // are saying to copy all of the pixels of the front buffer to the back.
    // Also when we self-copied in the buffer, the buffer space
    // changes and some changed buffer content isn't reflected in the
    // draw or invalidate region (on purpose!).  When this happens, we
    // need to read back the entire buffer too.
    updatedRegion = aVisibleRegion;
    mIsNewBuffer = false;
  } else {
    updatedRegion = aRegionToDraw;
  }

  NS_ASSERTION(BufferRect().Contains(aRegionToDraw.GetBounds()),
               "Update outside of buffer rect!");
  NS_ABORT_IF_FALSE(mTextureClient, "should have a back buffer by now");

  return updatedRegion;
}

void
ContentClientRemote::Updated(const nsIntRegion& aRegionToDraw,
                             const nsIntRegion& aVisibleRegion,
                             bool aDidSelfCopy)
{
  nsIntRegion updatedRegion = GetUpdatedRegion(aRegionToDraw,
                                               aVisibleRegion,
                                               aDidSelfCopy);

  // don't call m*Client->Updated*()
  MOZ_ASSERT(mTextureClient);
  mTextureClient->SetAccessMode(TextureClient::ACCESS_NONE);
  LockFrontBuffer();
  mForwarder->UpdateTextureRegion(this,
                                  ThebesBufferData(BufferRect(),
                                                   BufferRotation()),
                                  updatedRegion);
}

void
ContentClientRemote::SwapBuffers(const nsIntRegion& aFrontUpdatedRegion)
{
  MOZ_ASSERT(mTextureClient->GetAccessMode() == TextureClient::ACCESS_NONE);
  MOZ_ASSERT(mTextureClient);

  mFrontAndBackBufferDiffer = true;
  mTextureClient->SetAccessMode(TextureClient::ACCESS_READ_WRITE);
}

void
ContentClientRemote::SetBackingBuffer(gfxASurface* aBuffer,
                                      const nsIntRect& aRect,
                                      const nsIntPoint& aRotation)
{
#ifdef DEBUG
  gfxIntSize prevSize = gfxIntSize(BufferRect().width, BufferRect().height);
  gfxIntSize newSize = aBuffer->GetSize();
  NS_ABORT_IF_FALSE(newSize == prevSize,
                    "Swapped-in buffer size doesn't match old buffer's!");
#endif
  nsRefPtr<gfxASurface> oldBuffer;
  oldBuffer = SetBuffer(aBuffer, aRect, aRotation);
}

ContentClientDoubleBuffered::~ContentClientDoubleBuffered()
{
  if (mTextureClient) {
    MOZ_ASSERT(mFrontClient);
    mTextureClient->SetDescriptor(SurfaceDescriptor());
    mFrontClient->SetDescriptor(SurfaceDescriptor());
  }
}

void
ContentClientDoubleBuffered::CreateFrontBufferAndNotify(const nsIntRect& aBufferRect,
                                                        uint32_t aFlags)
{
  mFrontClient = CreateTextureClient(TEXTURE_CONTENT, aFlags);
  mFrontBufferRect = aBufferRect;
  mFrontBufferRotation = nsIntPoint();
  mFrontClient->EnsureAllocated(mSize, mContentType);

  mForwarder->CreatedDoubleBuffer(this, mFrontClient, mTextureClient);
}

void
ContentClientDoubleBuffered::DestroyFrontBuffer()
{
  MOZ_ASSERT(mFrontClient);
  MOZ_ASSERT(mFrontClient->GetAccessMode() != TextureClient::ACCESS_NONE);

  // dont't call mFrontClient->Destroyed();
  mFrontClient = nullptr;
}

void
ContentClientDoubleBuffered::LockFrontBuffer()
{
  MOZ_ASSERT(mFrontClient);
  mFrontClient->SetAccessMode(TextureClient::ACCESS_NONE);
}

void
ContentClientDoubleBuffered::SwapBuffers(const nsIntRegion& aFrontUpdatedRegion)
{
  mFrontUpdatedRegion = aFrontUpdatedRegion;

  RefPtr<TextureClient> oldBack = mTextureClient;
  mTextureClient = mFrontClient;
  mFrontClient = oldBack;

  nsIntRect oldBufferRect = mBufferRect;
  mBufferRect = mFrontBufferRect;
  mFrontBufferRect = oldBufferRect;

  nsIntPoint oldBufferRotation = mBufferRotation;
  mBufferRotation = mFrontBufferRotation;
  mFrontBufferRotation = oldBufferRotation;

  MOZ_ASSERT(mFrontClient);
  mFrontClient->SetAccessMode(TextureClient::ACCESS_READ_ONLY);

  ContentClientRemote::SwapBuffers(aFrontUpdatedRegion);
}

struct AutoTextureClient {
  AutoTextureClient()
    : mTexture(nullptr)
  {}
  ~AutoTextureClient()
  {
    if (mTexture) {
      mTexture->Unlock();
    }
  }
  gfxASurface* GetSurface(TextureClient* aTexture)
  {
    MOZ_ASSERT(!mTexture);
    mTexture = aTexture;
    return mTexture->LockSurface();
  }
  DrawTarget* GetDrawTarget(TextureClient* aTexture)
  {
    MOZ_ASSERT(!mTexture);
    mTexture = aTexture;
    return mTexture->LockDrawTarget();
  }
private:
  TextureClient* mTexture;
};

void
ContentClientDoubleBuffered::SyncFrontBufferToBackBuffer()
{
  if (!mFrontAndBackBufferDiffer) {
    return;
  }
  MOZ_ASSERT(mFrontClient);
  MOZ_ASSERT(mFrontClient->GetAccessMode() == TextureClient::ACCESS_READ_ONLY);

  MOZ_LAYERS_LOG(("BasicShadowableThebes(%p): reading back <x=%d,y=%d,w=%d,h=%d>",
                  this,
                  mFrontUpdatedRegion.GetBounds().x,
                  mFrontUpdatedRegion.GetBounds().y,
                  mFrontUpdatedRegion.GetBounds().width,
                  mFrontUpdatedRegion.GetBounds().height));

  AutoTextureClient autoTextureFront;
  if (gfxPlatform::GetPlatform()->SupportsAzureContent()) {
    RotatedBuffer frontBuffer(autoTextureFront.GetDrawTarget(mFrontClient),
                              mFrontBufferRect,
                              mFrontBufferRotation);
    UpdateDestinationFrom(frontBuffer,
                          mFrontUpdatedRegion);
  } else {
    RotatedBuffer frontBuffer(autoTextureFront.GetSurface(mFrontClient),
                              mFrontBufferRect,
                              mFrontBufferRotation);
    UpdateDestinationFrom(frontBuffer,
                          mFrontUpdatedRegion);
  }

  mIsNewBuffer = false;
  mBufferRect = mFrontBufferRect;
  mBufferRotation = mFrontBufferRotation;
  mFrontAndBackBufferDiffer = false;
}

void
ContentClientDoubleBuffered::UpdateDestinationFrom(const RotatedBuffer& aSource,
                                                   const nsIntRegion& aUpdateRegion)
{
  nsRefPtr<gfxContext> destCtx =
    GetContextForQuadrantUpdate(aUpdateRegion.GetBounds());
  destCtx->SetOperator(gfxContext::OPERATOR_SOURCE);
  if (IsClippingCheap(destCtx, aUpdateRegion)) {
    gfxUtils::ClipToRegion(destCtx, aUpdateRegion);
  }

  if (gfxPlatform::GetPlatform()->SupportsAzureContent()) {
    MOZ_ASSERT(!destCtx->IsCairo());
    aSource.DrawBufferWithRotation(destCtx->GetDrawTarget());
  } else {
    aSource.DrawBufferWithRotation(destCtx);
  }
}

ContentClientSingleBuffered::~ContentClientSingleBuffered()
{
  if (mTextureClient) {
    mTextureClient->SetDescriptor(SurfaceDescriptor());
  }
}

void
ContentClientSingleBuffered::CreateFrontBufferAndNotify(const nsIntRect& aBufferRect,
                                                        uint32_t aFlags)
{
  mForwarder->CreatedSingleBuffer(this, mTextureClient);
}

void
ContentClientSingleBuffered::SyncFrontBufferToBackBuffer()
{
  if (!mFrontAndBackBufferDiffer) {
    return;
  }

  gfxASurface* backBuffer = GetBuffer();
  if (!backBuffer && mTextureClient) {
    backBuffer = mTextureClient->LockSurface();
  }

  nsRefPtr<gfxASurface> oldBuffer;
  oldBuffer = SetBuffer(backBuffer,
                        mBufferRect,
                        mBufferRotation);

  mIsNewBuffer = false;
  mFrontAndBackBufferDiffer = false;
}

}
}
