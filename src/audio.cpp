#include "audio.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <thread>
#include <curl/curl.h>
#include <coreinit/debug.h>
#include "threadutil.h"

extern "C" {
#include <libavformat/avio.h>
// Stub for alarm(), missing from newlib on Wii U (needed by mbedtls)
unsigned int alarm(unsigned int s) {
    (void)s;
    return 0;
}
}

struct MemoryBuffer {
    const uint8_t* data = nullptr;
    int64_t size = 0;
    int64_t pos = 0;
};

static int MemRead(void* opaque, uint8_t* buf, int buf_size) {
    auto* mb = (MemoryBuffer*)opaque;
    if (mb->pos >= mb->size) return AVERROR_EOF;
    int64_t avail = mb->size - mb->pos;
    int to_copy = std::min((int64_t)buf_size, avail);
    memcpy(buf, mb->data + mb->pos, to_copy);
    mb->pos += to_copy;
    return to_copy;
}

static int64_t MemSeek(void* opaque, int64_t offset, int whence) {
    auto* mb = (MemoryBuffer*)opaque;
    if (whence == AVSEEK_SIZE) return mb->size;
    int64_t new_pos = 0;
    if (whence == SEEK_SET) new_pos = offset;
    else if (whence == SEEK_CUR) new_pos = mb->pos + offset;
    else if (whence == SEEK_END) new_pos = mb->size + offset;
    else return -1;

    if (new_pos < 0 || new_pos > mb->size) return -1;
    mb->pos = new_pos;
    return mb->pos;
}

static int CurlProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    auto* self = (AudioPlayer*)clientp;
    if (self && self->IsStopRequested()) {
        return 1; // Abort curl immediately
    }
    return 0;
}

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* vec = (std::vector<uint8_t>*)userp;
    size_t total = size * nmemb;
    // Out-of-memory guard: 12 MB max so the Wii U heap does not overflow
    if (vec->size() + total > 12 * 1024 * 1024) {
        return 0; // Abort curl cleanly
    }
    size_t cur = vec->size();
    vec->resize(cur + total);
    memcpy(vec->data() + cur, contents, total);
    // Give some CPU back to the renderer during the download (TLS decryption is very heavy on PPC)
    static uint32_t s_calls = 0;
    if ((++s_calls & 3) == 0) SDL_Delay(1);
    return total;
}

AudioPlayer& AudioPlayer::Instance() {
    static AudioPlayer instance;
    return instance;
}

AudioPlayer::AudioPlayer() {
    m_spectrum_energy.resize(32, 0.0f);
    m_audio_buf.resize(192000, 0);
}

AudioPlayer::~AudioPlayer() {
    Cleanup();
}

int AudioPlayer::thread_entry(void* ptr) {
    OSSetThreadAffinity(OSGetCurrentThread(), 1);   // core 0: rendering runs on core 1 (priority unchanged)
    auto* self = (AudioPlayer*)ptr;
    if (self) {
        self->decode_loop();
    }
    return 0;
}

int AudioPlayer::interrupt_cb(void* ptr) {
    auto* self = (AudioPlayer*)ptr;
    if (self && self->m_should_stop.load()) {
        return 1;
    }
    return 0;
}

bool AudioPlayer::Initialize() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        OSReport("[MiiTune Audio] SDL_InitSubSystem error: %s\n", SDL_GetError());
        return false;
    }

    avformat_network_init();

    SDL_AudioSpec wantedSpec;
    SDL_zero(wantedSpec);
    wantedSpec.freq = 48000;
    wantedSpec.format = AUDIO_S16SYS;
    wantedSpec.channels = 2;
    wantedSpec.samples = 8192;   // ~170 ms: the audio thread wakes up 4x less often and tolerates a slow renderer
    wantedSpec.callback = nullptr;

    m_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wantedSpec, nullptr, 0);
    if (!m_audio_dev) {
        OSReport("[MiiTune Audio] SDL_OpenAudioDevice error: %s\n", SDL_GetError());
        return false;
    }

    SDL_PauseAudioDevice(m_audio_dev, 1);
    OSReport("[MiiTune Audio] Audio device initialised (48000Hz S16 stereo)\n");
    return true;
}

