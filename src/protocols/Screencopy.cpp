#include "Screencopy.hpp"
#include "../Compositor.hpp"

#include <algorithm>

#include "ToplevelExportWlrFuncs.hpp"

#define SCREENCOPY_VERSION 3

static void bindManagerInt(wl_client* client, void* data, uint32_t version, uint32_t id) {
    g_pProtocolManager->m_pScreencopyProtocolManager->bindManager(client, data, version, id);
}

static void handleDisplayDestroy(struct wl_listener* listener, void* data) {
    g_pProtocolManager->m_pScreencopyProtocolManager->displayDestroy();
}

void CScreencopyProtocolManager::displayDestroy() {
    wl_global_destroy(m_pGlobal);
}

static SScreencopyFrame* frameFromResource(wl_resource*);

CScreencopyProtocolManager::CScreencopyProtocolManager() {

    m_pGlobal = wl_global_create(g_pCompositor->m_sWLDisplay, &zwlr_screencopy_manager_v1_interface, SCREENCOPY_VERSION, this, bindManagerInt);

    if (!m_pGlobal) {
        Debug::log(ERR, "ScreencopyProtocolManager could not start! Screensharing will not work!");
        return;
    }

    m_liDisplayDestroy.notify = handleDisplayDestroy;
    wl_display_add_destroy_listener(g_pCompositor->m_sWLDisplay, &m_liDisplayDestroy);

    Debug::log(LOG, "ScreencopyProtocolManager started successfully!");
}

static void handleCaptureOutput(wl_client* client, wl_resource* resource, uint32_t frame, int32_t overlay_cursor, wl_resource* output) {
    g_pProtocolManager->m_pScreencopyProtocolManager->captureOutput(client, resource, frame, overlay_cursor, output);
}

static void handleCaptureRegion(wl_client* client, wl_resource* resource, uint32_t frame, int32_t overlay_cursor, wl_resource* output, int32_t x, int32_t y, int32_t width,
                                int32_t height) {
    g_pProtocolManager->m_pScreencopyProtocolManager->captureOutput(client, resource, frame, overlay_cursor, output, {x, y, width, height});
}

