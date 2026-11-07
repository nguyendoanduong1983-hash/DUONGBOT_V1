#include "robot_mcp_tools.h"

#include "config/hardware_config.h"
#include "config/tuning.h"
#include "control/robot_controller.h"
#include "mcp_server.h"
#include "sensors/environment_derived.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <string>
#include <algorithm>
#include <cctype>

#include "music_player.h"
#include "online_music_player.h"
#include "radio_player.h"
#include "robot/games/game_manager.h"

#define TAG "RobotMcpTools"

namespace {

bool ParseDirection(const std::string& direction, MotorController::Direction& command) {
    if (direction == "forward") {
        command = MotorController::Direction::kForward;
    } else if (direction == "backward") {
        command = MotorController::Direction::kBackward;
    } else if (direction == "left") {
        command = MotorController::Direction::kLeft;
    } else if (direction == "right") {
        command = MotorController::Direction::kRight;
    } else {
        return false;
    }
    return true;
}

}  // namespace

void RobotMcpTools::Register(RobotController& controller) {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool(
        "self.robot.drive",
        "Move forward, backward, left, or right, then stop after `duration_ms`. Cliff safety may "
        "block movement.",
        PropertyList({
            Property("direction", kPropertyTypeString),
            Property("duration_ms", kPropertyTypeInteger, 250, 50, 5000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            const auto direction = properties["direction"].value<std::string>();
            MotorController::Direction command;
            if (!ParseDirection(direction, command)) {
                controller.NotifyToolResult(false);
                return std::string("direction must be forward, backward, left, or right");
            }
            const int duration_ms = properties["duration_ms"].value<int>();
            const bool moved = controller.Move(command, duration_ms,
                                               RobotController::MovePolicy::kPreserveQueued);
            controller.NotifyToolResult(moved);
            if (!moved) {
                return std::string(
                    "Movement blocked: table edge detected; reverse remains available");
            }
            return true;
        });
    mcp_server.AddTool("self.robot.stop", "Stop both drive motors now.", PropertyList(),
                       [&controller](const PropertyList&) -> ReturnValue {
                           controller.Stop();
                           return true;
                       });
    mcp_server.AddTool(
        "self.robot.get_status", "Get current drive-motor state.", PropertyList(),
        [&controller](const PropertyList&) -> ReturnValue {
            return MotorController::StatusJson(controller.GetStatus().motors);
        });
    mcp_server.AddTool("self.robot.dance", "Run one bounded random dance.",
                       PropertyList(), [&controller](const PropertyList&) -> ReturnValue {
                           const bool danced = controller.Dance();
                           controller.NotifyToolResult(danced);
                           return danced;
                       });
    mcp_server.AddTool(
        "self.robot.wobble",
        "Chỉ bật chế độ lắc lư thủ công khi người dùng yêu cầu rõ ràng robot lắc lư; chế độ này tiếp tục cho đến khi người dùng yêu cầu dừng. Không gọi công cụ này cho yêu cầu phát nhạc vì lắc theo nhạc được điều khiển tự động và tự dừng khi nhạc kết thúc. Enable persistent manual wobble only when the user explicitly asks the robot to wobble; keep it enabled until the user asks to stop. Never call this tool for music requests; music wobble is automatic and stops when music ends.",
        PropertyList({Property("enabled", kPropertyTypeBoolean, true)}),
        [&controller](const PropertyList& properties) -> ReturnValue {
            const bool enabled = properties["enabled"].value<bool>();
            return controller.SetWobbleEnabled(enabled);
        });
    mcp_server.AddTool(
        "self.robot.react",
        "Run one coordinated semantic reaction across the face, status light, optional OLED "
        "text, and safe optional motion. Reactions: acknowledge, success, celebrate, thinking, "
        "curious, confused, warning, error, startled, sleepy, nope, attention.",
        PropertyList({
            Property("reaction", kPropertyTypeString, "acknowledge"),
            Property("duration_ms", kPropertyTypeInteger, 3000, 250, 30000),
            Property("oled_text", kPropertyTypeString, ""),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            if (!controller.React(properties["reaction"].value<std::string>(),
                                  properties["duration_ms"].value<int>(),
                                  properties["oled_text"].value<std::string>())) {
                return std::string(
                    "Unsupported reaction or blocked by a higher-priority active reaction");
            }
            return true;
        });
    mcp_server.AddTool(
        "self.face.set_emotion",
        "Show a temporary face emotion: neutral, happy, bored, laughing, funny, sad, angry, "
        "crying, loving, "
        "embarrassed, surprised, shocked, thinking, winking, cool, relaxed, delicious, "
        "kissy, confident, sleepy, silly, confused, suspicious, and shake.",
        PropertyList({
            Property("emotion", kPropertyTypeString, "neutral"),
            Property("duration_ms", kPropertyTypeInteger, 5000, 250, 30000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            const std::string emotion = properties["emotion"].value<std::string>();
            if (!controller.ShowEmotion(emotion, properties["duration_ms"].value<int>())) {
                return std::string("Unsupported face emotion");
            }
            return true;
        });
    mcp_server.AddTool(
        "self.face.look",
        "Look temporarily: center, left, right, up, down, up_left, up_right, down_left, or "
        "down_right.",
        PropertyList({
            Property("direction", kPropertyTypeString, "center"),
            Property("duration_ms", kPropertyTypeInteger, 2500, 250, 15000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            std::string direction = properties["direction"].value<std::string>();
            if (direction == "center") {
                direction = "neutral";
            }
            if (!controller.ShowEmotion(direction, properties["duration_ms"].value<int>())) {
                return std::string("Unsupported look direction");
            }
            return true;
        });
#ifdef SECONDARY_OLED_I2C_ADDRESS
    mcp_server.AddTool(
        "self.secondary_display.show_text",
        "Show short UTF-8 text, including Vietnamese, on the secondary OLED; telemetry returns "
        "afterward.",
        PropertyList({
            Property("text", kPropertyTypeString),
            Property("duration_ms", kPropertyTypeInteger, 5000, 500, 60000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            if (!controller.ShowSecondaryText(properties["text"].value<std::string>(),
                                              properties["duration_ms"].value<int>())) {
                return std::string("OLED is unavailable or text is empty");
            }
            return true;
        });
#endif
    mcp_server.AddTool(
        "self.status_light.set_effect",
        "Temporarily set the status lights to steady, breathe, blink, or off. Motor indication "
        "has priority.",
        PropertyList({
            Property("effect", kPropertyTypeString, "steady"),
            Property("duration_ms", kPropertyTypeInteger, 5000, 250, 30000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            if (!controller.SetStatusLightEffect(properties["effect"].value<std::string>(),
                                                 properties["duration_ms"].value<int>())) {
                return std::string("Unsupported status-light effect");
            }
            return true;
        });
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
    mcp_server.AddTool(
        "self.distance.get",
        "Get downward floor distance and cliff-detection state.", PropertyList(),
        [&controller](const PropertyList&) -> ReturnValue {
            const auto cliff = controller.GetStatus().cliff;
            if (!cliff.available) {
                return std::string(R"({"available":false})");
            }
            return std::string("{\"available\":true,\"valid\":") +
                   (cliff.valid ? "true" : "false") +
                   ",\"distance_mm\":" + std::to_string(cliff.distance_mm) +
                   ",\"cliff_detected\":" + (cliff.cliff_detected ? "true" : "false") +
                   ",\"edge_mm\":" + std::to_string(cliff.edge_mm) + "}";
        });
#endif
    mcp_server.AddTool(
        "self.environment.get",
        "Get cached temperature, humidity, pressure, and illuminance with per-sensor validity.",
        PropertyList(), [&controller](const PropertyList&) -> ToolResult {
            cJSON* result = cJSON_CreateObject();
            if (result == nullptr) {
                return std::unexpected("Out of memory");
            }
            const EnvironmentStatus environment = controller.GetStatus().environment;
            cJSON_AddBoolToObject(result, "aht20_available", environment.aht20_available);
            cJSON_AddBoolToObject(result, "bmp280_available", environment.bmp280_available);
            cJSON_AddBoolToObject(result, "bh1750_available", environment.bh1750_available);
            cJSON_AddBoolToObject(result, "temperature_valid", environment.temperature_valid);
            cJSON_AddBoolToObject(result, "humidity_valid", environment.humidity_valid);
            cJSON_AddBoolToObject(result, "pressure_valid", environment.pressure_valid);
            cJSON_AddBoolToObject(result, "illuminance_valid", environment.illuminance_valid);
            cJSON_AddStringToObject(result, "light_level",
                                    LightLevelName(environment.light_level));
            cJSON_AddStringToObject(result, "comfort_level",
                                    ComfortLevelName(environment.comfort_level));
            cJSON_AddStringToObject(result, "pressure_trend",
                                    PressureTrendName(environment.pressure_trend));
            if (environment.temperature_valid) {
                cJSON_AddNumberToObject(result, "temperature_c", environment.temperature_c);
            }
            if (environment.humidity_valid) {
                cJSON_AddNumberToObject(result, "humidity_percent",
                                        environment.humidity_percent);
            }
            if (environment.pressure_valid) {
                cJSON_AddNumberToObject(result, "pressure_hpa", environment.pressure_hpa);
            }
            if (environment.illuminance_valid) {
                cJSON_AddNumberToObject(result, "illuminance_lux",
                                        environment.illuminance_lux);
            }
            return result;
        });
#ifdef INA219_I2C_ADDRESS
    mcp_server.AddTool(
        "self.battery.get_status",
        "Get detailed battery charge, voltage, current, power, and SoC diagnostics.",
        PropertyList(), [&controller](const PropertyList&) -> ToolResult {
            cJSON* result = cJSON_CreateObject();
            if (result == nullptr) {
                return std::unexpected("Out of memory");
            }
            const auto battery = controller.GetStatus().battery;
            cJSON_AddBoolToObject(result, "available", battery.available);
            cJSON_AddBoolToObject(result, "valid", battery.valid);
            if (battery.valid) {
                cJSON_AddNumberToObject(result, "percent", battery.percent);
                cJSON_AddNumberToObject(result, "voltage_v", battery.voltage_v);
                cJSON_AddNumberToObject(result, "current_ma", battery.current_ma);
                cJSON_AddNumberToObject(result, "power_mw", battery.power_mw);
                cJSON_AddNumberToObject(result, "signed_current_ma", battery.signed_current_ma);
                cJSON_AddNumberToObject(result, "shunt_voltage_mv", battery.shunt_voltage_mv);
                cJSON_AddNumberToObject(result, "bus_voltage_v", battery.bus_voltage_v);
                cJSON_AddNumberToObject(result, "remaining_mah", battery.remaining_mah);
                cJSON_AddNumberToObject(result, "capacity_mah", BATTERY_SOC_USABLE_CAPACITY_MAH);
                cJSON_AddStringToObject(result, "soc_method", "coulomb_quasi_rest_anchors");
                cJSON_AddBoolToObject(result, "soc_tracking_degraded",
                                      battery.soc_tracking_degraded);
                cJSON_AddBoolToObject(result, "soc_quasi_resting", battery.soc_quasi_resting);
                cJSON_AddNumberToObject(result, "soc_voltage_reference_percent",
                                        battery.soc_voltage_reference_percent);
                cJSON_AddNumberToObject(result, "soc_voltage_correction_mah",
                                        battery.soc_voltage_correction_mah);
                cJSON_AddBoolToObject(result, "soc_full_anchored", battery.soc_full_anchored);
                cJSON_AddBoolToObject(result, "soc_bootstrap_voltage_rebased",
                                      battery.soc_bootstrap_voltage_rebased);
                cJSON_AddBoolToObject(result, "soc_empty_anchored", battery.soc_empty_anchored);
                cJSON_AddBoolToObject(result, "conversion_ready", battery.conversion_ready);
                cJSON_AddBoolToObject(result, "math_overflow", battery.math_overflow);
                cJSON_AddBoolToObject(result, "charging", battery.charging);
                cJSON_AddBoolToObject(result, "discharging", battery.discharging);
            }
            return result;
        });
#endif
#ifdef MPU6050_I2C_ADDRESS
    mcp_server.AddTool(
        "self.motion.get_orientation",
        "Get orientation, acceleration, rotation, gesture, and gyro-turn state.",
        PropertyList(), [&controller](const PropertyList&) -> ToolResult {
            cJSON* result = cJSON_CreateObject();
            if (result == nullptr) {
                return std::unexpected("Out of memory");
            }
            const RobotStatus status = controller.GetStatus();
            cJSON_AddBoolToObject(result, "available", status.motion_sensor_available);
            cJSON_AddBoolToObject(result, "valid", status.motion_sensor_valid);
            cJSON_AddBoolToObject(result, "emotion_control", status.motion_emotions_enabled);
            const auto& gyro = status.gyro;
            cJSON_AddBoolToObject(result, "gyro_bias_valid", gyro.bias_valid);
            const int64_t gyro_sample_us = gyro.sample_timestamp_us;
            cJSON_AddNumberToObject(
                result, "gyro_sample_age_ms",
                gyro_sample_us > 0 ? (esp_timer_get_time() - gyro_sample_us) / 1000.0 : -1.0);
            cJSON_AddBoolToObject(result, "gyro_turn_available", gyro.available);
            cJSON_AddBoolToObject(result, "gyro_turn_pending", gyro.pending);
            cJSON_AddBoolToObject(result, "gyro_turn_active", gyro.active);
            cJSON_AddNumberToObject(result, "gyro_turn_target_deg", gyro.target_deg);
            cJSON_AddNumberToObject(result, "gyro_turn_progress_deg", gyro.progress_deg);
            cJSON_AddStringToObject(result, "gyro_turn_stop_reason",
                                    GyroTurnController::StopReasonName(gyro.stop_reason));
            cJSON_AddNumberToObject(result, "yaw_rate_dps", gyro.yaw_rate_dps);
            cJSON_AddNumberToObject(result, "yaw_bias_dps", gyro.yaw_bias_dps);
            if (status.motion_sensor_valid) {
                cJSON_AddNumberToObject(result, "roll_deg", status.motion_roll_deg);
                cJSON_AddNumberToObject(result, "pitch_deg", status.motion_pitch_deg);
                cJSON_AddNumberToObject(result, "acceleration_g", status.motion_acceleration_g);
                cJSON_AddNumberToObject(result, "rotation_dps", status.motion_rotation_dps);
                cJSON_AddStringToObject(result, "gesture", status.motion_gesture.c_str());
            }
            return result;
        });
    mcp_server.AddTool(
        "self.robot.turn_relative",
        "Turn by a gyro-measured angle when an MPU6050 is detected: positive is right, negative "
        "is left. If no MPU6050 is detected, perform an approximate timed turn and return a "
        "safety warning; the requested angle cannot be guaranteed.",
        PropertyList({Property("degrees", kPropertyTypeInteger, 90, -180, 180)}),
        [&controller](const PropertyList& properties) -> ToolResult {
            std::string message;
            const bool turned =
                controller.TurnRelative(properties["degrees"].value<int>(), message);
            controller.NotifyToolResult(turned);
            if (!turned) {
                return std::unexpected(message);
            }
            return message.empty() ? ReturnValue(true) : ReturnValue(message);
        });
    ESP_LOGI(TAG, "MPU6050 MCP tools registered");
#endif

    // Music tools for online search and GitHub playlist interaction
    mcp_server.AddTool(
        "self.music.play_online",
        "Tìm kiếm bài hát hoặc ca sĩ trên Internet và phát nhạc trực tuyến. Dùng công cụ này khi người dùng yêu cầu nghe một bài hát, ca sĩ cụ thể hoặc thể loại nhạc online.",
        PropertyList({Property("query", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            const std::string query = properties["query"].value<std::string>();
            if (query.empty()) {
                return std::unexpected("Từ khóa tìm kiếm không được để trống");
            }
            // Ưu tiên kiểm tra kho nhạc GitHub trước (đồng bộ với STT auto-play để tránh hủy nhầm bài GitHub vừa bật)
            if (MusicPlayer::GetInstance().PlayByName(query)) {
                return "Đang phát từ kho nhạc: " + MusicPlayer::GetInstance().GetCurrentTrackName();
            }
            if (OnlineMusicPlayer::GetInstance().PlayByQuery(query)) {
                return "Đang tìm kiếm và phát: " + query;
            }
            return std::unexpected("Không thể tìm và phát bài hát online");
        });

    mcp_server.AddTool(
        "self.music.list_folders",
        "Thống kê các thư mục nhạc trên kho nhạc GitHub và trả về lời thoại hỏi người dùng muốn nghe thư mục nào. Gọi công cụ này khi người dùng muốn nghe nhạc GitHub, duyệt kho nhạc.",
        PropertyList(),
        [](const PropertyList&) -> ToolResult {
            std::string text = MusicPlayer::GetInstance().GetFolderListSpeechText();
            return text;
        });

    mcp_server.AddTool(
        "self.music.list_tracks",
        "Liệt kê các bài hát trong một thư mục nhạc GitHub và trả về lời thoại hỏi người dùng muốn chọn bài nào.",
        PropertyList({Property("folder", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            std::string folder_query = properties["folder"].value<std::string>();
            std::string matched_folder = MusicPlayer::GetInstance().MatchFolder(folder_query);
            if (matched_folder.empty()) {
                return "Không tìm thấy thư mục: " + folder_query + ". Vui lòng thử lại với tên hoặc số thứ tự thư mục.";
            }
            std::string text = MusicPlayer::GetInstance().GetFolderTracksSpeechText(matched_folder);
            return text;
        });

    mcp_server.AddTool(
        "self.music.play_github",
        "Phát một bài hát trong kho GitHub theo tên bài hát hoặc số thứ tự.",
        PropertyList({
            Property("song", kPropertyTypeString),
            Property("folder", kPropertyTypeString, "")
        }),
        [](const PropertyList& properties) -> ToolResult {
            std::string song = properties["song"].value<std::string>();
            std::string folder = properties["folder"].value<std::string>();
            if (folder.empty()) {
                folder = MusicPlayer::GetInstance().GetActiveFolder();
            } else {
                folder = MusicPlayer::GetInstance().MatchFolder(folder);
            }
            if (OnlineMusicPlayer::GetInstance().IsPlaying()) {
                OnlineMusicPlayer::GetInstance().Stop();
            }
            if (MusicPlayer::GetInstance().PlayInFolder(folder, song)) {
                return "Đang phát bài: " + song + (folder.empty() ? "" : " trong thư mục " + folder);
            }
            return std::unexpected("Không tìm thấy bài hát yêu cầu trong kho GitHub");
        });

    mcp_server.AddTool(
        "self.music.stop",
        "Dừng phát nhạc ngay lập tức (cả nhạc online và nhạc GitHub).",
        PropertyList(),
        [](const PropertyList&) -> ToolResult {
            bool was_playing = MusicPlayer::GetInstance().IsPlaying() ||
                               OnlineMusicPlayer::GetInstance().IsPlaying() ||
                               RadioPlayer::GetInstance().IsPlaying() ||
                               GameManager::GetInstance().IsRunning();
            MusicPlayer::GetInstance().Stop();
            OnlineMusicPlayer::GetInstance().Stop();
            RadioPlayer::GetInstance().Stop();
            GameManager::GetInstance().Stop();
            MusicPlayer::GetInstance().ResetDialog();
            return was_playing ? "Đã dừng phát nhạc, radio và trò chơi" : "Hiện tại không có âm thanh hay trò chơi nào đang phát";
        });

    mcp_server.AddTool(
        "self.music.get_status",
        "Lấy thông tin trạng thái phát nhạc hiện tại.",
        PropertyList(),
        [](const PropertyList&) -> ToolResult {
            cJSON* result = cJSON_CreateObject();
            if (result == nullptr) {
                return std::unexpected("Out of memory");
            }
            const bool online = OnlineMusicPlayer::GetInstance().IsPlaying();
            const bool github = MusicPlayer::GetInstance().IsPlaying();
            cJSON_AddBoolToObject(result, "is_playing", online || github);
            cJSON_AddStringToObject(result, "source", online ? "online" : (github ? "github" : "none"));
            cJSON_AddStringToObject(result, "current_track",
                online ? OnlineMusicPlayer::GetInstance().GetCurrentTitle().c_str()
                       : (github ? MusicPlayer::GetInstance().GetCurrentTrackName().c_str() : ""));
            const auto dialog_state = MusicPlayer::GetInstance().GetDialogState();
            cJSON_AddStringToObject(result, "dialog_state",
                dialog_state == MusicDialogState::kWaitingFolderSelection ? "waiting_folder" :
                (dialog_state == MusicDialogState::kWaitingSongSelection ? "waiting_song" : "idle"));
            return result;
        });

    ESP_LOGI(TAG, "Music MCP tools registered");

    // Radio MCP tools for voice-controlled live radio broadcasting
    mcp_server.AddTool(
        "self.radio.control",
        "Điều khiển phát đài phát thanh (Radio FM/AM trực tuyến) của Việt Nam bằng giọng nói.\n"
        "Tham số action:\n"
        "- 'play': Bật đài phát thanh. Cần truyền thêm tên hoặc số kênh vào 'station' (ví dụ: 'vov1', 'vov2', 'vov3', 'vov giao thông', 'hà nội 90', 'hà nội 96', 'voh 99.9', 'voh 95.6', 'voh 610', hoặc '1', '2', '3'...). Nếu người dùng chỉ nói chung chung 'bật đài/mở radio' thì mặc định mở đài 1.\n"
        "- 'stop': Tắt đài phát thanh.\n"
        "- 'list': Đọc danh sách 10 đài phát thanh hiện có.\n"
        "- 'status': Kiểm tra xem đài phát thanh có đang phát không và tên đài hiện tại.",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("station", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ToolResult {
            std::string action = properties["action"].value<std::string>();
            std::transform(action.begin(), action.end(), action.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (action == "list") {
                return RadioPlayer::GetInstance().GetStationListSpeechText();
            }

            if (action == "stop") {
                bool was_playing = RadioPlayer::GetInstance().IsPlaying();
                RadioPlayer::GetInstance().Stop();
                return was_playing ? "Đã tắt đài phát thanh." : "Hiện tại không có đài nào đang phát.";
            }

            if (action == "status") {
                cJSON* result = cJSON_CreateObject();
                if (result == nullptr) {
                    return std::unexpected("Out of memory");
                }
                const bool playing = RadioPlayer::GetInstance().IsPlaying();
                cJSON_AddBoolToObject(result, "is_playing", playing);
                cJSON_AddNumberToObject(result, "station_id", RadioPlayer::GetInstance().GetCurrentStationId());
                cJSON_AddStringToObject(result, "station_name", RadioPlayer::GetInstance().GetCurrentStationName().c_str());
                cJSON_AddStringToObject(result, "station_desc", RadioPlayer::GetInstance().GetCurrentStationDescription().c_str());
                return result;
            }

            if (action == "play") {
                std::string station_query = properties["station"].value<std::string>();
                if (station_query.empty()) {
                    station_query = "1"; // Mặc định kênh 1 nếu không chỉ định rõ
                }

                if (RadioPlayer::GetInstance().PlayByName(station_query)) {
                    return "Đang mở đài " + RadioPlayer::GetInstance().GetCurrentStationName();
                } else {
                    return std::unexpected("Không tìm thấy đài phát thanh theo yêu cầu. Hãy thử VOV1, VOV2, VOV3, VOV Giao thông, Hà Nội hoặc VOH.");
                }
            }

            return std::unexpected("Hành động không hợp lệ. Chỉ chấp nhận 'play', 'stop', 'list', 'status'");
        });

    ESP_LOGI(TAG, "Radio MCP tools registered");

    // Game Mode MCP tools for voice-controlled interactive robot games
    mcp_server.AddTool(
        "self.game.control",
        "Điều khiển 5 chế độ trò chơi tương tác của robot:\n"
        "  1. Cảnh sát bắt cướp (tự chạy truy đuổi, né mép bàn an toàn, hú còi cảnh sát 3 giai đoạn)\n"
        "  2. Bị săn đuổi (di chuyển nhanh, ngoặt gấp né tránh, tiếng nhịp tim dồn dập và còi truy đuổi)\n"
        "  3. Tuần tra kiểm soát (di chuyển ổn định, quét quan sát địa hình, âm thanh Sonar Radar và bộ đàm)\n"
        "  4. Chạy trốn khỏi Drone (chạy zíc-zắc tốc độ cao, đổi hướng cắt đuôi, tiếng cánh quạt Drone và còi khóa mục tiêu)\n"
        "  5. Lang thang sa mạc (chạy chậm thong thả, dừng nghỉ ngơi, tiếng sáo sa mạc du dương và tiếng gió cát)\n"
        "Tham số:\n"
        "- `action`: 'start' (bắt đầu chơi), 'stop' (dừng trò chơi), 'list' (đọc danh sách trò chơi), 'status' (trạng thái trò chơi hiện tại)\n"
        "- `game`: Số thứ tự (1 đến 5) hoặc tên trò chơi khi action='start'",
        PropertyList({
            Property("action", kPropertyTypeString, "list"),
            Property("game", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ToolResult {
            std::string action = properties["action"].value<std::string>();
            std::transform(action.begin(), action.end(), action.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            auto& gm = GameManager::GetInstance();

            if (action == "list") {
                return gm.GetGameListSpeechText();
            }

            if (action == "stop" || action == "exit" || action == "quit" || action == "off") {
                bool was_running = gm.IsRunning();
                gm.Stop();
                return was_running ? "Đã dừng trò chơi và đưa robot về trạng thái bình thường." : "Hiện không có trò chơi nào đang chạy.";
            }

            if (action == "status") {
                cJSON* result = cJSON_CreateObject();
                if (result == nullptr) {
                    return std::unexpected("Out of memory");
                }
                const bool running = gm.IsRunning();
                cJSON_AddBoolToObject(result, "is_running", running);
                cJSON_AddNumberToObject(result, "game_id", gm.GetCurrentGameId());
                cJSON_AddStringToObject(result, "game_name", gm.GetCurrentGameName().c_str());
                return result;
            }

            if (action == "start" || action == "play") {
                std::string g = properties["game"].value<std::string>();
                if (g.empty()) {
                    g = "1"; // Mặc định trò số 1 nếu không chỉ định rõ
                }
                if (gm.StartByName(g)) {
                    return "Đang bắt đầu trò chơi " + gm.GetCurrentGameName();
                } else {
                    return std::unexpected("Không tìm thấy trò chơi theo yêu cầu. Hãy chọn từ 1 đến 5 hoặc: Cảnh sát bắt cướp, Bị săn đuổi, Tuần tra kiểm soát, Chạy trốn khỏi Drone, Lang thang sa mạc.");
                }
            }

            return std::unexpected("Hành động không hợp lệ. Chỉ chấp nhận 'start', 'stop', 'list', 'status'");
        });

    ESP_LOGI(TAG, "Game MCP tools registered");
}
