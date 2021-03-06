/*
 *      Copyright (C) 2017 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this Program; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "RPProcessInfo.h"
#include "ServiceBroker.h"
#include "cores/RetroPlayer/process/RenderBufferManager.h"
#include "cores/RetroPlayer/rendering/RenderContext.h"
#include "cores/DataCacheCore.h"
#include "windowing/GraphicContext.h"
#include "rendering/RenderSystem.h"
#include "settings/DisplaySettings.h"
#include "settings/MediaSettings.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"

extern "C" {
#include "libavutil/pixdesc.h"
}

#include <utility>

using namespace KODI;
using namespace RETRO;

CreateRPProcessControl CRPProcessInfo::m_processControl = nullptr;
std::vector<std::unique_ptr<IRendererFactory>> CRPProcessInfo::m_rendererFactories;
CCriticalSection CRPProcessInfo::m_createSection;

CRPProcessInfo::CRPProcessInfo(std::string platformName) :
  m_platformName(std::move(platformName)),
  m_renderBufferManager(new CRenderBufferManager),
  m_renderContext(new CRenderContext(&CServiceBroker::GetRenderSystem(),
                                     &CServiceBroker::GetWinSystem(),
                                     CServiceBroker::GetWinSystem().GetGfxContext(),
                                     CDisplaySettings::GetInstance(),
                                     CMediaSettings::GetInstance()))
{
  for (auto &rendererFactory : m_rendererFactories)
  {
    RenderBufferPoolVector bufferPools = rendererFactory->CreateBufferPools(*m_renderContext);
    m_renderBufferManager->RegisterPools(rendererFactory.get(), std::move(bufferPools));
  }

  // Initialize default scaling method
  for (auto scalingMethod : GetScalingMethods())
  {
    if (HasScalingMethod(scalingMethod))
    {
      m_defaultScalingMethod = scalingMethod;
      break;
    }
  }
}

CRPProcessInfo::~CRPProcessInfo() = default;

CRPProcessInfo* CRPProcessInfo::CreateInstance()
{
  CRPProcessInfo *processInfo = nullptr;

  CSingleLock lock(m_createSection);

  if (m_processControl != nullptr)
  {
    processInfo = m_processControl();

    if (processInfo != nullptr)
      CLog::Log(LOGINFO, "RetroPlayer[PROCESS]: Created process info for %s", processInfo->GetPlatformName().c_str());
    else
      CLog::Log(LOGERROR, "RetroPlayer[PROCESS]: Failed to create process info");
  }
  else
  {
    CLog::Log(LOGERROR, "RetroPlayer[PROCESS]: No process control registered");
  }

  return processInfo;
}

void CRPProcessInfo::RegisterProcessControl(CreateRPProcessControl createFunc)
{
  std::unique_ptr<CRPProcessInfo> processInfo(createFunc());

  CSingleLock lock(m_createSection);

  if (processInfo)
  {
    CLog::Log(LOGINFO, "RetroPlayer[PROCESS]: Registering process control for %s",
              processInfo->GetPlatformName().c_str());
    m_processControl = createFunc;
  }
  else
  {
    CLog::Log(LOGERROR, "RetroPlayer[PROCESS]: Failed to register process control");
    m_processControl = nullptr;
  }
}

void CRPProcessInfo::RegisterRendererFactory(IRendererFactory *factory)
{
  CSingleLock lock(m_createSection);

  CLog::Log(LOGINFO, "RetroPlayer[RENDER]: Registering renderer factory for %s",
            factory->RenderSystemName().c_str());

  m_rendererFactories.emplace_back(factory);
}

std::string CRPProcessInfo::GetRenderSystemName(IRenderBufferPool *renderBufferPool) const
{
  return m_renderBufferManager->GetRenderSystemName(renderBufferPool);
}

CRPBaseRenderer *CRPProcessInfo::CreateRenderer(IRenderBufferPool *renderBufferPool, const CRenderSettings &renderSettings)
{
  CSingleLock lock(m_createSection);

  for (auto &rendererFactory : m_rendererFactories)
  {
    RenderBufferPoolVector bufferPools = m_renderBufferManager->GetPools(rendererFactory.get());
    for (auto &bufferPool : bufferPools)
    {
      if (bufferPool.get() == renderBufferPool)
        return rendererFactory->CreateRenderer(renderSettings, *m_renderContext, std::move(bufferPool));
    }
  }

  CLog::Log(LOGERROR, "RetroPlayer[RENDER]: Failed to find a suitable renderer factory");

  return nullptr;
}

void CRPProcessInfo::SetDataCache(CDataCacheCore *cache)
{
  m_dataCache = cache;;
}

void CRPProcessInfo::ResetInfo()
{
  if (m_dataCache != nullptr)
  {
    m_dataCache->SetVideoDecoderName("", false);
    m_dataCache->SetVideoDeintMethod("");
    m_dataCache->SetVideoPixelFormat("");
    m_dataCache->SetVideoDimensions(0, 0);
    m_dataCache->SetVideoFps(0.0f);
    m_dataCache->SetVideoDAR(1.0f);
    m_dataCache->SetAudioDecoderName("");
    m_dataCache->SetAudioChannels("");
    m_dataCache->SetAudioSampleRate(0);
    m_dataCache->SetAudioBitsPerSample(0);
    m_dataCache->SetRenderClockSync(false);
    m_dataCache->SetStateSeeking(false);
    m_dataCache->SetSpeed(1.0f, 1.0f);
    m_dataCache->SetGuiRender(true); //! @todo
    m_dataCache->SetVideoRender(false); //! @todo
    m_dataCache->SetPlayTimes(0, 0, 0, 0);
  }
}

bool CRPProcessInfo::HasScalingMethod(ESCALINGMETHOD scalingMethod) const
{
  return m_renderBufferManager->HasScalingMethod(scalingMethod);
}

std::vector<ESCALINGMETHOD> CRPProcessInfo::GetScalingMethods()
{
  return {
    VS_SCALINGMETHOD_NEAREST,
    VS_SCALINGMETHOD_LINEAR,
  };
}

//******************************************************************************
// video codec
//******************************************************************************
void CRPProcessInfo::SetVideoPixelFormat(AVPixelFormat pixFormat)
{
  const char *videoPixelFormat = av_get_pix_fmt_name(pixFormat);

  if (m_dataCache != nullptr)
    m_dataCache->SetVideoPixelFormat(videoPixelFormat != nullptr ? videoPixelFormat : "");
}

void CRPProcessInfo::SetVideoDimensions(int width, int height)
{
  if (m_dataCache != nullptr)
    m_dataCache->SetVideoDimensions(width, height);
}

void CRPProcessInfo::SetVideoFps(float fps)
{
  if (m_dataCache != nullptr)
    m_dataCache->SetVideoFps(fps);
}

//******************************************************************************
// player audio info
//******************************************************************************
void CRPProcessInfo::SetAudioChannels(const std::string &channels)
{
  if (m_dataCache != nullptr)
    m_dataCache->SetAudioChannels(channels);
}

void CRPProcessInfo::SetAudioSampleRate(int sampleRate)
{
  if (m_dataCache != nullptr)
    m_dataCache->SetAudioSampleRate(sampleRate);
}

void CRPProcessInfo::SetAudioBitsPerSample(int bitsPerSample)
{
  if (m_dataCache != nullptr)
    m_dataCache->SetAudioBitsPerSample(bitsPerSample);
}

//******************************************************************************
// player states
//******************************************************************************
void CRPProcessInfo::SetSpeed(float speed)
{
  if (m_dataCache != nullptr)
    m_dataCache->SetSpeed(1.0f, speed);
}

void CRPProcessInfo::SetPlayTimes(time_t start, int64_t current, int64_t min, int64_t max)
{
  if (m_dataCache != nullptr)
    m_dataCache->SetPlayTimes(start, current, min, max);
}
