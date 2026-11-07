#ifndef ONLINE_MUSIC_PLAYER_H
#define ONLINE_MUSIC_PLAYER_H

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_http_client.h>

class OnlineMusicPlayer {
public:
    static OnlineMusicPlayer& GetInstance();

    void Initialize();

    // Tìm và phát bài hát online theo từ khóa (YouTube / SoundCloud qua Render API)
    bool PlayByQuery(const std::string& query);

    // Phát trực tiếp từ một URL âm thanh online
    bool PlayUrl(const std::string& url, const std::string& title = "Nhạc Online");

    // Dừng phát nhạc online
    void Stop();

    // Trạng thái hiện tại
    bool IsPlaying() const { return is_playing_ || is_running_; }
    std::string GetCurrentQuery() const;
    std::string GetCurrentTitle() const;
    std::string GetCurrentSource() const;
    std::string GetStatusMessage() const;

    // Cấu hình địa chỉ server API (mặc định: https://xiaozhi-music-api.onrender.com)
    void SetApiBaseUrl(const std::string& base_url);
    std::string GetApiBaseUrl() const;

    // Callback khi trạng thái phát thay đổi
    void SetStateChangeCallback(std::function<void(bool playing, const std::string& title)> callback);

private:
    OnlineMusicPlayer();
    ~OnlineMusicPlayer();
    OnlineMusicPlayer(const OnlineMusicPlayer&) = delete;
    OnlineMusicPlayer& operator=(const OnlineMusicPlayer&) = delete;

    mutable std::mutex meta_mutex_;
    std::mutex control_mutex_;
    std::string api_base_url_ = "https://xiaozhi-music-api.onrender.com";
    std::string current_query_;
    std::string current_title_;
    std::string current_source_;
    std::string current_stream_url_;
    std::string ext_hint_;
    std::string status_message_;

    volatile bool is_playing_ = false;
    volatile bool is_running_ = false;
    std::atomic<uint32_t> session_id_{0};

    TaskHandle_t stream_task_handle_ = nullptr;
    TaskHandle_t decode_task_handle_ = nullptr;
    RingbufHandle_t ring_buffer_ = nullptr;
    esp_http_client_handle_t http_client_ = nullptr;

    std::function<void(bool playing, const std::string& title)> state_callback_;

    void StreamTask(uint32_t session);
    void DecodeTask(uint32_t session);
    void StopInternal();
    void EnsureRingBuffer();
    void FlushRingBuffer();

    bool ResolveSongUrl(const std::string& query, uint32_t session, std::string& out_url,
                        std::string& out_title, std::string& out_source, std::string& out_ext);
    static std::string UrlEncode(const std::string& value);
};

#endif // ONLINE_MUSIC_PLAYER_H
