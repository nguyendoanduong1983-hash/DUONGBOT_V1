#include "online_music_player.h"
#include "music_player.h"
#include "radio/radio_player.h"
#include "platform/board.h"
#include "audio/audio_codec.h"
#include "robot/display/dotted_spectrum_visualizer.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <cstdio>

#include "esp_mp3_dec.h"
#include "esp_aac_dec.h"
#include "esp_ae_rate_cvt.h"

#define TAG "OnlineMusic"

#define ONLINE_MUSIC_RING_BUFFER_SIZE (512 * 1024)
#define ONLINE_MUSIC_PREBUFFER_MIN    (32 * 1024)

template <typename T>
struct PsramAllocator {
    typedef T value_type;
    PsramAllocator() = default;
    template <class U> constexpr PsramAllocator(const PsramAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = malloc(n * sizeof(T));
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept {
        heap_caps_free(p);
    }
};

static inline uint32_t ReadBe32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline bool MatchFourCC(const uint8_t* p, const char* cc) {
    return p[0] == (uint8_t)cc[0] && p[1] == (uint8_t)cc[1] &&
           p[2] == (uint8_t)cc[2] && p[3] == (uint8_t)cc[3];
}

static std::string CleanSongSearchQuery(const std::string& input) {
    std::string s = input;
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const char* stop_phrases[] = {
        " cua ca si ", " cua ca sy ", " cua ca sĩ ", " cua ca sỹ ",
        " cua ", " của ",
        " ca si ", " ca sy ", " ca sĩ ", " ca sỹ ",
        " do ca si ", " do ca sĩ ",
        " do ",
        " hat boi ", " hát bởi ",
        " trinh bay ", " trình bày ",
        " boi ", " bởi "
    };

    for (const char* phrase : stop_phrases) {
        size_t p = 0;
        size_t len = strlen(phrase);
        while ((p = lower.find(phrase, p)) != std::string::npos) {
            lower.replace(p, len, " ");
            s.replace(p, len, " ");
            p += 1;
        }
    }

    std::string result;
    bool in_space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!in_space && !result.empty()) {
                result += ' ';
                in_space = true;
            }
        } else {
            result += c;
            in_space = false;
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result.empty() ? input : result;
}

template <typename Alloc = std::allocator<uint32_t>>
static void ParseTrunSamples(const uint8_t* data, uint32_t payload_len, std::vector<uint32_t, Alloc>& out_sizes) {
    if (payload_len < 8) return;
    uint32_t flags = ReadBe32(data) & 0x00FFFFFF;
    uint32_t sample_count = ReadBe32(data + 4);
    uint32_t offset = 8;

    if (flags & 0x000001) offset += 4;
    if (flags & 0x000004) offset += 4;

    bool has_duration = (flags & 0x000100) != 0;
    bool has_size     = (flags & 0x000200) != 0;
    bool has_flags    = (flags & 0x000400) != 0;
    bool has_cto      = (flags & 0x000800) != 0;

    uint32_t entry_size = (has_duration ? 4 : 0) + (has_size ? 4 : 0) +
                          (has_flags ? 4 : 0) + (has_cto ? 4 : 0);
    if (!has_size || entry_size == 0) return;

    out_sizes.reserve(out_sizes.size() + sample_count);
    for (uint32_t i = 0; i < sample_count; i++) {
        if (offset + entry_size > payload_len) break;
        uint32_t p = offset;
        if (has_duration) p += 4;
        uint32_t sz = ReadBe32(data + p);
        if (sz > 0 && sz < 16384) {
            out_sizes.push_back(sz);
        }
        offset += entry_size;
    }
}

template <typename Alloc = std::allocator<uint32_t>>
static void ParseMoofContainer(const uint8_t* data, uint32_t len, std::vector<uint32_t, Alloc>& out_sizes) {
    uint32_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t box_sz = ReadBe32(data + pos);
        if (box_sz < 8 || pos + box_sz > len) break;
        const uint8_t* type = data + pos + 4;
        if (MatchFourCC(type, "traf")) {
            ParseMoofContainer(data + pos + 8, box_sz - 8, out_sizes);
        } else if (MatchFourCC(type, "trun")) {
            ParseTrunSamples(data + pos + 8, box_sz - 8, out_sizes);
        }
        pos += box_sz;
    }
}

