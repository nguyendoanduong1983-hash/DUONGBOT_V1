#include "radio_player.h"
#include "music/music_player.h"
#include "music/online_music_player.h"
#include "platform/board.h"
#include "audio/audio_codec.h"
#include "robot/display/dotted_spectrum_visualizer.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <memory>

#include "esp_mp3_dec.h"
#include "esp_aac_dec.h"
#include "esp_ae_rate_cvt.h"

#define TAG "RadioPlayer"

#define RADIO_RING_BUFFER_SIZE (96 * 1024)
#define RADIO_PREBUFFER_MIN    (24 * 1024)

RadioPlayer& RadioPlayer::GetInstance() {
    static RadioPlayer instance;
    return instance;
}

RadioPlayer::RadioPlayer() {
    Initialize();
}

RadioPlayer::~RadioPlayer() {
    Stop();
    if (ring_buffer_) {
        vRingbufferDelete(ring_buffer_);
        ring_buffer_ = nullptr;
    }
}

void RadioPlayer::EnsureRingBuffer() {
    if (ring_buffer_ == nullptr) {
        ring_buffer_ = xRingbufferCreateWithCaps(RADIO_RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ring_buffer_ == nullptr) {
            ESP_LOGW(TAG, "Could not allocate SPIRAM ring buffer, trying internal RAM");
            ring_buffer_ = xRingbufferCreate(32 * 1024, RINGBUF_TYPE_BYTEBUF);
        }
    }
}

void RadioPlayer::FlushRingBuffer() {
    if (!ring_buffer_) return;
    size_t size = 0;
    void* item = nullptr;
    while ((item = xRingbufferReceiveUpTo(ring_buffer_, &size, 0, 4096)) != nullptr) {
        vRingbufferReturnItem(ring_buffer_, item);
    }
}

void RadioPlayer::Initialize() {
    EnsureRingBuffer();
    if (!stations_.empty()) {
        return;
    }

    stations_ = {
        {
            1,
            "Đài Hà Nội FM 90 MHz",
            "Hà Nội Tin tức & Giao thông",
            "http://14.162.146.90:8000/HANOI90",
            false // Direct MP3 stream
        },
        {
            2,
            "Đài Hà Nội FM 96 MHz",
            "Âm nhạc & Giải trí Thủ Đô",
            "http://222.252.21.96:8000/HANOI96",
            false // Direct MP3 stream
        },
        {
            3,
            "Đài VOV1",
            "Thời sự - Chính trị Quốc gia (VOV1)",
            "http://media.kythuatvov.vn:1936/live/VOV1.sdp/chunklist.m3u8",
            true // HLS AAC
        },
        {
            4,
            "Đài VOV2",
            "Văn hóa - Xã hội - Đời sống (VOV2)",
            "http://media.kythuatvov.vn:1936/live/VOV2.sdp/chunklist.m3u8",
            true // HLS AAC
        },
        {
            5,
            "Đài VOV3",
            "Âm nhạc & Giải trí Trẻ (VOV3)",
            "http://media.kythuatvov.vn:1936/live/VOV3.sdp/chunklist.m3u8",
            true // HLS AAC
        },
        {
            6,
            "Đài VOV Giao thông Hà Nội",
            "Kênh Giao thông Đô thị Hà Nội (FM 91 MHz)",
            "http://media.kythuatvov.vn:1936/live/VOVGTHN.sdp/chunklist.m3u8",
            true // HLS AAC
        },
        {
            7,
            "Đài VOV Giao thông TP.HCM",
            "Kênh Giao thông Đô thị TP.HCM (FM 91 MHz)",
            "http://media.kythuatvov.vn:1936/live/VOVGTHCM.sdp/chunklist.m3u8",
            true // HLS AAC
        },
        {
            8,
            "Đài VOH TP.HCM FM 99.9 MHz",
            "Thông tin & Ca nhạc TP. Hồ Chí Minh",
            "https://1011337676.vnns.net/-eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1cmkiOiIvaHR2Yy9WT0gtRk05OS45LmFkbS50bXMvIiwiZXhwIjoxOTI0OTI1MjI4fQ.zVLoKcCW3tFLySpEQHcYh11PHIxk2ZE1pj5P-GFIm1I-/htvc/VOH-FM99.9.adm.tms/chunks.m3u8",
            true // HLS AAC
        },
        {
            9,
            "Đài VOH TP.HCM FM 95.6 MHz",
            "Giao thông Đô thị TP. Hồ Chí Minh",
            "https://1011337676.vnns.net/-eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1cmkiOiIvaHR2Yy9WT0gtRk05NS42LmFkbS50bXMvIiwiZXhwIjoxOTI0OTI1MjI4fQ.isr2nlUkXOPGSxT-KT8FcZQFCcXn58RZrCyqoB3zDGg-/htvc/VOH-FM95.6.adm.tms/chunks.m3u8",
            true // HLS AAC
        },
        {
            10,
            "Đài VOH TP.HCM AM 610 KHz",
            "Tin tức - Thời sự Tổng hợp Nam Bộ",
            "https://1011337676.vnns.net/-eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1cmkiOiIvaHR2Yy9WT0gtQU02MTAuYWRtLnRtcy8iLCJleHAiOjE5MjQ5MjUyMjh9.dAa1es3LlAVnvQ-oLGNBqfEI0yz7fPrjEML_rJAklMQ-/htvc/VOH-AM610.adm.tms/chunks.m3u8",
            true // HLS AAC
        }
    };
    ESP_LOGI(TAG, "RadioPlayer initialized with %d stations", (int)stations_.size());
}

