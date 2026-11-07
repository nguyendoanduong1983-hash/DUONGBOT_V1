#include "roboeyes_face_renderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <esp_timer.h>

// Arduino compatibility hooks used by the upstream animation header.
extern "C" unsigned long millis() {
    return static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
}

long random(long upper_bound) {
    if (upper_bound <= 0) return 0;
    return static_cast<long>(std::rand() % static_cast<unsigned long>(upper_bound));
}

#include "third_party/roboeyes/src/FluxGarage_RoboEyes.h"

namespace {

class OledDrawBuffer {
public:
    OledDrawBuffer(int width, int height) : width_(width), height_(height), pixels_(width * height) {}

    void clearDisplay() { std::fill(pixels_.begin(), pixels_.end(), 0); }
    void display() {}

    void fillRoundRect(int x, int y, int width, int height, int radius, uint8_t color) {
        if (width <= 0 || height <= 0) return;
        const int left = std::max(0, x);
        const int top = std::max(0, y);
        const int right = std::min(width_ - 1, x + width - 1);
        const int bottom = std::min(height_ - 1, y + height - 1);
        const int corner = std::clamp(radius, 0, std::min(width, height) / 2);
        for (int py = top; py <= bottom; ++py) {
            for (int px = left; px <= right; ++px) {
                const int cx = px < x + corner ? x + corner
                                : px >= x + width - corner ? x + width - corner - 1
                                                           : px;
                const int cy = py < y + corner ? y + corner
                                : py >= y + height - corner ? y + height - corner - 1
                                                            : py;
                const int dx = px - cx;
                const int dy = py - cy;
                if (corner == 0 || dx * dx + dy * dy <= corner * corner) {
                    pixels_[static_cast<size_t>(py) * width_ + px] = color != 0;
                }
            }
        }
    }

    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color) {
        const int left = std::max(0, std::min({x0, x1, x2}));
        const int right = std::min(width_ - 1, std::max({x0, x1, x2}));
        const int top = std::max(0, std::min({y0, y1, y2}));
        const int bottom = std::min(height_ - 1, std::max({y0, y1, y2}));
        const auto edge = [](int ax, int ay, int bx, int by, int px, int py) {
            return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
        };
        for (int py = top; py <= bottom; ++py) {
            for (int px = left; px <= right; ++px) {
                const int e0 = edge(x0, y0, x1, y1, px, py);
                const int e1 = edge(x1, y1, x2, y2, px, py);
                const int e2 = edge(x2, y2, x0, y0, px, py);
                if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                    pixels_[static_cast<size_t>(py) * width_ + px] = color != 0;
                }
            }
        }
    }

    void CopyToSsd1306(uint8_t* framebuffer) const {
        if (framebuffer == nullptr) return;
        std::fill(framebuffer, framebuffer + width_ * height_ / 8, 0);
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                if (pixels_[static_cast<size_t>(y) * width_ + x]) {
                    framebuffer[x + (y / 8) * width_] |= static_cast<uint8_t>(1U << (y & 7));
                }
            }
        }
    }

private:
    int width_;
    int height_;
    std::vector<uint8_t> pixels_;
};

std::string NormalizeEmotion(const std::string& emotion) {
    std::string normalized = emotion.empty() ? "neutral" : emotion;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized == "😶") return "neutral";
    if (normalized == "🙂") return "happy";
    if (normalized == "😆") return "laughing";
    if (normalized == "😂") return "funny";
    if (normalized == "😔") return "sad";
    if (normalized == "😠") return "angry";
    if (normalized == "😭") return "crying";
    if (normalized == "😍") return "loving";
    if (normalized == "😳") return "embarrassed";
    if (normalized == "😲") return "surprised";
    if (normalized == "😱") return "shocked";
    if (normalized == "🤔") return "thinking";
    if (normalized == "😉") return "winking";
    if (normalized == "😎") return "cool";
    if (normalized == "😌") return "relaxed";
    if (normalized == "🤤") return "delicious";
    if (normalized == "😘") return "kissy";
    if (normalized == "😏") return "confident";
    if (normalized == "😴") return "sleepy";
    if (normalized == "😜") return "silly";
    if (normalized == "🙄") return "confused";
    return normalized;
}

}  // namespace

struct RoboEyesFaceRenderer::Impl {
    explicit Impl(int width, int height) : draw_buffer(width, height), eyes(draw_buffer) {
        eyes.begin(width, height, 15);
        eyes.setAutoblinker(true, 3, 4);
        eyes.setIdleMode(true, 1, 4);
    }

    OledDrawBuffer draw_buffer;
    RoboEyes<OledDrawBuffer> eyes;
};

RoboEyesFaceRenderer::RoboEyesFaceRenderer() = default;

RoboEyesFaceRenderer::~RoboEyesFaceRenderer() { delete impl_; }

bool RoboEyesFaceRenderer::Initialize(int width, int height) {
    if (width != 128 || height != 64) return false;
    delete impl_;
    impl_ = new (std::nothrow) Impl(width, height);
    if (impl_ == nullptr) return false;
    emotion_.clear();
    SetEmotion("neutral");
    return true;
}

