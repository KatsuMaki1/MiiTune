#ifndef MIITUNE_AUDIO_H
#define MIITUNE_AUDIO_H

#include "models.h"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <SDL2/SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

class AudioPlayer {
public:
    static AudioPlayer& Instance();

    bool Initialize();
    void Cleanup();

    void PlayStream(const std::string& streamUrl, int durationSec = 0);

    // Prefetch of the next track: downloads ahead (blocking: call from a background thread)
    // so the track starts without waiting when its turn comes.
    void Prefetch(const std::string& id, const std::string& url);
    bool HasPrefetched(const std::string& id);
    bool PlayPrefetched(const std::string& id, int durationSec = 0);   // false if nothing is cached
    void Pause();
    void Resume();
    void TogglePlayPause();
    void Stop();
    void Seek(int targetSeconds);

    void SetVolume(int vol); // 0 - 100
    int GetVolume() const { return m_volume.load(); }

    PlaybackState GetState() const { return m_state.load(); }
    int GetCurrentPosition() const { return m_current_pos_sec.load(); }
    int GetDuration() const { return m_duration_sec.load(); }
    bool IsStopRequested() const { return m_should_stop.load(); }

    // Diagnostics (buffer in s, decoding speed in x real time, number of dropouts)
    void GetDebugInfo(float& bufferSec, float& speedX, int& underruns);

    // Smooth, lightweight audio visualizer
    std::vector<float> GetSpectrumBars(int barCount);

private:
    AudioPlayer();
    ~AudioPlayer();

    static int thread_entry(void* ptr);
    static int interrupt_cb(void* ptr);
    void decode_loop();
    void begin_playback_locked(int durationSec);
    void stop_decode_thread();
    void stop_decode_thread_locked();

    SDL_AudioDeviceID m_audio_dev = 0;
    std::string m_stream_url;

    std::atomic<PlaybackState> m_state{PlaybackState::STOPPED};
    std::atomic<bool> m_should_stop{false};
    std::atomic<bool> m_is_paused{false};
    std::atomic<int> m_seek_target_sec{-1};
    std::atomic<int> m_current_pos_sec{0};
    std::atomic<int> m_duration_sec{0};
    std::atomic<int> m_volume{85};
    std::atomic<int> m_dbg_speed_x10{0};
    std::atomic<int> m_dbg_underruns{0};

    std::mutex m_pf_mutex;
    std::string m_pf_id;
    std::vector<uint8_t> m_pf_data;
    std::vector<uint8_t> m_start_data;   // already-downloaded data handed to the decode thread
    bool m_start_has_data = false;

    SDL_Thread* m_decode_thread = nullptr;
    std::mutex m_thread_mutex;

    // Audio visualizer
    std::vector<float> m_spectrum_energy;
    std::mutex m_spectrum_mutex;
    std::vector<uint8_t> m_audio_buf;
};

#endif // MIITUNE_AUDIO_H
