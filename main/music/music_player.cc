#include "music_player.h"
#include "online_music_player.h"
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
#include <set>

#include "esp_mp3_dec.h"
#include "esp_ae_rate_cvt.h"

#define TAG "MusicPlayer"

#define GITHUB_INDEX_URL "https://raw.githubusercontent.com/nguyendoanduong1983-hash/music/main/index.json"
#define GITHUB_RAW_BASE  "https://raw.githubusercontent.com/nguyendoanduong1983-hash/music/main/"

#define MUSIC_RING_BUFFER_SIZE (512 * 1024)
#define MUSIC_PREBUFFER_MIN    (64 * 1024)

static std::string UrlDecode(const std::string& str) {
    std::string res;
    res.reserve(str.size());
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '%' && i + 2 < str.size()) {
            auto hex2val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex2val(str[i + 1]);
            int l = hex2val(str[i + 2]);
            if (h >= 0 && l >= 0) {
                res += (char)((h << 4) | l);
                i += 2;
                continue;
            }
        } else if (str[i] == '+') {
            res += ' ';
            continue;
        }
        res += str[i];
    }
    return res;
}

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer instance;
    return instance;
}

MusicPlayer::MusicPlayer() {
    Initialize();
}

MusicPlayer::~MusicPlayer() {
    Stop();
}

