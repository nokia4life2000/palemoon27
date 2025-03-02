/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"

#include "Common.h"
#include "imgIContainer.h"
#include "imgITools.h"
#include "ImageOps.h"
#include "mozilla/gfx/2D.h"
#include "nsComponentManagerUtils.h"
#include "nsCOMPtr.h"
#include "nsIInputStream.h"
#include "nsIRunnable.h"
#include "nsIThread.h"
#include "mozilla/RefPtr.h"
#include "nsString.h"
#include "nsThreadUtils.h"

using namespace mozilla;
using namespace mozilla::gfx;
using namespace mozilla::image;


TEST(ImageDecodeToSurface, ImageModuleAvailable)
{
  // We can run into problems if XPCOM modules get initialized in the wrong
  // order. It's important that this test run first, both as a sanity check and
  // to ensure we get the module initialization order we want.
  nsCOMPtr<imgITools> imgTools =
    do_CreateInstance("@mozilla.org/image/tools;1");
  EXPECT_TRUE(imgTools != nullptr);
}

class DecodeToSurfaceRunnable : public nsRunnable
{
public:
  DecodeToSurfaceRunnable(nsIInputStream* aInputStream,
                          const ImageTestCase& aTestCase)
    : mInputStream(aInputStream)
    , mTestCase(aTestCase)
  { }

  NS_IMETHOD Run()
  {
    Go();
    return NS_OK;
  }

  void Go()
  {
    RefPtr<SourceSurface> surface =
      ImageOps::DecodeToSurface(mInputStream,
                                nsAutoCString(mTestCase.mMimeType),
                                imgIContainer::DECODE_FLAGS_DEFAULT);
    ASSERT_TRUE(surface != nullptr);

    EXPECT_EQ(SurfaceType::DATA, surface->GetType());
    EXPECT_TRUE(surface->GetFormat() == SurfaceFormat::B8G8R8X8 ||
                surface->GetFormat() == SurfaceFormat::B8G8R8A8);
    EXPECT_EQ(mTestCase.mSize, surface->GetSize());

    EXPECT_TRUE(IsSolidColor(surface, BGRAColor::Green(),
                             mTestCase.mFlags & TEST_CASE_IS_FUZZY));
  }

private:
  nsCOMPtr<nsIInputStream> mInputStream;
  ImageTestCase mTestCase;
};

static void
RunDecodeToSurface(const ImageTestCase& aTestCase)
{
  nsCOMPtr<nsIInputStream> inputStream = LoadFile(aTestCase.mPath);
  ASSERT_TRUE(inputStream != nullptr);

  nsCOMPtr<nsIThread> thread;
  nsresult rv = NS_NewThread(getter_AddRefs(thread), nullptr);
  ASSERT_TRUE(NS_SUCCEEDED(rv));

  // We run the DecodeToSurface tests off-main-thread to ensure that
  // DecodeToSurface doesn't require any main-thread-only code.
  nsCOMPtr<nsIRunnable> runnable =
    new DecodeToSurfaceRunnable(inputStream, aTestCase);
  thread->Dispatch(runnable, nsIThread::DISPATCH_SYNC);

  thread->Shutdown();
}

TEST(ImageDecodeToSurface, PNG) { RunDecodeToSurface(GreenPNGTestCase()); }
TEST(ImageDecodeToSurface, GIF) { RunDecodeToSurface(GreenGIFTestCase()); }
TEST(ImageDecodeToSurface, JPG) { RunDecodeToSurface(GreenJPGTestCase()); }
TEST(ImageDecodeToSurface, BMP) { RunDecodeToSurface(GreenBMPTestCase()); }
TEST(ImageDecodeToSurface, ICO) { RunDecodeToSurface(GreenICOTestCase()); }

TEST(ImageDecodeToSurface, AnimatedGIF)
{
  RunDecodeToSurface(GreenFirstFrameAnimatedGIFTestCase());
}

TEST(ImageDecodeToSurface, AnimatedPNG)
{
  RunDecodeToSurface(GreenFirstFrameAnimatedPNGTestCase());
}

TEST(ImageDecodeToSurface, Corrupt)
{
  ImageTestCase testCase = CorruptTestCase();

  nsCOMPtr<nsIInputStream> inputStream = LoadFile(testCase.mPath);
  ASSERT_TRUE(inputStream != nullptr);

  RefPtr<SourceSurface> surface =
    ImageOps::DecodeToSurface(inputStream,
                              nsAutoCString(testCase.mMimeType),
                              imgIContainer::DECODE_FLAGS_DEFAULT);
  EXPECT_TRUE(surface == nullptr);
}
