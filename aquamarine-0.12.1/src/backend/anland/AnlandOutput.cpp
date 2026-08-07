#include <aquamarine/backend/AnlandBackend.hpp>
#include <aquamarine/backend/AnlandOutput.hpp>
#include <aquamarine/allocator/GBM.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <format>

extern "C" {
#include "display_producer.h"
}

#include "AnlandAllocator.hpp"
#include "Shared.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
template <typename T>
using SP = CSharedPointer<T>;

// ── CAnlandOutput ─────────────────────────────────────────────────

CAnlandOutput::CAnlandOutput(CWeakPointer<CAnlandBackend> backend, const SAnlandOutputOptions& options)
    : m_backend(backend) {
    name = options.name;
}

CAnlandOutput::~CAnlandOutput() {
    events.destroy.emit();
}

/* ── onConsumerBufferReady ────────────────────────────────────────
 *
 * Called when the consumer signals that it has a new buffer ready for us
 * to render into (the buf_ready eventfd is readable).
 *
 * Mirrors niri's buf_ready_efd handler:
 *   1. Refresh the swapchain to match the consumer's current buffer selection
 *   2. Mark consumer as ready; if a frame was pending, emit present
 *   3. Schedule the next frame */
void CAnlandOutput::onConsumerBufferReady() {
    auto backend = m_backend.lock();
    if (!backend)
        return;

    display_ctx* ctx = (display_ctx*)backend->m_display;
    if (!ctx)
        return;

    int selectedIdx = ::get_selected_idx(ctx);

    /* Lock the buffer index for this render cycle.
     * The consumer updates get_selected_idx() via shared memory as it cycles
     * through its 4 dmabuf slots.  By saving the index here and using it in
     * scheduleFrame() → refreshBuffers() → acquire(), we guarantee the entire
     * render cycle targets the buffer the consumer just signalled as ready,
     * even if the consumer advances selected_idx during rendering. */
    m_pendingBufferIdx = selectedIdx;

    /* Propagate the locked index to the allocator so that refreshBuffers()
     * below uses this index instead of re-reading the (potentially
     * already-changed) get_selected_idx(). */
    if (this->swapchain) {
        auto alc = this->swapchain->getAllocator();
        if (alc) {
            auto anlandAlloc = Hyprutils::Memory::dynamicPointerCast<CAnlandDMABufAllocator>(alc);
            if (anlandAlloc)
                anlandAlloc->lockBufferIndex(selectedIdx);
        }
    }

    /* Rebuild swapchain slots with the locked buffer index so that
     * beginRender() → next() returns the buffer the consumer just
     * signalled.  refreshBuffers() calls fullReconfigure() which
     * calls acquire() for each slot; acquire() honours the locked
     * index set above. */
    if (this->swapchain)
        this->swapchain->refreshBuffers();

    /* A buf_ready has two distinct meanings:
     *
     *   1. It makes this selected buffer available for a new render.
     *   2. Only if we had submitted a frame already, it also completes that frame.
     *
     * The first signal after initial connect (and after reconnect) has meaning #1
     * only.  Reporting it as present fabricated a page-flip before any frame had
     * been committed.  Hyprland's initial monitor setup reacts to presentation
     * events by scheduling/damaging work, so that fabricated presentation could
     * seed an unpaced startup render loop.  Keep m_awaitingConsumer true while
     * emitting a real present: its VFR tail-call is then rejected by scheduleFrame.
     */
    const bool completedFrame = m_awaitingConsumer;
    m_consumerReady = true;

    if (completedFrame) {
        timespec mono{};
        clock_gettime(CLOCK_MONOTONIC, &mono);
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when      = &mono,
            .seq       = 0,
            .refresh   = (int)backend->m_screenRefresh,
            .flags     = IOutput::AQ_OUTPUT_PRESENT_VSYNC | IOutput::AQ_OUTPUT_PRESENT_HW_CLOCK,
        });
        m_frameSerial++;
        m_awaitingConsumer = false;
    }

    /* A ready token must not itself request a new repaint.  It completes the
     * preceding frame; presenting it lets Hyprland's normal frame scheduler
     * decide whether new damage needs rendering.  Scheduling unconditionally
     * here forms a self-sustaining loop even on an idle desktop:
     *
     *   buf_ready -> scheduleFrame -> render/commit -> trigger_refresh
     *             -> next buf_ready
     *
     * The initial monitor request and actual damage set m_pendingFrame while no
     * buffer is available (or while the previous one is in flight).  Consume
     * exactly that deferred request once a concrete Consumer buffer arrives. */
    if (m_pendingFrame)
        scheduleFrame(AQ_SCHEDULE_NEEDS_FRAME);
}

