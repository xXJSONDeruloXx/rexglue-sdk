/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026-06-06 - Added auto-discovery of eatrax/ directory,
 *                           fallback for garbage playlist handles,
 *                           and actual WMA audio playback via FFmpeg.
 */

#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <algorithm>
#include <cctype>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/error.h"
}  // extern "C"

#include <rex/audio/audio_system.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
using rex::memory::kSystemHeapPhysical;
namespace apps {
using namespace rex::system;

XmpApp::XmpApp(KernelState* kernel_state)
    : App(kernel_state, 0xFA),
      state_(State::kIdle),
      playback_client_(PlaybackClient::kTitle),
      playback_mode_(PlaybackMode::kUnknown),
      repeat_mode_(RepeatMode::kUnknown),
      unknown_flags_(0),
      volume_(1.0f),
      active_playlist_(nullptr),
      active_song_index_(0),
      next_playlist_handle_(1),
      next_song_handle_(1),
      xmp_client_id_(0) {}

X_HRESULT XmpApp::XMPGetStatus(uint32_t state_ptr) {
  if (!XThread::GetCurrentThread()->main_thread()) {
    // Some stupid games will hammer this on a thread - induce a delay
    // here to keep from starving real threads.
    rex::thread::Sleep(std::chrono::milliseconds(1));
  }

  REXKRNL_TRACE("XMPGetStatus({:08X})", state_ptr);
  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(state_ptr),
                                   static_cast<uint32_t>(state_));
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPCreateTitlePlaylist(uint32_t storage_ptr, uint32_t songs_ptr,
                                         uint32_t song_count, uint32_t playlist_name_ptr,
                                         const std::u16string& playlist_name, uint32_t flags,
                                         uint32_t out_song_handles, uint32_t out_playlist_handle) {
  REXKRNL_WARN(
      "XMPCreateTitlePlaylist storage={:08X} songs_ptr={:08X} song_count={} name='{}' flags={:08X} song_handles={:08X} out_playlist={:08X}",
      storage_ptr, songs_ptr, song_count, rex::string::to_utf8(playlist_name), flags,
      out_song_handles, out_playlist_handle);
  auto playlist = std::make_unique<Playlist>();
  playlist->handle = ++next_playlist_handle_;
  playlist->storage_ptr = storage_ptr;
  playlist->name = playlist_name;
  playlist->flags = flags;
  if (songs_ptr) {
    for (uint32_t i = 0; i < song_count; ++i) {
      auto song = std::make_unique<Song>();
      song->handle = ++next_song_handle_;
      uint8_t* song_base = memory_->TranslateVirtual(songs_ptr + (i * 36));
      song->file_path = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 0)));
      song->name = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 4)));
      song->artist = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 8)));
      song->album = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 12)));
      song->album_artist = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 16)));
      song->genre = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 20)));
      song->track_number = memory::load_and_swap<uint32_t>(song_base + 24);
      song->duration_ms = memory::load_and_swap<uint32_t>(song_base + 28);
      song->format = static_cast<Song::Format>(memory::load_and_swap<uint32_t>(song_base + 32));
      if (out_song_handles) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(out_song_handles + (i * 4)),
                                         song->handle);
      }
      if (i < 4) {
        REXKRNL_WARN("  song[{}] handle={:08X} format={} path='{}' name='{}' artist='{}' duration_ms={}",
                     i, song->handle, static_cast<uint32_t>(song->format),
                     rex::string::to_utf8(song->file_path), rex::string::to_utf8(song->name),
                     rex::string::to_utf8(song->artist), song->duration_ms);
      }
      playlist->songs.emplace_back(std::move(song));
    }
  }
  if (out_playlist_handle) {
    memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(out_playlist_handle),
                                     playlist->handle);
  }

  auto global_lock = global_critical_region_.Acquire();
  playlists_.insert({playlist->handle, playlist.get()});
  if (storage_ptr) {
    playlists_by_storage_ptr_.insert({storage_ptr, playlist.get()});
  }
  playlist.release();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPDeleteTitlePlaylist(uint32_t playlist_handle) {
  REXKRNL_DEBUG("XMPDeleteTitlePlaylist({:08X})", playlist_handle);
  auto global_lock = global_critical_region_.Acquire();
  auto it = playlists_.find(playlist_handle);
  if (it == playlists_.end()) {
    REXKRNL_ERROR("Playlist {:08X} not found", playlist_handle);
    return X_E_NOTFOUND;
  }
  auto playlist = it->second;
  if (playlist == active_playlist_) {
    XMPStop(0);
  }
  playlists_.erase(it);
  if (playlist->storage_ptr) {
    playlists_by_storage_ptr_.erase({playlist->storage_ptr});
  }
  delete playlist;
  return X_E_SUCCESS;
}