static void handleDestroy(wl_client* client, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static void handleCopyFrame(wl_client* client, wl_resource* resource, wl_resource* buffer) {
    const auto PFRAME = frameFromResource(resource);

    if (!PFRAME)
        return;

    g_pProtocolManager->m_pScreencopyProtocolManager->copyFrame(client, resource, buffer);
}

static void handleCopyWithDamage(wl_client* client, wl_resource* resource, wl_resource* buffer) {
    const auto PFRAME = frameFromResource(resource);

    if (!PFRAME)
        return;

    PFRAME->withDamage = true;
    handleCopyFrame(client, resource, buffer);
}

static void handleDestroyFrame(wl_client* client, wl_resource* resource) {
    wl_resource_destroy(resource);
}

static const struct zwlr_screencopy_manager_v1_interface screencopyMgrImpl = {
    .capture_output        = handleCaptureOutput,
    .capture_output_region = handleCaptureRegion,
    .destroy               = handleDestroy,
};

static const struct zwlr_screencopy_frame_v1_interface screencopyFrameImpl = {
    .copy             = handleCopyFrame,
    .destroy          = handleDestroyFrame,
    .copy_with_damage = handleCopyWithDamage,
};

static CScreencopyClient* clientFromResource(wl_resource* resource) {
    ASSERT(wl_resource_instance_of(resource, &zwlr_screencopy_manager_v1_interface, &screencopyMgrImpl));
    return (CScreencopyClient*)wl_resource_get_user_data(resource);
}

static SScreencopyFrame* frameFromResource(wl_resource* resource) {
    ASSERT(wl_resource_instance_of(resource, &zwlr_screencopy_frame_v1_interface, &screencopyFrameImpl));
    return (SScreencopyFrame*)wl_resource_get_user_data(resource);
}

void CScreencopyProtocolManager::removeClient(CScreencopyClient* client, bool force) {
    if (!force) {
        if (!client || client->ref <= 0)
            return;

        if (--client->ref != 0)
            return;
    }

    m_lClients.remove(*client); // TODO: this doesn't get cleaned up after sharing app exits???
}

static void handleManagerResourceDestroy(wl_resource* resource) {
    const auto PCLIENT = clientFromResource(resource);

    g_pProtocolManager->m_pScreencopyProtocolManager->removeClient(PCLIENT, true);
}

CScreencopyClient::~CScreencopyClient() {
    g_pHookSystem->unhook(tickCallback);
}

CScreencopyClient::CScreencopyClient() {
    lastMeasure.reset();
    lastFrame.reset();
    tickCallback = g_pHookSystem->hookDynamic("tick", [&](void* self, SCallbackInfo& info, std::any data) { onTick(); });
}

void CScreencopyClient::onTick() {
    if (lastMeasure.getMillis() < 500)
        return;

    framesInLastHalfSecond = frameCounter;
    frameCounter           = 0;
    lastMeasure.reset();

    const auto LASTFRAMEDELTA = lastFrame.getMillis() / 1000.0;
    const bool FRAMEAWAITING  = std::ranges::any_of(g_pProtocolManager->m_pScreencopyProtocolManager->m_lFrames, [&](const auto& frame) { return frame.client == this; }) ||
        std::ranges::any_of(g_pProtocolManager->m_pToplevelExportProtocolManager->m_lFrames, [&](const auto& frame) { return frame.client == this; });

    if (framesInLastHalfSecond > 3 && !sentScreencast) {
        EMIT_HOOK_EVENT("screencast", (std::vector<uint64_t>{1, (uint64_t)framesInLastHalfSecond, (uint64_t)clientOwner}));
        g_pEventManager->postEvent(SHyprIPCEvent{"screencast", "1," + std::to_string(clientOwner)});
        sentScreencast = true;
    } else if (framesInLastHalfSecond < 4 && sentScreencast && LASTFRAMEDELTA > 1.0 && !FRAMEAWAITING) {
        EMIT_HOOK_EVENT("screencast", (std::vector<uint64_t>{0, (uint64_t)framesInLastHalfSecond, (uint64_t)clientOwner}));
        g_pEventManager->postEvent(SHyprIPCEvent{"screencast", "0," + std::to_string(clientOwner)});
        sentScreencast = false;
    }
}

void CScreencopyProtocolManager::bindManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
    const auto PCLIENT = &m_lClients.emplace_back();

    PCLIENT->resource = wl_resource_create(client, &zwlr_screencopy_manager_v1_interface, version, id);

    if (!PCLIENT->resource) {
        Debug::log(ERR, "ScreencopyProtocolManager could not bind! (out of memory?)");
        m_lClients.remove(*PCLIENT);
        wl_client_post_no_memory(client);
        return;
    }

    PCLIENT->ref = 1;

    wl_resource_set_implementation(PCLIENT->resource, &screencopyMgrImpl, PCLIENT, handleManagerResourceDestroy);

    Debug::log(LOG, "ScreencopyProtocolManager bound successfully!");
}

static void handleFrameResourceDestroy(wl_resource* resource) {
    const auto PFRAME = frameFromResource(resource);

    g_pProtocolManager->m_pScreencopyProtocolManager->removeFrame(PFRAME);
}

void CScreencopyProtocolManager::removeFrame(SScreencopyFrame* frame, bool force) {
    if (!frame)
        return;

    std::erase_if(m_vFramesAwaitingWrite, [&](const auto& other) { return other == frame; });

    wl_resource_set_user_data(frame->resource, nullptr);
    if (frame->buffer && frame->buffer->n_locks > 0)
        wlr_buffer_unlock(frame->buffer);
    removeClient(frame->client, force);
    m_lFrames.remove(*frame);
}

