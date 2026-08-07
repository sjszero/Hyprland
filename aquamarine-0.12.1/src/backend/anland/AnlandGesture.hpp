#pragma once
#include <cstdint>
#include <chrono>
#include <map>
#include <aquamarine/input/Input.hpp>
#include <hyprutils/math/Vector2D.hpp>

/* ── Anland Touch Gesture Recognizer ──────────────────────────────────
 *
 * Ported from niri's TouchGestureRecognizer + TouchLongPressRecognizer.
 * Detects multi-finger swipes and long-press on touchscreen input from
 * the Android consumer. Emits IPointer::swipe* events that Hyprland's
 * CInputManager::onSwipe* routes to gesture actions (workspace switch,
 * monitor switch, etc.).
 *
 * Usage:
 *   AnlandGestureRecognizer recognizer;
 *   recognizer.pointer = m_pointer;  // IPointer* to emit events on
 *   recognizer.screenW = 2480;
 *   recognizer.screenH = 1116;
 *
 *   // Per touch event:
 *   recognizer.onTouchDown(pointerId, x, y);
 *   recognizer.onTouchUp(pointerId);
 *   recognizer.onTouchMotion(pointerId, x, y);
 *   recognizer.onTouchFrame();   // batch end
 *
 *   // Long press polling (call periodically):
 *   recognizer.pollLongPress();
 *
 * Design: 3 fingers = workspace switch, 4 fingers = monitor switch.
 * Long press (>500ms, stationary) = right click emulation.
 * Default thresholds are tuned for mobile touchscreens (hand-friendly).
 * ─────────────────────────────────────────────────────────────────── */

namespace Aquamarine {

    struct AnlandTouchPoint {
        float x, y;
        uint64_t timeUs;
    };

    class AnlandGestureRecognizer {
      public:
        IPointer* pointer = nullptr;
        float     screenW = 1920, screenH = 1080;

        /* ── Thresholds ──────────────────────────────────────────── */
        float     swipeThreshold = 10.0f;   // px before swipe begins
        uint64_t  longPressTimeUs = 500000; // 500ms
        float     longPressRadius = 20.0f;  // px tolerance for long press

        /* ── Touch input ─────────────────────────────────────────── */
        void onTouchDown(uint32_t pointerId, float x, float y) {
            auto now = nowUs();
            m_points[pointerId] = {x, y, now};

            // Long press tracking: single finger
            if (m_points.size() == 1 && m_state == IDLE) {
                m_longPressId = pointerId;
                m_longPressStart = {x, y, now};
                m_longPressActive = true;
            } else {
                m_longPressActive = false;
            }

            // Start tracking when 3+ fingers
            if (m_points.size() >= 3 && m_state == IDLE) {
                m_state = TRACKING;
                m_centroid = computeCentroid();
                m_swipeFingers = (int)m_points.size();
            }
        }

        void onTouchUp(uint32_t pointerId) {
            m_points.erase(pointerId);

            if (pointerId == m_longPressId)
                m_longPressActive = false;

            if (m_state == SWIPING) {
                if (m_points.size() < 3) {
                    // End swipe
                    if (pointer) {
                        pointer->events.swipeEnd.emit(IPointer::SSwipeEndEvent{
                            .timeMs = (uint32_t)(nowUs() / 1000),
                            .cancelled = false,
                        });
                    }
                    m_state = IDLE;
                }
            } else if (m_state == TRACKING && m_points.size() < 3) {
                m_state = IDLE;
            }
        }

