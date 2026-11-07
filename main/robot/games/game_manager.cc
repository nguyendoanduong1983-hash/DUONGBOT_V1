#include "game_manager.h"
#include "robot/control/robot_controller.h"
#include "platform/board.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "music/music_player.h"
#include "music/online_music_player.h"
#include "radio/radio_player.h"
#include "robot/display/dotted_spectrum_visualizer.h"

#include <esp_log.h>
#include <esp_random.h>
#include <cmath>
#include <algorithm>
#include <cstring>

#define TAG "GameManager"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

GameManager& GameManager::GetInstance() {
    static GameManager instance;
    return instance;
}

GameManager::GameManager() {
    games_ = {
        {
            1,
            GameMode::kPoliceChase,
            "Cảnh sát bắt cướp",
            "Tự chạy truy đuổi, né mép bàn an toàn, đổi hướng linh hoạt và hú còi cảnh sát 3 giai đoạn"
        },
        {
            2,
            GameMode::kHunted,
            "Bị săn đuổi",
            "Di chuyển nhanh, ngoặt gấp né tránh, tiếng nhịp tim dồn dập và còi truy đuổi"
        },
        {
            3,
            GameMode::kPatrol,
            "Tuần tra kiểm soát",
            "Di chuyển ổn định, định kỳ dừng quét quan sát địa hình, âm thanh Sonar Radar và bộ đàm"
        },
        {
            4,
            GameMode::kDroneEscape,
            "Chạy trốn khỏi Drone",
            "Di chuyển zig-zag tốc độ cao, đổi hướng cắt đuôi, tiếng cánh quạt Drone và còi khóa mục tiêu"
        },
        {
            5,
            GameMode::kDesertWander,
            "Lang thang sa mạc",
            "Di chuyển thong thả, dừng nghỉ ngơi, tiếng sáo sa mạc du dương và tiếng gió cát vi vu"
        }
    };
}

GameManager::~GameManager() {
    Stop();
}

void GameManager::Initialize(RobotController* controller) {
    controller_ = controller;
    ESP_LOGI(TAG, "GameManager initialized with RobotController (%d modes)", (int)games_.size());
}

std::string GameManager::GetCurrentGameName() const {
    int id = current_mode_.load(std::memory_order_relaxed);
    for (const auto& g : games_) {
        if (g.id == id) {
            return g.name;
        }
    }
    return "";
}

std::string GameManager::GetGameListSpeechText() const {
    return "Robot có 5 chế độ trò chơi: "
           "Số 1 là Cảnh sát bắt cướp, "
           "Số 2 là Bị săn đuổi, "
           "Số 3 là Tuần tra kiểm soát, "
           "Số 4 là Chạy trốn khỏi Drone, "
           "Số 5 là Lang thang sa mạc. "
           "Bạn muốn chơi trò nào?";
}

bool GameManager::StartById(int id) {
    if (id < 1 || id > 5) {
        return false;
    }
    return Start(static_cast<GameMode>(id));
}

bool GameManager::StartByName(const std::string& query) {
    if (query.empty()) {
        return false;
    }

    std::string q = MusicPlayer::RemoveVietnameseDiacritics(query);
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (q.find("canh sat") != std::string::npos || q.find("bat cuop") != std::string::npos ||
        q.find("police") != std::string::npos || q == "1" || q.find("so 1") != std::string::npos ||
        q.find("tro 1") != std::string::npos) {
        return Start(GameMode::kPoliceChase);
    }

    if (q.find("san duoi") != std::string::npos || q.find("truy duoi") != std::string::npos ||
        q.find("hunted") != std::string::npos || q == "2" || q.find("so 2") != std::string::npos ||
        q.find("tro 2") != std::string::npos) {
        return Start(GameMode::kHunted);
    }

    if (q.find("tuan tra") != std::string::npos || q.find("kiem soat") != std::string::npos ||
        q.find("patrol") != std::string::npos || q == "3" || q.find("so 3") != std::string::npos ||
        q.find("tro 3") != std::string::npos) {
        return Start(GameMode::kPatrol);
    }

    if (q.find("drone") != std::string::npos || q.find("chay tron") != std::string::npos ||
        q.find("tron khoi") != std::string::npos || q == "4" || q.find("so 4") != std::string::npos ||
        q.find("tro 4") != std::string::npos) {
        return Start(GameMode::kDroneEscape);
    }

    if (q.find("sa mac") != std::string::npos || q.find("lang thang") != std::string::npos ||
        q.find("desert") != std::string::npos || q == "5" || q.find("so 5") != std::string::npos ||
        q.find("tro 5") != std::string::npos) {
        return Start(GameMode::kDesertWander);
    }

    return false;
}