void CScreencopyProtocolManager::captureOutput(wl_client* client, wl_resource* resource, uint32_t frame, int32_t overlay_cursor, wl_resource* output, CBox box) {
    const auto PCLIENT = clientFromResource(resource);

    const auto PFRAME     = &m_lFrames.emplace_back();
    PFRAME->overlayCursor = !!overlay_cursor;
    PFRAME->resource      = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface, wl_resource_get_version(resource), frame);
    PFRAME->pMonitor      = g_pCompositor->getMonitorFromOutput(wlr_output_from_resource(output));

    if (!PFRAME->pMonitor) {
        Debug::log(ERR, "client requested sharing of a monitor that doesnt exist");
        zwlr_screencopy_frame_v1_send_failed(PFRAME->resource);
        removeFrame(PFRAME);
        return;
    }

    if (!PFRAME->resource) {
        Debug::log(ERR, "Couldn't alloc frame for sharing! (no memory)");
        removeFrame(PFRAME);
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(PFRAME->resource, &screencopyFrameImpl, PFRAME, handleFrameResourceDestroy);

    PFRAME->client = PCLIENT;
    PCLIENT->ref++;

    g_pHyprRenderer->makeEGLCurrent();
    PFRAME->shmFormat = g_pHyprOpenGL->getPreferredReadFormat(PFRAME->pMonitor);
    if (PFRAME->shmFormat == DRM_FORMAT_INVALID) {
        Debug::log(ERR, "No format supported by renderer in capture output");
        zwlr_screencopy_frame_v1_send_failed(PFRAME->resource);
        removeFrame(PFRAME);
        return;
    }

    const auto PSHMINFO = drm_get_pixel_format_info(PFRAME->shmFormat);
    if (!PSHMINFO) {
        Debug::log(ERR, "No pixel format supported by renderer in capture output");
        zwlr_screencopy_frame_v1_send_failed(PFRAME->resource);
        removeFrame(PFRAME);
        return;
    }

    if (PFRAME->pMonitor->output->allocator && (PFRAME->pMonitor->output->allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF)) {
        PFRAME->dmabufFormat = PFRAME->pMonitor->output->render_format;
    } else {
        PFRAME->dmabufFormat = DRM_FORMAT_INVALID;
    }

    if (box.width == 0 && box.height == 0)
        PFRAME->box = {0, 0, (int)(PFRAME->pMonitor->vecSize.x), (int)(PFRAME->pMonitor->vecSize.y)};
    else {
        PFRAME->box = box;
    }
    int ow, oh;
    wlr_output_effective_resolution(PFRAME->pMonitor->output, &ow, &oh);
    PFRAME->box.transform(PFRAME->pMonitor->transform, ow, oh).scale(PFRAME->pMonitor->scale).round();

    PFRAME->shmStride = pixel_format_info_min_stride(PSHMINFO, PFRAME->box.w);

    zwlr_screencopy_frame_v1_send_buffer(PFRAME->resource, convert_drm_format_to_wl_shm(PFRAME->shmFormat), PFRAME->box.width, PFRAME->box.height, PFRAME->shmStride);

    if (wl_resource_get_version(resource) >= 3) {
        if (PFRAME->dmabufFormat != DRM_FORMAT_INVALID) {
            zwlr_screencopy_frame_v1_send_linux_dmabuf(PFRAME->resource, PFRAME->dmabufFormat, PFRAME->box.width, PFRAME->box.height);
        }

        zwlr_screencopy_frame_v1_send_buffer_done(PFRAME->resource);
    }
}

void CScreencopyProtocolManager::copyFrame(wl_client* client, wl_resource* resource, wl_resource* buffer) {
    const auto PFRAME = frameFromResource(resource);

    if (!PFRAME) {
        Debug::log(ERR, "No frame in copyFrame??");
        return;
    }

    if (!g_pCompositor->monitorExists(PFRAME->pMonitor)) {
        Debug::log(ERR, "client requested sharing of a monitor that is gone");
        zwlr_screencopy_frame_v1_send_failed(PFRAME->resource);
        removeFrame(PFRAME);
        return;
    }

    const auto PBUFFER = wlr_buffer_try_from_resource(buffer);
    if (!PBUFFER) {
        Debug::log(ERR, "[sc] invalid buffer in {:x}", (uintptr_t)PFRAME);
        wl_resource_post_error(PFRAME->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "invalid buffer");
        removeFrame(PFRAME);
        return;
    }

    if (PBUFFER->width != PFRAME->box.width || PBUFFER->height != PFRAME->box.height) {
        Debug::log(ERR, "[sc] invalid dimensions in {:x}", (uintptr_t)PFRAME);
        wl_resource_post_error(PFRAME->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "invalid buffer dimensions");
        removeFrame(PFRAME);
        return;
    }

    if (PFRAME->buffer) {
        Debug::log(ERR, "[sc] buffer used in {:x}", (uintptr_t)PFRAME);
        wl_resource_post_error(PFRAME->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED, "frame already used");
        removeFrame(PFRAME);
        return;
    }

    wlr_dmabuf_attributes dmabufAttrs;
    void*                 wlrBufferAccessData;
    uint32_t              wlrBufferAccessFormat;
    size_t                wlrBufferAccessStride;
    if (wlr_buffer_get_dmabuf(PBUFFER, &dmabufAttrs)) {
        PFRAME->bufferCap = WLR_BUFFER_CAP_DMABUF;

        if (dmabufAttrs.format != PFRAME->dmabufFormat) {
            Debug::log(ERR, "[sc] invalid buffer dma format in {:x}", (uintptr_t)PFRAME);
            wl_resource_post_error(PFRAME->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "invalid buffer format");
            removeFrame(PFRAME);
            return;
        }
    } else if (wlr_buffer_begin_data_ptr_access(PBUFFER, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &wlrBufferAccessData, &wlrBufferAccessFormat, &wlrBufferAccessStride)) {
        wlr_buffer_end_data_ptr_access(PBUFFER);

        if (wlrBufferAccessFormat != PFRAME->shmFormat) {
            Debug::log(ERR, "[sc] invalid buffer shm format in {:x}", (uintptr_t)PFRAME);
            wl_resource_post_error(PFRAME->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "invalid buffer format");
            removeFrame(PFRAME);
            return;
        } else if ((int)wlrBufferAccessStride != PFRAME->shmStride) {
            Debug::log(ERR, "[sc] invalid buffer shm stride in {:x}", (uintptr_t)PFRAME);
            wl_resource_post_error(PFRAME->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "invalid buffer stride");
            removeFrame(PFRAME);
            return;
        }
    } else {
        Debug::log(ERR, "[sc] invalid buffer type in {:x}", (uintptr_t)PFRAME);
        wl_resource_post_error(PFRAME->resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "invalid buffer type");
        removeFrame(PFRAME);
        return;
    }

    PFRAME->buffer = PBUFFER;

    m_vFramesAwaitingWrite.emplace_back(PFRAME);

    g_pHyprRenderer->m_bDirectScanoutBlocked = true;
    if (PFRAME->overlayCursor)
        g_pHyprRenderer->m_bSoftwareCursorsLocked = true;

    if (!PFRAME->withDamage)
        g_pCompositor->scheduleFrameForMonitor(PFRAME->pMonitor);
}

