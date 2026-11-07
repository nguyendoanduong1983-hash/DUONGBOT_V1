#include "dotted_spectrum_visualizer.h"
#include "platform/board.h"
#include "audio/audio_codec.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <cmath>
#include <algorithm>

#define TAG "DottedVisualizer"

// Bảng font ASCII 5x7 (từ ký tự 32 ' ' đến 126 '~')
static const uint8_t FONT_5X7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' ' 32
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // '!' 33
    {0x00, 0x07, 0x00, 0x07, 0x00}, // '"' 34
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // '#' 35
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // '$' 36
    {0x23, 0x13, 0x08, 0x64, 0x62}, // '%' 37
    {0x36, 0x49, 0x55, 0x22, 0x50}, // '&' 38
    {0x00, 0x05, 0x03, 0x00, 0x00}, // ''' 39
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // '(' 40
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // ')' 41
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // '*' 42
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // '+' 43
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ',' 44
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-' 45
    {0x00, 0x60, 0x60, 0x00, 0x00}, // '.' 46
    {0x20, 0x10, 0x08, 0x04, 0x02}, // '/' 47
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0' 48
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1' 49
    {0x42, 0x61, 0x51, 0x49, 0x46}, // '2' 50
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // '3' 51
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4' 52
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5' 53
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // '6' 54
    {0x01, 0x71, 0x09, 0x05, 0x03}, // '7' 55
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8' 56
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // '9' 57
    {0x00, 0x36, 0x36, 0x00, 0x00}, // ':' 58
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ';' 59
    {0x08, 0x14, 0x22, 0x41, 0x00}, // '<' 60
    {0x14, 0x14, 0x14, 0x14, 0x14}, // '=' 61
    {0x00, 0x41, 0x22, 0x14, 0x08}, // '>' 62
    {0x02, 0x01, 0x51, 0x09, 0x06}, // '?' 63
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // '@' 64
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 'A' 65
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 'B' 66
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C' 67
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 'D' 68
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 'E' 69
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 'F' 70
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 'G' 71
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 'H' 72
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 'I' 73
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 'J' 74
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 'K' 75
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 'L' 76
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 'M' 77
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 'N' 78
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 'O' 79
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 'P' 80
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 'Q' 81
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 'R' 82
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 'S' 83
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 'T' 84
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 'U' 85
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 'V' 86
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 'W' 87
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 'X' 88
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 'Y' 89
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 'Z' 90
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // '[' 91
    {0x02, 0x04, 0x08, 0x10, 0x20}, // '\' 92
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // ']' 93
    {0x04, 0x02, 0x01, 0x02, 0x04}, // '^' 94
    {0x40, 0x40, 0x40, 0x40, 0x40}, // '_' 95
    {0x00, 0x01, 0x02, 0x04, 0x00}, // '`' 96
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 'a' 97
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 'b' 98
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 'c' 99
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 'd' 100
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 'e' 101
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 'f' 102
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // 'g' 103
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 'h' 104
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 'i' 105
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 'j' 106
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 'k' 107
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 'l' 108
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 'm' 109
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 'n' 110
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o' 111
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 'p' 112
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 'q' 113
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 'r' 114
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 's' 115
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 't' 116
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 'u' 117
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 'v' 118
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 'w' 119
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 'x' 120
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 'y' 121
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 'z' 122
    {0x00, 0x08, 0x36, 0x41, 0x00}, // '{' 123
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // '|' 124
    {0x00, 0x41, 0x36, 0x08, 0x00}, // '}' 125
    {0x08, 0x08, 0x2A, 0x1C, 0x08}  // '~' 126
};

DottedSpectrumVisualizer& DottedSpectrumVisualizer::GetInstance() {
    static DottedSpectrumVisualizer instance;
    return instance;
}

DottedSpectrumVisualizer::DottedSpectrumVisualizer() {
    for (int i = 0; i < NUM_BARS; ++i) {
        current_height_[i] = 2.0f;
        target_height_[i] = 2.0f;
        peak_height_[i] = 2.0f;
        peak_hold_[i] = 0;
        peak_decay_[i] = 0.0f;
    }
}