static void BuildAdtsHeader(uint8_t* adts, uint32_t raw_aac_len, int profile = 1, int freq_idx = 4, int chan_cfg = 2) {
    uint32_t frame_len = raw_aac_len + 7;
    adts[0] = 0xFF;
    adts[1] = 0xF1;
    adts[2] = (uint8_t)(((profile & 0x03) << 6) | ((freq_idx & 0x0F) << 2) | ((chan_cfg >> 2) & 0x01));
    adts[3] = (uint8_t)(((chan_cfg & 0x03) << 6) | ((frame_len >> 11) & 0x03));
    adts[4] = (uint8_t)((frame_len >> 3) & 0xFF);
    adts[5] = (uint8_t)(((frame_len & 0x07) << 5) | 0x1F);
    adts[6] = 0xFC;
}

OnlineMusicPlayer& OnlineMusicPlayer::GetInstance() {
    static OnlineMusicPlayer instance;
    return instance;
}

OnlineMusicPlayer::OnlineMusicPlayer() {}

OnlineMusicPlayer::~OnlineMusicPlayer() {
    Stop();
    if (ring_buffer_) {
        vRingbufferDelete(ring_buffer_);
        ring_buffer_ = nullptr;
    }
}

void OnlineMusicPlayer::EnsureRingBuffer() {
    if (ring_buffer_ == nullptr) {
        ring_buffer_ = xRingbufferCreateWithCaps(ONLINE_MUSIC_RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ring_buffer_ == nullptr) {
            ESP_LOGW(TAG, "Could not allocate SPIRAM ring buffer, trying internal RAM");
            ring_buffer_ = xRingbufferCreate(48 * 1024, RINGBUF_TYPE_BYTEBUF);
        }
        if (ring_buffer_ == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate ring buffer for OnlineMusicPlayer");
        }
    }
}

void OnlineMusicPlayer::FlushRingBuffer() {
    if (!ring_buffer_) return;
    size_t size = 0;
    void* item = nullptr;
    while ((item = xRingbufferReceiveUpTo(ring_buffer_, &size, 0, 4096)) != nullptr) {
        vRingbufferReturnItem(ring_buffer_, item);
    }
}

void OnlineMusicPlayer::Initialize() {
    EnsureRingBuffer();
    ESP_LOGI(TAG, "OnlineMusicPlayer initialized (API: %s)", api_base_url_.c_str());
}

std::string OnlineMusicPlayer::UrlEncode(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped += (char)c;
        } else if (c == ' ') {
            escaped += '+';
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            escaped += buf;
        }
    }
    return escaped;
}

void OnlineMusicPlayer::SetApiBaseUrl(const std::string& base_url) {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    api_base_url_ = base_url;
    while (!api_base_url_.empty() && api_base_url_.back() == '/') {
        api_base_url_.pop_back();
    }
}

std::string OnlineMusicPlayer::GetApiBaseUrl() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return api_base_url_;
}

std::string OnlineMusicPlayer::GetCurrentQuery() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return current_query_;
}

std::string OnlineMusicPlayer::GetCurrentTitle() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return current_title_;
}

std::string OnlineMusicPlayer::GetCurrentSource() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return current_source_;
}

std::string OnlineMusicPlayer::GetStatusMessage() const {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    return status_message_;
}

void OnlineMusicPlayer::SetStateChangeCallback(std::function<void(bool playing, const std::string& title)> callback) {
    state_callback_ = callback;
}

