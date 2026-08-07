#include <aquamarine/backend/AnlandBackend.hpp>
#include <aquamarine/backend/AnlandOutput.hpp>
#include "AnlandGesture.hpp"
#include "AnlandGesture.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <poll.h>
#include <time.h>
#include <format>

extern "C" {
#include "display_producer.h"
#include "anland_camera.h"
}

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
template <typename T>
using SP = CSharedPointer<T>;

void CAnlandBackend::onInputReadable() {
    display_ctx* ctx = (display_ctx*)m_display;
    if (!ctx || m_inFallback)
        return;

    m_backend->log(AQ_LOG_TRACE, "anland: onInputReadable() called");

    InputEvent ev;
    int count = 0;
    while (::poll_input_event(ctx, &ev, 0) > 0) {
        count++;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t timeMs = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);

        if (count == 1) {
            m_backend->log(AQ_LOG_TRACE, std::format("anland: onInputReadable processing event type={}", (int)ev.type));
        }

        switch (ev.type) {
            case INPUT_TYPE_POINTER_MOTION: {
                if (m_pointer) {
                    Vector2D relDelta((double)ev.pointer_motion.dx, (double)ev.pointer_motion.dy);

                    /* Suppress zero-delta: Android sends continuous pointer
                     * motion even when idle. Skip to avoid unnecessary
                     * cursor updates and damage-driven renders. */
                    if (relDelta.x == 0.0 && relDelta.y == 0.0)
                        break;


                    m_pointer->events.move.emit(IPointer::SMoveEvent{
                        .timeMs  = timeMs,
                        .delta   = relDelta,
                        .unaccel = relDelta,
                    });

                    m_pointer->events.frame.emit();
                }
                break;
            }
            case INPUT_TYPE_POINTER_BUTTON: {
                if (m_pointer) {
                    m_pointer->events.button.emit(IPointer::SButtonEvent{
                        .timeMs  = timeMs,
                        .button  = ev.pointer_button.button,
                        .pressed = ev.pointer_button.pressed != 0,
                    });
                    m_pointer->events.frame.emit();
                }
                break;
            }
            case INPUT_TYPE_POINTER_AXIS: {
                if (m_pointer) {
                    /* Consumer applies its own scroll direction logic:
                     *   Android AXIS_VSCROLL: wheel down → +1, wheel up → -1
                     *   MainActivity.java: sendMouseScroll(0, -vScroll * 10f)
                     *   Touchpad.java: sendScroll(0, -avgDy * scale)
                     * So the wire value is already negated: swipe/wheel down → negative.
                     *
                     * Wayland convention: positive delta = scroll down (content moves up).
                     * The consumer's negated value means swipe down → negative delta →
                     * content moves down, which is the expected "traditional" direction
                     * (non-natural scrolling). We pass the value through as-is.
                     *
                     * discrete is set to ±120 (one standard mouse wheel step) because
                     * nativeSendMouseScroll always sets discrete=0. Sign follows delta
                     * so that Hyprland's discrete emulation logic works correctly.
                     * source is WHEEL for ACTION_SCROLL events; Touchpad.java scroll
                     * events also come through here but the consumer path is the same. */
                    IPointer::ePointerAxis axis =
                        ev.pointer_axis.axis == 0 ? IPointer::AQ_POINTER_AXIS_VERTICAL
                                                  : IPointer::AQ_POINTER_AXIS_HORIZONTAL;
                    double val = (double)ev.pointer_axis.value;
                    m_pointer->events.axis.emit(IPointer::SAxisEvent{
                        .timeMs   = timeMs,
                        .axis     = axis,
                        .source   = IPointer::AQ_POINTER_AXIS_SOURCE_WHEEL,
                        .delta    = val,
                        .discrete = (val != 0.0) ? (val > 0.0 ? 120.0 : -120.0) : 0.0,
                    });
                    m_pointer->events.frame.emit();
                }
                break;
            }
            case INPUT_TYPE_KEY: {
                /* Log keycodes for debugging */
                m_backend->log(AQ_LOG_DEBUG, std::format("anland: key event action={} keycode={}",
                    (int)ev.key.action, (int)ev.key.keycode));

                /* The consumer sends evdev (Linux input-event-codes.h) keycodes,
                 * which is exactly what the compositor's keyboard handler expects.
                 * Forward the keycode directly — no ASCII conversion needed here,
                 * because the soft keyboard delivers text through INPUT_TYPE_TEXT_INPUT
                 * (see SystemIME.java → ForwardingInputConnection → sendTextInput).
                 * KEY events are for editing keys (Enter, Backspace, Tab, Escape,
                 * arrows, modifiers) and keyboard shortcuts. */
                if (m_keyboard) {
                    m_keyboard->events.key.emit(IKeyboard::SKeyEvent{
                        .timeMs  = timeMs,
                        .key     = (uint32_t)ev.key.keycode,
                        .pressed = ev.key.action == INPUT_ACTION_DOWN,
                    });
                }
                break;
            }
            case INPUT_TYPE_TOUCH: {
                Vector2D normPos(
                    m_screenWidth > 0 ? (double)ev.touch.x / (double)m_screenWidth : 0.0,
                    m_screenHeight > 0 ? (double)ev.touch.y / (double)m_screenHeight : 0.0);

                int touchId = ev.touch.pointer_id;
                if (touchId < 0 || touchId > 15)
                    break;

                /* Route through gesture recognizer (niri-style).
                 * When gesture is active, suppress raw touch events. */
                switch (ev.touch.action) {
                    case INPUT_ACTION_DOWN:
                        if (m_gesture) m_gesture->onTouchDown(touchId, normPos.x, normPos.y);
                        if (m_touch && !(m_gesture && m_gesture->isActive())) {
                            m_touch->events.down.emit(ITouch::SDownEvent{
                                .timeMs  = timeMs, .touchID = touchId, .pos = normPos});
                            m_touch->events.frame.emit();
                        }
                        break;
                    case INPUT_ACTION_UP:
                        if (m_gesture) m_gesture->onTouchUp(touchId);
                        if (m_touch && !(m_gesture && m_gesture->isActive())) {
                            m_touch->events.up.emit(ITouch::SUpEvent{
                                .timeMs  = timeMs, .touchID = touchId});
                            m_touch->events.frame.emit();
                        }
                        break;
                    case INPUT_ACTION_MOVE:
                        if (m_gesture) m_gesture->onTouchMotion(touchId, normPos.x, normPos.y);
                        if (m_touch && !(m_gesture && m_gesture->isActive())) {
                            m_touch->events.move.emit(ITouch::SMotionEvent{
                                .timeMs  = timeMs, .touchID = touchId, .pos = normPos});
                            m_touch->events.frame.emit();
                        }
                        break;
                }
                break;
            }
            case INPUT_TYPE_TOUCH_FRAME: {
                if (m_gesture) m_gesture->onTouchFrame();
                if (m_touch && !(m_gesture && m_gesture->isActive()))
                    m_touch->events.frame.emit();
                break;
            }
            case INPUT_TYPE_DISPLAY_REFRESH: {
                const uint32_t refreshMhz = ev.display.refresh_mhz;
                if (refreshMhz > 0 && refreshMhz != m_screenRefresh) {
                    m_backend->log(AQ_LOG_DEBUG,
                        std::format("anland: display refresh updated: {} -> {} mHz",
                            m_screenRefresh, refreshMhz));
                    m_screenRefresh = refreshMhz;

                    /* Update the output mode so Hyprland's RenderLoop uses
                     * the host's actual display refresh rate for scheduling
                     * frames.  This mirrors niri's behaviour: the consumer
                     * reports its real vsync rate over the input channel and
                     * the producer adjusts its output mode to match. */
                    if (!m_outputs.empty()) {
                        auto output = m_outputs[0];
                        output->modes.clear();
                        output->modes.push_back(makeShared<SOutputMode>(
                            Vector2D((double)m_screenWidth, (double)m_screenHeight),
                            (int)m_screenRefresh, true));
                        /* Emit state event to notify Hyprland of the new mode */
                        output->events.state.emit(IOutput::SStateEvent{
                            .size = Vector2D((double)m_screenWidth, (double)m_screenHeight),
                        });
                    }
                }
                break;
            }
            case INPUT_TYPE_CLIPBOARD: {
                const uint32_t size = ev.clipboard.size;
                if (size == 0) {
                    m_clipboardText.clear();
                    if (onClipboardFromConsumer)
                        onClipboardFromConsumer("", 0, m_clipboardFromConsumerUserdata);
                    break;
                }
                std::string text(size, '\0');
                if (::poll_input_event_extend_data(ctx, text.data(), size, 5000) == 1) {
                    m_clipboardText = text;
                    if (onClipboardFromConsumer)
                        onClipboardFromConsumer(text.data(), text.size(), m_clipboardFromConsumerUserdata);
                }
                break;
            }
            case INPUT_TYPE_TEXT_INPUT: {
                uint32_t textSize = ev.text_input.size;
                m_backend->log(AQ_LOG_DEBUG, std::format("anland: TEXT_INPUT size={} hasCallback={}",
                    textSize, (bool)onTextFromConsumer));
                if (textSize == 0)
                    break;
                std::string text(textSize, '\0');
                if (::poll_input_event_extend_data(ctx, text.data(), textSize, 5000) == 1) {
                    m_backend->log(AQ_LOG_DEBUG, std::format("anland: TEXT_INPUT data='{}'", text));
                    if (onTextFromConsumer)
                        onTextFromConsumer(text.data(), text.size(), m_textFromConsumerUserdata);
                }
                break;
            }
            case INPUT_TYPE_ACTION:
            case INPUT_TYPE_RESOURCE_INVALID:
                break;
            case INPUT_TYPE_RESOURCE: {
                const uint32_t fdnum = ev.resource.fdnum;
                if (fdnum == 0 || fdnum > 16)
                    break;
                int fds[16];
                int got = ::poll_input_event_extend_fds(ctx, fds, (int)fdnum, 5000);
                if (got < (int)fdnum) {
                    for (int i = 0; i < got; i++)
                        close(fds[i]);
                    break;
                }
                /* Dispatch to camera engine if this is a camera resource.
                 * fds[0] = ctrl_fd, fds[1..N-1] = per-camera stream fds. */
                if (ev.resource.type == SERVICE_TYPE_CAMERA && fdnum >= 2) {
                    anland_camera_set_resources(fds[0], fds + 1, (int)fdnum - 1);
                } else {
                    for (uint32_t i = 0; i < fdnum; i++)
                        close(fds[i]);
                }
                break;
            }
            default:
                break;
        }
    }
}