std::string DottedSpectrumVisualizer::TransliterateVietnamese(const std::string& str) {
    std::string out;
    out.reserve(str.length());
    for (size_t i = 0; i < str.length(); ) {
        unsigned char c = (unsigned char)str[i];
        if (c < 0x80) {
            out.push_back((char)c);
            i++;
        } else if ((c == 0xC3 || c == 0xC4 || c == 0xC5 || c == 0xE1) && i + 1 < str.length()) {
            unsigned char c2 = (unsigned char)str[i+1];
            uint16_t unicode = ((uint16_t)c << 8) | c2;
            switch (unicode) {
                case 0xC380: case 0xC381: case 0xC382: case 0xC383: case 0xC482: out.push_back('A'); break;
                case 0xC3A0: case 0xC3A1: case 0xC3A2: case 0xC3A3: case 0xC483: out.push_back('a'); break;
                case 0xC388: case 0xC389: case 0xC38A: out.push_back('E'); break;
                case 0xC3A8: case 0xC3A9: case 0xC3AA: out.push_back('e'); break;
                case 0xC38C: case 0xC38D: out.push_back('I'); break;
                case 0xC3AC: case 0xC3AD: out.push_back('i'); break;
                case 0xC392: case 0xC393: case 0xC394: case 0xC395: case 0xC6A0: out.push_back('O'); break;
                case 0xC3B2: case 0xC3B3: case 0xC3B4: case 0xC3B5: case 0xC6A1: out.push_back('o'); break;
                case 0xC399: case 0xC39A: case 0xC6AF: out.push_back('U'); break;
                case 0xC3B9: case 0xC3BA: case 0xC6B0: out.push_back('u'); break;
                case 0xC39D: out.push_back('Y'); break;
                case 0xC3BD: out.push_back('y'); break;
                case 0xC490: out.push_back('D'); break;
                case 0xC491: out.push_back('d'); break;
                default:
                    if (c == 0xE1 && i + 2 < str.length()) {
                        i += 3;
                        out.push_back('?');
                        continue;
                    }
                    out.push_back(' ');
                    break;
            }
            i += 2;
        } else {
            i++;
            out.push_back(' ');
        }
    }
    return out;
}

void DottedSpectrumVisualizer::Start(const std::string& title) {
    SetTitle(title);
    scroll_offset_ = 0;
    frame_count_ = 0;
    last_audio_feed_ms_.store(esp_timer_get_time() / 1000);
    is_active_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Dotted Spectrum Visualizer started for '%s'", clean_title_.c_str());
}

void DottedSpectrumVisualizer::Stop() {
    is_active_.store(false, std::memory_order_release);
    for (int i = 0; i < NUM_BARS; ++i) {
        current_height_[i] = 2.0f;
        target_height_[i] = 2.0f;
        peak_height_[i] = 2.0f;
    }
    ESP_LOGI(TAG, "Dotted Spectrum Visualizer stopped");
}

void DottedSpectrumVisualizer::SetTitle(const std::string& title) {
    std::lock_guard<std::mutex> lock(title_mutex_);
    title_ = title;
    clean_title_ = TransliterateVietnamese(title_);
    scroll_offset_ = 0;
}