void AudioPlayer::Cleanup() {
    Stop();
    if (m_audio_dev) {
        SDL_CloseAudioDevice(m_audio_dev);
        m_audio_dev = 0;
    }
    avformat_network_deinit();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioPlayer::stop_decode_thread() {
    std::lock_guard<std::mutex> lock(m_thread_mutex);
    stop_decode_thread_locked();
}

void AudioPlayer::stop_decode_thread_locked() {
    m_should_stop.store(true);
    if (m_decode_thread) {
        int status = 0;
        SDL_WaitThread(m_decode_thread, &status);
        m_decode_thread = nullptr;
    }
    m_should_stop.store(false);
    if (m_audio_dev) {
        SDL_ClearQueuedAudio(m_audio_dev);
        SDL_PauseAudioDevice(m_audio_dev, 1);
    }
}

void AudioPlayer::begin_playback_locked(int durationSec) {
    m_duration_sec.store(durationSec);
    m_current_pos_sec.store(0);
    m_state.store(PlaybackState::BUFFERING);
    m_is_paused.store(false);
    m_seek_target_sec.store(-1);
    m_should_stop.store(false);

    // Dedicated thread with a 1 MB stack to avoid stack overflows on Wii U / Cemu
    m_decode_thread = SDL_CreateThreadWithStackSize(thread_entry, "AudioDecoder", 1024 * 1024, this);
    if (!m_decode_thread) {
        OSReport("[MiiTune Audio] Could not create the audio thread: %s\n", SDL_GetError());
        m_state.store(PlaybackState::STOPPED);
    }
}

void AudioPlayer::PlayStream(const std::string& streamUrl, int durationSec) {
    if (streamUrl.empty()) return;

    std::lock_guard<std::mutex> lock(m_thread_mutex);
    stop_decode_thread_locked();

    m_stream_url = streamUrl;
    m_start_has_data = false;
    begin_playback_locked(durationSec);
}

bool AudioPlayer::HasPrefetched(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_pf_mutex);
    return !id.empty() && m_pf_id == id && !m_pf_data.empty();
}

bool AudioPlayer::PlayPrefetched(const std::string& id, int durationSec) {
    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(m_pf_mutex);
        if (id.empty() || m_pf_id != id || m_pf_data.empty()) return false;
        data = std::move(m_pf_data);
        m_pf_data.clear();
        m_pf_id.clear();
    }
    std::lock_guard<std::mutex> lock(m_thread_mutex);
    stop_decode_thread_locked();
    m_stream_url.clear();
    m_start_data = std::move(data);
    m_start_has_data = true;
    begin_playback_locked(durationSec);
    return true;
}

void AudioPlayer::Prefetch(const std::string& id, const std::string& url) {
    if (id.empty() || url.empty() || HasPrefetched(id)) return;
    std::vector<uint8_t> data;
    data.reserve(6 * 1024 * 1024);
    CURL* curl = curl_easy_init();
    if (!curl) return;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 64 * 1024);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || data.empty()) {
        OSReport("[MiiTune Audio] Prefetch %s echoue (%s)\n", id.c_str(), curl_easy_strerror(res));
        return;
    }
    std::lock_guard<std::mutex> lock(m_pf_mutex);
    m_pf_id = id;
    m_pf_data = std::move(data);
    OSReport("[MiiTune Audio] Prefetch %s pret (%u octets)\n", id.c_str(), (unsigned int)m_pf_data.size());
}

void AudioPlayer::Pause() {
    if (m_state.load() == PlaybackState::PLAYING) {
        m_is_paused.store(true);
        m_state.store(PlaybackState::PAUSED);
        if (m_audio_dev) SDL_PauseAudioDevice(m_audio_dev, 1);
    }
}

void AudioPlayer::Resume() {
    if (m_state.load() == PlaybackState::PAUSED) {
        m_is_paused.store(false);
        m_state.store(PlaybackState::PLAYING);
        if (m_audio_dev) SDL_PauseAudioDevice(m_audio_dev, 0);
    }
}

void AudioPlayer::TogglePlayPause() {
    if (m_state.load() == PlaybackState::PLAYING) {
        Pause();
    } else if (m_state.load() == PlaybackState::PAUSED) {
        Resume();
    }
}

