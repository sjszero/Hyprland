#pragma once

#include <aquamarine/allocator/Allocator.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <aquamarine/backend/Backend.hpp>

#include <cstdint>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <drm_fourcc.h>

extern "C" {
#include "display_producer.h"
}

namespace Aquamarine {

    using Hyprutils::Math::Vector2D;

    /*
     * CAnlandDMABuf – wraps a single daemon-provided dmabuf fd.
     */
    class CAnlandDMABuf : public IBuffer {
      public:
        CAnlandDMABuf(uint32_t w, uint32_t h, uint32_t drmFormat,
                       uint64_t modifier, int fd,
                       uint32_t stride, uint32_t offset)
            : m_fd(-1) {
            size = Vector2D((double)w, (double)h);

            m_attrs.success = true;
            m_attrs.size    = size;
            m_attrs.format  = drmFormat;
            m_attrs.modifier = modifier;
            m_attrs.planes  = 1;
            m_attrs.fds[0]  = dup(fd);
            m_attrs.strides[0] = stride;
            m_attrs.offsets[0] = offset;

            m_fd = m_attrs.fds[0];
        }

        virtual ~CAnlandDMABuf() {
            if (m_fd >= 0) {
                close(m_fd);
                m_fd = -1;
            }
            events.destroy.emit();
        }

        /* IBuffer interface */
        eBufferCapability caps() override { return (eBufferCapability)0; }
        eBufferType       type() override { return BUFFER_TYPE_DMABUF; }
        void              update(const Hyprutils::Math::CRegion& damage) override { /* no-op */ }
        bool              isSynchronous() override { return false; }
        bool              good() override { return m_fd >= 0; }

        SDMABUFAttrs dmabuf() override {
            if (m_formatOverride != DRM_FORMAT_INVALID) {
                SDMABUFAttrs attrs = m_attrs;
                attrs.format = m_formatOverride;
                return attrs;
            }
            return m_attrs;
        }

        void setFormatOverride(uint32_t drmFormat) { m_formatOverride = drmFormat; }
        uint32_t getFormatOverride() { return m_formatOverride; }

        int rawFD() const { return m_fd; }

      private:
        int          m_fd = -1;
        SDMABUFAttrs m_attrs;
        uint32_t     m_formatOverride = DRM_FORMAT_INVALID;
    };

    /*
     * CAnlandDMABufAllocator – wraps the daemon's fixed set of dmabufs.
     *
     * KWin-style: each call to acquire() returns the buffer matching the
     * consumer's current get_selected_idx(), so producer and consumer always
     * agree on which buffer to render into.
     */
    class CAnlandDMABufAllocator : public IAllocator {
      public:
        static Hyprutils::Memory::CSharedPointer<CAnlandDMABufAllocator>
        create(display_ctx *ctx, Hyprutils::Memory::CWeakPointer<CBackend> backend) {
            int count = get_buf_count(ctx);
            if (count <= 0) {
                backend->log(AQ_LOG_ERROR, "anland allocator: no dmabufs from daemon");
                return nullptr;
            }

            auto alloc = Hyprutils::Memory::CSharedPointer<CAnlandDMABufAllocator>(
                new CAnlandDMABufAllocator(backend));

            alloc->m_ctx = ctx;
            alloc->m_rawFds.reserve(count);
            alloc->m_rawBufInfos.reserve(count);

            for (int i = 0; i < count; i++) {
                int fd = get_dmabuf_fd_at(ctx, i);
                if (fd < 0) {
                    backend->log(AQ_LOG_WARNING,
                        std::format("anland allocator: dmabuf {} has no fd, skipping", i));
                    alloc->m_rawFds.push_back(-1);
                    alloc->m_rawBufInfos.push_back({});
                    continue;
                }

                buf_info info;
                if (get_dmabuf_info_at(ctx, i, &info) < 0) {
                    backend->log(AQ_LOG_WARNING,
                        std::format("anland allocator: dmabuf {} has no info, skipping", i));
                    alloc->m_rawFds.push_back(-1);
                    alloc->m_rawBufInfos.push_back({});
                    continue;
                }

                alloc->m_rawFds.push_back(fd);
                alloc->m_rawBufInfos.push_back(info);

                /* Consumer-side pixel-format enum (protocol.h): 1 == RGBA_8888
                 * in Android memory layout == ABGR8888 in DRM fourcc terms.
                 * Mirrors KWin's protocol_format_to_drm(). Using XRGB8888 here
                 * swaps R/B channels -> blue-yellow color blindness. */
                uint32_t drmFormat = info.format == 1 ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XRGB8888;

                auto buf = Hyprutils::Memory::CSharedPointer<CAnlandDMABuf>(
                    new CAnlandDMABuf(info.width, info.height, drmFormat,
                                      info.modifier, fd,
                                      info.stride, info.offset));
                alloc->m_slotBuffers.push_back(buf);

                backend->log(AQ_LOG_DEBUG,
                    std::format("anland allocator: registered dmabuf {} fd={} {}x{} "
                                "stride={} offset={} mod=0x{:x} fmt={}",
                                i, fd, (uint32_t)info.width, (uint32_t)info.height,
                                (uint32_t)info.stride, (uint32_t)info.offset, (uint64_t)info.modifier, (uint32_t)info.format));
            }

            if (alloc->m_rawFds.empty()) {
                backend->log(AQ_LOG_ERROR, "anland allocator: no dmabuf fds registered");
                return nullptr;
            }

            alloc->m_self = alloc;
            return alloc;
        }