bool GameManager::Start(GameMode mode) {
    if (mode == GameMode::kNone) {
        Stop();
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Dừng nhạc / radio nếu đang phát
    if (MusicPlayer::GetInstance().IsPlaying()) {
        MusicPlayer::GetInstance().Stop();
    }
    if (OnlineMusicPlayer::GetInstance().IsPlaying()) {
        OnlineMusicPlayer::GetInstance().Stop();
    }
    if (RadioPlayer::GetInstance().IsPlaying()) {
        RadioPlayer::GetInstance().Stop();
    }
    // Dừng triệt để Visualizer / Equalizer để giữ nguyên Dashboard OLED
    DottedSpectrumVisualizer::GetInstance().Stop();

    // 2. Dừng phiên game cũ nếu có
    if (is_running_.load()) {
        is_running_.store(false);
        ++session_id_;
        Brake();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    uint32_t session = ++session_id_;
    current_mode_.store(static_cast<int>(mode));
    is_running_.store(true);

    // 3. Khóa wobble nhún nhảy để bánh xe chạy theo đúng kịch bản game
    if (controller_) {
        controller_->SetWobbleEnabled(false);
    }

    // 4. Bật ngõ ra âm thanh nếu đang tắt
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec && !codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    // 5. Cập nhật biểu cảm mắt Mochan tương ứng
    std::string emotion = "neutral";
    switch (mode) {
        case GameMode::kPoliceChase: emotion = "angry"; break;
        case GameMode::kHunted: emotion = "shocked"; break;
        case GameMode::kPatrol: emotion = "thinking"; break;
        case GameMode::kDroneEscape: emotion = "surprised"; break;
        case GameMode::kDesertWander: emotion = "relaxed"; break;
        default: break;
    }
    Board::GetInstance().ApplyEmotion(emotion.c_str());

    // 6. Hiển thị tên trò chơi trên màn hình phụ dạng thông báo chữ (Dashboard OLED giữ nguyên)
    std::string game_name = GetCurrentGameName();
    if (controller_ && !game_name.empty()) {
        controller_->ShowSecondaryText(game_name, 3500);
    }

    // 7. Đảm bảo trạng thái Idle, ngắt mọi hội thoại/ASR cũ
    Application::GetInstance().Schedule([]() {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            Application::GetInstance().SetDeviceState(kDeviceStateIdle);
        }
    });

    ESP_LOGI(TAG, "Starting game mode %d (%s), session=%u", (int)mode, game_name.c_str(), (unsigned)session);

    struct TaskCtx {
        GameManager* self;
        uint32_t session;
        GameMode mode;
    };

    TaskCtx* motor_ctx = new TaskCtx{this, session, mode};
    xTaskCreatePinnedToCore([](void* arg) {
        auto* ctx = static_cast<TaskCtx*>(arg);
        ctx->self->MotorBehaviorTask(ctx->session, ctx->mode);
        delete ctx;
        vTaskDelete(NULL);
    }, "game_motor", 8192, motor_ctx, 4, &motor_task_handle_, 0);

    TaskCtx* audio_ctx = new TaskCtx{this, session, mode};
    xTaskCreatePinnedToCore([](void* arg) {
        auto* ctx = static_cast<TaskCtx*>(arg);
        ctx->self->AudioEffectTask(ctx->session, ctx->mode);
        delete ctx;
        vTaskDelete(NULL);
    }, "game_audio", 6144, audio_ctx, 5, &audio_task_handle_, 1);

    return true;
}

void GameManager::Stop() {
    bool was_running = is_running_.exchange(false);
    current_mode_.store(0);
    ++session_id_;

    if (!was_running) {
        return;
    }

    ESP_LOGI(TAG, "Stopping game mode immediately -> Motor=0, Audio=OFF");

    // Lập tức phanh motor
    Brake();

    // Hiển thị trạng thái dừng trò chơi ngắn trên OLED
    if (controller_) {
        controller_->ShowSecondaryText("Dừng trò chơi", 2000);
    }

    // Khôi phục biểu cảm mắt bình thường
    Board::GetInstance().ApplyEmotion("neutral");

    // Đảm bảo dừng mọi hiệu ứng visualizer nếu có
    DottedSpectrumVisualizer::GetInstance().Stop();

    // Đưa robot về trạng thái Idle an toàn
    Application::GetInstance().Schedule([]() {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle &&
            !MusicPlayer::GetInstance().IsPlaying() &&
            !OnlineMusicPlayer::GetInstance().IsPlaying() &&
            !RadioPlayer::GetInstance().IsPlaying()) {
            Application::GetInstance().SetDeviceState(kDeviceStateIdle);
        }
    });
}

