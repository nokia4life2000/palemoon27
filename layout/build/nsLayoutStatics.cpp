/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/basictypes.h"

#include "nsLayoutStatics.h"
#include "nscore.h"

#include "nsAttrValue.h"
#include "nsAutoCopyListener.h"
#include "nsColorNames.h"
#include "nsComputedDOMStyle.h"
#include "nsContentDLF.h"
#include "nsContentUtils.h"
#include "nsCSSAnonBoxes.h"
#include "mozilla/css/ErrorReporter.h"
#include "nsCSSKeywords.h"
#include "nsCSSParser.h"
#include "nsCSSProps.h"
#include "nsCSSPseudoClasses.h"
#include "nsCSSPseudoElements.h"
#include "nsCSSRendering.h"
#include "mozilla/dom/Attr.h"
#include "nsDOMClassInfo.h"
#include "nsEditorEventListener.h"
#include "mozilla/EventListenerManager.h"
#include "nsFrame.h"
#include "nsGlobalWindow.h"
#include "nsGkAtoms.h"
#include "nsImageFrame.h"
#include "nsLayoutStylesheetCache.h"
#include "mozilla/RuleProcessorCache.h"
#include "nsPrincipal.h"
#include "nsRange.h"
#include "nsRegion.h"
#include "nsRepeatService.h"
#include "nsFloatManager.h"
#include "nsSprocketLayout.h"
#include "nsStackLayout.h"
#include "nsStyleSet.h"
#include "nsTextControlFrame.h"
#include "nsXBLService.h"
#include "txMozillaXSLTProcessor.h"
#include "nsTreeSanitizer.h"
#include "nsCellMap.h"
#include "nsTextFrame.h"
#include "nsCCUncollectableMarker.h"
#include "nsTextFragment.h"
#include "nsCSSRuleProcessor.h"
#include "nsCORSListenerProxy.h"
#include "nsHTMLDNSPrefetch.h"
#include "nsHtml5Module.h"
#include "mozilla/dom/FallbackEncoding.h"
#include "nsFocusManager.h"
#include "nsListControlFrame.h"
#include "mozilla/dom/HTMLInputElement.h"
#include "SVGElementFactory.h"
#include "nsSVGUtils.h"
#include "nsMathMLAtoms.h"
#include "nsMathMLOperators.h"
#include "Navigator.h"
#include "DOMStorageObserver.h"
#include "CacheObserver.h"
#include "DisplayItemClip.h"
#include "ActiveLayerTracker.h"
#include "CounterStyleManager.h"
#include "FrameLayerBuilder.h"
#include "mozilla/dom/RequestSyncWifiService.h"
#include "AnimationCommon.h"
#include "LayerAnimationInfo.h"

#include "AudioChannelService.h"
#include "mozilla/dom/DataStoreService.h"
#include "mozilla/dom/PromiseDebugging.h"

#ifdef MOZ_XUL
#include "nsXULPopupManager.h"
#include "nsXULContentUtils.h"
#include "nsXULPrototypeCache.h"
#include "nsXULTooltipListener.h"

#include "inDOMView.h"

#include "nsMenuBarListener.h"
#endif

#include "nsHTMLEditor.h"
#include "nsTextServicesDocument.h"

#ifdef MOZ_WEBSPEECH
#include "nsSynthVoiceRegistry.h"
#endif

#ifdef MOZ_ANDROID_OMX
#include "AndroidMediaPluginHost.h"
#endif

#ifdef MOZ_GSTREAMER
#include "GStreamerFormatHelper.h"
#endif

#ifdef MOZ_FFMPEG
#include "FFmpegRuntimeLinker.h"
#endif

#include "CubebUtils.h"
#include "Latency.h"
#include "WebAudioUtils.h"

#ifdef MOZ_WIDGET_GONK
#include "nsVolumeService.h"
#include "SpeakerManagerService.h"
using namespace mozilla::system;
#endif

#include "nsError.h"

#include "nsJSEnvironment.h"
#include "nsContentSink.h"
#include "nsFrameMessageManager.h"
#include "nsRefreshDriver.h"
#include "nsDOMMutationObserver.h"
#include "nsHyphenationManager.h"
#include "nsEditorSpellCheck.h"
#include "nsWindowMemoryReporter.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/ProcessPriorityManager.h"
#include "nsPermissionManager.h"
#include "nsCookieService.h"
#include "nsApplicationCacheService.h"
#include "mozilla/dom/time/DateCacheCleaner.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/IMEStateManager.h"
#include "nsDocument.h"
#include "mozilla/dom/HTMLVideoElement.h"
#include "CameraPreferences.h"
#include "TouchManager.h"
#include "MediaDecoder.h"
#include "mozilla/layers/CompositorLRU.h"
#include "mozilla/dom/devicestorage/DeviceStorageStatics.h"