const RadioStation* RadioPlayer::GetStation(int id) const {
    for (const auto& s : stations_) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

std::string RadioPlayer::GetCurrentStationName() const {
    const auto* s = GetStation(current_station_id_);
    return s ? s->name : "";
}

std::string RadioPlayer::GetCurrentStationDescription() const {
    const auto* s = GetStation(current_station_id_);
    return s ? s->description : "";
}

std::string RadioPlayer::GetStationListSpeechText() const {
    std::string text = "Tôi có các đài phát thanh: "
                       "Số 1 là Đài Hà Nội FM 90, "
                       "Số 2 là Đài Hà Nội FM 96, "
                       "Số 3 là Đài VOV1 Thời sự Quốc gia, "
                       "Số 4 là Đài VOV2 Văn hóa Xã hội, "
                       "Số 5 là Đài VOV3 Âm nhạc Giải trí, "
                       "Số 6 là Đài VOV Giao thông Hà Nội, "
                       "Số 7 là Đài VOV Giao thông Thành phố Hồ Chí Minh, "
                       "Số 8 là Đài VOH Thành phố Hồ Chí Minh FM 99.9, "
                       "Số 9 là Đài VOH FM 95.6, "
                       "Số 10 là Đài VOH AM 610. "
                       "Bạn muốn nghe đài nào?";
    return text;
}

void RadioPlayer::SetStateChangeCallback(std::function<void(bool playing, const std::string& station_name)> callback) {
    state_callback_ = callback;
}

bool RadioPlayer::PlayByName(const std::string& query) {
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);

    // 1. Kiểm tra các đài VOV cụ thể
    if (q.find("vov1") != std::string::npos || q.find("vov 1") != std::string::npos ||
        q.find("thời sự") != std::string::npos || q.find("thoi su") != std::string::npos) {
        return Play(3);
    }
    if (q.find("vov2") != std::string::npos || q.find("vov 2") != std::string::npos ||
        q.find("văn hóa") != std::string::npos || q.find("van hoa") != std::string::npos) {
        return Play(4);
    }
    if (q.find("vov3") != std::string::npos || q.find("vov 3") != std::string::npos ||
        q.find("âm nhạc") != std::string::npos || q.find("am nhac") != std::string::npos ||
        q.find("ca nhạc") != std::string::npos || q.find("ca nhac") != std::string::npos) {
        return Play(5);
    }

    // 2. VOV Giao thông
    if (q.find("giao thông") != std::string::npos || q.find("giao thong") != std::string::npos) {
        if (q.find("hồ chí minh") != std::string::npos || q.find("ho chi minh") != std::string::npos ||
            q.find("tphcm") != std::string::npos || q.find("sài gòn") != std::string::npos ||
            q.find("sai gon") != std::string::npos || q.find("hcm") != std::string::npos) {
            return Play(7);
        }
        return Play(6); // VOV Giao thông Hà Nội
    }

    // 3. VOH TP.HCM
    if (q.find("voh") != std::string::npos || q.find("đài tphcm") != std::string::npos ||
        q.find("đài thành phố") != std::string::npos || q.find("đài sài gòn") != std::string::npos) {
        if (q.find("95") != std::string::npos) return Play(9);
        if (q.find("610") != std::string::npos || q.find("am") != std::string::npos) return Play(10);
        return Play(8); // VOH FM 99.9 MHz
    }

    // 4. Đài phát thanh Hà Nội
    if (q.find("hà nội") != std::string::npos || q.find("ha noi") != std::string::npos || q.find("hanoi") != std::string::npos) {
        if (q.find("96") != std::string::npos) return Play(2);
        return Play(1); // Hà Nội FM 90
    }

    // 5. Kiểm tra số kênh từ 10 xuống 1
    if (q.find("10") != std::string::npos || q.find("mười") != std::string::npos || q.find("muoi") != std::string::npos) {
        return Play(10);
    }
    if (q.find("9") != std::string::npos || q.find("chín") != std::string::npos || q.find("chin") != std::string::npos) {
        return Play(9);
    }
    if (q.find("8") != std::string::npos || q.find("tám") != std::string::npos || q.find("tam") != std::string::npos) {
        return Play(8);
    }
    if (q.find("7") != std::string::npos || q.find("bảy") != std::string::npos || q.find("bay") != std::string::npos) {
        return Play(7);
    }
    if (q.find("6") != std::string::npos || q.find("sáu") != std::string::npos || q.find("sau") != std::string::npos) {
        return Play(6);
    }
    if (q.find("5") != std::string::npos || q.find("năm") != std::string::npos || q.find("nam") != std::string::npos) {
        return Play(5);
    }
    if (q.find("4") != std::string::npos || q.find("bốn") != std::string::npos || q.find("bon") != std::string::npos) {
        return Play(4);
    }
    if (q.find("3") != std::string::npos || q.find("ba") != std::string::npos) {
        return Play(3);
    }
    if (q.find("2") != std::string::npos || q.find("hai") != std::string::npos) {
        return Play(2);
    }
    if (q.find("1") != std::string::npos || q.find("một") != std::string::npos || q.find("mot") != std::string::npos) {
        return Play(1);
    }

    if (q.find("vov") != std::string::npos) {
        return Play(3);
    }

    // Chỉ khi nói "mở radio" chung chung mới mở kênh 1
    if (q.find("mở radio") != std::string::npos || q.find("bật radio") != std::string::npos ||
        q.find("nghe radio") != std::string::npos || q.find("phát radio") != std::string::npos) {
        return Play(1);
    }

    return false;
}