void RoboEyesFaceRenderer::SetEmotion(const std::string& emotion) {
    if (impl_ == nullptr) return;
    const std::string mood = NormalizeEmotion(emotion);
    if (mood == emotion_) return;
    emotion_ = mood;

    auto& eyes = impl_->eyes;
    eyes.setIdleMode(false);
    eyes.setCuriosity(false);
    eyes.setSweat(false);
    eyes.setHFlicker(false);
    eyes.setVFlicker(false);
    eyes.setMood(DEFAULT);

    // Keep the reference firmware's RoboEyes emotion mapping and animation timing.
    if (mood == "smile") {
        eyes.setMood(HAPPY);
    } else if (mood == "laugh") {
        eyes.anim_laugh();
        eyes.setMood(HAPPY);
    } else if (mood == "cry") {
        eyes.setMood(TIRED);
        eyes.setSweat(true);
    } else if (mood == "wink") {
        eyes.anim_laugh();
        eyes.setMood(HAPPY);
    } else if (mood == "neutral") {
        eyes.setMood(DEFAULT);
        eyes.setIdleMode(true, 2, 4);
    } else if (mood == "happy" || mood == "joy") {
        eyes.setMood(HAPPY);
        eyes.setIdleMode(true, 1, 6);
    } else if (mood == "laughing" || mood == "funny") {
        eyes.anim_laugh();
        eyes.setMood(HAPPY);
    } else if (mood == "sad") {
        eyes.setMood(TIRED);
        eyes.setIdleMode(true, 1, 8);
    } else if (mood == "angry") {
        eyes.setMood(ANGRY);
        eyes.setIdleMode(true, 2, 2);
    } else if (mood == "crying") {
        eyes.setMood(TIRED);
        eyes.setSweat(true);
        eyes.setIdleMode(true, 1, 5);
    } else if (mood == "loving") {
        eyes.setMood(LOVING);
        eyes.setIdleMode(true, 1, 7);
    } else if (mood == "embarrassed") {
        eyes.setIdleMode(true, 2, 2);
        eyes.setMood(DEFAULT);
    } else if (mood == "surprised") {
        eyes.setMood(SURPRISED);
        eyes.setIdleMode(true, 2, 1);
    } else if (mood == "shocked") {
        eyes.anim_confused();
        eyes.setMood(DEFAULT);
    } else if (mood == "thinking") {
        eyes.setIdleMode(true, 2, 2);
        eyes.setMood(DEFAULT);
    } else if (mood == "winking" || mood == "speaking") {
        eyes.anim_laugh();
        eyes.setMood(HAPPY);
    } else if (mood == "cool") {
        eyes.setMood(HAPPY);
        eyes.setIdleMode(true, 1, 10);
    } else if (mood == "relaxed") {
        eyes.setMood(HAPPY);
        eyes.setIdleMode(true, 1, 12);
    } else if (mood == "delicious") {
        eyes.setMood(HAPPY);
        eyes.setIdleMode(true, 1, 8);
    } else if (mood == "kissy") {
        eyes.setMood(LOVING);
        eyes.setIdleMode(true, 2, 3);
    } else if (mood == "confident") {
        eyes.setMood(HAPPY);
        eyes.setIdleMode(true, 1, 9);
    } else if (mood == "sleepy") {
        eyes.setMood(SLEEPY);
    } else if (mood == "silly") {
        eyes.anim_laugh();
        eyes.setMood(HAPPY);
    } else if (mood == "confused") {
        eyes.setIdleMode(true, 3, 1);
        eyes.setMood(DEFAULT);
    } else if (mood == "curious") {
        eyes.setCuriosity(true);
        eyes.setIdleMode(true, 2, 2);
    } else if (mood == "wake") {
        eyes.setMood(SLEEPY);
        eyes.setIdleMode(true, 2, 3);
    } else if (mood == "listening") {
        eyes.setMood(HAPPY);
        eyes.setIdleMode(true, 1, 4);
    } else if (mood == "excited") {
        eyes.setMood(HAPPY);
        eyes.setVFlicker(true, 8);
        eyes.setIdleMode(true, 1, 2);
    } else if (mood == "twinkle") {
        eyes.setMood(HAPPY);
        eyes.setHFlicker(true, 3);
        eyes.setVFlicker(true, 3);
    } else if (mood == "bounce") {
        eyes.setMood(HAPPY);
        eyes.setVFlicker(true, 12);
        eyes.setIdleMode(true, 1, 1);
    } else {
        eyes.setMood(DEFAULT);
        eyes.setIdleMode(true, 2, 4);
    }
}

void RoboEyesFaceRenderer::Render(uint8_t* page_framebuffer, int width, int height) {
    if (impl_ == nullptr || width != 128 || height != 64 || page_framebuffer == nullptr) return;
    impl_->eyes.update();
    impl_->draw_buffer.CopyToSsd1306(page_framebuffer);
}