void CScreencopyProtocolManager::onOutputCommit(CMonitor* pMonitor, wlr_output_event_commit* e) {
    m_pLastMonitorBackBuffer = e->state->buffer;
    shareAllFrames(pMonitor);
    m_pLastMonitorBackBuffer = nullptr;
}

void CScreencopyProtocolManager::shareAllFrames(CMonitor* pMonitor) {
    if (m_vFramesAwaitingWrite.empty())
        return; // nothing to share

    std::vector<SScreencopyFrame*> framesToRemove;

    // share frame if correct output
    for (auto& f : m_vFramesAwaitingWrite) {
        if (!f->pMonitor || !f->buffer) {
            framesToRemove.push_back(f);
            continue;
        }

        if (f->pMonitor != pMonitor)
            continue;

        shareFrame(f);

        f->client->lastFrame.reset();
        ++f->client->frameCounter;

        framesToRemove.push_back(f);
    }

    for (auto& f : framesToRemove) {
        removeFrame(f);
    }

    g_pHyprRenderer->m_bSoftwareCursorsLocked = false;

    if (m_vFramesAwaitingWrite.empty()) {
        g_pHyprRenderer->m_bDirectScanoutBlocked = false;
    } else {
        for (auto& f : m_vFramesAwaitingWrite) {
            if (f->overlayCursor) {
                g_pHyprRenderer->m_bSoftwareCursorsLocked = true;
                break;
            }
        }
    }
}

void CScreencopyProtocolManager::shareFrame(SScreencopyFrame* frame) {
    if (!frame->buffer)
        return;

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    uint32_t flags = 0;
    if (frame->bufferCap == WLR_BUFFER_CAP_DMABUF) {
        if (!copyFrameDmabuf(frame)) {
            Debug::log(ERR, "[sc] dmabuf copy failed in {:x}", (uintptr_t)frame);
            zwlr_screencopy_frame_v1_send_failed(frame->resource);
            return;
        }
    } else {
        if (!copyFrameShm(frame, &now)) {
            Debug::log(ERR, "[sc] shm copy failed in {:x}", (uintptr_t)frame);
            zwlr_screencopy_frame_v1_send_failed(frame->resource);
            return;
        }
    }

    zwlr_screencopy_frame_v1_send_flags(frame->resource, flags);
    sendFrameDamage(frame);
    uint32_t tvSecHi = (sizeof(now.tv_sec) > 4) ? now.tv_sec >> 32 : 0;
    uint32_t tvSecLo = now.tv_sec & 0xFFFFFFFF;
    zwlr_screencopy_frame_v1_send_ready(frame->resource, tvSecHi, tvSecLo, now.tv_nsec);
}

void CScreencopyProtocolManager::sendFrameDamage(SScreencopyFrame* frame) {
    if (!frame->withDamage)
        return;

    for (auto& RECT : frame->pMonitor->lastFrameDamage.getRects()) {

        if (frame->buffer->width < 1 || frame->buffer->height < 1 || frame->buffer->width - RECT.x1 < 1 || frame->buffer->height - RECT.y1 < 1) {
            Debug::log(ERR, "[sc] Failed to send damage");
            break;
        }

        zwlr_screencopy_frame_v1_send_damage(frame->resource, std::clamp(RECT.x1, 0, frame->buffer->width), std::clamp(RECT.y1, 0, frame->buffer->height),
                                             std::clamp(RECT.x2 - RECT.x1, 0, frame->buffer->width - RECT.x1), std::clamp(RECT.y2 - RECT.y1, 0, frame->buffer->height - RECT.y1));
    }
}