// ---- Auto-discovery of eatrax/ directory ----

void XmpApp::AutoDiscoverTitleMusic() {
  if (auto_discovered_) {
    return;
  }
  auto_discovered_ = true;

  auto* vfs = kernel_state_->file_system();
  if (!vfs) {
    REXKRNL_WARN("XMP: No VFS available for auto-discovery");
    return;
  }

  // Try multiple possible paths for the eatrax directory
  std::vector<std::string> candidate_paths = {
      "\\Device\\Harddisk0\\Partition1\\eatrax\\",
      "game:\\eatrax\\",
      "d:\\eatrax\\",
  };

  for (const auto& path : candidate_paths) {
    auto* eatrax_entry = vfs->ResolvePath(path);
    if (eatrax_entry && eatrax_entry->child_count() > 0) {
      REXKRNL_INFO("XMP: Auto-discovering title music from '{}'", path);

      auto playlist = std::make_unique<Playlist>();
      playlist->handle = ++next_playlist_handle_;
      playlist->storage_ptr = 0;
      playlist->name = u"Auto-discovered title music";
      playlist->flags = 0;

      uint32_t song_count = 0;
      for (size_t i = 0; i < eatrax_entry->child_count(); i++) {
        auto* child = eatrax_entry->children()[i].get();
        if (!child) continue;

        std::string name = child->name();
        // Filter for .wma files (case insensitive)
        if (name.size() < 4) continue;
        std::string ext = name.substr(name.size() - 4);
        bool is_wma = false;
        for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (ext == ".wma" || ext == ".xma") {
          is_wma = true;
        }

        if (is_wma) {
          auto song = std::make_unique<Song>();
          song->handle = ++next_song_handle_;
          // Get the host path so FFmpeg can open the file directly
          std::string host_path_str;
          auto* host_entry = dynamic_cast<filesystem::HostPathEntry*>(child);
          if (host_entry) {
            host_path_str = host_entry->host_path().string();
          } else {
            host_path_str = name;  // Fallback to VFS name
          }
          song->file_path = std::u16string(host_path_str.begin(), host_path_str.end());
          song->name = std::u16string(name.begin(), name.end());
          song->artist = u"";
          song->album = u"";
          song->album_artist = u"";
          song->genre = u"";
          song->track_number = song_count;
          song->duration_ms = 0;  // Unknown until played
          song->format = Song::Format::kWma;
          playlist->songs.emplace_back(std::move(song));
          song_count++;
        }
      }

      if (song_count > 0) {
        REXKRNL_INFO("XMP: Auto-discovered {} songs from '{}'", song_count, path);

        auto global_lock = global_critical_region_.Acquire();
        auto_playlist_ = playlist.get();
        playlists_.insert({playlist->handle, playlist.get()});
        playlist.release();
        return;
      }
    }
  }

  REXKRNL_WARN("XMP: No eatrax/ directory found or no music files discovered");
}

Playlist* XmpApp::GetOrCreateDefaultPlaylist() {
  if (!auto_discovered_) {
    AutoDiscoverTitleMusic();
  }
  return auto_playlist_;
}

// ---- Audio playback pipeline ----

void XmpApp::StartPlayback(Playlist* playlist, int song_index) {
  // Stop any existing playback
  StopPlayback();

  if (!playlist || playlist->songs.empty()) {
    return;
  }

  // Clamp song index
  if (song_index < 0 || song_index >= static_cast<int>(playlist->songs.size())) {
    song_index = 0;
  }

  current_playlist_ = playlist;
  current_song_index_ = song_index;
  playback_running_.store(true);
  playback_paused_.store(false);

  REXKRNL_INFO("XMP: Starting playback thread for playlist handle={:08X}, song_index={}",
               playlist->handle, song_index);

  playback_thread_ = std::make_unique<std::thread>(
      [this]() { PlaybackThreadMain(); });
}