using namespace mozilla;
using namespace mozilla::net;
using namespace mozilla::dom;
using namespace mozilla::dom::ipc;

nsrefcnt nsLayoutStatics::sLayoutStaticRefcnt = 0;

nsresult
nsLayoutStatics::Initialize()
{
  NS_ASSERTION(sLayoutStaticRefcnt == 0,
               "nsLayoutStatics isn't zero!");

  sLayoutStaticRefcnt = 1;
  NS_LOG_ADDREF(&sLayoutStaticRefcnt, sLayoutStaticRefcnt,
                "nsLayoutStatics", 1);

  nsresult rv;

  ContentParent::StartUp();

  // Register all of our atoms once
  nsCSSAnonBoxes::AddRefAtoms();
  nsCSSPseudoClasses::AddRefAtoms();
  nsCSSPseudoElements::AddRefAtoms();
  nsCSSKeywords::AddRefTable();
  nsCSSProps::AddRefTable();
  nsColorNames::AddRefTable();
  nsGkAtoms::AddRefAtoms();

  StartupJSEnvironment();
  rv = nsRegion::InitStatic();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize nsRegion");
    return rv;
  }

  nsGlobalWindow::Init();
  Navigator::Init();
  nsXBLService::Init();

  rv = nsContentUtils::Init();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize nsContentUtils");
    return rv;
  }

  rv = nsAttrValue::Init();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize nsAttrValue");
    return rv;
  }

  rv = nsTextFragment::Init();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize nsTextFragment");
    return rv;
  }

  nsCellMap::Init();

  nsCSSRendering::Init();

  nsTextFrameTextRunCache::Init();

  rv = nsHTMLDNSPrefetch::Initialize();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize HTML DNS prefetch");
    return rv;
  }

#ifdef MOZ_XUL
  rv = nsXULContentUtils::Init();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize nsXULContentUtils");
    return rv;
  }

#endif

  nsMathMLOperators::AddRefTable();

  nsTextServicesDocument::RegisterAtoms();

#ifdef DEBUG
  nsFrame::DisplayReflowStartup();
#endif
  Attr::Initialize();

  rv = txMozillaXSLTProcessor::Startup();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize txMozillaXSLTProcessor");
    return rv;
  }

  rv = DOMStorageObserver::Init();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize DOMStorageObserver");
    return rv;
  }

  rv = nsCCUncollectableMarker::Init();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize nsCCUncollectableMarker");
    return rv;
  }

  nsCSSParser::Startup();
  nsCSSRuleProcessor::Startup();

#ifdef MOZ_XUL
  rv = nsXULPopupManager::Init();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize nsXULPopupManager");
    return rv;
  }
#endif

  rv = nsFocusManager::Init();
  if (NS_FAILED(rv)) {
    NS_ERROR("Could not initialize nsFocusManager");
    return rv;
  }

  AsyncLatencyLogger::InitializeStatics();
  CubebUtils::InitLibrary();

  nsContentSink::InitializeStatics();
  nsHtml5Module::InitializeStatics();
  mozilla::dom::FallbackEncoding::Initialize();
  nsLayoutUtils::Initialize();
  nsIPresShell::InitializeStatics();
  TouchManager::InitializeStatics();
  nsRefreshDriver::InitializeStatics();
  nsPrincipal::InitializeStatics();

  nsCORSListenerProxy::Startup();

  NS_SealStaticAtomTable();

  nsWindowMemoryReporter::Init();

  SVGElementFactory::Init();
  nsSVGUtils::Init();

  ProcessPriorityManager::Init();

  nsPermissionManager::AppClearDataObserverInit();
  nsCookieService::AppClearDataObserverInit();
  nsApplicationCacheService::AppClearDataObserverInit();

  HTMLVideoElement::Init();

#ifdef MOZ_XUL
  nsMenuBarListener::InitializeStatics();
#endif

  CacheObserver::Init();

  CounterStyleManager::InitializeBuiltinCounterStyles();

  CameraPreferences::Initialize();

  IMEStateManager::Init();

  ServiceWorkerRegistrar::Initialize();