void MusicPlayer::Initialize() {
    // Khởi chạy tác vụ đồng bộ nền kiểm tra nhạc mới theo chu kỳ (mỗi 10 phút)
    if (sync_task_handle_ == nullptr) {
        xTaskCreatePinnedToCoreWithCaps([](void* arg) {
            static_cast<MusicPlayer*>(arg)->SyncTask();
            vTaskDelete(NULL);
        }, "music_sync", 10240, this, 2, &sync_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

std::string MusicPlayer::BuildRawUrl(const std::string& relative_path) const {
    if (relative_path.rfind("http://", 0) == 0 || relative_path.rfind("https://", 0) == 0) {
        return relative_path;
    }
    std::string rel = relative_path;
    while (!rel.empty() && rel[0] == '/') {
        rel.erase(0, 1);
    }
    return std::string(GITHUB_RAW_BASE) + rel;
}

std::string MusicPlayer::RemoveVietnameseDiacritics(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const struct { const char* utf8; const char* asc; } mapping[] = {
        {"à","a"},{"á","a"},{"ả","a"},{"ã","a"},{"ạ","a"},
        {"ă","a"},{"ằ","a"},{"ắ","a"},{"ẳ","a"},{"ẵ","a"},{"ặ","a"},
        {"â","a"},{"ầ","a"},{"ấ","a"},{"ẩ","a"},{"ẫ","a"},{"ậ","a"},
        {"è","e"},{"é","e"},{"ẻ","e"},{"ẽ","e"},{"ẹ","e"},
        {"ê","e"},{"ề","e"},{"ế","e"},{"ể","e"},{"ễ","e"},{"ệ","e"},
        {"ì","i"},{"í","i"},{"ỉ","i"},{"ĩ","i"},{"ị","i"},
        {"ò","o"},{"ó","o"},{"ỏ","o"},{"õ","o"},{"ọ","o"},
        {"ô","o"},{"ồ","o"},{"ố","o"},{"ổ","o"},{"ỗ","o"},{"ộ","o"},
        {"ơ","o"},{"ờ","o"},{"ớ","o"},{"ở","o"},{"ỡ","o"},{"ợ","o"},
        {"ù","u"},{"ú","u"},{"ủ","u"},{"ũ","u"},{"ụ","u"},
        {"ư","u"},{"ừ","u"},{"ứ","u"},{"ử","u"},{"ữ","u"},{"ự","u"},
        {"ỳ","y"},{"ý","y"},{"ỷ","y"},{"ỹ","y"},{"ỵ","y"},
        {"đ","d"},
        {"À","a"},{"Á","a"},{"Ả","a"},{"Ã","a"},{"Ạ","a"},
        {"Ă","a"},{"Ằ","a"},{"Ắ","a"},{"Ẳ","a"},{"Ẵ","a"},{"Ặ","a"},
        {"Â","a"},{"Ầ","a"},{"Ấ","a"},{"Ẩ","a"},{"Ẫ","a"},{"Ậ","a"},
        {"È","e"},{"É","e"},{"Ẻ","e"},{"Ẽ","e"},{"Ẹ","e"},
        {"Ê","e"},{"Ề","e"},{"Ế","e"},{"Ể","e"},{"Ễ","e"},{"Ệ","e"},
        {"Ì","i"},{"Í","i"},{"Ỉ","i"},{"Ĩ","i"},{"Ị","i"},
        {"Ò","o"},{"Ó","o"},{"Ỏ","o"},{"Õ","o"},{"Ọ","o"},
        {"Ô","o"},{"Ồ","o"},{"Ố","o"},{"Ổ","o"},{"Ỗ","o"},{"Ộ","o"},
        {"Ơ","o"},{"Ờ","o"},{"Ớ","o"},{"Ở","o"},{"Ỡ","o"},{"Ợ","o"},
        {"Ù","u"},{"Ú","u"},{"Ủ","u"},{"Ũ","u"},{"Ụ","u"},
        {"Ư","u"},{"Ừ","u"},{"Ứ","u"},{"Ử","u"},{"Ữ","u"},{"Ự","u"},
        {"Ỳ","y"},{"Ý","y"},{"Ỷ","y"},{"Ỹ","y"},{"Ỵ","y"},
        {"Đ","d"}
    };

    for (const auto& m : mapping) {
        size_t p = 0;
        size_t l = strlen(m.utf8);
        while ((p = s.find(m.utf8, p)) != std::string::npos) {
            s.replace(p, l, m.asc);
            p += strlen(m.asc);
        }
    }
    return s;
}

int MusicPlayer::SyncPlaylistFromGitHub(bool force) {
    if (!force && (is_playing_ || OnlineMusicPlayer::GetInstance().IsPlaying())) {
        ESP_LOGI(TAG, "Skipping background GitHub music sync while playback is active");
        return (int)tracks_.size();
    }
    ESP_LOGI(TAG, "Checking for music updates from GitHub: %s", GITHUB_INDEX_URL);

    esp_http_client_config_t config = {};
    config.url = GITHUB_INDEX_URL;
    config.timeout_ms = 8000;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for music sync");
        return -1;
    }

    esp_http_client_set_header(client, "User-Agent", "DUONGBOT-Music/1.0");
    if (!force && !last_etag_.empty()) {
        esp_http_client_set_header(client, "If-None-Match", last_etag_.c_str());
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open connection to GitHub index: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code == 304) {
        ESP_LOGI(TAG, "GitHub playlist index is up to date (304 Not Modified)");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return GetTrackCount();
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "GitHub index returned status code: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    char *etag_val = nullptr;
    if (esp_http_client_get_header(client, "ETag", &etag_val) == ESP_OK && etag_val != nullptr) {
        last_etag_ = etag_val;
    }

    std::string json_data;
    if (content_length > 0 && content_length < 256 * 1024) {
        json_data.reserve(content_length + 1);
    }
    char buf[1024];
    int r = 0;
    while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        json_data.append(buf, r);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (json_data.empty()) {
        ESP_LOGE(TAG, "GitHub playlist response is empty");
        return -1;
    }

    cJSON* root = cJSON_Parse(json_data.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON playlist from GitHub");
        return -1;
    }

    cJSON* arr = nullptr;
    if (cJSON_IsArray(root)) {
        arr = root;
    } else if (cJSON_IsObject(root)) {
        arr = cJSON_GetObjectItem(root, "songs");
        if (!arr) arr = cJSON_GetObjectItem(root, "tracks");
    }

    if (!arr || !cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "JSON does not contain a track array");
        cJSON_Delete(root);
        return -1;
    }

    std::vector<MusicTrack> new_tracks;
    int count = cJSON_GetArraySize(arr);
    new_tracks.reserve(count);

    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(item)) continue;

        cJSON* j_id = cJSON_GetObjectItem(item, "id");
        cJSON* j_name = cJSON_GetObjectItem(item, "name");
        cJSON* j_cat = cJSON_GetObjectItem(item, "category");
        cJSON* j_path = cJSON_GetObjectItem(item, "path");

        if (!j_path || !cJSON_IsString(j_path) || !j_path->valuestring) continue;

        MusicTrack t;
        t.path = j_path->valuestring;
        t.id = (j_id && cJSON_IsString(j_id) && j_id->valuestring) ? j_id->valuestring : ("track_" + std::to_string(i + 1));
        t.name = (j_name && cJSON_IsString(j_name) && j_name->valuestring) ? j_name->valuestring : t.id;
        t.category = (j_cat && cJSON_IsString(j_cat) && j_cat->valuestring) ? j_cat->valuestring : "song";

        std::string raw_path = t.path;
        std::string folder = "Khác";
        size_t slash_pos = raw_path.find('/');
        if (slash_pos != std::string::npos) {
            folder = UrlDecode(raw_path.substr(0, slash_pos));
        } else {
            folder = (t.category.empty() || t.category == "song") ? "songs" : t.category;
        }
        t.folder = folder;

        cJSON* j_tags = cJSON_GetObjectItem(item, "tags");
        if (j_tags && cJSON_IsArray(j_tags)) {
            int tag_count = cJSON_GetArraySize(j_tags);
            for (int k = 0; k < tag_count; k++) {
                cJSON* tag_item = cJSON_GetArrayItem(j_tags, k);
                if (cJSON_IsString(tag_item) && tag_item->valuestring) {
                    t.tags.push_back(tag_item->valuestring);
                }
            }
        }
        new_tracks.push_back(std::move(t));
    }
    cJSON_Delete(root);

    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        tracks_ = std::move(new_tracks);
    }

    ESP_LOGI(TAG, "Successfully synced %d tracks from GitHub", (int)tracks_.size());
    return (int)tracks_.size();
}