        virtual ~CAnlandDMABufAllocator() {
            m_slotBuffers.clear();
        }

        void setDisplayCtx(display_ctx *ctx) { m_ctx = ctx; }

        /*
         * Lock the buffer index for the next render cycle.
         * Must be called from onConsumerBufferReady() before scheduleFrame()
         * triggers refreshBuffers(), so that acquire() uses the index the
         * consumer signalled via buf_ready rather than the (potentially
         * already-changed) value of get_selected_idx().
         */
        void lockBufferIndex(int idx) { m_forcedBufferIdx = idx; }

        /*
         * Unlock – after commit() has sent trigger_refresh, the next
         * acquire() will fall back to get_selected_idx() again.
         */
        void unlockBufferIndex() { m_forcedBufferIdx = -1; }

        /*
         * acquire() – return the pre-allocated CAnlandDMABuf matching the
         * consumer's current buffer selection.
         *
         * If m_forcedBufferIdx has been set (by lockBufferIndex()), that
         * index is used instead of the live get_selected_idx() value.  This
         * prevents a race: the consumer may advance selected_idx via shared
         * memory between the buf_ready notification and the actual acquire()
         * call inside refreshBuffers(), causing the producer to render into
         * a buffer the consumer is no longer expecting.
         */
        Hyprutils::Memory::CSharedPointer<IBuffer>
        acquire(const SAllocatorBufferParams& params,
                Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain) override {
            if (m_slotBuffers.empty())
                return nullptr;

            if (!m_ctx)
                return m_slotBuffers[0];

            int idx;
            if (m_forcedBufferIdx >= 0) {
                idx = m_forcedBufferIdx;
            } else {
                /* Return the consumer's currently selected buffer. */
                idx = ::get_selected_idx(m_ctx);
            }

            if (idx < 0 || idx >= (int)m_slotBuffers.size())
                idx = 0;

            return m_slotBuffers[idx];
        }

        Hyprutils::Memory::CSharedPointer<CBackend> getBackend() override {
            return m_backend.lock();
        }

        int drmFD() override { return -1; }
        eAllocatorType type() override { return AQ_ALLOCATOR_TYPE_GBM; }
        bool supportsBufferAge() override { return false; } /* Anland: all slots point to same physical dmabuf */

        Hyprutils::Memory::CWeakPointer<CAnlandDMABufAllocator> m_self;

        std::vector<int>                   m_rawFds;
        std::vector<buf_info>              m_rawBufInfos;
        std::vector<Hyprutils::Memory::CSharedPointer<CAnlandDMABuf>> m_slotBuffers;

      private:
        CAnlandDMABufAllocator(Hyprutils::Memory::CWeakPointer<CBackend> backend)
            : m_backend(backend) {}

        Hyprutils::Memory::CWeakPointer<CBackend> m_backend;
        display_ctx *m_ctx = nullptr;
        int m_forcedBufferIdx = -1;
    };

} // namespace Aquamarine