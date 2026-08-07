#pragma once
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/output/Output.hpp>
#include <aquamarine/input/Input.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <aquamarine/input/Input.hpp>
#include <aquamarine/input/Input.hpp>
#include <string>
#include <vector>

namespace Aquamarine {
    class AnlandGestureRecognizer;
    class CAnlandOutput;
    struct SAnlandOutputOptions {
        std::string name = "ANLAND-1";
        Hyprutils::Math::Vector2D size = {1920, 1080};
        int refreshRate = 60000;
    };

    /*
     * CAnlandPointer – virtual pointer fed from the daemon's data fd.
     */
    class CAnlandPointer : public IPointer {
      public:
        CAnlandPointer() = default;
        virtual ~CAnlandPointer() = default;
        std::string m_name = "anland-virtual-pointer";
        virtual const std::string& getName() override { return m_name; }
    };

    /*
     * CAnlandKeyboard – virtual keyboard fed from the daemon's data fd.
     */
    class CAnlandKeyboard : public IKeyboard {
      public:
        CAnlandKeyboard() = default;
        virtual ~CAnlandKeyboard() = default;
        std::string m_name = "anland-virtual-keyboard";
        virtual const std::string& getName() override { return m_name; }
    };

    /*
     * CAnlandTouch – virtual touch device fed from the daemon's data fd.
     */
    class CAnlandTouch : public ITouch {
      public:
        CAnlandTouch() = default;
        virtual ~CAnlandTouch() = default;
        std::string m_name = "anland-virtual-touch";
        virtual const std::string& getName() override { return m_name; }
    };

    class CAnlandBackend : public IBackendImplementation {
      public:
        CAnlandBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend);
        virtual ~CAnlandBackend();

        using IBackendImplementation::createOutput;
        virtual eBackendType                                                               type() override;
        virtual bool                                                                       start() override;
        virtual std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>                    pollFDs() override;
        virtual int                                                                        drmFD() override;
        virtual int                                                                        drmRenderNodeFD() override;
        virtual bool                                                                       dispatchEvents() override;
        virtual uint32_t                                                                   capabilities() override;
        virtual void                                                                       onReady() override;
        virtual std::vector<SDRMFormat>                                                    getRenderFormats() override;
        virtual std::vector<SDRMFormat>                                                    getCursorFormats() override;
        virtual bool                                                                       createOutput(const std::string& name) override;
        virtual Hyprutils::Memory::CSharedPointer<IAllocator>                              preferredAllocator() override;
        virtual std::vector<Hyprutils::Memory::CSharedPointer<IAllocator>>                 getAllocators() override;
        virtual Hyprutils::Memory::CWeakPointer<IBackendImplementation>                    getPrimary() override;

        bool                createOutput(const SAnlandOutputOptions& options);
        int                 bufReadyFd() const;
        void                onBufferReady();
        void                onInputReadable();

        /* True while the consumer is disconnected / in fallback mode. */
        bool inFallback() const { return m_inFallback; }

        /* ── Frame pacing (niri/KWin-style throttle) ──────────────
         * The consumer drives buf_ready -> render -> refresh_done with NO
         * sleep on its side, so the loop speed equals our render speed.
         * Idle frames render in <1ms, causing a CPU spin.  Pace the loop
         * to the screen refresh rate with a timerfd: when buf_ready
         * arrives too soon after the last frame, arm the timer for the
         * remaining interval instead of rendering immediately. */
        Hyprutils::Memory::CWeakPointer<CAnlandBackend> self;

        /* ── Clipboard API ──────────────────────────────────────── */
        /* Send clipboard text to the consumer (OUTPUT_TYPE_CLIPBOARD). */
        void                sendClipboardToConsumer(const std::string& text);
        /* Get the clipboard text last received from the consumer. */
        const std::string&  getClipboardText() const { return m_clipboardText; }

        /* ── Resource request API ───────────────────────────────── */
        /* Request a service (e.g. camera) from the consumer. */
        void                sendResourcesRequest(uint32_t type, uint32_t arg0 = 0, uint32_t arg1 = 0, uint32_t arg2 = 0);
        /* Set a consumer runtime variable (e.g. pointer capture). */
        void                sendConsumerVar(uint32_t var, uint32_t value);

        /* ── Pointer-capture tracking ───────────────────────────── */
        /* Called by the compositor (Hyprland) when the active window's pointer
         * constraint state changes. Sends CONSUMER_VAR_CAPTURE_MOUSE to the
         * Android consumer to force relative mouse mode when a Wayland client
         * holds an active pointer lock or confine. */
        void                updateMouseCapture(bool active);

        /* ── Callbacks (set by Hyprland integration code) ───────── */
        /* Called when clipboard text arrives from the consumer. */
        void (*onClipboardFromConsumer)(const char* text, size_t len, void* userdata) = nullptr;
        void* m_clipboardFromConsumerUserdata = nullptr;
        /* Called when text input (IME commit) arrives from the consumer. */
        void (*onTextFromConsumer)(const char* text, size_t len, void* userdata) = nullptr;
        void* m_textFromConsumerUserdata = nullptr;

      private:
        friend class CAnlandOutput;
        bool                connectToDaemon();
        void                rebuildSwapchain();
        int                 openRenderNode();

        Hyprutils::Memory::CSharedPointer<CBackend> m_backend;
        void*                               m_display = nullptr;
        int                                 m_bufReadyFd = -1;
        int                                 m_renderNodeFd = -1;
        int                                 m_heartbeatTimerFd = -1; /* 200ms periodic heartbeat, always running */
        std::chrono::steady_clock::time_point m_lastFrameTime;      /* last trigger_refresh() timestamp */
        int                                 m_inputDataFd = -1; /* dup of data_fd for event-loop polling */
        bool                                m_inFallback = true;
        bool                                m_haveDmabufs = false;
        uint32_t                            m_screenWidth = 0;
        uint32_t                            m_screenHeight = 0;
        uint32_t                            m_screenRefresh = 60000;
        std::vector<Hyprutils::Memory::CSharedPointer<CAnlandOutput>> m_outputs;
        Hyprutils::Memory::CSharedPointer<CAnlandPointer>  m_pointer;
        Hyprutils::Memory::CSharedPointer<CAnlandKeyboard> m_keyboard;
        Hyprutils::Memory::CSharedPointer<CAnlandTouch>    m_touch;
        AnlandGestureRecognizer*                              m_gesture = nullptr;
        bool                                                m_inputDevicesEmitted = false;
        bool                                                m_captureMouseActive = false; /* last CONSUMER_VAR_CAPTURE_MOUSE value sent */
        std::string                                         m_clipboardText;

        /* (Removed) touch-to-pointer tracking — KWin-style pure touch forwarding
         * does not synthesize pointer events from touch. */
    };
};