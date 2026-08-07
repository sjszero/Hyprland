#pragma once
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/output/Output.hpp>
#include <aquamarine/input/Input.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace Aquamarine {
    class CAnlandBackend;

    class CAnlandOutput : public IOutput {
      public:
        CAnlandOutput(Hyprutils::Memory::CWeakPointer<CAnlandBackend> backend, const SAnlandOutputOptions& options);
        virtual ~CAnlandOutput();

        virtual bool                                                                      commit() override;
        virtual bool                                                                      test() override;
        virtual Hyprutils::Memory::CSharedPointer<IBackendImplementation>                 getBackend() override;
        virtual std::vector<SDRMFormat>                                                   getRenderFormats() override;
        virtual bool                                                                      pendingPageFlip() override;
        virtual void                                                                      scheduleFrame(const scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN) override;
        virtual bool                                                                      destroy() override;

        void onConsumerBufferReady();

        /* Called when the transport drops. A subsequent buf_ready is the first
         * buffer of a new session, not completion of the old in-flight frame. */
        void resetConsumerState();

        Hyprutils::Memory::CWeakPointer<CAnlandOutput> self;

      private:
        friend class CAnlandBackend;
        Hyprutils::Memory::CWeakPointer<CAnlandBackend> m_backend;

        /* ── Frame state (niri pending_frame model) ── */
        bool m_awaitingConsumer = false;
        bool m_consumerReady    = false;
        bool m_triggerSent      = false;
        bool m_pendingFrame     = false;  // damage arrived while waiting for consumer
        uint64_t  m_frameCount = 0;
        uint64_t  m_frameSerial = 0;
        std::chrono::steady_clock::time_point m_lastReport;
        std::chrono::steady_clock::time_point m_lastIdleFrame;
        uint64_t  m_lastReportCount = 0;

        /* Buffer index locked at onConsumerBufferReady() time.
         * The consumer updates get_selected_idx() via shared memory as it
         * cycles through its 4 dmabuf slots.  If we call get_selected_idx()
         * again during scheduleFrame() → refreshBuffers(), the consumer may
         * have already advanced to a *different* slot, causing us to render
         * into the wrong buffer while the consumer displays stale content.
         * By saving the index here and using it in acquire(), we guarantee
         * the entire render cycle targets the buffer the consumer just
         * signalled as ready. */
        int m_pendingBufferIdx = -1;
    };
};