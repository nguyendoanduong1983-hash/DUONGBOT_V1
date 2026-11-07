#ifndef RADIO_PLAYER_H
#define RADIO_PLAYER_H

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_http_client.h>

struct RadioStation {
    int id;
    std::string name;
    std::string description;
    std::string url;
    bool is_hls;
};

class RadioPlayer {
public:
    static RadioPlayer& GetInstance();

    // Khởi tạo danh sách đài và tài nguyên
    void Initialize();

    // Mở đài theo ID (bắt đầu từ 1)
    bool Play(int station_id);

    // Mở đài theo tên hoặc số (ví dụ "1", "hanoi", "vov1", "voh")
    bool PlayByName(const std::string& query);

    // Dừng phát đài
    void Stop();

    // Trạng thái phát
    bool IsPlaying() const { return is_playing_ || is_running_; }
    int GetCurrentStationId() const { return current_station_id_; }
    std::string GetCurrentStationName() const;
    std::string GetCurrentStationDescription() const;

    // Danh sách các đài
    const std::vector<RadioStation>& GetStations() const { return stations_; }
    const RadioStation* GetStation(int id) const;

    // Chuỗi giới thiệu các đài bằng tiếng Việt để robot đọc
    std::string GetStationListSpeechText() const;

    // Đăng ký callback khi trạng thái radio thay đổi
    void SetStateChangeCallback(std::function<void(bool playing, const std::string& station_name)> callback);

private:
    RadioPlayer();
    ~RadioPlayer();
    RadioPlayer(const RadioPlayer&) = delete;
    RadioPlayer& operator=(const RadioPlayer&) = delete;

    std::vector<RadioStation> stations_;
    std::mutex control_mutex_;
    volatile bool is_playing_ = false;
    volatile bool is_running_ = false;
    volatile int current_station_id_ = 0;
    std::atomic<uint32_t> session_id_{0};

    TaskHandle_t stream_task_handle_ = nullptr;
    TaskHandle_t decode_task_handle_ = nullptr;
    RingbufHandle_t ring_buffer_ = nullptr;
    esp_http_client_handle_t http_client_ = nullptr;

    std::function<void(bool playing, const std::string& station_name)> state_callback_;

    void StreamTask(uint32_t session);
    void DecodeTask(uint32_t session);
    void StreamDirect(const std::string& url, uint32_t session);
    void StreamHls(const std::string& playlist_url, uint32_t session);
    void EnsureRingBuffer();
    void FlushRingBuffer();
    void StopInternal();
};

#endif // RADIO_PLAYER_H
