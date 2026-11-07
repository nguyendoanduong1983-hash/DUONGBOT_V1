#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <atomic>
#include <mutex>

class DottedSpectrumVisualizer {
public:
    static DottedSpectrumVisualizer& GetInstance();

    // Bắt đầu hiệu ứng sóng nhạc ma trận hạt với tên bài hát
    void Start(const std::string& title);

    // Dừng hiệu ứng sóng nhạc và xóa sạch trạng thái
    void Stop();

    // Cập nhật tên bài hát đang phát
    void SetTitle(const std::string& title);

    // Nạp dữ liệu PCM âm thanh thời gian thực từ Decoder
    void FeedAudio(const int16_t* samples, size_t count);

    // Kiểm tra trạng thái đang hoạt động
    bool IsActive() const { return is_active_.load(std::memory_order_relaxed); }

    // Render trực tiếp khung hình STYLE_DOT_BARS vào framebuffer 128x64 (1024 bytes)
    void Render(uint8_t* framebuffer, int width = 128, int height = 64);

private:
    DottedSpectrumVisualizer();
    ~DottedSpectrumVisualizer() = default;

    DottedSpectrumVisualizer(const DottedSpectrumVisualizer&) = delete;
    DottedSpectrumVisualizer& operator=(const DottedSpectrumVisualizer&) = delete;

    void ClearBuffer(uint8_t* buffer);
    void SetPixel(uint8_t* buffer, int x, int y, bool color = true);
    void DrawFastHLine(uint8_t* buffer, int x1, int x2, int y, bool color = true);
    void DrawChar(uint8_t* buffer, int x, int y, char c);
    void DrawString(uint8_t* buffer, int x, int y, const char* str);
    void DrawMusicNote(uint8_t* buffer, int x, int y);
    std::string TransliterateVietnamese(const std::string& str);

    std::atomic<bool> is_active_{false};
    std::atomic<int64_t> last_audio_feed_ms_{0};

    std::mutex title_mutex_;
    std::string title_;
    std::string clean_title_;
    int scroll_offset_ = 0;
    uint32_t frame_count_ = 0;

    static const int NUM_BARS = 16;
    float current_height_[NUM_BARS]{};
    float target_height_[NUM_BARS]{};
    float peak_height_[NUM_BARS]{};
    int peak_hold_[NUM_BARS]{};
    float peak_decay_[NUM_BARS]{};
};