void XmpApp::StopPlayback() {
  if (!playback_thread_) {
    return;
  }

  playback_running_.store(false);
  playback_cv_.notify_all();

  if (playback_thread_->joinable()) {
    playback_thread_->join();
  }
  playback_thread_.reset();

  current_playlist_ = nullptr;
  current_song_index_ = 0;
}

void XmpApp::PlaybackThreadMain() {
  // Allocate a PCM buffer in guest memory for audio submission.
  // Xbox 360 audio frames: typically 16 bits per sample, mono or stereo.
  // We'll use a buffer large enough for ~100ms of audio at 48kHz stereo.
  constexpr size_t kPcmBufferSize = 2 * 48000 / 10 * 2 * 2;  // 19200 bytes

  uint32_t guest_pcm_buffer = memory_->SystemHeapAlloc(kPcmBufferSize, 64,
                                                        memory::kSystemHeapPhysical);
  if (!guest_pcm_buffer) {
    REXKRNL_ERROR("XMP: Failed to allocate PCM buffer in guest memory");
    return;
  }

  uint8_t* host_pcm_buffer = memory_->TranslateVirtual(guest_pcm_buffer);
  memset(host_pcm_buffer, 0, kPcmBufferSize);

  auto* audio_system =
      static_cast<audio::AudioSystem*>(kernel_state_->emulator()->audio_system());
  if (!audio_system) {
    REXKRNL_ERROR("XMP: No audio system available");
    memory_->SystemHeapFree(guest_pcm_buffer);
    return;
  }

  // Register an audio render client for XMP playback
  uint32_t driver_handle = 0;
  {
    uint8_t* callback_buf = memory_->SystemHeapAlloc(8, 4, memory::kSystemHeapPhysical);
    if (callback_buf) {
      uint32_t* cb = reinterpret_cast<uint32_t*>(callback_buf);
      cb[0] = 0;  // callback (unused)
      cb[1] = 0;  // callback_arg
      uint32_t* handle_out = reinterpret_cast<uint32_t*>(host_pcm_buffer);
      // Use XAudioRegisterRenderDriverClient through the kernel function
      // For simplicity, we use client index 0 directly (same as XAudioRegisterRenderDriverClient)
      size_t client_index = 0;
      audio_system->RegisterClient(0, 0, &client_index);
      driver_handle = 0x41550000 | static_cast<uint32_t>(client_index & 0x0000FFFF);
      memory_->SystemHeapFree(reinterpret_cast<uint32_t>(callback_buf));
    }
  }

  REXKRNL_INFO("XMP: Playback thread started, driver={:08X}", driver_handle);

  while (playback_running_.load()) {
    // Get current song
    Playlist* playlist = current_playlist_;
    int song_index = current_song_index_;

    if (!playlist || playlist->songs.empty()) {
      break;
    }

    if (song_index < 0 || song_index >= static_cast<int>(playlist->songs.size())) {
      break;
    }

    auto& song = playlist->songs[song_index];
    // file_path is stored as the host filesystem path (std::u16string but contains host path)
    std::string host_path = std::string(song->file_path.begin(), song->file_path.end());
    REXKRNL_INFO("XMP: Playing song '{}' ({})", song->name.data(), host_path);

    // Open the file using FFmpeg
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, host_path.c_str(), nullptr, nullptr) != 0) {
      REXKRNL_WARN("XMP: Failed to open file '{}' for playback", host_path);
      break;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
      REXKRNL_WARN("XMP: Failed to find stream info for '{}'", host_path);
      avformat_close_input(&fmt_ctx);
      break;
    }

    // Find the audio stream
    int audio_stream_index = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
      if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        audio_stream_index = i;
        break;
      }
    }

    if (audio_stream_index < 0) {
      REXKRNL_WARN("XMP: No audio stream found in '{}'", host_path);
      avformat_close_input(&fmt_ctx);
      break;
    }

    // Open the decoder
    AVCodecParameters* codec_params = fmt_ctx->streams[audio_stream_index]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
      REXKRNL_WARN("XMP: No decoder found for codec {} in '{}'", codec_params->codec_id,
                    host_path);
      avformat_close_input(&fmt_ctx);
      break;
    }

    std::unique_ptr<AVCodecContext, decltype(&avcodec_free_context)> codec_ctx(
        avcodec_alloc_context3(codec), avcodec_free_context);
    if (!codec_ctx) {
      REXKRNL_WARN("XMP: Failed to allocate codec context for '{}'", host_path);
      avformat_close_input(&fmt_ctx);
      break;
    }

    if (avcodec_parameters_to_context(codec_ctx.get(), codec_params) < 0) {
      REXKRNL_WARN("XMP: Failed to copy codec params for '{}'", host_path);
      avformat_close_input(&fmt_ctx);
      break;
    }

    if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
      REXKRNL_WARN("XMP: Failed to open codec for '{}'", host_path);
      avformat_close_input(&fmt_ctx);
      break;
    }

    REXKRNL_INFO("XMP: Decoding '{}' (codec={}, sample_rate={}, channels={})",
                 host_path, codec->name, codec_ctx->sample_rate, codec_ctx->channels);

    std::unique_ptr<AVPacket, decltype(&av_packet_free)> packet(av_packet_alloc(), av_packet_free);
    std::unique_ptr<AVFrame, decltype(&av_frame_free)> frame(av_frame_alloc(), av_frame_free);

    // Decode loop
    while (playback_running_.load()) {
      // Check for pause
      while (playback_paused_.load() && playback_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }

      if (av_read_frame(fmt_ctx, packet.get()) < 0) {
        // End of file or error
        break;
      }

      if (packet->stream_index != audio_stream_index) {
        continue;
      }

      int ret = avcodec_send_packet(codec_ctx.get(), packet.get());
      if (ret < 0) {
        continue;
      }

      ret = avcodec_receive_frame(codec_ctx.get(), frame.get());
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        continue;
      }
      if (ret < 0) {
        REXKRNL_WARN("XMP: Decode error for '{}'", host_path);
        continue;
      }

      // Convert to int16 PCM
      int samples = frame->nb_samples;
      int channels = codec_ctx->channels;
      size_t pcm_size = samples * channels * 2;

      if (pcm_size > kPcmBufferSize) {
        // Buffer too small — truncate to fit
        samples = static_cast<int>(kPcmBufferSize / (channels * 2));
      }

      // Simple conversion: assume float32 input, convert to int16
      if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLT) {
        for (int ch = 0; ch < channels; ch++) {
          const float* in = reinterpret_cast<const float*>(frame->data[ch]);
          int16_t* out = reinterpret_cast<int16_t*>(host_pcm_buffer) + ch;
          for (int i = 0; i < samples; i++) {
            float val = in[i] * 32767.0f;
            val = std::max(-32768.0f, std::min(32767.0f, val));
            // Interleave channels
            reinterpret_cast<int16_t*>(host_pcm_buffer)[i * channels + ch] =
                static_cast<int16_t>(val);
          }
        }
      } else if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        // Planar float
        for (int ch = 0; ch < channels; ch++) {
          const float* in = reinterpret_cast<const float*>(frame->data[ch]);
          for (int i = 0; i < samples; i++) {
            float val = in[i] * 32767.0f;
            val = std::max(-32768.0f, std::min(32767.0f, val));
            reinterpret_cast<int16_t*>(host_pcm_buffer)[i * channels + ch] =
                static_cast<int16_t>(val);
          }
        }
      } else {
        // Fallback: try direct copy (for int16 formats)
        // This is a best-effort approach
        REXKRNL_WARN("XMP: Unsupported sample format {}, skipping frame", codec_ctx->sample_fmt);
        continue;
      }

      // Submit to audio system
      audio_system->SubmitFrame(driver_handle & 0x0000FFFF, guest_pcm_buffer);

      // Pace playback: sleep proportional to frame duration
      if (frame->sample_rate > 0 && samples > 0) {
        double frame_duration_ms = (1000.0 * samples) / frame->sample_rate;
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(std::max(1.0, frame_duration_ms - 5.0))));  // -5ms for processing time
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }

    av_packet_unref(packet.get());
    avformat_close_input(&fmt_ctx);

    REXKRNL_INFO("XMP: Finished playing song '{}'", song->name.data());

    // Advance to next song if still running
    if (playback_running_.load() && playlist) {
      {
        std::lock_guard<std::mutex> lock(playback_mutex_);
        current_song_index_++;
        if (current_song_index_ >= static_cast<int>(playlist->songs.size())) {
          current_song_index_ = 0;  // Loop back to start
        }
      }
    }
  }

  // Clean up
  memory_->SystemHeapFree(guest_pcm_buffer);

  // Unregister audio client
  if (driver_handle) {
    audio_system->UnregisterClient(driver_handle & 0x0000FFFF);
  }

  REXKRNL_INFO("XMP: Playback thread exiting");
}