        void onTouchMotion(uint32_t pointerId, float x, float y) {
            auto it = m_points.find(pointerId);
            if (it == m_points.end()) return;

            it->second.x = x;
            it->second.y = y;

            // Check long press: moved too far → cancel
            if (m_longPressActive && pointerId == m_longPressId) {
                float dx = x - m_longPressStart.x;
                float dy = y - m_longPressStart.y;
                if (dx * dx + dy * dy > longPressRadius * longPressRadius)
                    m_longPressActive = false;
            }

            if (m_state == TRACKING) {
                auto newCentroid = computeCentroid();
                float dx = newCentroid.x - m_centroid.x;
                float dy = newCentroid.y - m_centroid.y;
                if (dx * dx + dy * dy > swipeThreshold * swipeThreshold) {
                    // Begin swipe
                    m_state = SWIPING;
                    if (pointer) {
                        pointer->events.swipeBegin.emit(IPointer::SSwipeBeginEvent{
                            .timeMs = (uint32_t)(nowUs() / 1000),
                            .fingers = (uint32_t)m_swipeFingers,
                        });
                    }
                }
            }

            if (m_state == SWIPING) {
                auto newCentroid = computeCentroid();
                float dx = newCentroid.x - m_centroid.x;
                float dy = newCentroid.y - m_centroid.y;
                m_centroid = newCentroid;

                if (pointer && (dx != 0.0f || dy != 0.0f)) {
                    pointer->events.swipeUpdate.emit(IPointer::SSwipeUpdateEvent{
                        .timeMs = (uint32_t)(nowUs() / 1000),
                        .fingers = (uint32_t)m_swipeFingers,
                        .delta = Hyprutils::Math::Vector2D((double)dx, (double)dy),
                    });
                }
            }
        }

        void onTouchFrame() {
            // Niri: frame() processes gesture signals and determines suppression.
            // We process swipes inline in onTouchMotion, so frame is a no-op.
        }

        /* ── Long press polling ──────────────────────────────────── */
        void pollLongPress() {
            if (!m_longPressActive || !pointer)
                return;

            auto now = nowUs();
            auto it = m_points.find(m_longPressId);
            if (it == m_points.end()) return;

            if (now - m_longPressStart.timeUs < longPressTimeUs)
                return;

            // Long press triggered → right click
            m_longPressActive = false;
            m_points.clear();
            m_state = IDLE;

            uint32_t timeMs = (uint32_t)(now / 1000);
            float x = it->second.x;
            float y = it->second.y;

            // Move pointer to long press position
            pointer->events.move.emit(IPointer::SMoveEvent{
                .timeMs = timeMs,
                .delta = Hyprutils::Math::Vector2D(0, 0),
                .unaccel = Hyprutils::Math::Vector2D(0, 0),
            });

            // Right button press + release
            constexpr uint32_t BTN_RIGHT = 0x111;
            pointer->events.button.emit(IPointer::SButtonEvent{
                .timeMs = timeMs,
                .button = BTN_RIGHT,
                .pressed = true,
            });
            pointer->events.button.emit(IPointer::SButtonEvent{
                .timeMs = timeMs,
                .button = BTN_RIGHT,
                .pressed = false,
            });
            pointer->events.frame.emit();
        }

        bool isActive() const { return m_state != IDLE; }

        void cancel() {
            if (m_state == SWIPING && pointer) {
                pointer->events.swipeEnd.emit(IPointer::SSwipeEndEvent{
                    .timeMs = (uint32_t)(nowUs() / 1000),
                    .cancelled = true,
                });
            }
            m_points.clear();
            m_state = IDLE;
            m_longPressActive = false;
        }

      private:
        enum State { IDLE, TRACKING, SWIPING };
        State m_state = IDLE;
        int m_swipeFingers = 0;

        std::map<uint32_t, AnlandTouchPoint> m_points;
        Hyprutils::Math::Vector2D m_centroid;

        // Long press
        bool m_longPressActive = false;
        uint32_t m_longPressId = 0;
        AnlandTouchPoint m_longPressStart;

        Hyprutils::Math::Vector2D computeCentroid() {
            float cx = 0, cy = 0;
            for (auto& [id, pt] : m_points) { cx += pt.x; cy += pt.y; }
            float n = (float)m_points.size();
            return {cx / n, cy / n};
        }

        static uint64_t nowUs() {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        }
    };

} // namespace Aquamarine