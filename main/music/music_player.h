#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_http_client.h>

struct MusicTrack {
    std::string id;
    std::string name;
    std::string category;
    std::string path;
    std::string folder; // Thư mục trên GitHub (bolero, truong vu, truyen ma, truyen thieu nhi...)
    std::vector<std::string> tags;
    std::string stream_url;
};

enum class MusicDialogState {
    kIdle,
    kWaitingFolderSelection,
    kWaitingSongSelection
};

class MusicPlayer {
public:
    static MusicPlayer& GetInstance();

    // Khởi tạo và kích hoạt tác vụ đồng bộ nền
    void Initialize();

    // Đồng bộ danh sách từ GitHub (trả về số bài tìm thấy)
    int SyncPlaylistFromGitHub(bool force = false);
    void TriggerSync();

    // Phát nhạc theo ID bài hát
    bool PlayById(const std::string& track_id);

    // Phát nhạc theo tên, số thứ tự (1, 2, 3...) hoặc từ khóa tìm kiếm
    bool PlayByName(const std::string& query);

    // Phát nhạc trong một thư mục cụ thể (theo tên bài hoặc số thứ tự 1, 2, 3...)
    bool PlayInFolder(const std::string& folder, const std::string& query);

    // Dừng phát nhạc
    void Stop();

    // Trạng thái phát
    bool IsPlaying() const { return is_playing_ || is_running_; }
    std::string GetCurrentTrackId() const { return current_track_id_; }
    std::string GetCurrentTrackName() const { return current_track_name_; }

    // Quản lý danh sách thư mục và bài hát
    std::vector<MusicTrack> GetTracks() const;
    const MusicTrack* GetTrack(const std::string& id) const;
    int GetTrackCount() const;

    std::vector<std::string> GetFolders() const;
    std::vector<MusicTrack> GetTracksInFolder(const std::string& folder) const;

    // Chuỗi thoại để robot đọc danh sách thư mục và hỏi người dùng
    std::string GetFolderListSpeechText();

    // Chuỗi thoại để robot đọc các bài trong thư mục và hỏi muốn nghe bài nào
    std::string GetFolderTracksSpeechText(const std::string& folder, int max_tracks = 5);

    // Chuỗi giới thiệu chung các bài hát
    std::string GetTrackListSpeechText(int max_tracks = 5) const;

    // Quản lý trạng thái hội thoại tương tác 2 bước (Hỏi thư mục -> Hỏi bài)
    MusicDialogState GetDialogState() const { return dialog_state_; }
    void SetDialogState(MusicDialogState state) { dialog_state_ = state; }
    std::string GetActiveFolder() const { return active_folder_; }
    void SetActiveFolder(const std::string& folder) { active_folder_ = folder; }
    void ResetDialog() {
        dialog_state_ = MusicDialogState::kIdle;
        active_folder_.clear();
    }

    std::string MatchFolder(const std::string& query) const;
    std::string HandleDialogInput(const std::string& user_text, bool& out_action_taken);

    // Callback khi trạng thái phát thay đổi
    void SetStateChangeCallback(std::function<void(bool playing, const std::string& track_name)> callback);

    static std::string RemoveVietnameseDiacritics(const std::string& str);

private:
    MusicPlayer();
    ~MusicPlayer();
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    mutable std::mutex tracks_mutex_;
    std::vector<MusicTrack> tracks_;

    volatile bool is_playing_ = false;
    volatile bool is_running_ = false;
    std::string current_track_id_;
    std::string current_track_name_;
    std::string current_track_url_;
    std::atomic<uint32_t> session_id_{0};

    MusicDialogState dialog_state_ = MusicDialogState::kIdle;
    std::string active_folder_;

    std::string last_etag_;
    uint32_t last_sync_timestamp_ = 0;

    TaskHandle_t stream_task_handle_ = nullptr;
    TaskHandle_t decode_task_handle_ = nullptr;
    TaskHandle_t sync_task_handle_ = nullptr;
    RingbufHandle_t ring_buffer_ = nullptr;
    esp_http_client_handle_t http_client_ = nullptr;

    std::function<void(bool playing, const std::string& track_name)> state_callback_;

    void StreamTask(uint32_t session);
    void DecodeTask(uint32_t session);
    void SyncTask();

    std::string BuildRawUrl(const std::string& relative_path) const;
};

#endif // MUSIC_PLAYER_H