bool RadioPlayer::Play(int station_id) {
    const auto* s = GetStation(station_id);
    if (!s) {
        ESP_LOGE(TAG, "Station id %d not found", station_id);
        return false;
    }

    std::lock_guard<std::mutex> lock(control_mutex_);

    if (is_running_ && current_station_id_ == station_id) {
        ESP_LOGI(TAG, "Radio station %d (%s) is already running, skipping duplicate trigger",
                 station_id, s->name.c_str());
        return true;
    }

    // Dừng nhạc nếu đang hoạt động để giải phóng tài nguyên âm thanh & socket
    if (MusicPlayer::GetInstance().IsPlaying()) {
        MusicPlayer::GetInstance().Stop();
    }
    if (OnlineMusicPlayer::GetInstance().IsPlaying()) {
        OnlineMusicPlayer::GetInstance().Stop();
    }

    StopInternal();
    vTaskDelay(pdMS_TO_TICKS(25));

    EnsureRingBuffer();
    if (ring_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create ring buffer for radio!");
        return false;
    }
    FlushRingBuffer();

    uint32_t session = ++session_id_;
    current_station_id_ = station_id;
    is_running_ = true;
    is_playing_ = true;

    // Chuyển WiFi sang MIN_MODEM để đảm bảo luồng stream ổn định
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Bật ngõ ra âm thanh nếu đang tắt
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec && !codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    // Bắt đầu visualizer sóng nhạc trên màn hình phụ với tên đài
    DottedSpectrumVisualizer::GetInstance().Start(s->name);

    ESP_LOGI(TAG, "Starting radio station %d (session %u): %s (%s)", s->id, (unsigned)session, s->name.c_str(), s->url.c_str());

    // Khởi chạy tác vụ decode âm thanh (12KB stack)
    struct TaskParam {
        RadioPlayer* self;
        uint32_t session;
    };
    TaskParam* param_decode = new TaskParam{this, session};
    xTaskCreatePinnedToCore([](void* arg) {
        auto* p = static_cast<TaskParam*>(arg);
        p->self->DecodeTask(p->session);
        delete p;
        vTaskDelete(NULL);
    }, "radio_decode", 12288, param_decode, 5, &decode_task_handle_, 1);

    // Khởi chạy tác vụ stream tải dữ liệu mạng (12KB stack)
    TaskParam* param_stream = new TaskParam{this, session};
    xTaskCreatePinnedToCore([](void* arg) {
        auto* p = static_cast<TaskParam*>(arg);
        p->self->StreamTask(p->session);
        delete p;
        vTaskDelete(NULL);
    }, "radio_stream", 12288, param_stream, 4, &stream_task_handle_, 0);

    if (state_callback_) {
        state_callback_(true, s->name);
    }

    return true;
}