#ifdef MOZ_B2G
  RequestSyncWifiService::Init();
#endif

#ifdef DEBUG
  nsStyleContext::Initialize();
  mozilla::LayerAnimationInfo::Initialize();
#endif

  MediaDecoder::InitStatics();

  PromiseDebugging::Init();

  layers::CompositorLRU::Init();

  mozilla::dom::devicestorage::DeviceStorageStatics::Initialize();

  return NS_OK;
}

void
nsLayoutStatics::Shutdown()
{
  // Don't need to shutdown nsWindowMemoryReporter, that will be done by the
  // memory reporter manager.

  nsMessageManagerScriptExecutor::Shutdown();
  nsFocusManager::Shutdown();
#ifdef MOZ_XUL
  nsXULPopupManager::Shutdown();
#endif
  DOMStorageObserver::Shutdown();
  txMozillaXSLTProcessor::Shutdown();
  Attr::Shutdown();
  EventListenerManager::Shutdown();
  IMEStateManager::Shutdown();
  nsCSSParser::Shutdown();
  nsCSSRuleProcessor::Shutdown();
  nsTextFrameTextRunCache::Shutdown();
  nsHTMLDNSPrefetch::Shutdown();
  nsCSSRendering::Shutdown();
#ifdef DEBUG
  nsFrame::DisplayReflowShutdown();
#endif
  nsCellMap::Shutdown();
  ActiveLayerTracker::Shutdown();

  // Release all of our atoms
  nsColorNames::ReleaseTable();
  nsCSSProps::ReleaseTable();
  nsCSSKeywords::ReleaseTable();
  nsRepeatService::Shutdown();
  nsStackLayout::Shutdown();
  nsBox::Shutdown();

#ifdef MOZ_XUL
  nsXULContentUtils::Finish();
  nsXULPrototypeCache::ReleaseGlobals();
  nsSprocketLayout::Shutdown();
#endif

  SVGElementFactory::Shutdown();
  nsMathMLOperators::ReleaseTable();

  nsFloatManager::Shutdown();
  nsImageFrame::ReleaseGlobals();

  mozilla::css::ErrorReporter::ReleaseGlobals();

  nsTextFragment::Shutdown();

  nsAttrValue::Shutdown();
  nsContentUtils::Shutdown();
  nsLayoutStylesheetCache::Shutdown();
  RuleProcessorCache::Shutdown();

  ShutdownJSEnvironment();
  nsGlobalWindow::ShutDown();
  nsDOMClassInfo::ShutDown();
  nsListControlFrame::Shutdown();
  nsXBLService::Shutdown();
  nsAutoCopyListener::Shutdown();
  FrameLayerBuilder::Shutdown();

#ifdef MOZ_ANDROID_OMX
  AndroidMediaPluginHost::Shutdown();
#endif

#ifdef MOZ_GSTREAMER
  GStreamerFormatHelper::Shutdown();
#endif

#ifdef MOZ_FFMPEG
  FFmpegRuntimeLinker::Unlink();
#endif

  CubebUtils::ShutdownLibrary();
  AsyncLatencyLogger::ShutdownLogger();
  WebAudioUtils::Shutdown();

#ifdef MOZ_WIDGET_GONK
  nsVolumeService::Shutdown();
  SpeakerManagerService::Shutdown();
#endif

#ifdef MOZ_WEBSPEECH
  nsSynthVoiceRegistry::Shutdown();
#endif

  nsCORSListenerProxy::Shutdown();

  nsIPresShell::ReleaseStatics();

  TouchManager::ReleaseStatics();

  nsTreeSanitizer::ReleaseStatics();

  nsHtml5Module::ReleaseStatics();

  mozilla::dom::FallbackEncoding::Shutdown();

  nsRegion::ShutdownStatic();

  mozilla::EventDispatcher::Shutdown();

  HTMLInputElement::DestroyUploadLastDir();

  nsLayoutUtils::Shutdown();

  nsHyphenationManager::Shutdown();
  nsDOMMutationObserver::Shutdown();

  DataStoreService::Shutdown();

  ContentParent::ShutDown();

  nsRefreshDriver::Shutdown();

  DisplayItemClip::Shutdown();

  nsDocument::XPCOMShutdown();

  CacheObserver::Shutdown();

  CameraPreferences::Shutdown();

  PromiseDebugging::Shutdown();
}