void AudioPlayer::Stop() {
    std::lock_guard<std::mutex> lock(m_thread_mutex);
    stop_decode_thread_locked();
    m_state.store(PlaybackState::STOPPED);
    m_current_pos_sec.store(0);
}

void AudioPlayer::Seek(int targetSeconds) {
    if (targetSeconds >= 0) {
        m_seek_target_sec.store(targetSeconds);
        m_state.store(PlaybackState::BUFFERING);
        if (m_audio_dev) {
            SDL_ClearQueuedAudio(m_audio_dev);
            SDL_PauseAudioDevice(m_audio_dev, 1);
        }
    }
}

void AudioPlayer::SetVolume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    m_volume.store(vol);
}

std::vector<float> AudioPlayer::GetSpectrumBars(int barCount) {
    std::lock_guard<std::mutex> lock(m_spectrum_mutex);
    std::vector<float> result(barCount, 0.0f);
    if (m_spectrum_energy.empty()) return result;

    for (int i = 0; i < barCount; i++) {
        int srcIdx = (i * (int)m_spectrum_energy.size()) / barCount;
        result[i] = m_spectrum_energy[srcIdx];
        m_spectrum_energy[srcIdx] *= 0.88f; // Soft decay
    }
    return result;
}

void AudioPlayer::decode_loop() {
    OSReport("[MiiTune Audio] Starting stream download via libcurl HTTPS...\n");

    std::vector<uint8_t> mediaData;
    CURLcode res = CURLE_OK;

    if (m_start_has_data) {
        // Prefetched track: nothing to download, start immediately
        mediaData = std::move(m_start_data);
        m_start_has_data = false;
        OSReport("[MiiTune Audio] Prefetched track: %u bytes, decoding directly\n", (unsigned int)mediaData.size());
    } else {
        mediaData.reserve(8 * 1024 * 1024);

        CURL* curl = curl_easy_init();
        if (!curl) {
            OSReport("[MiiTune Audio] curl_easy_init error\n");
            m_state.store(PlaybackState::STOPPED);
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, m_stream_url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mediaData);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 64 * 1024);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }

    if (m_should_stop.load() || mediaData.empty() || (res != CURLE_OK && res != CURLE_ABORTED_BY_CALLBACK)) {
        OSReport("[MiiTune Audio] Download cancelled or failed (%s, size: %u)\n", curl_easy_strerror(res), (unsigned int)mediaData.size());
        m_state.store(PlaybackState::STOPPED);
        return;
    }

    OSReport("[MiiTune Audio] Track downloaded (%u bytes), starting FFmpeg decoding\n", (unsigned int)mediaData.size());

    MemoryBuffer mb{ mediaData.data(), (int64_t)mediaData.size(), 0 };

    const int avioBufferSize = 32768;
    unsigned char* avioBuffer = (unsigned char*)av_malloc(avioBufferSize);

    AVIOContext* avioCtx = avio_alloc_context(
        avioBuffer, avioBufferSize, 0, &mb, MemRead, nullptr, MemSeek
    );

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (fmt_ctx) {
        fmt_ctx->pb = avioCtx;
        fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
        fmt_ctx->interrupt_callback.callback = interrupt_cb;
        fmt_ctx->interrupt_callback.opaque = this;
    }

    int open_ret = avformat_open_input(&fmt_ctx, nullptr, nullptr, nullptr);
    if (open_ret < 0) {
        OSReport("[MiiTune Audio] avformat_open_input (AVIO) error (%d)\n", open_ret);
        av_freep(&avioCtx->buffer);
        avio_context_free(&avioCtx);
        if (fmt_ctx) avformat_free_context(fmt_ctx);
        m_state.store(PlaybackState::STOPPED);
        return;
    }

    fmt_ctx->probesize = 128 * 1024;
    fmt_ctx->max_analyze_duration = AV_TIME_BASE;

    int info_ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (info_ret < 0) {
        OSReport("[MiiTune Audio] Stream info error (%d)\n", info_ret);
        avformat_close_input(&fmt_ctx);
        av_freep(&avioCtx->buffer);
        avio_context_free(&avioCtx);
        m_state.store(PlaybackState::STOPPED);
        return;
    }

    int a_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (a_idx < 0) {
        OSReport("[MiiTune Audio] No audio stream found among %d streams\n", fmt_ctx->nb_streams);
        avformat_close_input(&fmt_ctx);
        av_freep(&avioCtx->buffer);
        avio_context_free(&avioCtx);
        m_state.store(PlaybackState::STOPPED);
        return;
    }

    AVStream* st = fmt_ctx->streams[a_idx];
    AVRational a_time_base = st->time_base;

    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        OSReport("[MiiTune Audio] Unsupported decoder (%d)\n", st->codecpar->codec_id);
        avformat_close_input(&fmt_ctx);
        av_freep(&avioCtx->buffer);
        avio_context_free(&avioCtx);
        m_state.store(PlaybackState::STOPPED);
        return;
    }

    AVCodecContext* a_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(a_ctx, st->codecpar);
    a_ctx->pkt_timebase = st->time_base;
    a_ctx->thread_count = 1;
    a_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (avcodec_open2(a_ctx, codec, nullptr) < 0) {
        OSReport("[MiiTune Audio] Could not open the audio codec\n");
        avcodec_free_context(&a_ctx);
        avformat_close_input(&fmt_ctx);
        av_freep(&avioCtx->buffer);
        avio_context_free(&avioCtx);
        m_state.store(PlaybackState::STOPPED);
        return;
    }

    // Actual duration
    if (fmt_ctx->duration > 0) {
        m_duration_sec.store((int)(fmt_ctx->duration / AV_TIME_BASE));
    }

    // Resample to 48000 Hz S16 stereo
    int64_t in_ch_layout = a_ctx->channel_layout ? a_ctx->channel_layout : av_get_default_channel_layout(a_ctx->channels);
    SwrContext* swr = swr_alloc_set_opts(
        nullptr,
        AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000,
        in_ch_layout, a_ctx->sample_fmt, a_ctx->sample_rate,
        0, nullptr
    );
    swr_init(swr);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    const int maxDstSamples = 8192;
    int16_t* resampleBuffer = (int16_t*)av_malloc(maxDstSamples * 2 * sizeof(int16_t));

    bool device_started = false;
    uint32_t dbgStart = 0;
    double dbgDecodedSec = 0.0;
    unsigned int rebuffer_bytes = 192000;   // (re)start threshold: 1 s initially, 2 s after a dropout
    m_dbg_speed_x10.store(0);
    m_dbg_underruns.store(0);

    while (!m_should_stop.load()) {
        // Pause
        if (m_is_paused.load()) {
            SDL_Delay(20);
            continue;
        }

        // Seek
        int targetSec = m_seek_target_sec.exchange(-1);
        if (targetSec >= 0) {
            int64_t targetTs = av_rescale_q(targetSec, {1, 1}, a_time_base);
            av_seek_frame(fmt_ctx, a_idx, targetTs, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(a_ctx);
            if (m_audio_dev) {
                SDL_ClearQueuedAudio(m_audio_dev);
                SDL_PauseAudioDevice(m_audio_dev, 1);
            }
            device_started = false;
            rebuffer_bytes = 192000;
            m_current_pos_sec.store(targetSec);
        }

        // Jitter buffer: keep up to ~3 s ahead (192000 bytes = 1 s) to absorb slowdowns
        if (m_audio_dev && SDL_GetQueuedAudioSize(m_audio_dev) > 576000) {
            SDL_Delay(10);
            continue;
        }

        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            // End of track
            break;
        }

        if (pkt->stream_index == a_idx) {
            ret = avcodec_send_packet(a_ctx, pkt);
            while (ret >= 0 && !m_should_stop.load()) {
                ret = avcodec_receive_frame(a_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    break;
                }

                int dstSamples = (int)av_rescale_rnd(
                    swr_get_delay(swr, a_ctx->sample_rate) + frame->nb_samples,
                    48000, a_ctx->sample_rate, AV_ROUND_UP);

                if (dstSamples > maxDstSamples) dstSamples = maxDstSamples;

                uint8_t* outData[1] = { (uint8_t*)resampleBuffer };
                int convertedSamples = swr_convert(swr, outData, dstSamples,
                                                   (const uint8_t**)frame->data, frame->nb_samples);

                if (convertedSamples > 0 && m_audio_dev) {
                    // Fixed-point volume (no floats in the hot loop) + sampled spectrum energy
                    int vq = m_volume.load() * 256 / 100;   // 0..256
                    int totalSamples = convertedSamples * 2;
                    int energySum = 0, energyN = 0;
                    if (vq != 256) {
                        for (int i = 0; i < totalSamples; i++) {
                            int v = (resampleBuffer[i] * vq) >> 8;
                            resampleBuffer[i] = (int16_t)v;
                        }
                    }
                    for (int i = 0; i < totalSamples; i += 8) { energySum += std::abs((int)resampleBuffer[i]); energyN++; }
                    float avgEnergy = energyN ? (float)energySum / (energyN * 32768.0f) : 0.0f;
                    {
                        std::lock_guard<std::mutex> sLock(m_spectrum_mutex);
                        int curPos = m_current_pos_sec.load();
                        for (size_t b = 0; b < m_spectrum_energy.size(); b++) {
                            float factor = 0.5f + 0.5f * std::sin((float)b * 0.45f + (float)curPos * 1.5f);
                            float barTarget = avgEnergy * factor * 3.0f;
                            if (barTarget > 1.0f) barTarget = 1.0f;
                            m_spectrum_energy[b] = std::max(m_spectrum_energy[b], barTarget);
                        }
                    }

                    // Measurements (shown in the player): decoding speed and dropouts
                    if (dbgStart == 0) dbgStart = SDL_GetTicks();
                    dbgDecodedSec += (double)convertedSamples / 48000.0;
                    double wall = (SDL_GetTicks() - dbgStart) / 1000.0;
                    if (wall > 0.5) m_dbg_speed_x10.store((int)(dbgDecodedSec / wall * 10.0));
                    if (device_started && SDL_GetQueuedAudioSize(m_audio_dev) == 0) {
                        m_dbg_underruns.fetch_add(1);
                        // Empty buffer: refill 2 s before resuming (instead of micro-dropouts)
                        SDL_PauseAudioDevice(m_audio_dev, 1);
                        device_started = false;
                        rebuffer_bytes = 384000;
                    }

                    SDL_QueueAudio(m_audio_dev, resampleBuffer, convertedSamples * 2 * sizeof(int16_t));

                    // Start playback once ~1 s of audio is buffered (was 0.15 s: stutter at the slightest hiccup)
                    if (!device_started && SDL_GetQueuedAudioSize(m_audio_dev) >= rebuffer_bytes) {
                        SDL_PauseAudioDevice(m_audio_dev, 0);
                        m_state.store(PlaybackState::PLAYING);
                        device_started = true;
                    }
                }

                // Update the playback position
                if (frame->pts != AV_NOPTS_VALUE) {
                    m_current_pos_sec.store((int)(frame->pts * av_q2d(a_time_base)));
                }
            }
        }
        av_packet_unref(pkt);
    }

    av_free(resampleBuffer);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&a_ctx);
    avformat_close_input(&fmt_ctx);

    av_freep(&avioCtx->buffer);
    avio_context_free(&avioCtx);

    if (!m_should_stop.load()) {
        // Track shorter than the initial buffer: start it now
        if (!device_started && m_audio_dev) {
            SDL_PauseAudioDevice(m_audio_dev, 0);
            m_state.store(PlaybackState::PLAYING);
        }
        // Wait for the last samples to play
        while (m_audio_dev && SDL_GetQueuedAudioSize(m_audio_dev) > 0 && !m_should_stop.load()) {
            SDL_Delay(50);
        }
        m_state.store(PlaybackState::STOPPED);
    }
}

void AudioPlayer::GetDebugInfo(float& bufferSec, float& speedX, int& underruns) {
    bufferSec = m_audio_dev ? (float)SDL_GetQueuedAudioSize(m_audio_dev) / 192000.0f : 0.0f;
    speedX = m_dbg_speed_x10.load() / 10.0f;
    underruns = m_dbg_underruns.load();
}