void DottedSpectrumVisualizer::FeedAudio(const int16_t* samples, size_t count) {
    if (!samples || count == 0) return;
    if (!is_active_.load(std::memory_order_relaxed)) {
        return; // Chỉ cập nhật dữ liệu khi visualizer đã được kích hoạt qua Start()
    }

    last_audio_feed_ms_.store(esp_timer_get_time() / 1000);

    float band_accum[NUM_BARS] = {0};
    int band_counts[NUM_BARS] = {0};

    static const float BAND_RESONANCE[NUM_BARS] = {
        1.25f, 0.90f, 1.35f, 0.85f,   // Bass (0..3)
        1.30f, 0.88f, 1.40f, 0.92f,   // Low-Mid (4..7)
        1.38f, 0.85f, 1.42f, 0.95f,   // High-Mid (8..11)
        1.20f, 1.45f, 1.05f, 1.50f    // Treble (12..15)
    };

    const size_t step = std::max((size_t)1, count / 256);
    for (size_t i = 8; i < count; i += step) {
        int16_t s0 = samples[i];
        int16_t s1 = samples[i - 1];
        int16_t s2 = samples[i - 2];
        int16_t s4 = samples[i - 4];
        int16_t s8 = samples[i - 8];

        int amp = std::abs((int)s0);
        int diff1 = std::abs((int)s0 - (int)s1);
        int diff2 = std::abs((int)s0 - (int)s2);
        int diff4 = std::abs((int)s0 - (int)s4);
        int diff8 = std::abs((int)s0 - (int)s8);
        int low_sum = std::abs((int)s0 + (int)s2 + (int)s4 + (int)s8) / 4;

        float e[NUM_BARS];
        e[0]  = (float)low_sum * 1.30f;
        e[1]  = (float)amp * 0.70f + (float)low_sum * 0.40f;
        e[2]  = (float)low_sum * 1.10f + (float)diff8 * 0.35f;
        e[3]  = (float)diff8 * 0.80f + (float)amp * 0.30f;

        e[4]  = (float)diff8 * 0.95f + (float)diff4 * 0.40f;
        e[5]  = (float)diff4 * 0.75f + (float)amp * 0.25f;
        e[6]  = (float)diff4 * 1.15f + (float)diff2 * 0.30f;
        e[7]  = (float)diff2 * 0.80f + (float)diff4 * 0.45f;

        e[8]  = (float)diff2 * 1.20f + (float)diff1 * 0.30f;
        e[9]  = (float)diff2 * 0.75f + (float)amp * 0.20f;
        e[10] = (float)diff1 * 1.05f + (float)diff2 * 0.50f;
        e[11] = (float)diff1 * 0.85f + (float)diff4 * 0.20f;

        e[12] = (float)diff1 * 1.10f;
        e[13] = (float)diff1 * 1.35f;
        e[14] = (float)diff1 * 0.90f + (float)diff2 * 0.30f;
        e[15] = (float)diff1 * 1.60f;

        for (int b = 0; b < NUM_BARS; ++b) {
            band_accum[b] += e[b] * BAND_RESONANCE[b];
            band_counts[b]++;
        }
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    int vol = codec ? codec->output_volume() : 100;
    float vol_factor = (float)vol / 100.0f;
    if (vol_factor < 0.20f) vol_factor = 0.20f;

    float max_peak_height = 45.0f * vol_factor;
    if (max_peak_height < 8.0f) max_peak_height = 8.0f;

    for (int b = 0; b < NUM_BARS; ++b) {
        if (band_counts[b] > 0) {
            float avg = band_accum[b] / (float)band_counts[b];
            float norm = std::min(1.0f, avg / 4500.0f);
            norm = std::pow(norm, 1.25f);
            float h = 2.0f + norm * max_peak_height;
            if (h > target_height_[b]) {
                target_height_[b] = h;
            }
        }
    }
}

void DottedSpectrumVisualizer::ClearBuffer(uint8_t* buffer) {
    std::memset(buffer, 0, 1024);
}

void DottedSpectrumVisualizer::SetPixel(uint8_t* buffer, int x, int y, bool color) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    int page = y / 8;
    int bit = y % 8;
    int index = page * 128 + x;
    if (color) {
        buffer[index] |= (1 << bit);
    } else {
        buffer[index] &= ~(1 << bit);
    }
}

void DottedSpectrumVisualizer::DrawFastHLine(uint8_t* buffer, int x1, int x2, int y, bool color) {
    if (y < 0 || y >= 64) return;
    if (x1 > x2) std::swap(x1, x2);
    x1 = std::max(0, x1);
    x2 = std::min(127, x2);
    int page = y / 8;
    int bit = y % 8;
    uint8_t mask = (1 << bit);
    for (int x = x1; x <= x2; ++x) {
        int index = page * 128 + x;
        if (color) buffer[index] |= mask;
        else buffer[index] &= ~mask;
    }
}

void DottedSpectrumVisualizer::DrawChar(uint8_t* buffer, int x, int y, char c) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t* glyph = FONT_5X7[c - 32];
    for (int col = 0; col < 5; ++col) {
        int px = x + col;
        if (px < 0 || px >= 128) continue;
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; ++row) {
            int py = y + row;
            if (py >= 0 && py < 64) {
                SetPixel(buffer, px, py, (line & (1 << row)) != 0);
            }
        }
    }
}

void DottedSpectrumVisualizer::DrawString(uint8_t* buffer, int x, int y, const char* str) {
    if (!str) return;
    while (*str) {
        DrawChar(buffer, x, y, *str);
        x += 6;
        str++;
    }
}