/* ── commit ───────────────────────────────────────────────────────
 *
 * Called by Hyprland after it has rendered a frame into the swapchain
 * buffer.  This is the render-done half of the frame cycle. */
bool CAnlandOutput::commit() {
    auto backend = m_backend.lock();
    if (!backend)
        return false;

    display_ctx* ctx = (display_ctx*)backend->m_display;

    /* Fallback path */
    if (!ctx || backend->m_inFallback) {
        needsFrame = false;
        events.commit.emit();
        events.present.emit(IOutput::SPresentEvent{
            .presented = true,
            .when      = nullptr,
            .seq       = 0,
            .refresh   = (int)backend->m_screenRefresh,
            .flags     = IOutput::AQ_OUTPUT_PRESENT_VSYNC,
        });
        return true;
    }

    /* 1. If no frame was scheduled (needsFrame already false), this is a
     *    duplicate commit() call from Hyprland's render pipeline.  Skip all
     *    GPU work and trigger_refresh(): the consumer has already been notified
     *    and the buffer content hasn't changed. */
    if (!needsFrame) {
        events.commit.emit();
        return true;
    }

    /* 2. KWin-style explicit synchronization.
     *
     * Submit the GL work, export EGL_SYNC_NATIVE_FENCE_ANDROID as a sync_file fd,
     * and send it with the render-done message. Android Consumer supplies that fd
     * to ANativeWindow_queueBuffer(), so SurfaceFlinger waits on the GPU instead of
     * blocking Hyprland's render thread. This restores producer/consumer pipelining.
     *
     * ANLAND_FENCE_MODE=wait keeps the former conservative CPU wait for diagnosis;
     * =finish forces glFinish. The default and =async use the KWin path. A failed
     * export falls back to the former CPU wait, preserving correctness. */
    EGLDisplay eglDisplay = eglGetCurrentDisplay();
    bool fenceOk = false;
    const char* fenceMode = std::getenv("ANLAND_FENCE_MODE");
    const bool forceWait = fenceMode && !std::strcmp(fenceMode, "wait");
    const bool forceFinish = fenceMode && !std::strcmp(fenceMode, "finish");

    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR =
        (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR =
        (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR =
        (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");

    if (forceFinish) {
        glFinish();
        fenceOk = true;
    } else if (eglDisplay != EGL_NO_DISPLAY && eglCreateSyncKHR && eglDestroySyncKHR) {
        glFlush();
        EGLSyncKHR sync = eglCreateSyncKHR(eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        if (sync != EGL_NO_SYNC_KHR) {
            if (!forceWait && eglDupNativeFenceFDANDROID) {
                const int fenceFd = eglDupNativeFenceFDANDROID(eglDisplay, sync);
                if (fenceFd >= 0) {
                    ::set_render_fence(ctx, fenceFd); // ctx owns and transmits this fd.
                    fenceOk = true;
                }
            }

            /* Do not wait when fd export worked: this is KWin's asynchronous path. */
            if (!fenceOk && eglClientWaitSyncKHR) {
                const EGLint result = eglClientWaitSyncKHR(eglDisplay, sync, 0, EGL_FOREVER_KHR);
                fenceOk = (result == EGL_CONDITION_SATISFIED_KHR);
                if (!fenceOk)
                    backend->m_backend->log(AQ_LOG_DEBUG, "anland: native-fence CPU wait failed, fallback glFinish()");
            }
            eglDestroySyncKHR(eglDisplay, sync);
        }

        if (!fenceOk) {
            backend->m_backend->log(AQ_LOG_DEBUG, "anland: native-fence export unavailable, fallback glFinish()");
            glFinish();
            fenceOk = true;
        }
    } else {
        backend->m_backend->log(AQ_LOG_DEBUG, "anland: no native-fence EGL support, using glFinish()");
        glFinish();
        fenceOk = true;
    }

    /* 3. Signal the consumer that the frame is ready for display.
     *    trigger_refresh() is non-blocking; the consumer's event loop will
     *    pick up the notification and display the buffer. */
    ::trigger_refresh(ctx);
    m_awaitingConsumer = true;

    /* Unlock the buffer index: after trigger_refresh(), the consumer may
     * advance selected_idx.  The next onConsumerBufferReady() will lock
     * a fresh index. */
    if (this->swapchain) {
        auto alc = this->swapchain->getAllocator();
        if (alc) {
            auto anlandAlloc = Hyprutils::Memory::dynamicPointerCast<CAnlandDMABufAllocator>(alc);
            if (anlandAlloc)
                anlandAlloc->unlockBufferIndex();
        }
    }

    /* 4. Clear needsFrame before emitting commit, so Hyprland's duplicate
     *    commit() call (if any) skips the GPU work above. */
    needsFrame = false;
    events.commit.emit();

    return true;
}

bool CAnlandOutput::test() {
    return true;
}

CSharedPointer<IBackendImplementation> CAnlandOutput::getBackend() {
    auto bk = m_backend.lock();
    return CSharedPointer<IBackendImplementation>(bk);
}

std::vector<SDRMFormat> CAnlandOutput::getRenderFormats() {
    /* Consumer buffer is Android RGBA_8888 == DRM ABGR8888 (KWin mapping). */
    return {
        SDRMFormat{.drmFormat = DRM_FORMAT_ABGR8888, .modifiers = {DRM_FORMAT_INVALID}},
        SDRMFormat{.drmFormat = DRM_FORMAT_XRGB8888, .modifiers = {DRM_FORMAT_INVALID}},
    };
}

bool CAnlandOutput::pendingPageFlip() {
    return m_awaitingConsumer;
}

void CAnlandOutput::resetConsumerState() {
    /* The old Consumer session can no longer complete its submitted frame. */
    m_awaitingConsumer = false;
    m_consumerReady    = false;
    m_pendingFrame     = true;  // request an initial frame once the new session is ready
    m_pendingBufferIdx = -1;
    needsFrame         = false;
}

/* ── scheduleFrame ────────────────────────────────────────────────
 *
 * Called by Hyprland to request a new frame.  This is the entry point
 * of the render half of the frame cycle.
 *
 * Mirrors niri's render():
 *   1. Check m_awaitingConsumer (already waiting for consumer)
 *   2. Check m_consumerReady (consumer has a buffer ready)
 *   3. Mark consumer as not ready, ourselves as awaiting
 *   4. Emit events.frame to trigger Hyprland's render pipeline */
void CAnlandOutput::scheduleFrame(const scheduleFrameReason reason) {
    auto backend = m_backend.lock();
    if (!backend)
        return;

    /* If we're already waiting for the consumer, don't schedule another */
    if (m_awaitingConsumer) {
        /* VFR tail call (AQ_SCHEDULE_RENDER_MONITOR): Hyprland's renderMonitor()
         * unconditionally schedules the next frame when debug:vfr=0 (default).
         * For DRM backends this is fine (page flip waits for vblank), but for
         * Anland it creates a tight render loop: events.frame → render → commit
         * → trigger_refresh → consumer buf_ready → scheduleFrame → ...
         * We must NOT set m_pendingFrame here, otherwise the loop never breaks.
         * Only real damage-driven requests should be deferred. */
        if (reason == AQ_SCHEDULE_RENDER_MONITOR)
            return;
        /* Damage arrived while waiting for consumer — defer to next cycle. */
        m_pendingFrame = true;
        return;
    }

    /* A render is only legal after Consumer has handed us a concrete buffer.
     * In particular, monitor creation may schedule AQ_SCHEDULE_NEW_MONITOR before
     * the first buf_ready arrives. Defer that request instead of rendering slot 0
     * speculatively; onConsumerBufferReady() will consume the pending request.
     *
     * Never drop an already-started cycle without trigger_refresh(): Consumer is
     * waiting in refresh_done() for its render-done message. */
    if (!m_consumerReady) {
        if (reason != AQ_SCHEDULE_RENDER_MONITOR)
            m_pendingFrame = true;
        return;
    }

    /* Consume exactly one consumer buffer for this render cycle. */
    m_consumerReady    = false;
    m_pendingFrame     = false;
    m_awaitingConsumer = true;
    m_triggerSent      = false;
    needsFrame         = true;

    /* events.frame.emit() asks Hyprland to render a frame.  Hyprland's
     * handler is asynchronous (scheduled via the event loop), so commit()
     * will be called later.  When commit() runs, it will call
     * trigger_refresh() to signal the consumer. */
    events.frame.emit();

    /* FPS counter */
    m_frameCount++;
    auto now = std::chrono::steady_clock::now();
    if (m_lastReport == std::chrono::steady_clock::time_point{}) {
        m_lastReport = now;
        m_lastReportCount = m_frameCount;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastReport);
    if (elapsed.count() >= 5000) {
        uint64_t frames = m_frameCount - m_lastReportCount;
        if (frames > 0) {
            float fps = (float)frames / ((float)elapsed.count() / 1000.0f);
            backend->m_backend->log(AQ_LOG_DEBUG,
                std::format("anland: {} frames in {}ms ({} fps)", frames, elapsed.count(), (int)fps));
        }
        m_lastReport = now;
        m_lastReportCount = m_frameCount;
    }
}

bool CAnlandOutput::destroy() {
    events.destroy.emit();
    return true;
}