// ---- Playback control ----

bool XmpApp::TryAutoDiscoverAndPlay(uint32_t playlist_handle, uint32_t song_handle) {
  // Try to find the playlist first
  Playlist* playlist = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = playlists_.find(playlist_handle);
    if (it != playlists_.end()) {
      playlist = it->second;
    }
  }

  if (!playlist) {
    REXKRNL_WARN("XMP: Playlist {:08X} not found, attempting auto-discovery", playlist_handle);
    playlist = GetOrCreateDefaultPlaylist();
    if (playlist) {
      REXKRNL_INFO("XMP: Fallback to auto-discovered playlist ({} songs)",
                   playlist->songs.size());
    } else {
      REXKRNL_ERROR("XMP: Auto-discovery failed, no music available");
      return false;
    }
  }

  // Find song index from song_handle, or default to 0
  int song_index = 0;
  for (int i = 0; i < static_cast<int>(playlist->songs.size()); i++) {
    if (playlist->songs[i]->handle == song_handle) {
      song_index = i;
      break;
    }
  }

  // Start playback
  StartPlayback(playlist, song_index);
  return true;
}

X_HRESULT XmpApp::XMPPlayTitlePlaylist(uint32_t playlist_handle, uint32_t song_handle) {
  REXKRNL_DEBUG("XMPPlayTitlePlaylist({:08X}, {:08X})", playlist_handle, song_handle);

  if (playback_client_ == PlaybackClient::kSystem) {
    REXKRNL_WARN("XMPPlayTitlePlaylist: System playback is enabled; continuing with title playlist");
  }

  bool started = TryAutoDiscoverAndPlay(playlist_handle, song_handle);

  if (started) {
    active_playlist_ = current_playlist_;
    active_song_index_ = current_song_index_;
    state_ = State::kPlaying;
    OnStateChanged();
    kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 1);
    return X_E_SUCCESS;
  }

  REXKRNL_ERROR("Playlist {:08X} not found (auto-discovery also failed)", playlist_handle);
  return X_E_NOTFOUND;
}

