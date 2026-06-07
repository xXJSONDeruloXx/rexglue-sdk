#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>
#include <rex/thread/mutex.h>

namespace rex {
namespace kernel {
namespace xam {
namespace apps {

// Only source of docs for a lot of these functions:
// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Scenes/Media/Music/ScnMusic.cpp

class XmpApp : public system::xam::App {
 public:
  enum class State : uint32_t {
    kIdle = 0,
    kPlaying = 1,
    kPaused = 2,
  };
  enum class PlaybackClient : uint32_t {
    kSystem = 0,
    kTitle = 1,
  };
  enum class PlaybackMode : uint32_t {
    // kInOrder = ?,
    kUnknown = 0,
  };
  enum class RepeatMode : uint32_t {
    // kNoRepeat = ?,
    kUnknown = 0,
  };
  struct Song {
    enum class Format : uint32_t {
      kWma = 0,
      kMp3 = 1,
    };

    uint32_t handle;
    std::u16string file_path;
    std::u16string name;
    std::u16string artist;
    std::u16string album;
    std::u16string album_artist;
    std::u16string genre;
    uint32_t track_number;
    uint32_t duration_ms;
    Format format;
  };
  struct Playlist {
    uint32_t handle;
    uint32_t storage_ptr;
    std::u16string name;
    uint32_t flags;
    std::vector<std::unique_ptr<Song>> songs;
  };

  explicit XmpApp(system::KernelState* kernel_state);

  X_HRESULT XMPGetStatus(uint32_t status_ptr);

  X_HRESULT XMPCreateTitlePlaylist(uint32_t storage_ptr, uint32_t songs_ptr, uint32_t song_count,
                                   uint32_t playlist_name_ptr, const std::u16string& playlist_name,
                                   uint32_t flags, uint32_t out_song_handles,
                                   uint32_t out_playlist_handle);
  X_HRESULT XMPDeleteTitlePlaylist(uint32_t playlist_handle);
  X_HRESULT XMPPlayTitlePlaylist(uint32_t playlist_handle, uint32_t song_handle);
  X_HRESULT XMPContinue();
  X_HRESULT XMPStop(uint32_t unk);
  X_HRESULT XMPPause();
  X_HRESULT XMPNext();
  X_HRESULT XMPPrevious();

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  static const uint32_t kMsgStateChanged = 0x0A000001;
  static const uint32_t kMsgPlaybackBehaviorChanged = 0x0A000002;
  static const uint32_t kMsgPlaybackControllerChanged = 0x0A000003;

  // Auto-discovery of eatrax/ directory and fallback for garbage handles
  void AutoDiscoverTitleMusic();
  bool TryAutoDiscoverAndPlay(uint32_t playlist_handle, uint32_t song_handle);
  Playlist* GetOrCreateDefaultPlaylist();

  // Audio playback pipeline
  void StartPlayback(Playlist* playlist, int song_index);
  void StopPlayback();
  void PlaybackThreadMain();

  void OnStateChanged();
  Playlist* LookupPlaylistByStoragePtr(uint32_t storage_ptr) const;

  // AVFormatContext* for the currently playing song (opaque to avoid C include in header).
  // Protected by playback_mutex_ for close — both StopPlayback and the playback thread
  // may try to close it; the mutex ensures only one wins.
  void* playback_fmt_ctx_ = nullptr;

  State state_;
  PlaybackClient playback_client_;
  PlaybackMode playback_mode_;
  RepeatMode repeat_mode_;
  uint32_t unknown_flags_;
  float volume_;
  Playlist* active_playlist_;
  int active_song_index_;

  // Playback thread state
  std::unique_ptr<std::thread> playback_thread_;
  std::atomic<bool> playback_running_ = {false};
  std::atomic<bool> playback_paused_ = {false};
  std::mutex playback_mutex_;
  std::condition_variable playback_cv_;
  Playlist* current_playlist_ = nullptr;
  int current_song_index_ = 0;
  bool auto_discovered_ = false;
  Playlist* auto_playlist_ = nullptr;

  rex::thread::global_critical_region global_critical_region_;
  std::unordered_map<uint32_t, Playlist*> playlists_;
  std::unordered_map<uint32_t, Playlist*> playlists_by_storage_ptr_;
  uint32_t next_playlist_handle_;
  uint32_t next_song_handle_;
  uint32_t xmp_client_id_;
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