void MusicPlayer::TriggerSync() {
    if (sync_task_handle_ != nullptr) {
        xTaskNotifyGive(sync_task_handle_);
    }
}

void MusicPlayer::SyncTask() {
    vTaskDelay(pdMS_TO_TICKS(6000));
    SyncPlaylistFromGitHub(true);

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10 * 60 * 1000));
        SyncPlaylistFromGitHub(false);
    }
}

std::vector<MusicTrack> MusicPlayer::GetTracks() const {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    return tracks_;
}

const MusicTrack* MusicPlayer::GetTrack(const std::string& id) const {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    for (const auto& t : tracks_) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

int MusicPlayer::GetTrackCount() const {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    return (int)tracks_.size();
}

std::vector<std::string> MusicPlayer::GetFolders() const {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    std::vector<std::string> folders;
    std::set<std::string> seen;
    for (const auto& t : tracks_) {
        if (!t.folder.empty() && seen.insert(t.folder).second) {
            folders.push_back(t.folder);
        }
    }
    return folders;
}

std::vector<MusicTrack> MusicPlayer::GetTracksInFolder(const std::string& folder) const {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    std::string norm_target = RemoveVietnameseDiacritics(folder);
    std::vector<MusicTrack> result;
    for (const auto& t : tracks_) {
        std::string norm_folder = RemoveVietnameseDiacritics(t.folder);
        if (norm_folder == norm_target || norm_folder.find(norm_target) != std::string::npos ||
            norm_target.find(norm_folder) != std::string::npos) {
            result.push_back(t);
        }
    }
    return result;
}

std::string MusicPlayer::GetFolderListSpeechText() {
    auto folders = GetFolders();
    if (folders.empty()) {
        return "Hiện chưa tải được danh mục nhạc từ GitHub. Vui lòng thử lại sau giây lát.";
    }

    dialog_state_ = MusicDialogState::kWaitingFolderSelection;
    active_folder_.clear();

    std::string text = "Kho nhạc GitHub có " + std::to_string(folders.size()) + " thư mục: ";
    for (size_t i = 0; i < folders.size(); i++) {
        text += "Số " + std::to_string(i + 1) + " là " + folders[i] + ". ";
    }
    text += "Bạn muốn nghe thư mục nào?";
    return text;
}

std::string MusicPlayer::GetFolderTracksSpeechText(const std::string& folder, int max_tracks) {
    auto folder_tracks = GetTracksInFolder(folder);
    if (folder_tracks.empty()) {
        return "Không tìm thấy bài hát nào trong thư mục " + folder + ".";
    }

    dialog_state_ = MusicDialogState::kWaitingSongSelection;
    active_folder_ = folder;

    std::string text = "Thư mục " + folder + " có " + std::to_string(folder_tracks.size()) + " bài. Ví dụ: ";
    int limit = std::min((int)folder_tracks.size(), max_tracks);
    for (int i = 0; i < limit; i++) {
        text += "Số " + std::to_string(i + 1) + " là " + folder_tracks[i].name + ", ";
    }
    text += "Bạn muốn nghe bài gì?";
    return text;
}

std::string MusicPlayer::GetTrackListSpeechText(int max_tracks) const {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    if (tracks_.empty()) {
        return "Hiện chưa tải được danh sách nhạc từ GitHub. Vui lòng thử lại sau giây lát.";
    }

    std::string text = "Tôi có " + std::to_string(tracks_.size()) + " bài trên GitHub. Ví dụ: ";
    int limit = std::min((int)tracks_.size(), max_tracks);
    for (int i = 0; i < limit; i++) {
        text += "Số " + std::to_string(i + 1) + " là " + tracks_[i].name + ", ";
    }
    text += "Bạn muốn nghe bài nào?";
    return text;
}

void MusicPlayer::SetStateChangeCallback(std::function<void(bool playing, const std::string& track_name)> callback) {
    state_callback_ = callback;
}

std::string MusicPlayer::MatchFolder(const std::string& query) const {
    auto folders = GetFolders();
    if (folders.empty()) return "";

    std::string q = RemoveVietnameseDiacritics(query);

    // 1. So khớp theo số thứ tự (ví dụ: "1", "so 1", "thu muc 1", "muc 1")
    for (size_t i = 0; i < folders.size(); i++) {
        std::string num = std::to_string(i + 1);
        if (q == num || q == "so " + num || q == "thu muc " + num || q == "muc " + num ||
            q.find("thu muc " + num) != std::string::npos || q.find("so " + num) != std::string::npos ||
            q.find("muc " + num) != std::string::npos) {
            return folders[i];
        }
    }

    // 2. So khớp theo tên thư mục (không dấu)
    for (const auto& f : folders) {
        std::string f_norm = RemoveVietnameseDiacritics(f);
        if (q == f_norm || q.find(f_norm) != std::string::npos || f_norm.find(q) != std::string::npos) {
            return f;
        }
    }

    return "";
}

std::string MusicPlayer::HandleDialogInput(const std::string& user_text, bool& out_action_taken) {
    out_action_taken = false;
    std::string text_norm = RemoveVietnameseDiacritics(user_text);

    // 1. Kiểm tra lệnh hủy/dừng hội thoại
    if (text_norm == "huy" || text_norm == "thoi" || text_norm == "khong" ||
        text_norm.find("huy bo") != std::string::npos || text_norm.find("dung lai") != std::string::npos ||
        text_norm.find("khong nghe") != std::string::npos || text_norm.find("tat") != std::string::npos) {
        ResetDialog();
        out_action_taken = true;
        return "Đã hủy chọn bài hát từ GitHub.";
    }

    // 2. Trạng thái: Chờ người dùng chọn thư mục
    if (dialog_state_ == MusicDialogState::kWaitingFolderSelection) {
        std::string matched_folder = MatchFolder(user_text);
        if (!matched_folder.empty()) {
            active_folder_ = matched_folder;
            dialog_state_ = MusicDialogState::kWaitingSongSelection;
            out_action_taken = true;
            return GetFolderTracksSpeechText(matched_folder, 5);
        } else {
            return "Tôi chưa tìm thấy thư mục bạn vừa nói. " + GetFolderListSpeechText();
        }
    }

    // 3. Trạng thái: Chờ người dùng chọn bài hát trong thư mục
    if (dialog_state_ == MusicDialogState::kWaitingSongSelection) {
        if (!active_folder_.empty()) {
            if (PlayInFolder(active_folder_, user_text)) {
                std::string track_name = GetCurrentTrackName();
                ResetDialog();
                out_action_taken = true;
                return "Đang phát bài " + track_name + " trong thư mục " + active_folder_ + ".";
            } else {
                return "Không tìm thấy bài hát này trong thư mục " + active_folder_ + ". Bạn vui lòng chọn theo số hoặc tên bài khác.";
            }
        }
    }

    return "";
}

bool MusicPlayer::PlayInFolder(const std::string& folder, const std::string& query) {
    auto folder_tracks = GetTracksInFolder(folder);
    if (folder_tracks.empty()) {
        ESP_LOGW(TAG, "No tracks in folder '%s'", folder.c_str());
        return false;
    }

    std::string q = RemoveVietnameseDiacritics(query);
    std::string selected_id;

    // 1. Kiểm tra theo số thứ tự trong thư mục (1, 2, 3...)
    for (size_t i = 0; i < folder_tracks.size(); i++) {
        std::string num_str = std::to_string(i + 1);
        if (q == num_str || q == "so " + num_str || q == "bai " + num_str || q == "bai so " + num_str) {
            selected_id = folder_tracks[i].id;
            break;
        }
    }

    // 2. Kiểm tra theo tên bài
    if (selected_id.empty()) {
        for (const auto& t : folder_tracks) {
            std::string t_name = RemoveVietnameseDiacritics(t.name);
            if (t_name.find(q) != std::string::npos || q.find(t_name) != std::string::npos) {
                selected_id = t.id;
                break;
            }
        }
    }

    // 3. Nếu nói chung chung -> phát bài đầu tiên của thư mục
    if (selected_id.empty()) {
        selected_id = folder_tracks[0].id;
    }

    return PlayById(selected_id);
}

bool MusicPlayer::PlayByName(const std::string& query) {
    std::string q = RemoveVietnameseDiacritics(query);
    ESP_LOGI(TAG, "Searching track for query: '%s' (cleaned: '%s')", query.c_str(), q.c_str());

    std::string selected_id;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        if (tracks_.empty()) {
            ESP_LOGW(TAG, "Track list is empty, cannot play");
            return false;
        }

        // 1. Kiểm tra số thứ tự
        for (size_t i = 0; i < tracks_.size(); i++) {
            int idx = (int)i + 1;
            std::string num_str = std::to_string(idx);
            std::string tag_exact = "so " + num_str;
            std::string tag_exact2 = "bai " + num_str;
            std::string tag_exact3 = "bai so " + num_str;

            if (q == num_str || q.find(tag_exact) != std::string::npos ||
                q.find(tag_exact2) != std::string::npos || q.find(tag_exact3) != std::string::npos) {
                selected_id = tracks_[i].id;
                break;
            }
        }

        // 2. Tìm kiếm theo tên bài hoặc ca sĩ (chuỗi không dấu)
        if (selected_id.empty()) {
            for (const auto& t : tracks_) {
                std::string t_name = RemoveVietnameseDiacritics(t.name);
                // Khớp chính xác hoàn toàn (ví dụ: "anh", "bay", "bien tinh", "cat bui cuoc doi")
                if (q == t_name) {
                    selected_id = t.id;
                    break;
                }
                // Khớp chuỗi con khi tên bài hoặc từ khóa đủ dài (>= 6 ký tự)
                // Ví dụ: user nói "mở bài áo mới cà mau của phi nhung" -> t_name "ao moi ca mau" (14 ký tự) nằm trong q -> KHỚP!
                // Ví dụ: user nói "tình đơn phương" -> t_name "tinh don phuong 1" (18 ký tự) chứa q -> KHỚP!
                // Tránh bắt nhầm các từ 1-2 chữ như "anh", "bay", "toi", "lanh" khi q là câu tìm bài hát dài khác.
                if (t_name.length() >= 6 && q.find(t_name) != std::string::npos) {
                    selected_id = t.id;
                    break;
                }
                if (q.length() >= 6 && t_name.find(q) != std::string::npos) {
                    selected_id = t.id;
                    break;
                }
            }
        }

        // 3. Tìm theo tên thư mục (chỉ khi truy vấn chỉ rõ tên thư mục cụ thể)
        if (selected_id.empty()) {
            for (const auto& t : tracks_) {
                std::string f_clean = RemoveVietnameseDiacritics(t.folder);
                if (!f_clean.empty() && f_clean != "songs" && f_clean.length() >= 5 &&
                    (q == f_clean || q.find(f_clean) != std::string::npos)) {
                    selected_id = t.id;
                    break;
                }
            }
        }

        // 4. Nếu nói chung chung không chỉ rõ bài: "mở nhạc", "bật nhạc", "phát nhạc", "nghe nhạc" -> phát bài đầu tiên
        if (selected_id.empty()) {
            if (q == "mo nhac" || q == "bat nhac" || q == "phat nhac" || q == "nghe nhac" ||
                q == "nghe nhac di" || q == "mo nhac di" || q == "bat nhac di" || q == "phat nhac di") {
                selected_id = tracks_[0].id;
            }
        }
    }

    if (selected_id.empty()) {
        ESP_LOGI(TAG, "No local/GitHub track matched '%s', delegating to Online Music", query.c_str());
        return false;
    }

    return PlayById(selected_id);
}