X_HRESULT XmpApp::XMPContinue() {
  REXKRNL_DEBUG("XMPContinue()");
  if (state_ == State::kPaused) {
    state_ = State::kPlaying;
    playback_paused_.store(false);
    playback_cv_.notify_all();
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPStop(uint32_t unk) {
  assert_zero(unk);
  REXKRNL_DEBUG("XMPStop({:08X})", unk);
  StopPlayback();
  active_playlist_ = nullptr;
  active_song_index_ = 0;
  state_ = State::kIdle;
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPause() {
  REXKRNL_DEBUG("XMPPause()");
  if (state_ == State::kPlaying) {
    state_ = State::kPaused;
    playback_paused_.store(true);
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPNext() {
  REXKRNL_DEBUG("XMPNext()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    current_song_index_ = (current_song_index_ + 1) % active_playlist_->songs.size();
    active_song_index_ = current_song_index_;
  }
  state_ = State::kPlaying;
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPrevious() {
  REXKRNL_DEBUG("XMPPrevious()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    if (!current_song_index_) {
      current_song_index_ = static_cast<int>(active_playlist_->songs.size()) - 1;
    } else {
      --current_song_index_;
    }
    active_song_index_ = current_song_index_;
  }
  state_ = State::kPlaying;
  OnStateChanged();
  return X_E_SUCCESS;
}

void XmpApp::OnStateChanged() {
  kernel_state_->BroadcastNotification(kMsgStateChanged, static_cast<uint32_t>(state_));
}

XmpApp::Playlist* XmpApp::LookupPlaylistByStoragePtr(uint32_t storage_ptr) const {
  auto it = playlists_by_storage_ptr_.find(storage_ptr);
  if (it == playlists_by_storage_ptr_.end()) {
    return nullptr;
  }
  return it->second;
}

X_HRESULT XmpApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00070002: {
      assert_true(!buffer_length || buffer_length == 12);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t storage_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t song_handle = memory::load_and_swap<uint32_t>(buffer + 8);  // 0?
      uint32_t playlist_handle = 0;
      if (auto* playlist = LookupPlaylistByStoragePtr(storage_ptr)) {
        playlist_handle = playlist->handle;
        REXKRNL_WARN(
            "XMPPlayTitlePlaylist request storage={:08X} song={:08X} resolved via storage map to handle={:08X} name='{}' songs={}",
            storage_ptr, song_handle, playlist_handle, rex::string::to_utf8(playlist->name),
            playlist->songs.size());
      } else if (storage_ptr) {
        playlist_handle = memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(storage_ptr));
        REXKRNL_WARN(
            "XMPPlayTitlePlaylist request storage={:08X} song={:08X} not in storage map, fallback deref handle={:08X}",
            storage_ptr, song_handle, playlist_handle);
      }
      assert_true(xmp_client == 0x00000002);
      return XMPPlayTitlePlaylist(playlist_handle, song_handle);
    }
    case 0x00070003: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPContinue();
    }
    case 0x00070004: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t unk = memory::load_and_swap<uint32_t>(buffer + 4);
      assert_true(xmp_client == 0x00000002);
      return XMPStop(unk);
    }
    case 0x00070005: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPause();
    }
    case 0x00070006: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPNext();
    }
    case 0x00070007: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPrevious();
    }
    case 0x00070008: {
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> playback_mode;
        rex::be<uint32_t> repeat_mode;
        rex::be<uint32_t> flags;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXKRNL_DEBUG("XMPSetPlaybackBehavior({:08X}, {:08X}, {:08X})", uint32_t(args->playback_mode),
                    uint32_t(args->repeat_mode), uint32_t(args->flags));
      playback_mode_ = static_cast<PlaybackMode>(uint32_t(args->playback_mode));
      repeat_mode_ = static_cast<RepeatMode>(uint32_t(args->repeat_mode));
      unknown_flags_ = args->flags;
      kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 0);
      return X_E_SUCCESS;
    }
    case 0x00070009: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t state_ptr = memory::load_and_swap<uint32_t>(buffer + 4);  // out ptr to 4b - expect 0
      assert_true(xmp_client == 0x00000002);
      return XMPGetStatus(state_ptr);
    }
    case 0x0007000B: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> volume_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPGetVolume({:08X})", uint32_t(args->volume_ptr));
      memory::store_and_swap<float>(memory_->TranslateVirtual(args->volume_ptr), volume_);
      return X_E_SUCCESS;
    }
    case 0x0007000C: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<float> value;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPSetVolume({:g})", float(args->value));
      volume_ = args->value;
      return X_E_SUCCESS;
    }
    case 0x0007000D: {
      assert_true(!buffer_length || buffer_length == 36);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> storage_ptr;
        rex::be<uint32_t> storage_size;
        rex::be<uint32_t> songs_ptr;
        rex::be<uint32_t> song_count;
        rex::be<uint32_t> playlist_name_ptr;
        rex::be<uint32_t> flags;
        rex::be<uint32_t> song_handles_ptr;
        rex::be<uint32_t> playlist_handle_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 36);

      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->playlist_handle_ptr),
                                       args->storage_ptr);
      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      std::u16string playlist_name;
      if (!args->playlist_name_ptr) {
        playlist_name = u"";
      } else {
        playlist_name = memory::load_and_swap<std::u16string>(
            memory_->TranslateVirtual(args->playlist_name_ptr));
      }
      // dummy_alloc_ptr is the result of a XamAlloc of storage_size.
      assert_true(uint32_t(args->storage_size) == 4 + uint32_t(args->song_count) * 128);
      return XMPCreateTitlePlaylist(args->storage_ptr, args->songs_ptr, args->song_count,
                                    args->playlist_name_ptr, playlist_name, args->flags,
                                    args->song_handles_ptr, args->storage_ptr);
    }
    case 0x0007000E: {
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> unk_ptr;  // 0
        rex::be<uint32_t> info_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      auto info = memory_->TranslateVirtual(args->info_ptr);
      assert_true(args->xmp_client == 0x00000002);
      assert_zero(args->unk_ptr);
      REXKRNL_ERROR("XMPGetInfo?({:08X}, {:08X})", uint32_t(args->unk_ptr),
                    uint32_t(args->info_ptr));
      if (!active_playlist_) {
        return X_E_FAIL;
      }
      auto& song = active_playlist_->songs[active_song_index_];
      memory::store_and_swap<uint32_t>(info + 0, song->handle);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 0, song->name);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 40, song->artist);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 80, song->album);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 120, song->album_artist);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 160, song->genre);
      memory::store_and_swap<uint32_t>(info + 4 + 572 + 200, song->track_number);
      memory::store_and_swap<uint32_t>(info + 4 + 572 + 204, song->duration_ms);
      memory::store_and_swap<uint32_t>(info + 4 + 572 + 208, static_cast<uint32_t>(song->format));
      return X_E_SUCCESS;
    }
    case 0x00070013: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> storage_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      uint32_t playlist_handle = 0;
      if (auto* playlist = LookupPlaylistByStoragePtr(args->storage_ptr)) {
        playlist_handle = playlist->handle;
        REXKRNL_WARN("XMPDeleteTitlePlaylist request storage={:08X} resolved via storage map to handle={:08X}",
                     uint32_t(args->storage_ptr), playlist_handle);
      } else if (args->storage_ptr) {
        playlist_handle =
            memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(args->storage_ptr));
        REXKRNL_WARN(
            "XMPDeleteTitlePlaylist request storage={:08X} not in storage map, fallback deref handle={:08X}",
            uint32_t(args->storage_ptr), playlist_handle);
      }
      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      return XMPDeleteTitlePlaylist(playlist_handle);
    }
    case 0x0007001A: {
      // XMPSetPlaybackController
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> controller;
        rex::be<uint32_t> playback_client;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true((args->xmp_client == 0x00000002 && args->controller == 0x00000000) ||
                  (args->xmp_client == 0x00000000 && args->controller == 0x00000001));
      REXKRNL_WARN("XMPSetPlaybackController client={:08X} controller={:08X} playback_client={:08X}",
                   uint32_t(args->xmp_client), uint32_t(args->controller),
                   uint32_t(args->playback_client));

      playback_client_ = PlaybackClient(uint32_t(args->playback_client));
      kernel_state_->BroadcastNotification(kMsgPlaybackControllerChanged, !args->playback_client);
      return X_E_SUCCESS;
    }
    case 0x0007001B: {
      // XMPGetPlaybackController
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> controller_ptr;
        rex::be<uint32_t> locked_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPGetPlaybackController({:08X}, {:08X}, {:08X})",
                    uint32_t(args->xmp_client), uint32_t(args->controller_ptr),
                    uint32_t(args->locked_ptr));
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->controller_ptr), 0);
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->locked_ptr), 0);

      if (!XThread::GetCurrentThread()->main_thread()) {
        // Atrain spawns a thread 82437FD0 to call this in a tight loop forever.
        rex::thread::Sleep(std::chrono::milliseconds(10));
      }

      return X_E_SUCCESS;
    }
    case 0x00070029: {
      // XMPGetPlaybackBehavior
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> playback_mode_ptr;
        rex::be<uint32_t> repeat_mode_ptr;
        rex::be<uint32_t> unk3_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXKRNL_DEBUG("XMPGetPlaybackBehavior({:08X}, {:08X}, {:08X})",
                    uint32_t(args->playback_mode_ptr), uint32_t(args->repeat_mode_ptr),
                    uint32_t(args->unk3_ptr));
      if (args->playback_mode_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->playback_mode_ptr),
                                         static_cast<uint32_t>(playback_mode_));
      }
      if (args->repeat_mode_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->repeat_mode_ptr),
                                         static_cast<uint32_t>(repeat_mode_));
      }
      if (args->unk3_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->unk3_ptr), unknown_flags_);
      }
      return X_E_SUCCESS;
    }
    case 0x0007002E: {
      assert_true(!buffer_length || buffer_length == 12);
      // Query of size for XamAlloc - the result of the alloc is passed to
      // 0x0007000D.
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> song_count;
        rex::be<uint32_t> size_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      // We don't use the storage, so just fudge the number.
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->size_ptr),
                                       4 + uint32_t(args->song_count) * 128);
      return X_E_SUCCESS;
    }
    case 0x0007003D: {
      // XMPCaptureOutput - not sure how this works :/
      REXKRNL_DEBUG("XMPCaptureOutput(...)");
      assert_always("XMP output not unimplemented");
      return X_E_FAIL;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XMP message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
