#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RobotController;

enum class GameMode : int {
    kNone = 0,
    kPoliceChase = 1,   // Cảnh sát bắt cướp
    kHunted = 2,        // Bị săn đuổi
    kPatrol = 3,        // Tuần tra kiểm soát
    kDroneEscape = 4,   // Chạy trốn khỏi Drone
    kDesertWander = 5   // Lang thang trên sa mạc
};

struct GameInfo {
    int id;
    GameMode mode;
    std::string name;
    std::string description;
};

class GameManager {
public:
    static GameManager& GetInstance();

    void Initialize(RobotController* controller);

    // Khởi chạy game theo ID (1..5) hoặc theo tên truy vấn giọng nói
    bool Start(GameMode mode);
    bool StartById(int id);
    bool StartByName(const std::string& query);

    // Dừng game ngay lập tức (phanh motor = 0, tắt âm thanh, về trạng thái an toàn)
    void Stop();

    bool IsRunning() const { return is_running_.load(std::memory_order_acquire); }
    GameMode GetCurrentMode() const { return static_cast<GameMode>(current_mode_.load(std::memory_order_relaxed)); }
    int GetCurrentGameId() const { return current_mode_.load(std::memory_order_relaxed); }
    std::string GetCurrentGameName() const;
    std::string GetGameListSpeechText() const;
    const std::vector<GameInfo>& GetGames() const { return games_; }

private:
    GameManager();
    ~GameManager();
    GameManager(const GameManager&) = delete;
    GameManager& operator=(const GameManager&) = delete;

    RobotController* controller_ = nullptr;
    std::vector<GameInfo> games_;
    std::atomic<bool> is_running_{false};
    std::atomic<int> current_mode_{0};
    std::atomic<uint32_t> session_id_{0};
    std::atomic<int> last_left_{0};
    std::atomic<int> last_right_{0};
    std::mutex mutex_;

    TaskHandle_t motor_task_handle_ = nullptr;
    TaskHandle_t audio_task_handle_ = nullptr;

    void MotorBehaviorTask(uint32_t session, GameMode mode);
    void AudioEffectTask(uint32_t session, GameMode mode);

    // Helpers di chuyển bảo đảm an toàn mép bàn
    bool WaitAndCheckSafety(uint32_t session, int duration_ms, bool& cliff_detected);
    void EvadeCliff(uint32_t session, int& turn_counter);
    void SetWheels(uint32_t session, int left_percent, int right_percent);
    void Brake();
};