void RadioPlayer::Stop() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    StopInternal();
}

void RadioPlayer::StopInternal() {
    if (!is_running_ && !is_playing_ && stream_task_handle_ == nullptr && decode_task_handle_ == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Stopping radio playback (session %u)...", session_id_.load());
    is_running_ = false;
    is_playing_ = false;
    current_station_id_ = 0;
    ++session_id_; // Thay đổi session để các tác vụ ngưng ngay lập tức

    // Dừng visualizer sóng nhạc
    DottedSpectrumVisualizer::GetInstance().Stop();

    // Đợi 2 tác vụ thoát một cách an toàn
    int wait_cycles = 50; // Tối đa 1.25 giây
    while ((stream_task_handle_ != nullptr || decode_task_handle_ != nullptr) && wait_cycles-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    // Không dùng vTaskDelete ép buộc khi task đang trong phiên TLS/HTTP để tránh hỏng heap/mutex
    if (stream_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Radio StreamTask still finishing network I/O, detaching handle to exit cleanly");
        stream_task_handle_ = nullptr;
    }
    if (decode_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Radio DecodeTask still finishing, detaching handle to exit cleanly");
        decode_task_handle_ = nullptr;
    }

    FlushRingBuffer();

    ESP_LOGI(TAG, "Radio playback stopped successfully");
    if (state_callback_) {
        state_callback_(false, "");
    }
}

void RadioPlayer::StreamTask(uint32_t session) {
    const auto* s = GetStation(current_station_id_);
    if (s && session == session_id_.load()) {
        if (s->is_hls) {
            StreamHls(s->url, session);
        } else {
            StreamDirect(s->url, session);
        }
    }
    if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
        stream_task_handle_ = nullptr;
    }
}

void RadioPlayer::StreamDirect(const std::string& url, uint32_t session) {
    while (is_running_ && session == session_id_.load()) {
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.timeout_ms = 8000;
        config.buffer_size = 4096;
        config.buffer_size_tx = 1024;
        config.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        http_client_ = client;

        esp_http_client_set_header(client, "User-Agent", "D-Bot-Radio/1.0");
        esp_http_client_set_header(client, "Icy-MetaData", "0");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open direct stream connection: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            http_client_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        esp_http_client_fetch_headers(client);

        uint8_t buffer[2048];
        while (is_running_ && session == session_id_.load()) {
            int read_len = esp_http_client_read(client, (char*)buffer, sizeof(buffer));
            if (read_len > 0) {
                while (is_running_ && session == session_id_.load()) {
                    if (ring_buffer_ && xRingbufferSend(ring_buffer_, buffer, read_len, pdMS_TO_TICKS(100)) == pdTRUE) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            } else if (read_len < 0) {
                ESP_LOGW(TAG, "Direct stream read error %d, reconnecting...", read_len);
                break;
            } else {
                // Stream EOF
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            }
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        http_client_ = nullptr;

        if (is_running_ && session == session_id_.load()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

struct HlsSegment {
    int64_t seq;
    std::string url;
};

void RadioPlayer::StreamHls(const std::string& playlist_url, uint32_t session) {
    std::string base_url = playlist_url;
    size_t last_slash = base_url.find_last_of('/');
    if (last_slash != std::string::npos) {
        base_url = base_url.substr(0, last_slash);
    }

    int64_t last_downloaded_seq = -1;
    bool is_first_fetch = true;

    while (is_running_ && session == session_id_.load()) {
        // 1. Tải nội dung playlist m3u8
        esp_http_client_config_t config = {};
        config.url = playlist_url.c_str();
        config.timeout_ms = 5000;
        config.buffer_size = 2048;
        config.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        http_client_ = client;
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            http_client_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        esp_http_client_fetch_headers(client);

        std::string playlist_text;
        char chunk[1024];
        int r = 0;
        while (is_running_ && session == session_id_.load() &&
               (r = esp_http_client_read(client, chunk, sizeof(chunk) - 1)) > 0) {
            chunk[r] = '\0';
            playlist_text += chunk;
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        http_client_ = nullptr;

        if (!is_running_ || session != session_id_.load()) break;

        // 2. Phân tích playlist để tìm media sequence và các segment
        int64_t media_sequence = 0;
        int target_duration = 6;
        bool has_media_sequence = false;
        std::vector<HlsSegment> segments;

        size_t pos = 0;
        int64_t seq_counter = 0;
        while (pos < playlist_text.size()) {
            size_t end_pos = playlist_text.find('\n', pos);
            if (end_pos == std::string::npos) end_pos = playlist_text.size();
            std::string line = playlist_text.substr(pos, end_pos - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

            if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
                media_sequence = std::strtoll(line.c_str() + 22, nullptr, 10);
                seq_counter = media_sequence;
                has_media_sequence = true;
            } else if (line.rfind("#EXT-X-TARGETDURATION:", 0) == 0) {
                target_duration = std::atoi(line.c_str() + 22);
                if (target_duration <= 0) target_duration = 6;
            } else if (!line.empty() && line[0] != '#') {
                if (line.find(".aac") != std::string::npos || line.find(".ts") != std::string::npos) {
                    int64_t seg_seq = seq_counter++;
                    // Nếu không có EXT-X-MEDIA-SEQUENCE, thử trích xuất số thứ tự từ tên file
                    if (!has_media_sequence) {
                        size_t dot = line.find_last_of('.');
                        size_t last_sep = line.find_last_of("_/=-", dot);
                        if (dot != std::string::npos && last_sep != std::string::npos && dot > last_sep + 1) {
                            std::string num_str = line.substr(last_sep + 1, dot - last_sep - 1);
                            char* endptr = nullptr;
                            long val = std::strtol(num_str.c_str(), &endptr, 10);
                            if (endptr != num_str.c_str() && *endptr == '\0') {
                                seg_seq = val;
                            }
                        }
                    }

                    std::string chunk_url;
                    if (line.rfind("http", 0) == 0) {
                        chunk_url = line;
                    } else {
                        chunk_url = base_url + "/" + line;
                    }
                    segments.push_back({seg_seq, chunk_url});
                }
            }
            pos = end_pos + 1;
        }

        if (segments.empty()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 3. Khởi tạo điểm bắt đầu cho lần đầu tải (chỉ lấy 2 chunk mới nhất để phát ngay live)
        if (is_first_fetch) {
            is_first_fetch = false;
            if (segments.size() > 1) {
                last_downloaded_seq = segments[segments.size() - 2].seq - 1;
            } else {
                last_downloaded_seq = segments.back().seq - 1;
            }
        }

        // 4. Tải các chunk mới chưa tải theo thứ tự tuần tự
        bool downloaded_any = false;
        for (const auto& seg : segments) {
            if (!is_running_ || session != session_id_.load()) break;

            // Kiểm tra theo sequence number chuẩn xác - tuyệt đối không tải lại chunk cũ
            if (seg.seq <= last_downloaded_seq) {
                continue;
            }

            // Nếu bị tụt lại quá xa (> 6 chunks) do mạng, nhảy bắt kịp live
            if (seg.seq > last_downloaded_seq + 6) {
                ESP_LOGW(TAG, "HLS sequence fell behind (last=%lld, next=%lld), catching up",
                         last_downloaded_seq, seg.seq);
                last_downloaded_seq = seg.seq - 1;
            }

            esp_http_client_config_t chunk_cfg = {};
            chunk_cfg.url = seg.url.c_str();
            chunk_cfg.timeout_ms = 5000;
            chunk_cfg.buffer_size = 4096;
            chunk_cfg.crt_bundle_attach = esp_crt_bundle_attach;

            esp_http_client_handle_t chunk_client = esp_http_client_init(&chunk_cfg);
            if (chunk_client) {
                http_client_ = chunk_client;
                if (esp_http_client_open(chunk_client, 0) == ESP_OK) {
                    esp_http_client_fetch_headers(chunk_client);
                    uint8_t buf[2048];
                    int n = 0;
                    while (is_running_ && session == session_id_.load() &&
                           (n = esp_http_client_read(chunk_client, (char*)buf, sizeof(buf))) > 0) {
                        while (is_running_ && session == session_id_.load()) {
                            if (ring_buffer_ && xRingbufferSend(ring_buffer_, buf, n, pdMS_TO_TICKS(100)) == pdTRUE) {
                                break;
                            }
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                    }
                    esp_http_client_close(chunk_client);
                }
                esp_http_client_cleanup(chunk_client);
                http_client_ = nullptr;

                last_downloaded_seq = seg.seq;
                downloaded_any = true;
            }
        }

        // 5. Chu kỳ thăm dò: nếu vừa tải chunk thì chờ nửa chu kỳ target_duration; nếu chưa có thì chờ 1s
        int poll_delay_ms = downloaded_any ? std::max(1500, (target_duration * 1000) / 2) : 1000;
        int steps = poll_delay_ms / 100;
        while (is_running_ && session == session_id_.load() && steps-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void RadioPlayer::DecodeTask(uint32_t session) {
    const auto* s = GetStation(current_station_id_);
    if (!s || session != session_id_.load() || !is_running_) {
        if (decode_task_handle_ == xTaskGetCurrentTaskHandle()) {
            decode_task_handle_ = nullptr;
        }
        return;
    }

    bool is_aac = s->is_hls;

    void* decoder = nullptr;
    if (is_aac) {
        esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
        esp_err_t err = esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &decoder);
        if (err != ESP_OK || !decoder) {
            ESP_LOGE(TAG, "Failed to open AAC decoder: %s", esp_err_to_name(err));
            if (decode_task_handle_ == xTaskGetCurrentTaskHandle()) decode_task_handle_ = nullptr;
            return;
        }
    } else {
        esp_err_t err = esp_mp3_dec_open(nullptr, 0, &decoder);
        if (err != ESP_OK || !decoder) {
            ESP_LOGE(TAG, "Failed to open MP3 decoder: %s", esp_err_to_name(err));
            if (decode_task_handle_ == xTaskGetCurrentTaskHandle()) decode_task_handle_ = nullptr;
            return;
        }
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    int output_rate = codec ? codec->output_sample_rate() : 24000;

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int resampler_in_rate = 0;

    // Chờ pre-buffer để đảm bảo âm thanh mượt mà không bị ngắt quãng
    int prebuf_wait = 0;
    while (is_running_ && session == session_id_.load() && prebuf_wait++ < 60) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!ring_buffer_) break;
        size_t free_sz = xRingbufferGetCurFreeSize(ring_buffer_);
        size_t buffered = RADIO_RING_BUFFER_SIZE - free_sz;
        if (buffered >= RADIO_PREBUFFER_MIN) {
            ESP_LOGI(TAG, "Pre-buffering complete (%u bytes buffered)", (unsigned)buffered);
            break;
        }
    }

    std::vector<uint8_t> in_buffer;
    in_buffer.reserve(8192);

    // Cấp phát out_pcm_raw động trên heap để không chiếm dụng bộ nhớ stack
    std::unique_ptr<int16_t[]> out_pcm_raw(new int16_t[4096]);

    while (is_running_ && session == session_id_.load()) {
        // Nạp thêm dữ liệu từ ring buffer nếu buffer cục bộ chưa đủ
        while (in_buffer.size() < 4096 && is_running_ && session == session_id_.load()) {
            if (!ring_buffer_) break;
            size_t item_size = 0;
            void* item = xRingbufferReceiveUpTo(ring_buffer_, &item_size, pdMS_TO_TICKS(40), 2048);
            if (item) {
                in_buffer.insert(in_buffer.end(), (uint8_t*)item, (uint8_t*)item + item_size);
                vRingbufferReturnItem(ring_buffer_, item);
            } else {
                break;
            }
        }

        if (in_buffer.empty()) {
            // Buffer cạn: kiểm tra dữ liệu ring buffer để tránh giật micro-stutter
            if (ring_buffer_) {
                size_t free_sz = xRingbufferGetCurFreeSize(ring_buffer_);
                size_t buffered = RADIO_RING_BUFFER_SIZE - free_sz;
                if (buffered < 8192) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }

        // Bỏ qua ID3 header nếu có (thường gặp ở đầu stream hoặc đầu chunk AAC)
        if (in_buffer.size() >= 10 && in_buffer[0] == 'I' && in_buffer[1] == 'D' && in_buffer[2] == '3') {
            uint32_t tag_size = ((in_buffer[6] & 0x7F) << 21) |
                                ((in_buffer[7] & 0x7F) << 14) |
                                ((in_buffer[8] & 0x7F) << 7)  |
                                (in_buffer[9] & 0x7F);
            uint32_t total_skip = 10 + tag_size;
            if (in_buffer[5] & 0x10) {
                total_skip += 10;
            }
            while (in_buffer.size() < total_skip && is_running_ && session == session_id_.load()) {
                if (!ring_buffer_) break;
                size_t item_size = 0;
                void* item = xRingbufferReceiveUpTo(ring_buffer_, &item_size, pdMS_TO_TICKS(50), 2048);
                if (item) {
                    in_buffer.insert(in_buffer.end(), (uint8_t*)item, (uint8_t*)item + item_size);
                    vRingbufferReturnItem(ring_buffer_, item);
                } else {
                    break;
                }
            }
            if (in_buffer.size() >= total_skip) {
                in_buffer.erase(in_buffer.begin(), in_buffer.begin() + total_skip);
            }
            continue;
        }

        // Cấu hình raw input và output frame cho decoder
        esp_audio_dec_in_raw_t raw = {
            .buffer = in_buffer.data(),
            .len = (uint32_t)in_buffer.size(),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer = (uint8_t*)out_pcm_raw.get(),
            .len = 4096 * sizeof(int16_t),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_dec_info_t info = {};

        esp_audio_err_t dec_ret = ESP_AUDIO_ERR_FAIL;
        if (is_aac) {
            dec_ret = esp_aac_dec_decode(decoder, &raw, &frame, &info);
        } else {
            dec_ret = esp_mp3_dec_decode(decoder, &raw, &frame, &info);
        }

        if (dec_ret == ESP_AUDIO_ERR_OK && raw.consumed > 0 && frame.decoded_size > 0) {
            in_buffer.erase(in_buffer.begin(), in_buffer.begin() + raw.consumed);

            if (codec && !codec->output_enabled()) {
                codec->EnableOutput(true);
            }

            // Chuyển đổi stereo sang mono nếu là âm thanh 2 kênh
            int channels = info.channel > 0 ? info.channel : 1;
            int samples_per_channel = frame.decoded_size / (channels * sizeof(int16_t));

            std::vector<int16_t> mono_pcm(samples_per_channel);
            if (channels == 2) {
                int16_t* src = out_pcm_raw.get();
                for (int i = 0; i < samples_per_channel; i++) {
                    mono_pcm[i] = (int16_t)(((int32_t)src[2 * i] + (int32_t)src[2 * i + 1]) / 2);
                }
            } else {
                memcpy(mono_pcm.data(), out_pcm_raw.get(), samples_per_channel * sizeof(int16_t));
            }

            // Resample nếu tần số lấy mẫu của đài khác với ngõ ra I2S (24000 Hz)
            if (info.sample_rate != output_rate && info.sample_rate > 0) {
                if (resampler == nullptr || resampler_in_rate != info.sample_rate) {
                    if (resampler) {
                        esp_ae_rate_cvt_close(resampler);
                        resampler = nullptr;
                    }
                    esp_ae_rate_cvt_cfg_t cvt_cfg = {
                        .src_rate = (uint32_t)info.sample_rate,
                        .dest_rate = (uint32_t)output_rate,
                        .channel = 1,
                        .bits_per_sample = 16,
                        .complexity = 1,
                        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                    };
                    esp_ae_rate_cvt_open(&cvt_cfg, &resampler);
                    resampler_in_rate = info.sample_rate;
                }

                if (resampler) {
                    uint32_t target_size = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, samples_per_channel, &target_size);
                    std::vector<int16_t> resampled(target_size);
                    uint32_t actual_output = target_size;
                    esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono_pcm.data(), samples_per_channel,
                                            (esp_ae_sample_t)resampled.data(), &actual_output);
                    resampled.resize(actual_output);
                    if (codec && is_running_ && session == session_id_.load()) {
                        codec->OutputData(resampled);
                        DottedSpectrumVisualizer::GetInstance().FeedAudio(resampled.data(), resampled.size());
                    }
                } else {
                    if (codec && is_running_ && session == session_id_.load()) {
                        codec->OutputData(mono_pcm);
                        DottedSpectrumVisualizer::GetInstance().FeedAudio(mono_pcm.data(), mono_pcm.size());
                    }
                }
            } else {
                if (codec && is_running_ && session == session_id_.load()) {
                    codec->OutputData(mono_pcm);
                    DottedSpectrumVisualizer::GetInstance().FeedAudio(mono_pcm.data(), mono_pcm.size());
                }
            }
        } else {
            // Không giải mã được hoặc lỗi sync, dò tìm byte sync tiếp theo chuẩn xác
            size_t sync_idx = 1;
            while (sync_idx + 1 < in_buffer.size()) {
                if (is_aac) {
                    // AAC ADTS sync: 12 bits 0xFFF và Layer = 00 (bits 13-14 == 0)
                    if (in_buffer[sync_idx] == 0xFF &&
                        (in_buffer[sync_idx + 1] & 0xF6) == 0xF0) {
                        break;
                    }
                } else {
                    // MP3 sync: 11 bits 0xFFE và Layer III (bits 13-14 == 01)
                    if (in_buffer[sync_idx] == 0xFF &&
                        (in_buffer[sync_idx + 1] & 0xE6) == 0xE2) {
                        break;
                    }
                }
                sync_idx++;
            }
            if (sync_idx + 1 < in_buffer.size()) {
                in_buffer.erase(in_buffer.begin(), in_buffer.begin() + sync_idx);
            } else {
                in_buffer.clear();
            }
        }
    }

    if (resampler) {
        esp_ae_rate_cvt_close(resampler);
        resampler = nullptr;
    }

    if (is_aac) {
        esp_aac_dec_close(decoder);
    } else {
        esp_mp3_dec_close(decoder);
    }

    if (decode_task_handle_ == xTaskGetCurrentTaskHandle()) {
        decode_task_handle_ = nullptr;
    }
}