bool MusicPlayer::PlayById(const std::string& track_id) {
    std::string track_name;
    std::string track_url;

    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        bool found = false;
        for (const auto& t : tracks_) {
            if (t.id == track_id) {
                track_name = t.name;
                track_url = t.stream_url.empty() ? BuildRawUrl(t.path) : t.stream_url;
                found = true;
                break;
            }
        }
        if (!found) {
            ESP_LOGE(TAG, "Track ID '%s' not found", track_id.c_str());
            return false;
        }
    }

    if (is_running_ && current_track_id_ == track_id) {
        ESP_LOGI(TAG, "Track '%s' (%s) is already running/starting, skipping duplicate trigger",
                 track_id.c_str(), track_name.c_str());
        return true;
    }

    // Dừng nhạc online hoặc radio nếu đang phát
    if (OnlineMusicPlayer::GetInstance().IsPlaying()) {
        OnlineMusicPlayer::GetInstance().Stop();
    }
    if (RadioPlayer::GetInstance().IsPlaying()) {
        RadioPlayer::GetInstance().Stop();
    }

    // Dừng bài nhạc trước nếu đang phát
    Stop();
    vTaskDelay(pdMS_TO_TICKS(40));

    uint32_t session = ++session_id_;
    current_track_id_ = track_id;
    current_track_name_ = track_name;
    current_track_url_ = track_url;
    is_running_ = true;
    is_playing_ = false;

    // Tạo ring buffer trong SPIRAM một lần và tái sử dụng an toàn
    if (ring_buffer_ == nullptr) {
        ring_buffer_ = xRingbufferCreateWithCaps(MUSIC_RING_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ring_buffer_ == nullptr) {
            ESP_LOGW(TAG, "Could not allocate SPIRAM ring buffer, trying internal RAM");
            ring_buffer_ = xRingbufferCreate(32 * 1024, RINGBUF_TYPE_BYTEBUF);
        }
    }

    if (ring_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create ring buffer for music player!");
        is_running_ = false;
        is_playing_ = false;
        current_track_id_.clear();
        current_track_name_.clear();
        return false;
    }

    // Xóa sạch dữ liệu cũ trong ring buffer
    size_t item_size = 0;
    while (true) {
        void* item = xRingbufferReceiveUpTo(ring_buffer_, &item_size, 0, 8192);
        if (!item) break;
        vRingbufferReturnItem(ring_buffer_, item);
    }

    esp_wifi_set_ps(WIFI_PS_NONE);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec && !codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    ESP_LOGI(TAG, "Starting track (session %u): %s (%s)", (unsigned)session, track_name.c_str(), track_url.c_str());

    struct TaskParam {
        MusicPlayer* self;
        uint32_t session;
    };
    TaskParam* param_decode = new TaskParam{this, session};
    xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        auto* p = static_cast<TaskParam*>(arg);
        p->self->DecodeTask(p->session);
        delete p;
        vTaskDelete(NULL);
    }, "music_decode", 16384, param_decode, 5, &decode_task_handle_, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    TaskParam* param_stream = new TaskParam{this, session};
    xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        auto* p = static_cast<TaskParam*>(arg);
        p->self->StreamTask(p->session);
        delete p;
        vTaskDelete(NULL);
    }, "music_stream", 16384, param_stream, 4, &stream_task_handle_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (state_callback_) {
        state_callback_(true, track_name);
    }

    return true;
}