bool OnlineMusicPlayer::PlayByQuery(const std::string& query) {
    std::string clean_q = CleanSongSearchQuery(query);
    if (clean_q.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(control_mutex_);

    // Tránh khởi tạo lại nếu cùng một từ khóa tìm kiếm vừa mới bắt đầu
    if (is_running_) {
        std::lock_guard<std::mutex> mlock(meta_mutex_);
        if (current_query_ == clean_q) {
            ESP_LOGI(TAG, "Already searching/playing '%s', skipping duplicate trigger", clean_q.c_str());
            return true;
        }
    }

    // Dừng phát nhạc GitHub hoặc Radio nếu đang phát
    if (MusicPlayer::GetInstance().IsPlaying()) {
        MusicPlayer::GetInstance().Stop();
    }
    if (RadioPlayer::GetInstance().IsPlaying()) {
        RadioPlayer::GetInstance().Stop();
    }

    StopInternal();
    vTaskDelay(pdMS_TO_TICKS(25));

    EnsureRingBuffer();
    if (ring_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer for OnlineMusicPlayer");
        return false;
    }
    FlushRingBuffer();

    uint32_t session = ++session_id_;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        current_query_ = clean_q;
        current_title_ = clean_q;
        current_source_ = "online";
        current_stream_url_.clear();
        ext_hint_.clear();
        status_message_ = "Đang tìm: " + clean_q;
    }

    is_running_ = true;
    is_playing_ = false;

    esp_wifi_set_ps(WIFI_PS_NONE);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec && !codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    ESP_LOGI(TAG, "Starting online music search (session %u): '%s'", (unsigned)session, clean_q.c_str());

    struct TaskParam {
        OnlineMusicPlayer* self;
        uint32_t session;
    };

    TaskParam* param_decode = new TaskParam{this, session};
    xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        auto* p = static_cast<TaskParam*>(arg);
        p->self->DecodeTask(p->session);
        delete p;
        vTaskDelete(NULL);
    }, "onl_music_dec", 20480, param_decode, 5, &decode_task_handle_, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    TaskParam* param_stream = new TaskParam{this, session};
    xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        auto* p = static_cast<TaskParam*>(arg);
        p->self->StreamTask(p->session);
        delete p;
        vTaskDelete(NULL);
    }, "onl_music_stm", 16384, param_stream, 4, &stream_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (state_callback_) {
        state_callback_(true, clean_q);
    }

    return true;
}

bool OnlineMusicPlayer::PlayUrl(const std::string& url, const std::string& title) {
    if (url.empty()) return false;

    std::lock_guard<std::mutex> lock(control_mutex_);

    if (MusicPlayer::GetInstance().IsPlaying()) {
        MusicPlayer::GetInstance().Stop();
    }
    if (RadioPlayer::GetInstance().IsPlaying()) {
        RadioPlayer::GetInstance().Stop();
    }

    StopInternal();
    vTaskDelay(pdMS_TO_TICKS(25));

    EnsureRingBuffer();
    if (ring_buffer_ == nullptr) {
        return false;
    }
    FlushRingBuffer();

    uint32_t session = ++session_id_;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        current_query_ = title;
        current_title_ = title;
        current_source_ = "direct_url";
        current_stream_url_ = url;
        ext_hint_.clear();
        status_message_ = "Đang phát: " + title;
    }

    is_running_ = true;
    is_playing_ = false;

    esp_wifi_set_ps(WIFI_PS_NONE);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec && !codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    struct TaskParam {
        OnlineMusicPlayer* self;
        uint32_t session;
    };

    TaskParam* param_decode = new TaskParam{this, session};
    xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        auto* p = static_cast<TaskParam*>(arg);
        p->self->DecodeTask(p->session);
        delete p;
        vTaskDelete(NULL);
    }, "onl_music_dec", 20480, param_decode, 5, &decode_task_handle_, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    TaskParam* param_stream = new TaskParam{this, session};
    xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        auto* p = static_cast<TaskParam*>(arg);
        p->self->StreamTask(p->session);
        delete p;
        vTaskDelete(NULL);
    }, "onl_music_stm", 16384, param_stream, 4, &stream_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (state_callback_) {
        state_callback_(true, title);
    }

    return true;
}

void OnlineMusicPlayer::Stop() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    StopInternal();
}

void OnlineMusicPlayer::StopInternal() {
    if (!is_running_ && !is_playing_ && stream_task_handle_ == nullptr && decode_task_handle_ == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Stopping online music playback (session %u)...", (unsigned)session_id_.load());
    is_running_ = false;
    is_playing_ = false;
    DottedSpectrumVisualizer::GetInstance().Stop();
    ++session_id_;

    // Chờ 2 tác vụ thoát tự nhiên, tuyệt đối không gọi vTaskDelete cưỡng bức khi task đang giữ khóa mbedtls
    int wait_cycles = 60;
    while ((stream_task_handle_ != nullptr || decode_task_handle_ != nullptr) && wait_cycles-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    if (stream_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "StreamTask still in blocking IO, detaching handle to exit cleanly via session_id");
        stream_task_handle_ = nullptr;
    }
    if (decode_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "DecodeTask still active, detaching handle to exit cleanly via session_id");
        decode_task_handle_ = nullptr;
    }

    FlushRingBuffer();

    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        status_message_ = "Đã dừng";
    }

    if (state_callback_) {
        state_callback_(false, "");
    }
    ESP_LOGI(TAG, "Online music playback stopped");
}