void GameManager::SetWheels(uint32_t session, int left_percent, int right_percent) {
    if (!is_running_.load(std::memory_order_relaxed) || session != session_id_.load()) {
        return;
    }
    last_left_.store(left_percent, std::memory_order_relaxed);
    last_right_.store(right_percent, std::memory_order_relaxed);
    if (controller_) {
        // Chỉ phanh khẩn cấp khi xe đang TIẾN về phía trước (cả 2 bánh cùng > 0)
        // Khi lùi (cả 2 < 0) hoặc xoay tại chỗ đổi hướng né (1 âm 1 dương) thì không cấm cản
        if (left_percent > 0 && right_percent > 0) {
            const auto cliff = controller_->GetCliffStatus();
            const bool danger = cliff.available && (cliff.mode == CliffSensor::Mode::kObstacleForward ?
                (cliff.cliff_detected || (cliff.distance_mm > 0 && cliff.distance_mm <= cliff.edge_mm)) :
                (!cliff.valid || cliff.cliff_detected || (cliff.distance_mm > 0 && cliff.distance_mm >= cliff.edge_mm)));
            if (danger) {
                Brake();
                return;
            }
        }
        controller_->SetLiveDrive(left_percent, right_percent);
    }
}

void GameManager::Brake() {
    last_left_.store(0, std::memory_order_relaxed);
    last_right_.store(0, std::memory_order_relaxed);
    if (controller_) {
        controller_->SetLiveDrive(0, 0);
        controller_->Stop();
    }
}