void MusicPlayer::Stop() {
    if (!is_running_ && !is_playing_) {
        return;
    }

    ESP_LOGI(TAG, "Stopping music playback (session %u)...", session_id_.load());
    is_running_ = false;
    is_playing_ = false;
    DottedSpectrumVisualizer::GetInstance().Stop();
    current_track_id_.clear();
    current_track_name_.clear();
    ++session_id_;

    int wait_cycles = 50;
    while ((stream_task_handle_ != nullptr || decode_task_handle_ != nullptr) && wait_cycles-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    // Không dùng vTaskDelete ép buộc khi task đang giữa kết nối TLS/HTTP để tránh hỏng heap/mutex
    if (stream_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "StreamTask still finishing TLS/HTTP, detaching handle to exit cleanly");
        stream_task_handle_ = nullptr;
    }
    if (decode_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "DecodeTask still finishing, detaching handle to exit cleanly");
        decode_task_handle_ = nullptr;
    }

    if (ring_buffer_) {
        size_t item_size = 0;
        while (true) {
            void* item = xRingbufferReceiveUpTo(ring_buffer_, &item_size, 0, 8192);
            if (!item) break;
            vRingbufferReturnItem(ring_buffer_, item);
        }
    }

    if (state_callback_) {
        state_callback_(false, "");
    }

    ESP_LOGI(TAG, "Music playback stopped successfully");
}