void DottedSpectrumVisualizer::DrawMusicNote(uint8_t* buffer, int x, int y) {
    static const uint8_t NOTE_ICON[8][8] = {
        {0,0,0,0,1,1,1,0},
        {0,0,0,0,1,0,0,1},
        {0,0,0,0,1,0,0,1},
        {0,0,0,0,1,0,0,1},
        {0,0,0,0,1,0,0,0},
        {0,0,1,1,1,0,0,0},
        {0,1,1,1,1,0,0,0},
        {0,0,1,1,0,0,0,0}
    };
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            if (NOTE_ICON[r][c]) {
                SetPixel(buffer, x + c, y + r, true);
            }
        }
    }
}

void DottedSpectrumVisualizer::Render(uint8_t* framebuffer, int width, int height) {
    (void)width;
    (void)height;
    if (!framebuffer) return;

    frame_count_++;
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool audio_active = (now_ms - last_audio_feed_ms_.load()) < 350;

    ClearBuffer(framebuffer);

    // 1. Icon nốt nhạc ở góc trái trên (x=2, y=2)
    DrawMusicNote(framebuffer, 2, 2);

    // 2. Cuộn tiêu đề bài hát ở thanh trên
    std::string disp_title;
    {
        std::lock_guard<std::mutex> lock(title_mutex_);
        disp_title = clean_title_.empty() ? "Now Playing" : clean_title_;
    }

    int text_px_width = disp_title.length() * 6;
    if (text_px_width <= 112) {
        DrawString(framebuffer, 13, 2, disp_title.c_str());
    } else {
        int total_scroll_width = text_px_width + 24;
        int offset = (scroll_offset_ / 2) % total_scroll_width;
        DrawString(framebuffer, 13 - offset, 2, disp_title.c_str());
        DrawString(framebuffer, 13 - offset + total_scroll_width, 2, disp_title.c_str());
        scroll_offset_++;
    }

    // 3. Đường kẻ phân cách thanh tiêu đề và vùng ma trận hạt (y=12)
    DrawFastHLine(framebuffer, 0, 127, 12, true);

    // 4. Cập nhật vật lý mượt mà và vẽ STYLE_DOT_BARS (16 cột hạt LED)
    for (int i = 0; i < NUM_BARS; ++i) {
        float target = audio_active ? target_height_[i] : 2.0f;

        if (target > current_height_[i]) {
            current_height_[i] = current_height_[i] * 0.25f + target * 0.75f;
        } else {
            current_height_[i] = current_height_[i] * 0.84f + 0.3f;
        }
        if (current_height_[i] < 2.0f) current_height_[i] = 2.0f;
        if (current_height_[i] > 46.0f) current_height_[i] = 46.0f;

        target_height_[i] = 2.0f;

        float h_val = current_height_[i];
        int bar_h = (int)((h_val - 2.0f) * 0.95f + 2.0f);
        if (bar_h > 46) bar_h = 46;

        // Xử lý hạt đỉnh (Peak dot with hold and gravity decay)
        if (h_val >= peak_height_[i]) {
            peak_height_[i] = h_val;
            peak_hold_[i] = 3;
            peak_decay_[i] = 0.0f;
        } else {
            if (peak_hold_[i] > 0) {
                peak_hold_[i]--;
            } else {
                peak_decay_[i] += 1.2f;
                peak_height_[i] -= peak_decay_[i];
                if (peak_height_[i] < h_val) peak_height_[i] = h_val;
            }
        }
        int peak_h = (int)((peak_height_[i] - 2.0f) * 0.95f + 2.0f);
        if (peak_h > 47) peak_h = 47;

        // Tọa độ cột hạt: 16 cột x 8px = 128px
        int x_base = i * 8 + 2;
        int y_top = 63 - bar_h;
        if (y_top < 16) y_top = 16;

        // Vẽ từng hạt LED từ đáy lên (ngắt quãng 2px để tạo hiệu ứng ma trận hạt Dotted LED)
        for (int y = 63; y >= y_top; y -= 2) {
            SetPixel(framebuffer, x_base, y, true);
            SetPixel(framebuffer, x_base + 1, y, true);
            SetPixel(framebuffer, x_base + 2, y, true);
            SetPixel(framebuffer, x_base + 3, y, true);
        }

        // Vẽ hạt đỉnh lơ lửng
        int y_peak = 63 - peak_h;
        if (y_peak < 15) y_peak = 15;
        SetPixel(framebuffer, x_base + 1, y_peak, true);
        SetPixel(framebuffer, x_base + 2, y_peak, true);
    }
}