bool CScreencopyProtocolManager::copyFrameShm(SScreencopyFrame* frame, timespec* now) {
    wlr_texture* sourceTex = wlr_texture_from_buffer(g_pCompositor->m_sWLRRenderer, m_pLastMonitorBackBuffer);
    if (!sourceTex)
        return false;

    void*    data;
    uint32_t format;
    size_t   stride;
    if (!wlr_buffer_begin_data_ptr_access(frame->buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
        wlr_texture_destroy(sourceTex);
        return false;
    }

    CRegion fakeDamage = {0, 0, INT16_MAX, INT16_MAX};

    g_pHyprRenderer->makeEGLCurrent();

    CFramebuffer fb;
    fb.alloc(frame->box.w, frame->box.h, g_pHyprRenderer->isNvidia() ? DRM_FORMAT_XBGR8888 : frame->pMonitor->drmFormat);

    if (!g_pHyprRenderer->beginRender(frame->pMonitor, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &fb)) {
        wlr_texture_destroy(sourceTex);
        wlr_buffer_end_data_ptr_access(frame->buffer);
        return false;
    }

    CBox monbox = CBox{0, 0, frame->pMonitor->vecTransformedSize.x, frame->pMonitor->vecTransformedSize.y}.translate({-frame->box.x, -frame->box.y});
    g_pHyprOpenGL->setMonitorTransformEnabled(false);
    g_pHyprOpenGL->renderTexture(sourceTex, &monbox, 1);
    g_pHyprOpenGL->setMonitorTransformEnabled(true);

#ifndef GLES2
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.m_iFb);
#else
    glBindFramebuffer(GL_FRAMEBUFFER, fb.m_iFb);
#endif

    const auto PFORMAT = g_pHyprOpenGL->getPixelFormatFromDRM(format);
    if (!PFORMAT) {
        g_pHyprRenderer->endRender();
        wlr_texture_destroy(sourceTex);
        wlr_buffer_end_data_ptr_access(frame->buffer);
        return false;
    }

    g_pHyprRenderer->endRender();

    g_pHyprRenderer->makeEGLCurrent();
    g_pHyprOpenGL->m_RenderData.pMonitor = frame->pMonitor;
    fb.bind();

    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    const wlr_pixel_format_info* drmFmtWlr  = drm_get_pixel_format_info(format);
    uint32_t                     packStride = pixel_format_info_min_stride(drmFmtWlr, frame->box.w);

    if (packStride == stride) {
        glReadPixels(0, 0, frame->box.w, frame->box.h, PFORMAT->glFormat, PFORMAT->glType, data);
    } else {
        for (size_t i = 0; i < frame->box.h; ++i) {
            uint32_t y = i;
            glReadPixels(0, y, frame->box.w, 1, PFORMAT->glFormat, PFORMAT->glType, ((unsigned char*)data) + i * stride);
        }
    }

    g_pHyprOpenGL->m_RenderData.pMonitor = nullptr;

    wlr_buffer_end_data_ptr_access(frame->buffer);
    wlr_texture_destroy(sourceTex);

    return true;
}

bool CScreencopyProtocolManager::copyFrameDmabuf(SScreencopyFrame* frame) {
    wlr_texture* sourceTex = wlr_texture_from_buffer(g_pCompositor->m_sWLRRenderer, m_pLastMonitorBackBuffer);
    if (!sourceTex)
        return false;

    CRegion fakeDamage = {0, 0, frame->box.width, frame->box.height};

    if (!g_pHyprRenderer->beginRender(frame->pMonitor, fakeDamage, RENDER_MODE_TO_BUFFER, frame->buffer))
        return false;

    CBox monbox = CBox{0, 0, frame->pMonitor->vecPixelSize.x, frame->pMonitor->vecPixelSize.y}.translate({-frame->box.x, -frame->box.y});
    g_pHyprOpenGL->setMonitorTransformEnabled(false);
    g_pHyprOpenGL->renderTexture(sourceTex, &monbox, 1);
    g_pHyprOpenGL->setMonitorTransformEnabled(true);

    g_pHyprRenderer->endRender();

    wlr_texture_destroy(sourceTex);

    return true;
}