void MusicPlayer::StreamTask(uint32_t session) {
    std::string url = current_track_url_;
    if (url.empty() || session != session_id_.load()) {
        if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
            stream_task_handle_ = nullptr;
        }
        return;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 4000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for stream");
        if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
            stream_task_handle_ = nullptr;
        }
        return;
    }
    http_client_ = client;

    esp_http_client_set_header(client, "User-Agent", "DUONGBOT-Music/1.0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK || !is_running_ || session != session_id_.load()) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        http_client_ = nullptr;
        if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
            stream_task_handle_ = nullptr;
        }
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Stream connected (status %d, length %d)", status_code, content_length);

    if (status_code != 200 && status_code != 206) {
        ESP_LOGE(TAG, "Stream HTTP error: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        http_client_ = nullptr;
        if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
            stream_task_handle_ = nullptr;
        }
        return;
    }

    char chunk[2048];
    while (is_running_ && session == session_id_.load()) {
        int read_len = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read_len < 0) {
            ESP_LOGW(TAG, "HTTP read error or stream completed");
            break;
        }
        if (read_len == 0) {
            ESP_LOGI(TAG, "End of HTTP stream reached");
            break;
        }

        size_t written = 0;
        while (written < (size_t)read_len && is_running_ && session == session_id_.load()) {
            if (!ring_buffer_) break;
            size_t to_write = (size_t)read_len - written;
            BaseType_t ret = xRingbufferSend(ring_buffer_, chunk + written, to_write, pdMS_TO_TICKS(100));
            if (ret == pdTRUE) {
                written += to_write;
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    http_client_ = nullptr;
    if (stream_task_handle_ == xTaskGetCurrentTaskHandle()) {
        stream_task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "Stream task finished");
}

void MusicPlayer::DecodeTask(uint32_t session) {
    void* decoder = nullptr;
    esp_audio_err_t err = esp_mp3_dec_open(nullptr, 0, &decoder);
    if (err != ESP_AUDIO_ERR_OK || !decoder) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", err);
        decode_task_handle_ = nullptr;
        return;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    int output_rate = codec ? codec->output_sample_rate() : 24000;
    if (output_rate <= 0) output_rate = 24000;

    void* resampler = nullptr;
    int resampler_in_rate = 0;

    // Chờ đệm ban đầu (pre-buffering)
    int wait_buf_count = 0;
    while (is_running_ && session == session_id_.load() && wait_buf_count++ < 150) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!ring_buffer_) break;
        size_t free_sz = xRingbufferGetCurFreeSize(ring_buffer_);
        size_t buffered = MUSIC_RING_BUFFER_SIZE - free_sz;
        if (buffered >= MUSIC_PREBUFFER_MIN) {
            ESP_LOGI(TAG, "Pre-buffering complete (%u bytes buffered)", (unsigned)buffered);
            break;
        }
    }

    std::vector<uint8_t> in_buffer;
    in_buffer.reserve(8192);
    std::unique_ptr<int16_t[]> out_pcm_raw(new int16_t[4096]);

    while (is_running_ && session == session_id_.load()) {
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
            if (stream_task_handle_ == nullptr) {
                ESP_LOGI(TAG, "Playback completed naturally");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Bỏ qua ID3 header nếu có
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

        if (in_buffer.size() < 1440 && stream_task_handle_ != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

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

        esp_audio_err_t dec_ret = esp_mp3_dec_decode(decoder, &raw, &frame, &info);

        if (dec_ret == ESP_AUDIO_ERR_OK && raw.consumed > 0 && frame.decoded_size > 0) {
            in_buffer.erase(in_buffer.begin(), in_buffer.begin() + raw.consumed);

            if (codec && !codec->output_enabled()) {
                codec->EnableOutput(true);
            }

            if (!is_playing_ && is_running_ && session == session_id_.load()) {
                is_playing_ = true;
                DottedSpectrumVisualizer::GetInstance().Start(current_track_name_);
                if (state_callback_) {
                    state_callback_(true, current_track_name_);
                }
            }

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
                        .complexity = 2,
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
            if (in_buffer.size() >= 1440) {
                size_t sync_idx = 1;
                while (sync_idx + 1 < in_buffer.size()) {
                    if (in_buffer[sync_idx] == 0xFF && (in_buffer[sync_idx + 1] & 0xE6) == 0xE2) {
                        break;
                    }
                    sync_idx++;
                }
                in_buffer.erase(in_buffer.begin(), in_buffer.begin() + sync_idx);
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    if (resampler) {
        esp_ae_rate_cvt_close(resampler);
        resampler = nullptr;
    }
    if (decoder) {
        esp_mp3_dec_close(decoder);
        decoder = nullptr;
    }

    if (decode_task_handle_ == xTaskGetCurrentTaskHandle()) {
        decode_task_handle_ = nullptr;
    }
    if (session == session_id_.load() && is_running_) {
        is_running_ = false;
        is_playing_ = false;
        DottedSpectrumVisualizer::GetInstance().Stop();
        if (state_callback_) {
            state_callback_(false, "");
        }
    }
    ESP_LOGI(TAG, "Decode task finished");
}