bool GameManager::WaitAndCheckSafety(uint32_t session, int duration_ms, bool& cliff_detected) {
    cliff_detected = false;
    int elapsed = 0;
    const int step_ms = 25;

    while (elapsed < duration_ms) {
        if (!is_running_.load(std::memory_order_relaxed) || session != session_id_.load()) {
            return false;
        }

        if (controller_) {
            const int l = last_left_.load(std::memory_order_relaxed);
            const int r = last_right_.load(std::memory_order_relaxed);

            // Chỉ kích hoạt ngắt an toàn né vật cản khi xe đang có hướng TIẾN thẳng/tiến vòng
            if (l > 0 && r > 0) {
                const auto cliff = controller_->GetCliffStatus();
                const bool danger = cliff.available && (cliff.mode == CliffSensor::Mode::kObstacleForward ?
                    (cliff.cliff_detected || (cliff.distance_mm > 0 && cliff.distance_mm <= cliff.edge_mm)) :
                    (!cliff.valid || cliff.cliff_detected || (cliff.distance_mm > 0 && cliff.distance_mm >= cliff.edge_mm)));
                if (danger) {
                    cliff_detected = true;
                    Brake();
                    return true;
                }
            }

            // Định kỳ refresh lệnh lái mỗi 100ms để không bị hết hạn lease 350ms
            if (elapsed > 0 && (elapsed % 100) == 0) {
                if (l != 0 || r != 0) {
                    controller_->SetLiveDrive(l, r);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(step_ms));
        elapsed += step_ms;
    }

    return is_running_.load(std::memory_order_relaxed) && (session == session_id_.load());
}

void GameManager::EvadeCliff(uint32_t session, int& turn_counter) {
    if (!is_running_.load(std::memory_order_relaxed) || session != session_id_.load()) return;

    // 1. Phanh khẩn cấp & Mắt hoảng hốt khi phát hiện vật cản/mép vực
    Brake();
    Board::GetInstance().ApplyEmotion("shocked");
    for (int t = 0; t < 80; t += 25) {
        if (!is_running_.load(std::memory_order_relaxed) || session != session_id_.load()) return;
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    // 2. Lùi xe dứt khoát ra xa (450ms ở tốc độ -70%) để tạo khoảng trống an toàn
    last_left_.store(-70, std::memory_order_relaxed);
    last_right_.store(-70, std::memory_order_relaxed);
    if (controller_) {
        controller_->SetLiveDrive(-70, -70);
    }
    for (int t = 0; t < 450; t += 25) {
        if (!is_running_.load(std::memory_order_relaxed) || session != session_id_.load()) {
            Brake();
            return;
        }
        if (t > 0 && (t % 100) == 0 && controller_) {
            controller_->SetLiveDrive(-70, -70);
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    Brake();
    vTaskDelay(pdMS_TO_TICKS(40));

    // 3. Xoay ngoắt góc né (khoảng 90-135 độ)
    turn_counter++;
    bool turn_left = (turn_counter % 2 == 0);
    int turn_l = turn_left ? -80 : 80;
    int turn_r = turn_left ? 80 : -80;

    Board::GetInstance().ApplyEmotion(turn_left ? "left" : "right");
    last_left_.store(turn_l, std::memory_order_relaxed);
    last_right_.store(turn_r, std::memory_order_relaxed);
    if (controller_) {
        controller_->SetLiveDrive(turn_l, turn_r);
    }

    int turn_ms = 400 + (int)(esp_random() % 150);
    for (int t = 0; t < turn_ms; t += 25) {
        if (!is_running_.load(std::memory_order_relaxed) || session != session_id_.load()) {
            Brake();
            return;
        }
        if (t > 0 && (t % 100) == 0 && controller_) {
            controller_->SetLiveDrive(turn_l, turn_r);
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    Brake();
    vTaskDelay(pdMS_TO_TICKS(40));

    // 4. Kiểm tra hướng mới:
    if (controller_) {
        const auto c = controller_->GetCliffStatus();
        if (c.mode == CliffSensor::Mode::kObstacleForward) {
            // Với cảm biến tránh vật cản phía trước: nếu sau khi xoay vẫn còn vướng vật cản (vd góc kẹt hẹp),
            // tiếp tục xoay thêm nhịp nữa (tối đa 4 lần) cho đến khi đường đi THÔNG THOÁNG hoàn toàn!
            for (int retry = 0; retry < 4; ++retry) {
                if (!is_running_.load(std::memory_order_relaxed) || session != session_id_.load()) return;
                const auto check = controller_->GetCliffStatus();
                const bool blocked = check.available &&
                    (check.cliff_detected || (check.distance_mm > 0 && check.distance_mm <= check.edge_mm));
                if (!blocked) {
                    break; // Đã tìm được hướng thông thoáng!
                }
                // Vẫn vướng: tiếp tục xoay thêm 250ms
                last_left_.store(turn_l, std::memory_order_relaxed);
                last_right_.store(turn_r, std::memory_order_relaxed);
                if (controller_) {
                    controller_->SetLiveDrive(turn_l, turn_r);
                }
                for (int t = 0; t < 250; t += 25) {
                    if (!is_running_.load(std::memory_order_relaxed) || session != session_id_.load()) {
                        Brake();
                        return;
                    }
                    if (t > 0 && (t % 100) == 0 && controller_) {
                        controller_->SetLiveDrive(turn_l, turn_r);
                    }
                    vTaskDelay(pdMS_TO_TICKS(25));
                }
                Brake();
                vTaskDelay(pdMS_TO_TICKS(40));
            }
        } else {
            // Với cảm biến mép bàn (hướng xuống): nếu vẫn hổng bánh thì dừng an toàn tại chỗ
            while (is_running_.load(std::memory_order_relaxed) && session == session_id_.load()) {
                const auto check = controller_->GetCliffStatus();
                const bool still_cliff = check.available &&
                    (!check.valid || check.cliff_detected || (check.distance_mm > 0 && check.distance_mm >= check.edge_mm));
                if (!still_cliff) break;
                Brake();
                vTaskDelay(pdMS_TO_TICKS(60));
            }
        }
    }
}

void GameManager::MotorBehaviorTask(uint32_t session, GameMode mode) {
    // 1. Chờ robot ổn định trước khi bắt đầu di chuyển
    int idle_streak = 0;
    while (is_running_.load(std::memory_order_relaxed) && session == session_id_.load()) {
        bool is_speaking = (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking);
        bool is_busy = !Application::GetInstance().GetAudioService().IsPlaybackIdle();
        if (!is_speaking && !is_busy) {
            if (++idle_streak >= 5) { // 5 * 50ms = 250ms ổn định sau khi dứt câu
                break;
            }
        } else {
            idle_streak = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    int turn_counter = 0;
    uint32_t cycle = 0;
    std::string current_emo = "";
    auto set_game_emotion = [&](const char* emo) {
        if (emo != nullptr && current_emo != emo) {
            current_emo = emo;
            Board::GetInstance().ApplyEmotion(emo);
        }
    };

    while (is_running_.load(std::memory_order_relaxed) && session == session_id_.load()) {
        // Tạm dừng bánh xe nếu có giọng nói phát ra (ví dụ câu thoại xen ngang)
        if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
            Brake();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool cliff = false;
        cycle++;

        switch (mode) {
            case GameMode::kPoliceChase: {
                // 1. Cảnh sát bắt cướp: Bứt tốc nhanh (88-92%), đổi hướng lùng sục liên tục
                set_game_emotion("confident");
                int fwd_speed = 88 + (int)(esp_random() % 5);
                int fwd_ms = 450 + (int)(esp_random() % 400); // 450 - 850ms nhịp tiến nhanh
                SetWheels(session, fwd_speed, fwd_speed);
                if (!WaitAndCheckSafety(session, fwd_ms, cliff)) break;

                if (cliff) {
                    current_emo = "shocked";
                    EvadeCliff(session, turn_counter);
                } else {
                    // Đổi hướng liên tục: xen kẽ bo cua tốc độ cao và xoay ngoắt góc
                    int action = (esp_random() % 3);
                    int turn_dir = (esp_random() & 1);
                    if (action == 0) {
                        // Bo cua lượn nhanh: nháy mắt tinh nghịch
                        set_game_emotion("winking");
                        int turn_ms = 250 + (int)(esp_random() % 150);
                        if (turn_dir) SetWheels(session, 45, 92);
                        else SetWheels(session, 92, 45);
                        if (!WaitAndCheckSafety(session, turn_ms, cliff)) break;
                    } else {
                        // Xoay ngoắt góc săn đuổi: liếc mắt theo hướng xoay
                        set_game_emotion(turn_dir ? "left" : "right");
                        int turn_ms = 200 + (int)(esp_random() % 140);
                        if (turn_dir) SetWheels(session, -85, 85);
                        else SetWheels(session, 85, -85);
                        if (!WaitAndCheckSafety(session, turn_ms, cliff)) break;
                    }
                }
                break;
            }

            case GameMode::kHunted: {
                // 2. Bị săn đuổi: Di chuyển cực nhanh (90-96%), đảo cua gắt liên hồi cắt đuôi
                set_game_emotion("surprised");
                int fwd_speed = 90 + (int)(esp_random() % 7);
                int fwd_ms = 350 + (int)(esp_random() % 350); // 350 - 700ms bứt tốc
                SetWheels(session, fwd_speed, fwd_speed);
                if (!WaitAndCheckSafety(session, fwd_ms, cliff)) break;

                if (cliff) {
                    current_emo = "shocked";
                    EvadeCliff(session, turn_counter);
                } else {
                    // Ngoặt gắt đổi hướng bất ngờ: nháy mắt láu cá
                    set_game_emotion("winking");
                    int turn_left = (esp_random() & 1);
                    int turn_ms = 180 + (int)(esp_random() % 160);
                    if (turn_left) SetWheels(session, -90, 90);
                    else SetWheels(session, 90, -90);
                    if (!WaitAndCheckSafety(session, turn_ms, cliff)) break;
                }
                break;
            }

            case GameMode::kPatrol: {
                // 3. Tuần tra kiểm soát: Di chuyển nhanh dứt khoát (82-86%), đổi góc tuần tra liên tục
                set_game_emotion("cool");
                int fwd_speed = 82 + (int)(esp_random() % 5);
                int fwd_ms = 500 + (int)(esp_random() % 400); // 500 - 900ms
                SetWheels(session, fwd_speed, fwd_speed);
                if (!WaitAndCheckSafety(session, fwd_ms, cliff)) break;

                if (cliff) {
                    current_emo = "shocked";
                    EvadeCliff(session, turn_counter);
                } else {
                    // Đổi hướng góc 90-120 độ tuần tra các hướng: mắt cảnh giác/nghi hoặc
                    set_game_emotion("suspicious");
                    int turn_left = (esp_random() & 1);
                    int turn_ms = 240 + (int)(esp_random() % 120);
                    if (turn_left) SetWheels(session, -80, 80);
                    else SetWheels(session, 80, -80);
                    if (!WaitAndCheckSafety(session, turn_ms, cliff)) break;
                }
                break;
            }

            case GameMode::kDroneEscape: {
                // 4. Trốn khỏi Drone: Tốc độ cao (90-95%), đánh võng zig-zag đảo hướng liên tục
                set_game_emotion((cycle % 4 < 2) ? "winking" : "silly");
                int zig_left = (cycle % 2 == 0);
                int fwd_ms = 300 + (int)(esp_random() % 250); // 300 - 550ms
                if (zig_left) SetWheels(session, 95, 50);
                else SetWheels(session, 50, 95);
                if (!WaitAndCheckSafety(session, fwd_ms, cliff)) break;

                if (cliff) {
                    current_emo = "shocked";
                    EvadeCliff(session, turn_counter);
                } else {
                    // Đảo góc tức thời
                    set_game_emotion("confident");
                    int turn_ms = 160 + (int)(esp_random() % 120);
                    if (zig_left) SetWheels(session, -85, 85);
                    else SetWheels(session, 85, -85);
                    if (!WaitAndCheckSafety(session, turn_ms, cliff)) break;
                }
                break;
            }

            case GameMode::kDesertWander: {
                // 5. Sa mạc thám hiểm: Lượn sóng uốn lượn liên tục (80-85%)
                set_game_emotion("relaxed");
                int wander_speed = 80 + (int)(esp_random() % 6);
                int wave_left = (cycle % 2 == 0);
                int wave_ms = 400 + (int)(esp_random() % 300); // 400 - 700ms
                if (wave_left) SetWheels(session, wander_speed, wander_speed - 25);
                else SetWheels(session, wander_speed - 25, wander_speed);
                if (!WaitAndCheckSafety(session, wave_ms, cliff)) break;

                if (cliff) {
                    current_emo = "shocked";
                    EvadeCliff(session, turn_counter);
                } else {
                    set_game_emotion("happy");
                    int turn_left = (esp_random() & 1);
                    int turn_ms = 220 + (int)(esp_random() % 140);
                    if (turn_left) SetWheels(session, -75, 75);
                    else SetWheels(session, 75, -75);
                    if (!WaitAndCheckSafety(session, turn_ms, cliff)) break;
                }
                break;
            }

            case GameMode::kNone:
            default:
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
        }
    }

    Brake();
    motor_task_handle_ = nullptr;
}

void GameManager::AudioEffectTask(uint32_t session, GameMode mode) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        audio_task_handle_ = nullptr;
        return;
    }

    int sample_rate = codec->output_sample_rate();
    if (sample_rate <= 0) {
        sample_rate = 16000;
    }

    const int chunk_samples = sample_rate / 50; // Frame 20ms
    std::vector<int16_t> pcm(chunk_samples);

    float phase1 = 0.0f;
    float phase2 = 0.0f;
    float elapsed_sec = 0.0f;
    const float dt = 1.0f / (float)sample_rate;
    uint32_t noise_seed = 0x12345678u;

    // 1. Chờ robot nói xong hoàn toàn câu thoại TTS (nếu có) trước khi phát âm thanh trò chơi
    int idle_streak = 0;
    while (is_running_.load(std::memory_order_relaxed) && session == session_id_.load()) {
        bool is_speaking = (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking);
        bool is_busy = !Application::GetInstance().GetAudioService().IsPlaybackIdle();
        if (!is_speaking && !is_busy) {
            if (++idle_streak >= 5) { // 5 * 50ms = 250ms ổn định sau khi dứt câu
                break;
            }
        } else {
            idle_streak = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 2. Phát âm hiệu khởi động trò chơi (Start Chime 200ms) báo hiệu vào game dứt khoát
    if (codec && is_running_.load(std::memory_order_relaxed) && session == session_id_.load()) {
        const int chime_samples = sample_rate / 5; // 200ms
        std::vector<int16_t> chime_pcm(chime_samples);
        for (int i = 0; i < chime_samples; i++) {
            float t = (float)i / (float)sample_rate;
            float freq = (t < 0.09f) ? 587.33f : 880.0f; // D5 -> A5
            float env = 1.0f - (t / 0.20f) * 0.4f;
            if (t >= 0.08f && t < 0.10f) env = 0.0f; // ngắt âm giữa 2 nốt bíp
            float s = sinf(2.0f * (float)M_PI * freq * t) * env;
            int32_t val = (int32_t)(s * 9500.0f);
            chime_pcm[i] = (int16_t)std::clamp<int32_t>(val, -32000, 32000);
        }
        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }
        codec->OutputData(chime_pcm);
    }

    while (is_running_.load(std::memory_order_relaxed) && session == session_id_.load()) {
        // Nhường audio nếu đang có TTS hoặc playback khác
        if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking ||
            !Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }

        for (int i = 0; i < chunk_samples; i++) {
            float sample = 0.0f;

            switch (mode) {
                case GameMode::kPoliceChase: {
                    float cycle_t = fmodf(elapsed_sec, 6.4f);
                    float freq = 800.0f;
                    if (cycle_t < 3.6f) {
                        float lfo = sinf(2.0f * (float)M_PI * 0.85f * cycle_t);
                        freq = 950.0f + 320.0f * lfo;
                    } else if (cycle_t < 5.4f) {
                        float lfo = sinf(2.0f * (float)M_PI * 4.6f * (cycle_t - 3.6f));
                        freq = 1040.0f + 340.0f * lfo;
                    } else {
                        int slot = (int)((cycle_t - 5.4f) / 0.25f);
                        freq = (slot % 2 == 0) ? 880.0f : 660.0f;
                    }

                    phase1 += 2.0f * (float)M_PI * freq * dt;
                    if (phase1 > 2.0f * (float)M_PI) phase1 -= 2.0f * (float)M_PI;
                    sample = 0.82f * sinf(phase1) + 0.18f * sinf(3.0f * phase1);
                    break;
                }

                case GameMode::kHunted: {
                    float cycle_t = fmodf(elapsed_sec, 2.4f);
                    float beat_t = fmodf(cycle_t, 0.30f);
                    float pulse_env = 0.0f;
                    float freq = 0.0f;

                    if (beat_t < 0.075f) {
                        static const float kNotes[8] = {880.0f, 1046.5f, 1174.7f, 1318.5f, 987.8f, 1174.7f, 1396.9f, 1760.0f};
                        int step_idx = ((int)(cycle_t / 0.30f)) & 7;
                        freq = kNotes[step_idx];
                        pulse_env = 1.0f - (beat_t / 0.075f) * 0.4f;
                    } else if (beat_t >= 0.11f && beat_t < 0.17f) {
                        freq = 587.3f;
                        pulse_env = 0.75f;
                    }

                    float bass_freq = 160.0f - 400.0f * std::min(beat_t, 0.15f);
                    float bass_env = (beat_t < 0.12f) ? (1.0f - beat_t / 0.12f) : 0.0f;

                    phase1 += 2.0f * (float)M_PI * freq * dt;
                    if (phase1 > 2.0f * (float)M_PI) phase1 -= 2.0f * (float)M_PI;
                    phase2 += 2.0f * (float)M_PI * bass_freq * dt;
                    if (phase2 > 2.0f * (float)M_PI) phase2 -= 2.0f * (float)M_PI;

                    sample = 0.65f * pulse_env * (sinf(phase1) + 0.3f * sinf(2.0f * phase1)) + 0.45f * bass_env * sinf(phase2);
                    break;
                }

                case GameMode::kPatrol: {
                    float cycle_t = fmodf(elapsed_sec, 2.4f);
                    float freq = 0.0f;
                    float env = 0.0f;

                    if (cycle_t < 0.12f) {
                        freq = 784.0f;
                        env = 0.85f;
                    } else if (cycle_t >= 0.15f && cycle_t < 0.28f) {
                        freq = 1046.5f;
                        env = 0.90f;
                    } else if (cycle_t >= 0.85f && cycle_t < 1.30f) {
                        float ping_t = cycle_t - 0.85f;
                        freq = 1318.5f;
                        env = expf(-ping_t * 7.5f);
                    } else if (cycle_t >= 1.45f && cycle_t < 1.75f) {
                        float echo_t = cycle_t - 1.45f;
                        freq = 1318.5f;
                        env = 0.40f * expf(-echo_t * 9.0f);
                    }

                    if (env > 0.001f) {
                        phase1 += 2.0f * (float)M_PI * freq * dt;
                        if (phase1 > 2.0f * (float)M_PI) phase1 -= 2.0f * (float)M_PI;
                        sample = env * sinf(phase1);
                    }
                    break;
                }

                case GameMode::kDroneEscape: {
                    float cycle_t = fmodf(elapsed_sec, 2.2f);
                    float doppler = sinf(2.0f * (float)M_PI * 0.9f * cycle_t);
                    float drone_freq = 165.0f + 35.0f * doppler;
                    float blade_mod = 0.70f + 0.30f * sinf(2.0f * (float)M_PI * 22.0f * elapsed_sec);

                    phase1 += 2.0f * (float)M_PI * drone_freq * dt;
                    if (phase1 > 2.0f * (float)M_PI) phase1 -= 2.0f * (float)M_PI;

                    float drone_wave = blade_mod * (0.50f * sinf(phase1) + 0.35f * sinf(2.0f * phase1) +
                                                    0.25f * sinf(3.0f * phase1) + 0.15f * sinf(4.0f * phase1));

                    float beep_t = fmodf(cycle_t, 0.16f);
                    float alarm_wave = 0.0f;
                    if (beep_t < 0.08f) {
                        float alarm_freq = (cycle_t < 1.1f) ? 1680.0f : 2050.0f;
                        phase2 += 2.0f * (float)M_PI * alarm_freq * dt;
                        if (phase2 > 2.0f * (float)M_PI) phase2 -= 2.0f * (float)M_PI;
                        alarm_wave = 0.55f * sinf(phase2);
                    }

                    sample = 0.60f * drone_wave + alarm_wave;
                    break;
                }

                case GameMode::kDesertWander: {
                    static const float kDesertScale[8] = {
                        293.66f, 349.23f, 369.99f, 440.00f, 466.16f, 440.00f, 369.99f, 349.23f
                    };
                    float cycle_t = fmodf(elapsed_sec, 6.4f);
                    int note_idx = ((int)(cycle_t / 0.80f)) & 7;
                    float note_t = fmodf(cycle_t, 0.80f);

                    float vibrato = 1.0f + 0.012f * sinf(2.0f * (float)M_PI * 5.5f * elapsed_sec);
                    float freq = kDesertScale[note_idx] * vibrato;

                    float env = 1.0f;
                    if (note_t < 0.08f) {
                        env = note_t / 0.08f;
                    } else if (note_t > 0.62f) {
                        env = std::max(0.0f, 1.0f - (note_t - 0.62f) / 0.18f);
                    }

                    phase1 += 2.0f * (float)M_PI * freq * dt;
                    if (phase1 > 2.0f * (float)M_PI) phase1 -= 2.0f * (float)M_PI;

                    noise_seed = noise_seed * 1664525u + 1013904223u;
                    float white = ((int32_t)(noise_seed >> 16) - 32768) / 32768.0f;
                    float wind_lfo = 0.06f + 0.04f * sinf(2.0f * (float)M_PI * 0.35f * elapsed_sec);

                    float flute = env * (0.85f * sinf(phase1) + 0.15f * sinf(2.0f * phase1));
                    sample = 0.80f * flute + wind_lfo * white;
                    break;
                }

                default:
                    break;
            }

            int32_t val = (int32_t)(sample * 9500.0f);
            if (val > 32000) val = 32000;
            if (val < -32000) val = -32000;
            pcm[i] = (int16_t)val;
            elapsed_sec += dt;
        }

        if (!is_running_.load(std::memory_order_relaxed) || session != session_id_.load()) {
            break;
        }

        codec->OutputData(pcm);
    }

    audio_task_handle_ = nullptr;
}
