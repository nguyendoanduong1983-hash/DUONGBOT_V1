#pragma once

#include "display/face_geometry.h"
#include "display/mochan_display.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

// Retains the Mochan state consumed by robot behavior when no main panel is fitted.
// Rendering-only calls intentionally have no hardware side effects.
class HeadlessMochanState {
public:
    using AmbientActivity = MochanDisplay::AmbientActivity;

    void SetDeskModeActive(bool active) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_attention_active_) {
            return;
        }
        if (!active) {
            desk_mode_active_.store(false);
        } else if (!face_override_active_ && !emotion_active_) {
            desk_mode_active_.store(true);
        }
    }
    bool IsDeskModeActive() const { return desk_mode_active_.load(); }
    void SetDeskModeUse24Hour(bool) {}
    void SetDeskModeEnvironment(bool, float, bool, float) {}
    void SetWifiConnected(bool) {}
    void SetBatteryStatus(int, float, bool) {}
    void ShowBootSplash() {}
    void HideBootSplash() {}

    bool CanCameraAttentionOwnFace() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !face_override_active_ && !emotion_active_;
    }
    bool StartCameraAttention(bool from_desk_mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (face_override_active_ || emotion_active_ || camera_attention_active_ ||
            desk_mode_active_.load() != from_desk_mode) {
            return false;
        }
        camera_attention_active_ = true;
        desk_mode_active_.store(false);
        camera_attention_centered_.store(false);
        current_emotion_ = "neutral";
        return true;
    }
    bool SetCameraAttentionTarget(float x, float y) {
        if (!camera_attention_active_.load()) {
            return false;
        }
        x = std::clamp(x, -1.0f, 1.0f);
        y = std::clamp(y, -1.0f, 1.0f);
        applied_x_.store(x);
        applied_y_.store(y);
        camera_attention_centered_.store(std::fabs(x) < 0.03f && std::fabs(y) < 0.03f);
        return true;
    }
    void EndCameraAttention(bool return_to_desk_mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_attention_active_.store(false);
        camera_attention_centered_.store(true);
        applied_x_.store(0.0f);
        applied_y_.store(0.0f);
        if (return_to_desk_mode && !face_override_active_ && !emotion_active_) {
            desk_mode_active_.store(true);
        }
    }
    bool IsCameraAttentionActive() const { return camera_attention_active_.load(); }
    bool IsCameraAttentionCentered() const { return camera_attention_centered_.load(); }
    float GetCameraAttentionAppliedX() const { return applied_x_.load(); }
    float GetCameraAttentionAppliedY() const { return applied_y_.load(); }
    int GetCameraAttentionAppliedPxX() const { return 0; }
    int GetCameraAttentionAppliedPxY() const { return 0; }

    void SetEmotion(const char* emotion) {
        if (emotion == nullptr || !MochanDisplay::IsSupportedEmotion(emotion)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        current_emotion_ = emotion;
        emotion_active_ = current_emotion_ != "neutral" && current_emotion_ != "robot_2";
        camera_attention_active_.store(false);
        camera_attention_centered_.store(true);
        applied_x_.store(0.0f);
        applied_y_.store(0.0f);
        desk_mode_active_.store(false);
    }
    std::string GetCurrentEmotion() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_emotion_;
    }
    void SetFaceOverrideActive(bool active) {
        std::lock_guard<std::mutex> lock(mutex_);
        face_override_active_ = active;
        if (active) {
            camera_attention_active_.store(false);
            camera_attention_centered_.store(true);
            applied_x_.store(0.0f);
            applied_y_.store(0.0f);
            desk_mode_active_.store(false);
        }
    }
    void RestoreActivityFace() {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_attention_active_.store(false);
        camera_attention_centered_.store(true);
        applied_x_.store(0.0f);
        applied_y_.store(0.0f);
        desk_mode_active_.store(false);
        current_emotion_ = "neutral";
        emotion_active_ = false;
    }
    AmbientActivity GetAmbientActivity() const { return AmbientActivity::kIdle; }
    AmbientGazePersonality GetAmbientGazePersonality() const {
        return AmbientGazePersonality::kNeutral;
    }
    void ResetAmbientPrimitives(uint32_t) {}
    void SetAmbientGazeTarget(uint32_t, int8_t, int8_t) {}
    void ClearAmbientGaze(uint32_t) {}
    void TriggerAmbientMouth(uint32_t) {}
    void TriggerAmbientYawn(uint32_t) {}
    void SetAmbientBaseFace(uint32_t, const std::string&) {}
    void ClearAmbientBaseFace(uint32_t) {}

    bool GetFaceGeometry(const std::string& emotion, FaceGeometry& geometry,
                         bool* customized = nullptr) const {
        return face_geometry_.Get(emotion, geometry, customized);
    }
    bool GetFaceGeometryLayoutYOffset(const std::string& emotion, int& offset_y) const {
        FaceGeometry geometry{};
        if (!face_geometry_.Get(emotion, geometry)) {
            return false;
        }
        offset_y = 0;
        return true;
    }
    bool SaveFaceGeometry(const std::string& emotion, FaceGeometry geometry) {
        return face_geometry_.Save(emotion, geometry);
    }
    bool ResetFaceGeometry(const std::string& emotion) { return face_geometry_.Reset(emotion); }
    bool ResetAllFaceGeometry() { return face_geometry_.ResetAll(); }

private:
    mutable std::mutex mutex_;
    mutable FaceGeometryStore face_geometry_;
    std::atomic_bool desk_mode_active_{false};
    std::atomic_bool camera_attention_active_{false};
    std::atomic_bool camera_attention_centered_{true};
    std::atomic<float> applied_x_{0.0f};
    std::atomic<float> applied_y_{0.0f};
    bool face_override_active_ = false;
    bool emotion_active_ = false;
    std::string current_emotion_ = "neutral";
};