bool OnlineMusicPlayer::ResolveSongUrl(const std::string& query, uint32_t session, std::string& out_url,
                                       std::string& out_title, std::string& out_source, std::string& out_ext) {
    std::string base = GetApiBaseUrl();
    std::string play_api_url = base + "/play?q=" + UrlEncode(query);
    ESP_LOGI(TAG, "Resolving song metadata from: %s", play_api_url.c_str());

    esp_http_client_config_t config = {};
    config.url = play_api_url.c_str();
    config.timeout_ms = 15000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }
    http_client_ = client;
    esp_http_client_set_header(client, "User-Agent", "DUONGBOT-ESP32/2.0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK || !is_running_ || session != session_id_.load()) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to open /play resolver: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
        http_client_ = nullptr;
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    std::string body;
    char buf[1024];
    int r = 0;
    while (is_running_ && session == session_id_.load() &&
           (r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        body.append(buf, r);
        if (body.size() > 16384) break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    http_client_ = nullptr;

    if (status_code != 200 || body.empty()) {
        ESP_LOGW(TAG, "/play resolver returned status %d", status_code);
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON from /play");
        return false;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data || !cJSON_IsObject(data)) {
        data = root;
    }

    cJSON* j_url = cJSON_GetObjectItem(data, "url");
    cJSON* j_title = cJSON_GetObjectItem(data, "title");
    cJSON* j_source = cJSON_GetObjectItem(data, "source");
    cJSON* j_ext = cJSON_GetObjectItem(data, "ext");

    if (j_url && cJSON_IsString(j_url) && j_url->valuestring) {
        out_url = j_url->valuestring;
    }
    if (j_title && cJSON_IsString(j_title) && j_title->valuestring) {
        out_title = j_title->valuestring;
    }
    if (j_source && cJSON_IsString(j_source) && j_source->valuestring) {
        out_source = j_source->valuestring;
    }
    if (j_ext && cJSON_IsString(j_ext) && j_ext->valuestring) {
        out_ext = j_ext->valuestring;
    }

    cJSON_Delete(root);
    return !out_url.empty();
}

void OnlineMusicPlayer::StreamTask(uint32_t session) {
    if (!is_running_ || session != session_id_.load()) {
        if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
            stream_task_handle_ = nullptr;
        }
        return;
    }

    std::string stream_url;
    std::string query;
    {
        std::lock_guard<std::mutex> lock(meta_mutex_);
        stream_url = current_stream_url_;
        query = current_query_;
    }

    if (stream_url.empty()) {
        std::string resolved_url, resolved_title, resolved_source, resolved_ext;
        if (ResolveSongUrl(query, session, resolved_url, resolved_title, resolved_source, resolved_ext)) {
            stream_url = resolved_url;
            {
                std::lock_guard<std::mutex> lock(meta_mutex_);
                current_stream_url_ = resolved_url;
                if (!resolved_title.empty()) current_title_ = resolved_title;
                if (!resolved_source.empty()) current_source_ = resolved_source;
                if (!resolved_ext.empty()) ext_hint_ = resolved_ext;
                status_message_ = "Đang phát: " + current_title_;
            }
            if (!resolved_title.empty()) {
                DottedSpectrumVisualizer::GetInstance().SetTitle(resolved_title);
            }
            ESP_LOGI(TAG, "Resolved song: '%s' (source=%s, ext=%s)",
                     resolved_title.c_str(), resolved_source.c_str(), resolved_ext.c_str());
        } else if (is_running_ && session == session_id_.load()) {
            stream_url = GetApiBaseUrl() + "/redirect?q=" + UrlEncode(query);
            ESP_LOGI(TAG, "Fallback to redirect endpoint: %s", stream_url.c_str());
        }
    }

    if (!is_running_ || session != session_id_.load() || stream_url.empty()) {
        if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
            stream_task_handle_ = nullptr;
        }
        return;
    }

    esp_http_client_config_t config = {};
    config.url = stream_url.c_str();
    config.timeout_ms = 20000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.max_redirection_count = 5;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = nullptr;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2 && is_running_ && session == session_id_.load(); ++attempt) {
        client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client for stream");
            if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
                stream_task_handle_ = nullptr;
            }
            return;
        }
        http_client_ = client;
        esp_http_client_set_header(client, "User-Agent", "DUONGBOT-ESP32/2.0");

        err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            break;
        }
        esp_http_client_cleanup(client);
        http_client_ = nullptr;
        client = nullptr;
        if (attempt == 0 && is_running_ && session == session_id_.load()) {
            ESP_LOGW(TAG, "Retrying audio stream connection after error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }

    if (err != ESP_OK || !client || !is_running_ || session != session_id_.load()) {
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open audio stream: %s", esp_err_to_name(err));
            std::lock_guard<std::mutex> lock(meta_mutex_);
            status_message_ = "Lỗi kết nối luồng nhạc";
        }
        if (client) {
            esp_http_client_cleanup(client);
            http_client_ = nullptr;
        }
        if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
            stream_task_handle_ = nullptr;
        }
        return;
    }

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    int redirect_hops = 0;
    while ((status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) &&
           redirect_hops < 4 && is_running_ && session == session_id_.load()) {
        redirect_hops++;
        esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            break;
        }
        esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
    }

    if (status_code != 200 && status_code != 206) {
        ESP_LOGW(TAG, "Audio stream returned HTTP %d", status_code);
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            status_message_ = "Không tìm thấy luồng phát (" + std::to_string(status_code) + ")";
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        http_client_ = nullptr;
        if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
            stream_task_handle_ = nullptr;
        }
        return;
    }

    ESP_LOGI(TAG, "Connected to audio stream (HTTP %d), streaming bytes...", status_code);

    uint8_t buffer[2048];
    int read_retry_count = 0;
    while (is_running_ && session == session_id_.load()) {
        int read_len = esp_http_client_read(client, (char*)buffer, sizeof(buffer));
        if (read_len > 0) {
            read_retry_count = 0;
            while (is_running_ && session == session_id_.load()) {
                if (ring_buffer_ && xRingbufferSend(ring_buffer_, buffer, read_len, pdMS_TO_TICKS(100)) == pdTRUE) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else if (read_len == 0) {
            ESP_LOGI(TAG, "Reached EOF of online audio stream");
            break;
        } else {
            if (is_running_ && session == session_id_.load() && ++read_retry_count <= 3) {
                ESP_LOGW(TAG, "Online stream transient read err %d, retrying (%d/3)...", read_len, read_retry_count);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            ESP_LOGW(TAG, "Online stream read error: %d, stream ended", read_len);
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    http_client_ = nullptr;
    if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
        stream_task_handle_ = nullptr;
    }
}

void OnlineMusicPlayer::DecodeTask(uint32_t session) {
    if (session != session_id_.load() || !is_running_) {
        if (decode_task_handle_ == xTaskGetCurrentTaskHandle()) {
            decode_task_handle_ = nullptr;
        }
        return;
    }

    int prebuf_wait = 0;
    while (is_running_ && session == session_id_.load() && prebuf_wait++ < 800) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!ring_buffer_) break;
        size_t free_sz = xRingbufferGetCurFreeSize(ring_buffer_);
        size_t buffered = ONLINE_MUSIC_RING_BUFFER_SIZE - free_sz;
        if (buffered >= ONLINE_MUSIC_PREBUFFER_MIN) {
            ESP_LOGI(TAG, "Pre-buffering complete (%u bytes)", (unsigned)buffered);
            break;
        }
        if (stream_task_handle_ == nullptr && buffered > 0) {
            break;
        }
        if (stream_task_handle_ == nullptr && buffered == 0 && prebuf_wait > 20) {
            break;
        }
    }

    void* mp3_decoder = nullptr;
    void* aac_decoder = nullptr;

    auto codec = Board::GetInstance().GetAudioCodec();
    int output_rate = codec ? codec->output_sample_rate() : 24000;

    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int resampler_in_rate = 0;

    std::vector<uint8_t, PsramAllocator<uint8_t>> in_buffer;
    in_buffer.reserve(48 * 1024);
    int16_t* out_pcm_raw = (int16_t*)heap_caps_malloc(4096 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_pcm_raw) {
        out_pcm_raw = (int16_t*)malloc(4096 * sizeof(int16_t));
    }

    auto pull_more_bytes = [&](size_t min_bytes, int max_wait_ms) -> bool {
        int waited = 0;
        while (in_buffer.size() < min_bytes && is_running_ && session == session_id_.load()) {
            if (!ring_buffer_) break;
            size_t item_size = 0;
            void* item = xRingbufferReceiveUpTo(ring_buffer_, &item_size, pdMS_TO_TICKS(30), 4096);
            if (item) {
                in_buffer.insert(in_buffer.end(), (uint8_t*)item, (uint8_t*)item + item_size);
                vRingbufferReturnItem(ring_buffer_, item);
            } else {
                if (stream_task_handle_ == nullptr && 
                    (!ring_buffer_ || xRingbufferGetCurFreeSize(ring_buffer_) == ONLINE_MUSIC_RING_BUFFER_SIZE)) {
                    break;
                }
                waited += 30;
                if (waited >= max_wait_ms) break;
            }
        }
        return in_buffer.size() >= min_bytes;
    };

    auto output_pcm_samples = [&](const int16_t* pcm_data, int decoded_bytes, int channels, int sample_rate) {
        if (decoded_bytes <= 0 || !is_running_ || session != session_id_.load()) return;
        if (codec && !codec->output_enabled()) {
            codec->EnableOutput(true);
        }

        if (!is_playing_ && is_running_ && session == session_id_.load()) {
            is_playing_ = true;
            std::string title = GetCurrentTitle();
            DottedSpectrumVisualizer::GetInstance().Start(title);
            if (state_callback_) {
                state_callback_(true, title);
            }
        }

        int ch = channels > 0 ? channels : 1;
        int samples_per_channel = decoded_bytes / (ch * sizeof(int16_t));
        if (samples_per_channel <= 0) return;

        std::vector<int16_t> mono_pcm(samples_per_channel);
        if (ch == 2) {
            for (int i = 0; i < samples_per_channel; i++) {
                mono_pcm[i] = (int16_t)(((int32_t)pcm_data[2 * i] + (int32_t)pcm_data[2 * i + 1]) / 2);
            }
        } else {
            memcpy(mono_pcm.data(), pcm_data, samples_per_channel * sizeof(int16_t));
        }

        if (sample_rate != output_rate && sample_rate > 0) {
            if (resampler == nullptr || resampler_in_rate != sample_rate) {
                if (resampler) {
                    esp_ae_rate_cvt_close(resampler);
                    resampler = nullptr;
                }
                esp_ae_rate_cvt_cfg_t cvt_cfg = {
                    .src_rate = (uint32_t)sample_rate,
                    .dest_rate = (uint32_t)output_rate,
                    .channel = 1,
                    .bits_per_sample = 16,
                    .complexity = 2,
                    .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                };
                esp_ae_rate_cvt_open(&cvt_cfg, &resampler);
                resampler_in_rate = sample_rate;
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
                return;
            }
        }

        if (codec && is_running_ && session == session_id_.load()) {
            codec->OutputData(mono_pcm);
            DottedSpectrumVisualizer::GetInstance().FeedAudio(mono_pcm.data(), mono_pcm.size());
        }
    };

    enum class StreamFormat {
        kUnknown,
        kFmp4Aac,
        kAdtsAac,
        kMp3
    };
    StreamFormat format = StreamFormat::kUnknown;
    std::vector<uint32_t, PsramAllocator<uint32_t>> fmp4_sample_sizes;

    while (is_running_ && session == session_id_.load()) {
        pull_more_bytes(4096, 200);

        if (in_buffer.empty()) {
            if (stream_task_handle_ == nullptr) {
                ESP_LOGI(TAG, "Online music playback finished naturally");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (format == StreamFormat::kUnknown && in_buffer.size() >= 12) {
            if (MatchFourCC(in_buffer.data() + 4, "styp") ||
                MatchFourCC(in_buffer.data() + 4, "ftyp") ||
                MatchFourCC(in_buffer.data() + 4, "sidx") ||
                MatchFourCC(in_buffer.data() + 4, "moof")) {
                format = StreamFormat::kFmp4Aac;
                ESP_LOGI(TAG, "Detected fMP4 AAC stream container");
                if (!aac_decoder) {
                    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
                    esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &aac_decoder);
                }
            } else if (in_buffer[0] == 0xFF && (in_buffer[1] & 0xF6) == 0xF0) {
                format = StreamFormat::kAdtsAac;
                ESP_LOGI(TAG, "Detected ADTS AAC stream");
                if (!aac_decoder) {
                    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
                    esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &aac_decoder);
                }
            } else {
                format = StreamFormat::kMp3;
                ESP_LOGI(TAG, "Detected MP3 stream");
                if (!mp3_decoder) {
                    esp_mp3_dec_open(nullptr, 0, &mp3_decoder);
                }
            }
        }

        if (format == StreamFormat::kFmp4Aac) {
            if (!pull_more_bytes(8, 1000)) {
                if (stream_task_handle_ == nullptr) break;
                continue;
            }

            uint32_t box_sz = ReadBe32(in_buffer.data());
            const uint8_t* box_type = in_buffer.data() + 4;

            if (MatchFourCC(box_type, "styp") || MatchFourCC(box_type, "sidx") ||
                MatchFourCC(box_type, "ftyp") || MatchFourCC(box_type, "moov") ||
                MatchFourCC(box_type, "free") || MatchFourCC(box_type, "skip")) {
                if (box_sz < 8 || box_sz > 256 * 1024) {
                    in_buffer.erase(in_buffer.begin());
                    continue;
                }
                if (!pull_more_bytes(box_sz, 3000)) {
                    if (stream_task_handle_ == nullptr) break;
                    continue;
                }
                in_buffer.erase(in_buffer.begin(), in_buffer.begin() + box_sz);
                continue;
            }

            if (MatchFourCC(box_type, "moof")) {
                if (box_sz < 8 || box_sz > 128 * 1024) {
                    in_buffer.erase(in_buffer.begin());
                    continue;
                }
                if (!pull_more_bytes(box_sz, 3000)) {
                    if (stream_task_handle_ == nullptr) break;
                    continue;
                }
                fmp4_sample_sizes.clear();
                ParseMoofContainer(in_buffer.data() + 8, box_sz - 8, fmp4_sample_sizes);
                in_buffer.erase(in_buffer.begin(), in_buffer.begin() + box_sz);
                continue;
            }

            if (MatchFourCC(box_type, "mdat")) {
                if (box_sz < 8) {
                    in_buffer.erase(in_buffer.begin());
                    continue;
                }
                uint32_t mdat_payload_remaining = box_sz - 8;
                in_buffer.erase(in_buffer.begin(), in_buffer.begin() + 8);

                std::vector<uint8_t, PsramAllocator<uint8_t>> adts_frame;
                adts_frame.reserve(2048);

                for (size_t idx = 0; idx < fmp4_sample_sizes.size(); idx++) {
                    if (!is_running_ || session != session_id_.load()) break;
                    uint32_t sample_sz = fmp4_sample_sizes[idx];
                    if (sample_sz > mdat_payload_remaining) {
                        sample_sz = mdat_payload_remaining;
                    }
                    if (sample_sz == 0) continue;

                    if (!pull_more_bytes(sample_sz, 3000)) {
                        if (stream_task_handle_ == nullptr) break;
                        continue;
                    }

                    adts_frame.resize(sample_sz + 7);
                    BuildAdtsHeader(adts_frame.data(), sample_sz, 1, 4, 2);
                    memcpy(adts_frame.data() + 7, in_buffer.data(), sample_sz);

                    in_buffer.erase(in_buffer.begin(), in_buffer.begin() + sample_sz);
                    mdat_payload_remaining -= sample_sz;

                    if (aac_decoder) {
                        esp_audio_dec_in_raw_t raw = {
                            .buffer = adts_frame.data(),
                            .len = (uint32_t)adts_frame.size(),
                            .consumed = 0,
                            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                        };
                        esp_audio_dec_out_frame_t frame = {
                            .buffer = (uint8_t*)out_pcm_raw,
                            .len = 4096 * sizeof(int16_t),
                            .needed_size = 0,
                            .decoded_size = 0,
                        };
                        esp_audio_dec_info_t info = {};
                        esp_audio_err_t dec_ret = esp_aac_dec_decode(aac_decoder, &raw, &frame, &info);
                        if (dec_ret == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
                            output_pcm_samples(out_pcm_raw, frame.decoded_size,
                                               info.channel > 0 ? info.channel : 2,
                                               info.sample_rate > 0 ? info.sample_rate : 44100);
                        }
                    }
                }

                while (mdat_payload_remaining > 0 && is_running_ && session == session_id_.load()) {
                    size_t chunk_skip = std::min((size_t)mdat_payload_remaining, (size_t)4096);
                    if (!pull_more_bytes(chunk_skip, 2000)) {
                        if (stream_task_handle_ == nullptr) break;
                        continue;
                    }
                    in_buffer.erase(in_buffer.begin(), in_buffer.begin() + chunk_skip);
                    mdat_payload_remaining -= chunk_skip;
                }
                fmp4_sample_sizes.clear();
                continue;
            }

            size_t sync = 1;
            while (sync + 8 <= in_buffer.size()) {
                if (MatchFourCC(in_buffer.data() + sync + 4, "styp") ||
                    MatchFourCC(in_buffer.data() + sync + 4, "moof") ||
                    MatchFourCC(in_buffer.data() + sync + 4, "sidx")) {
                    break;
                }
                sync++;
            }
            if (sync + 8 <= in_buffer.size()) {
                in_buffer.erase(in_buffer.begin(), in_buffer.begin() + sync);
            } else if (in_buffer.size() > 8) {
                in_buffer.erase(in_buffer.begin(), in_buffer.end() - 8);
            }
            continue;
        }

        if (in_buffer.size() >= 10 && in_buffer[0] == 'I' && in_buffer[1] == 'D' && in_buffer[2] == '3') {
            uint32_t tag_size = ((in_buffer[6] & 0x7F) << 21) |
                                ((in_buffer[7] & 0x7F) << 14) |
                                ((in_buffer[8] & 0x7F) << 7)  |
                                (in_buffer[9] & 0x7F);
            uint32_t total_skip = 10 + tag_size + ((in_buffer[5] & 0x10) ? 10 : 0);
            pull_more_bytes(total_skip, 3000);
            if (in_buffer.size() >= total_skip) {
                in_buffer.erase(in_buffer.begin(), in_buffer.begin() + total_skip);
            } else {
                in_buffer.clear();
            }
            continue;
        }

        if (in_buffer.size() < 1440 && stream_task_handle_ != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bool is_aac = (format == StreamFormat::kAdtsAac);
        void* active_decoder = is_aac ? aac_decoder : mp3_decoder;
        if (!active_decoder) break;

        esp_audio_dec_in_raw_t raw = {
            .buffer = in_buffer.data(),
            .len = (uint32_t)in_buffer.size(),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer = (uint8_t*)out_pcm_raw,
            .len = 4096 * sizeof(int16_t),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_dec_info_t info = {};

        esp_audio_err_t dec_ret = is_aac
            ? esp_aac_dec_decode(active_decoder, &raw, &frame, &info)
            : esp_mp3_dec_decode(active_decoder, &raw, &frame, &info);

        if (dec_ret == ESP_AUDIO_ERR_OK && raw.consumed > 0 && frame.decoded_size > 0) {
            in_buffer.erase(in_buffer.begin(), in_buffer.begin() + raw.consumed);
            output_pcm_samples(out_pcm_raw, frame.decoded_size, info.channel, info.sample_rate);
        } else {
            if (in_buffer.size() >= 1440) {
                size_t sync_idx = 1;
                while (sync_idx + 1 < in_buffer.size()) {
                    if (is_aac) {
                        if (in_buffer[sync_idx] == 0xFF && (in_buffer[sync_idx + 1] & 0xF6) == 0xF0) break;
                    } else {
                        if (in_buffer[sync_idx] == 0xFF && (in_buffer[sync_idx + 1] & 0xE6) == 0xE2) break;
                    }
                    sync_idx++;
                }
                if (sync_idx + 1 < in_buffer.size()) {
                    in_buffer.erase(in_buffer.begin(), in_buffer.begin() + sync_idx);
                } else {
                    in_buffer.erase(in_buffer.begin(), in_buffer.end() - 1);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(15));
            }
        }
    }

    if (resampler) {
        esp_ae_rate_cvt_close(resampler);
        resampler = nullptr;
    }
    if (mp3_decoder) {
        esp_mp3_dec_close(mp3_decoder);
        mp3_decoder = nullptr;
    }
    if (aac_decoder) {
        esp_aac_dec_close(aac_decoder);
        aac_decoder = nullptr;
    }
    if (out_pcm_raw) {
        heap_caps_free(out_pcm_raw);
        out_pcm_raw = nullptr;
    }

    if (is_running_ && session == session_id_.load()) {
        is_playing_ = false;
        is_running_ = false;
        DottedSpectrumVisualizer::GetInstance().Stop();
        {
            std::lock_guard<std::mutex> lock(meta_mutex_);
            status_message_ = "Đã phát xong";
        }
        if (state_callback_) {
            state_callback_(false, "");
        }
    }

    if (decode_task_handle_ == xTaskGetCurrentTaskHandle()) {
        decode_task_handle_ = nullptr;
    }
}
