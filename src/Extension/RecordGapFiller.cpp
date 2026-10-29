/* Copyright (c) ZLMediaKit contributors. SPDX-License-Identifier: MIT */
#include "RecordGapFiller.h"
#include "Common/config.h"
#include "Util/onceToken.h"
#include "Util/util.h"
#include "Record/MPEG.h"
#include "ext-codec/AAC.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <vector>

#if defined(__linux__) && !defined(ANDROID)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(ENABLE_MP4) && defined(ENABLE_FFMPEG)
#include "Extension/Factory.h"
#include "ext-codec/H264.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

using namespace toolkit;

namespace mediakit {
namespace {

const char *kEnable = "record_gap_fill.enable";
const char *kDetect = "record_gap_fill.detect_ms";
const char *kMax = "record_gap_fill.max_ms";
const char *kFps = "record_gap_fill.fps";
const char *kBitrate = "record_gap_fill.bitrate";

onceToken defaults([] {
    auto &ini = mINI::Instance();
    ini[kEnable] = false;
    ini[kDetect] = 200;
    ini[kMax] = 15000;
    ini[kFps] = 25;
    ini[kBitrate] = 2000000;
});

#if defined(ENABLE_MP4) && defined(ENABLE_FFMPEG)
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
constexpr size_t kLiveInputEvents = 256;
constexpr size_t kStartupInputEvents = 2048;

struct Options {
    int buffer_ms;
    int max_ms;
    int fps;
    int bitrate;
    bool audio;
};

void check(int result, const char *operation) {
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, error, sizeof(error));
        throw std::runtime_error(std::string("record_gap_fill: ") + operation + ": " + error);
    }
}

struct ContextFree { void operator()(AVCodecContext *p) const { avcodec_free_context(&p); } };
struct FrameFree { void operator()(AVFrame *p) const { av_frame_free(&p); } };
struct PacketFree { void operator()(AVPacket *p) const { av_packet_free(&p); } };
struct SwrFree { void operator()(SwrContext *p) const { swr_free(&p); } };
struct SwsFree { void operator()(SwsContext *p) const { sws_freeContext(p); } };
using Context = std::unique_ptr<AVCodecContext, ContextFree>;
using Picture = std::shared_ptr<AVFrame>;
using Packet = std::unique_ptr<AVPacket, PacketFree>;

Picture picture() {
    auto frame = Picture(av_frame_alloc(), FrameFree{});
    if (!frame) { throw std::bad_alloc(); }
    return frame;
}

Context context(const AVCodec *codec) {
    if (!codec) { throw std::runtime_error("record_gap_fill: required codec unavailable"); }
    Context result(avcodec_alloc_context3(codec));
    if (!result) { throw std::bad_alloc(); }
    return result;
}

AVCodecID inputCodec(CodecId codec) {
    switch (codec) {
        case CodecH264: return AV_CODEC_ID_H264;
        case CodecH265: return AV_CODEC_ID_HEVC;
        case CodecAAC: return AV_CODEC_ID_AAC;
        case CodecG711A: return AV_CODEC_ID_PCM_ALAW;
        case CodecG711U: return AV_CODEC_ID_PCM_MULAW;
        default: throw std::invalid_argument("record_gap_fill: unsupported input codec");
    }
}

double milliseconds(Time start, Time end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

struct Event {
    enum Kind { TrackAdded, TracksComplete, FrameAdded, Reset, Flush } kind;
    explicit Event(Kind value) : kind(value) {}
    Track::Ptr track;
    Frame::Ptr frame;
    Time received = Clock::now();
};

struct TimedPicture {
    double time_ms;
    Picture frame;
};

struct AudioChunk {
    int64_t start;
    Picture frame;
};

// All FFmpeg and downstream operations run on this worker. The network thread
// only clones metadata / retains cacheable frames and appends a bounded event.
class RecordGapFiller final : public MediaSinkInterface {
public:
    RecordGapFiller(MediaSinkInterface::Ptr downstream, Options options)
        : _downstream(std::move(downstream)), _options(options) {
        _thread = std::thread([this] { run(); });
    }

    ~RecordGapFiller() override {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stop_time = Clock::now();
            _stopping = true;
        }
        _condition.notify_one();
        _thread.join();
    }

    bool addTrack(const Track::Ptr &track) override {
        auto type = track->getTrackType();
        if (type == TrackAudio && !_options.audio) { return true; }
        if (type != TrackVideo && type != TrackAudio) { return false; }
        if (!avcodec_find_decoder(inputCodec(track->getCodecId()))) {
            throw std::invalid_argument("record_gap_fill: input decoder unavailable");
        }
        Event event{Event::TrackAdded};
        event.track = track->clone();
        return enqueue(std::move(event));
    }

    void addTrackCompleted() override { enqueue(Event{Event::TracksComplete}); }
    void resetTracks() override { enqueue(Event{Event::Reset}); }
    void flush() override { enqueue(Event{Event::Flush}); }

    bool inputFrame(const Frame::Ptr &frame) override {
        if (frame->getTrackType() == TrackAudio && !_options.audio) { return true; }
        Event event{Event::FrameAdded};
        event.frame = Frame::getCacheAbleFrame(frame);
        return enqueue(std::move(event));
    }

private:
    bool enqueue(Event event) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_failed || _stopping) { return false; }
        if (event.kind == Event::Reset) { _startup_queue = true; }
        size_t bytes = event.frame ? event.frame->size() : 0;
        // A newly created recorder receives a cached GOP synchronously. Seven
        // seconds of 25 fps video + 48 kHz AAC already exceeds 500 events, even
        // with a healthy encoder. Allow this bounded startup burst, then return
        // to the live limit once output has started and pending input is drained.
        const size_t max_events = _startup_queue ? kStartupInputEvents : kLiveInputEvents;
        if (_events.size() >= max_events || _queued_bytes + bytes > 32 * 1024 * 1024) {
            _failed = true;
            ErrorL << "record_gap_fill: input queue overflow; recording stopped, events=" << _events.size()
                   << ", limit=" << max_events << ", queued_bytes=" << _queued_bytes
                   << ", incoming_bytes=" << bytes << ", startup=" << _startup_queue;
            _condition.notify_one();
            return false;
        }
        _queued_bytes += bytes;
        _events.emplace_back(std::move(event));
        _condition.notify_one();
        return true;
    }

    void run() noexcept {
        try {
            setThreadName("record gap fill");
            // Linux threads inherit their creator's CPU mask and scheduling
            // policy. Pollers are pinned and may use FIFO/99 when run as root;
            // CPU-bound encoders must not share that core/real-time priority.
            setThreadAffinity(-1);
#if defined(__linux__) && !defined(ANDROID)
            sched_param priority{};
            int error = pthread_setschedparam(pthread_self(), SCHED_OTHER, &priority);
            if (error) {
                throw std::runtime_error(std::string("reset encoding thread scheduler: ") + std::strerror(error));
            }
#endif
            while (true) {
                std::deque<Event> batch;
                bool stopping;
                {
                    std::unique_lock<std::mutex> lock(_mutex);
                    _condition.wait_for(lock, std::chrono::milliseconds(10), [this] {
                        return _stopping || _failed || !_events.empty();
                    });
                    if (_failed) { break; }
                    // Consume a bounded batch before advancing the output clock.
                    // In particular, video slices/config and audio often arrive
                    // together; rendering after each NAL can overtake their decode.
                    while (!_events.empty() && batch.size() < 64) {
                        _queued_bytes -= _events.front().frame ? _events.front().frame->size() : 0;
                        batch.emplace_back(std::move(_events.front()));
                        _events.pop_front();
                    }
                    stopping = _stopping && _events.empty();
                }
                for (const auto &event : batch) { process(event); }
                if (stopping) {
                    finish(_stop_time);
                    break;
                }
                if (_started) {
                    auto ready_until = Clock::now();
                    {
                        std::lock_guard<std::mutex> lock(_mutex);
                        // Do not write black over the time of unread input.
                        if (!_events.empty()) {
                            ready_until = std::min(ready_until, _events.front().received);
                        }
                        if (_stopping) {
                            ready_until = std::min(ready_until, _stop_time);
                        }
                    }
                    render(milliseconds(_start_time, ready_until) - _options.buffer_ms);
                    if (_video_frame_index > 0) {
                        std::lock_guard<std::mutex> lock(_mutex);
                        if (_events.empty()) { _startup_queue = false; }
                    }
                }
            }
        } catch (const std::exception &error) {
            _failed = true;
            ErrorL << "record_gap_fill: recording stopped: " << error.what();
        } catch (...) {
            _failed = true;
            ErrorL << "record_gap_fill: recording stopped by unknown error";
        }
        // Never call a decoder callback during member destruction. The worker
        // drains normally in finish(); failed recordings are closed as-is.
        _merger.clear();
        _downstream.reset();
    }

    void process(const Event &event) {
        switch (event.kind) {
            case Event::TrackAdded: addInputTrack(event.track); break;
            case Event::TracksComplete:
                if (!_video_decoder) { throw std::runtime_error("video track required"); }
                break;
            case Event::FrameAdded: input(event.frame, event.received); break;
            case Event::Reset:
                finish(event.received);
                reset();
                _downstream->resetTracks();
                break;
            case Event::Flush:
                _merger.flush();
                _downstream->flush();
                break;
        }
    }

    void setExtra(AVCodecContext *decoder, const std::string &extra) {
        if (extra.empty()) { return; }
        decoder->extradata = static_cast<uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!decoder->extradata) { throw std::bad_alloc(); }
        std::memcpy(decoder->extradata, extra.data(), extra.size());
        decoder->extradata_size = static_cast<int>(extra.size());
    }

    void addInputTrack(const Track::Ptr &track) {
        bool video = track->getTrackType() == TrackVideo;
        if ((video && _video_decoder) || (!video && _audio_decoder)) {
            throw std::runtime_error("only one video and one audio track are supported");
        }
        auto decoder = context(avcodec_find_decoder(inputCodec(track->getCodecId())));
        decoder->thread_count = 1;
        decoder->pkt_timebase = AVRational{1, 1000};
        if (video) {
            auto video_track = std::dynamic_pointer_cast<VideoTrack>(track);
            if (!video_track) { throw std::runtime_error("invalid video track"); }
            std::string extra;
            for (auto &frame : video_track->getConfigFrames()) { extra.append(frame->data(), frame->size()); }
            setExtra(decoder.get(), extra);
            auto fps = video_track->getVideoFps();
            _input_frame_ms = fps >= 1 && fps <= 120 ? 1000.0 / fps : 1000.0 / _options.fps;
            _video_index = track->getIndex();
        } else {
            auto audio_track = std::dynamic_pointer_cast<AudioTrack>(track);
            if (!audio_track) { throw std::runtime_error("invalid audio track"); }
            auto extra = track->getExtraData();
            if (extra) { setExtra(decoder.get(), std::string(extra->data(), extra->size())); }
            decoder->sample_rate = audio_track->getAudioSampleRate();
#if LIBAVUTIL_VERSION_MAJOR >= 57
            av_channel_layout_default(&decoder->ch_layout, audio_track->getAudioChannel());
#else
            decoder->channels = audio_track->getAudioChannel();
            decoder->channel_layout = av_get_default_channel_layout(decoder->channels);
#endif
            _audio_index = track->getIndex();
        }
        check(avcodec_open2(decoder.get(), decoder->codec, nullptr), "open input decoder");
        if (video) { _video_decoder = std::move(decoder); }
        else { _audio_decoder = std::move(decoder); }
    }

    void input(const Frame::Ptr &frame, Time received) {
        if (_expired) { return; }
        bool video = frame->getTrackType() == TrackVideo;
        auto &decoder = video ? _video_decoder : _audio_decoder;
        if (!decoder || frame->getIndex() != (video ? _video_index : _audio_index)) { return; }
        auto &previous = video ? _last_video_input : _last_audio_input;
        auto &last_dts = video ? _last_video_dts : _last_audio_dts;
        const auto dts = static_cast<int64_t>(frame->dts());
        const double idle_ms = previous == Time{} ? 0 : milliseconds(previous, received);
        const double jump_ms = std::max(1000.0, _input_frame_ms * 4);
        const bool new_sequence = video && idle_ms > jump_ms &&
            (frame->configFrame() || frame->keyFrame());
        const bool discontinuity = previous != Time{} &&
            (dts < last_dts || new_sequence ||
             (idle_ms > _options.buffer_ms && dts - last_dts > jump_ms));
        // Receive jitter alone is not packet loss. Retain reference pictures
        // across a pause if media DTS remains continuous, including B frames.
        if (discontinuity) {
            DebugL << "record_gap_fill: input timeline reset, video=" << video
                   << ", idle_ms=" << idle_ms << ", dts_step=" << (dts - last_dts);
            if (video) {
                _merger.flush();
                _waiting_keyframe = true;
                _video_epoch_valid = false;
            } else {
                _audio_epoch_valid = false;
                _resampler.reset();
            }
            avcodec_flush_buffers(decoder.get());
        }
        previous = received;
        last_dts = dts;
        _received = received;
        if (video) {
            if (_waiting_keyframe && !frame->keyFrame() && !frame->configFrame()) { return; }
            if (frame->keyFrame()) { _waiting_keyframe = false; }
            _merger.inputFrame(frame, [this](uint64_t dts, uint64_t pts, const Buffer::Ptr &buffer, bool) {
                decode(_video_decoder.get(), buffer->data(), buffer->size(), dts, pts, true);
            });
        } else {
            // ZLM AAC frames may contain ADTS; decoder extradata already carries ASC.
            size_t prefix = frame->getCodecId() == CodecAAC ? frame->prefixSize() : 0;
            if (prefix >= frame->size()) { return; }
            decode(decoder.get(), frame->data() + prefix, frame->size() - prefix,
                   frame->dts(), frame->pts(), false);
        }
    }

    void decode(AVCodecContext *decoder, const char *data, size_t size,
                uint64_t dts, uint64_t pts, bool video) {
        Packet packet(av_packet_alloc());
        if (!packet) { throw std::bad_alloc(); }
        if (size > 16 * 1024 * 1024) { throw std::runtime_error("input packet too large"); }
        check(av_new_packet(packet.get(), static_cast<int>(size)), "allocate input packet");
        std::memcpy(packet->data, data, size);
        packet->dts = static_cast<int64_t>(dts);
        packet->pts = static_cast<int64_t>(pts);
        int result = avcodec_send_packet(decoder, packet.get());
        if (result == AVERROR(EAGAIN)) {
            receiveDecoded(decoder, video);
            result = avcodec_send_packet(decoder, packet.get());
        }
        // A damaged source frame should lead to black/silence, not a stopped recording.
        if (result == AVERROR_INVALIDDATA) { return; }
        check(result, "send input packet");
        receiveDecoded(decoder, video);
    }

    void receiveDecoded(AVCodecContext *decoder, bool video) {
        while (true) {
            auto decoded = picture();
            int result = avcodec_receive_frame(decoder, decoded.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) { break; }
            if (result == AVERROR_INVALIDDATA) { break; }
            check(result, "receive decoded frame");
            if (video) { onVideo(decoded); }
            else if (_started) { onAudio(decoded); }
        }
    }

    double mapTime(bool video, int64_t pts) {
        bool &valid = video ? _video_epoch_valid : _audio_epoch_valid;
        int64_t &source = video ? _video_epoch_source : _audio_epoch_source;
        double &target = video ? _video_epoch_ms : _audio_epoch_ms;
        double now = milliseconds(_start_time, _received);
        // Network gaps and source timestamp resets start a new input epoch;
        // the output clock is never reset. Upstream may already normalize RTMP stamps.
        if (!valid || pts == AV_NOPTS_VALUE || std::abs(target + pts - source - now) > _options.buffer_ms * 2 + 250) {
            if (valid && pts != AV_NOPTS_VALUE) {
                DebugL << "record_gap_fill: clock reanchor, sink=" << this << ", video=" << video
                       << ", pts=" << pts << ", mapped_ms=" << (target + pts - source)
                       << ", received_ms=" << now << ", output_ms=" << videoMs()
                       << ", discarded=" << (video ? _pictures.size() : _audio_chunks.size());
            }
            // Cached GOP replay advances media PTS by seconds while its receive
            // clock barely moves. Once this mapping changes, queued pictures /
            // audio still carry the old mapping and must not accumulate across
            // epochs or be played over the newly aligned live input.
            if (video) { _pictures.clear(); }
            else { _audio_chunks.clear(); }
            valid = true;
            source = pts == AV_NOPTS_VALUE ? 0 : pts;
            // Before any video has been written, a new startup mapping must
            // still cover t=0. Otherwise clearing pre-roll received a few ms
            // later leaves the very first output tick without a real picture.
            target = video && _video_frame_index == 0 ? 0 : now;
        }
        return pts == AV_NOPTS_VALUE ? now : target + pts - source;
    }

    void onVideo(const Picture &decoded) {
        if (!_video_encoder) { openOutput(decoded->width, decoded->height); }
        if (!_started) {
            _start_time = _received;
            _started = true;
        }
        double stamp = mapTime(true, decoded->best_effort_timestamp);
        _last_valid_ms = std::max(_last_valid_ms, milliseconds(_start_time, _received));
        if (stamp + _input_frame_ms < videoMs()) {
            if (!_late_video_frames++) {
                DebugL << "record_gap_fill: decoded picture too late, sink=" << this
                       << ", picture_ms=" << stamp << ", output_ms=" << videoMs()
                       << ", received_ms=" << milliseconds(_start_time, _received);
            }
            return; // Too late for already written samples.
        }
        if (_late_video_frames) {
            DebugL << "record_gap_fill: decoded picture caught up, sink=" << this
                   << ", late_frames=" << _late_video_frames;
            _late_video_frames = 0;
        }
        auto converted = picture();
        converted->format = AV_PIX_FMT_YUV420P;
        converted->width = _width;
        converted->height = _height;
        check(av_frame_get_buffer(converted.get(), 32), "allocate scaled picture");
        auto scaler = sws_getCachedContext(_scaler.release(), decoded->width, decoded->height,
            static_cast<AVPixelFormat>(decoded->format), _width, _height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        _scaler.reset(scaler);
        if (!scaler) { throw std::runtime_error("create video scaler failed"); }
        if (sws_scale(scaler, decoded->data, decoded->linesize, 0, decoded->height,
                      converted->data, converted->linesize) != _height) {
            throw std::runtime_error("scale video failed");
        }
        if (_pictures.size() >= 120) { throw std::runtime_error("decoded video queue overflow"); }
        auto position = std::upper_bound(_pictures.begin(), _pictures.end(), stamp,
            [](double time, const TimedPicture &item) { return time < item.time_ms; });
        _pictures.insert(position, TimedPicture{stamp, std::move(converted)});
    }

    void openOutput(int width, int height) {
        if (width < 16 || height < 16 || width > 4096 || height > 4096 || width % 2 || height % 2) {
            throw std::runtime_error("video size must be even and within 16..4096");
        }
        _width = width;
        _height = height;
        _video_encoder = context(avcodec_find_encoder_by_name("libx264"));
        auto video = _video_encoder.get();
        video->width = width;
        video->height = height;
        video->pix_fmt = AV_PIX_FMT_YUV420P;
        video->time_base = AVRational{1, _options.fps};
        video->framerate = AVRational{_options.fps, 1};
        video->gop_size = _options.fps * 2;
        video->max_b_frames = 0;
        video->bit_rate = _options.bitrate;
        video->thread_count = 1;
        video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(av_opt_set(video->priv_data, "preset", "veryfast", 0), "set x264 preset");
        check(av_opt_set(video->priv_data, "tune", "zerolatency", 0), "set x264 tune");
        check(avcodec_open2(video, video->codec, nullptr), "open H264 encoder");
        auto video_track = Factory::getTrackByCodecId(CodecH264);
        video_track->setIndex(0);
        splitH264(reinterpret_cast<const char *>(video->extradata), video->extradata_size,
            prefixSize(reinterpret_cast<const char *>(video->extradata), video->extradata_size),
            [&](const char *data, size_t bytes, size_t) {
                video_track->inputFrame(Factory::getFrameFromPtr(CodecH264, data, bytes, 0, 0));
            });
        if (!video_track->ready() || !_downstream->addTrack(video_track)) {
            throw std::runtime_error("register output H264 track failed");
        }

        _black = picture();
        _black->format = video->pix_fmt;
        _black->width = width;
        _black->height = height;
        check(av_frame_get_buffer(_black.get(), 32), "allocate black picture");
        for (int plane = 0; plane < 3; ++plane) {
            int rows = plane == 0 ? height : height / 2;
            for (int row = 0; row < rows; ++row) {
                std::memset(_black->data[plane] + row * _black->linesize[plane], plane == 0 ? 16 : 128,
                            plane == 0 ? width : width / 2);
            }
        }
        if (_options.audio) { openAudioOutput(); }
        _downstream->addTrackCompleted();
    }

    void openAudioOutput() {
        _audio_encoder = context(avcodec_find_encoder_by_name("aac"));
        auto audio = _audio_encoder.get();
        audio->sample_rate = 48000;
        audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio->time_base = AVRational{1, 48000};
        audio->bit_rate = 128000;
        audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_default(&audio->ch_layout, 2);
#else
        audio->channels = 2;
        audio->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
        check(avcodec_open2(audio, audio->codec, nullptr), "open AAC encoder");
        if (audio->frame_size <= 0) { throw std::runtime_error("AAC encoder has no fixed frame size"); }
        auto track = Factory::getTrackByCodecId(CodecAAC);
        track->setIndex(1);
        track->setExtraData(audio->extradata, audio->extradata_size);
        if (!track->ready() || !_downstream->addTrack(track)) { throw std::runtime_error("register output AAC track failed"); }
    }

    void onAudio(const Picture &decoded) {
        if (!_options.audio) { return; }
        // Reconfigure on source format changes; the output remains stereo/48kHz/FLTP.
#if LIBAVUTIL_VERSION_MAJOR >= 57
        std::string layout;
        char description[128];
        av_channel_layout_describe(&decoded->ch_layout, description, sizeof(description));
        layout = description;
#else
        std::string layout = std::to_string(decoded->channel_layout ? decoded->channel_layout :
                                                       av_get_default_channel_layout(decoded->channels));
#endif
        std::string format = layout + "/" + std::to_string(decoded->sample_rate) + "/" + std::to_string(decoded->format);
        if (!_resampler || format != _resample_format) {
            _resampler.reset();
            SwrContext *resampler = nullptr;
#if LIBAVUTIL_VERSION_MAJOR >= 57
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            check(swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLTP, 48000,
                  &decoded->ch_layout, static_cast<AVSampleFormat>(decoded->format), decoded->sample_rate, 0, nullptr), "create resampler");
#else
            resampler = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLTP, 48000,
                decoded->channel_layout ? decoded->channel_layout : av_get_default_channel_layout(decoded->channels),
                static_cast<AVSampleFormat>(decoded->format), decoded->sample_rate, 0, nullptr);
#endif
            _resampler.reset(resampler);
            if (!resampler) { throw std::bad_alloc(); }
            check(swr_init(resampler), "initialize resampler");
            _resample_format = std::move(format);
        }
        auto converted = picture();
        converted->format = AV_SAMPLE_FMT_FLTP;
        converted->sample_rate = 48000;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_default(&converted->ch_layout, 2);
#else
        converted->channel_layout = AV_CH_LAYOUT_STEREO;
        converted->channels = 2;
#endif
        check(swr_convert_frame(_resampler.get(), converted.get(), decoded.get()), "resample audio");
        int64_t start = static_cast<int64_t>(std::llround(mapTime(false, decoded->pts) * 48));
        if (start + converted->nb_samples <= _audio_sample_index) { return; }
        if (_audio_chunks.size() >= 256) { throw std::runtime_error("decoded audio queue overflow"); }
        auto position = std::upper_bound(_audio_chunks.begin(), _audio_chunks.end(), start,
            [](int64_t sample, const AudioChunk &item) { return sample < item.start; });
        _audio_chunks.insert(position, AudioChunk{start, std::move(converted)});
    }

    double videoMs() const { return _video_frame_index * 1000.0 / _options.fps; }
    double audioMs() const { return _audio_sample_index / 48.0; }

    void render(double end_ms) {
        if (!_started || _expired) { return; }
        double deadline = _last_valid_ms + _options.max_ms;
        end_ms = std::min(end_ms, deadline);
        if (end_ms - videoMs() > 2000) { throw std::runtime_error("encoding fell more than 2 seconds behind real time"); }
        while (videoMs() < end_ms || (_audio_encoder && audioMs() < end_ms)) {
            if (videoMs() < end_ms && (!_audio_encoder || videoMs() <= audioMs())) {
                double stamp = videoMs();
                while (!_pictures.empty() && _pictures.front().time_ms <= stamp + 0.5) {
                    _current = std::move(_pictures.front());
                    _pictures.pop_front();
                }
                // Hold the last picture through the configured jitter window.
                // A missing decode for one output tick must not flash black.
                // Longer outages still become black; the output clock continues.
                const double coverage = std::max<double>(_options.buffer_ms,
                    std::max(_input_frame_ms, 1000.0 / _options.fps)) + 2;
                bool real = _current.frame && stamp < _current.time_ms + coverage;
                if (real != _rendering_real) {
                    DebugL << "record_gap_fill: " << (real ? "picture recovered" : "black started")
                           << ", sink=" << this << ", output_ms=" << stamp
                           << ", picture_ms=" << _current.time_ms << ", pending=" << _pictures.size()
                           << ", video_received_ms=" << milliseconds(_start_time, _last_video_input);
                    _rendering_real = real;
                }
                auto selected = real ? _current.frame : _black;
                selected->pts = _video_frame_index++;
                encode(_video_encoder.get(), selected.get(), true);
            } else if (_audio_encoder && audioMs() < end_ms) {
                encodeAudio();
            } else {
                break;
            }
        }
        if (end_ms >= deadline) {
            drainEncoders();
            _downstream->flush();
            _expired = true;
            WarnL << "record_gap_fill: idle limit reached; recording paused until track reset or a new media source";
        }
    }

    void encodeAudio() {
        auto audio = picture();
        audio->format = AV_SAMPLE_FMT_FLTP;
        audio->sample_rate = 48000;
        audio->nb_samples = _audio_encoder->frame_size;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_default(&audio->ch_layout, 2);
#else
        audio->channels = 2;
        audio->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
        check(av_frame_get_buffer(audio.get(), 0), "allocate output audio");
        for (int channel = 0; channel < 2; ++channel) {
            std::memset(audio->data[channel], 0, audio->nb_samples * sizeof(float));
        }
        int64_t end = _audio_sample_index + audio->nb_samples;
        while (!_audio_chunks.empty() && _audio_chunks.front().start + _audio_chunks.front().frame->nb_samples <= _audio_sample_index) {
            _audio_chunks.pop_front();
        }
        for (auto &chunk : _audio_chunks) {
            if (chunk.start >= end) { break; }
            int64_t from = std::max(_audio_sample_index, chunk.start);
            int64_t to = std::min(end, chunk.start + chunk.frame->nb_samples);
            if (from >= to) { continue; }
            for (int channel = 0; channel < 2; ++channel) {
                std::memcpy(reinterpret_cast<float *>(audio->data[channel]) + from - _audio_sample_index,
                    reinterpret_cast<float *>(chunk.frame->data[channel]) + from - chunk.start,
                    (to - from) * sizeof(float));
            }
        }
        audio->pts = _audio_sample_index;
        _audio_sample_index = end;
        encode(_audio_encoder.get(), audio.get(), false);
    }

    void encode(AVCodecContext *encoder, AVFrame *frame, bool video) {
        int result = avcodec_send_frame(encoder, frame);
        if (result == AVERROR(EAGAIN)) {
            receiveEncoded(encoder, video);
            result = avcodec_send_frame(encoder, frame);
        }
        if (result != AVERROR_EOF) { check(result, "send output frame"); }
        receiveEncoded(encoder, video);
    }

    void receiveEncoded(AVCodecContext *encoder, bool video) {
        Packet packet(av_packet_alloc());
        if (!packet) { throw std::bad_alloc(); }
        while (true) {
            int result = avcodec_receive_packet(encoder, packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) { break; }
            check(result, "receive output packet");
            // AAC's negative priming packet cannot be represented by Frame's
            // unsigned timestamps. Drop it; normal output starts at zero.
            if (packet->pts >= 0 && packet->dts >= 0) {
                auto dts = av_rescale_q(packet->dts, encoder->time_base, AVRational{1, 1000});
                auto pts = av_rescale_q(packet->pts, encoder->time_base, AVRational{1, 1000});
                auto emit = [&](const char *data, size_t bytes) {
                    auto buffer = std::make_shared<BufferString>(std::string(data, bytes));
                    auto frame = Factory::getFrameFromBuffer(video ? CodecH264 : CodecAAC, buffer, dts, pts);
                    frame->setIndex(video ? 0 : 1);
                    _downstream->inputFrame(frame);
                };
                if (video) {
                    auto data = reinterpret_cast<const char *>(packet->data);
                    splitH264(data, packet->size, prefixSize(data, packet->size),
                        [&](const char *nal, size_t bytes, size_t) { emit(nal, bytes); });
                } else { emit(reinterpret_cast<const char *>(packet->data), packet->size); }
            }
            av_packet_unref(packet.get());
        }
    }

    void drainEncoders() {
        if (_video_encoder) { encode(_video_encoder.get(), nullptr, true); }
        if (_audio_encoder) { encode(_audio_encoder.get(), nullptr, false); }
    }

    void finish(Time end) {
        if (_expired) { return; }
        _merger.flush();
        if (!_started) { return; }
        if (_video_decoder) {
            avcodec_send_packet(_video_decoder.get(), nullptr);
            receiveDecoded(_video_decoder.get(), true);
        }
        if (_audio_decoder) {
            avcodec_send_packet(_audio_decoder.get(), nullptr);
            receiveDecoded(_audio_decoder.get(), false);
        }
        render(milliseconds(_start_time, end));
        if (!_expired) { drainEncoders(); }
        _downstream->flush();
    }

    void reset() {
        _merger.clear();
        _video_decoder.reset();
        _audio_decoder.reset();
        _video_encoder.reset();
        _audio_encoder.reset();
        _resampler.reset();
        _scaler.reset();
        _black.reset();
        _current = {};
        _pictures.clear();
        _audio_chunks.clear();
        _started = _expired = false;
        _rendering_real = true;
        _waiting_keyframe = true;
        _video_epoch_valid = _audio_epoch_valid = false;
        _last_video_input = _last_audio_input = {};
        _last_video_dts = _last_audio_dts = 0;
        _video_frame_index = _audio_sample_index = 0;
        _last_valid_ms = 0;
        _late_video_frames = 0;
    }

    MediaSinkInterface::Ptr _downstream;
    Options _options;
    std::mutex _mutex;
    std::condition_variable _condition;
    std::deque<Event> _events;
    size_t _queued_bytes = 0;
    bool _startup_queue = true; // Protected by _mutex; reset on queued track reset.
    bool _stopping = false;
    std::atomic<bool> _failed{false};
    Time _stop_time;
    std::thread _thread;

    Context _video_decoder, _audio_decoder, _video_encoder, _audio_encoder;
    std::unique_ptr<SwrContext, SwrFree> _resampler;
    std::unique_ptr<SwsContext, SwsFree> _scaler;
    std::string _resample_format;
    FrameMerger _merger{FrameMerger::h264_prefix};
    int _video_index = -1, _audio_index = -1;
    int _width = 0, _height = 0;
    bool _started = false, _expired = false, _waiting_keyframe = true;
    bool _rendering_real = true;
    bool _video_epoch_valid = false, _audio_epoch_valid = false;
    int64_t _video_epoch_source = 0, _audio_epoch_source = 0;
    double _video_epoch_ms = 0, _audio_epoch_ms = 0;
    Time _start_time, _received, _last_video_input, _last_audio_input;
    int64_t _last_video_dts = 0, _last_audio_dts = 0;
    double _last_valid_ms = 0, _input_frame_ms = 40;
    int64_t _video_frame_index = 0, _audio_sample_index = 0;
    size_t _late_video_frames = 0;
    Picture _black;
    TimedPicture _current{};
    std::deque<TimedPicture> _pictures;
    std::deque<AudioChunk> _audio_chunks;
};
#endif
} // namespace

MediaSinkInterface::Ptr wrapRecordGapFiller(MediaSinkInterface::Ptr recorder,
                                          uint32_t retention_ms,
                                          bool audio_enabled) {
    auto &ini = mINI::Instance();
    if (!ini[kEnable].as<bool>()) { return recorder; }
#if defined(ENABLE_MP4) && defined(ENABLE_FFMPEG)
    auto buffer_ms = ini[kDetect].as<int>();
    auto max_ms = ini[kMax].as<int>();
    auto fps = ini[kFps].as<int>();
    auto bitrate = ini[kBitrate].as<int>();
    if (!recorder || retention_ms == 0 || buffer_ms < 40 || buffer_ms > 2000 ||
        max_ms <= buffer_ms || max_ms > 600000 || fps < 5 || fps > 60 || bitrate < 64000 || bitrate > 100000000) {
        throw std::invalid_argument("record_gap_fill: invalid settings; require retention>0, detect_ms=40..2000, max_ms>detect_ms and <=600000, fps=5..60, bitrate=64000..100000000");
    }
    if (!avcodec_find_encoder_by_name("libx264") || (audio_enabled && !avcodec_find_encoder_by_name("aac"))) {
        throw std::invalid_argument("record_gap_fill requires FFmpeg libx264 and AAC encoders");
    }
    // This is a local idle cap, NOT the RTMP disconnect deadline: their start
    // times differ. Source destruction always ends the wrapper independently.
    max_ms = static_cast<int>(std::min<uint32_t>(max_ms, retention_ms));
    if (max_ms <= buffer_ms) { throw std::invalid_argument("record_gap_fill: retention must exceed detect_ms"); }
    return std::make_shared<RecordGapFiller>(std::move(recorder), Options{buffer_ms, max_ms, fps, bitrate, audio_enabled});
#else
    (void)retention_ms;
    (void)audio_enabled;
    throw std::invalid_argument("record_gap_fill requires ENABLE_MP4 and ENABLE_FFMPEG; feature is disabled by default");
#endif
}


#if defined(ENABLE_HLS) && defined(ENABLE_MP4) && defined(ENABLE_FFMPEG)
namespace {

// 状态独立于 HlsRecorder；输出任务不会持有外层裸 this。
struct TsDelivery : public std::enable_shared_from_this<TsDelivery> {
    TsDelivery(toolkit::EventPoller::Ptr executor,
               TsGapInput::OnPacket callback)
        : owner(std::move(executor)),
          output(std::move(callback)) {}

    void post(toolkit::Buffer::Ptr buffer,
              uint64_t stamp, bool key) {
        const size_t max_bytes = 32 * 1024 * 1024;
        const size_t bytes = buffer ? buffer->size() : 0;
        const size_t old_count = pending.fetch_add(1);
        const size_t old_bytes = pending_bytes.fetch_add(bytes);

        if (failed.load() || old_count >= 256 ||
            bytes > max_bytes || old_bytes > max_bytes - bytes) {
            pending.fetch_sub(1);
            pending_bytes.fetch_sub(bytes);
            failed = true;
            throw std::runtime_error("TS output queue full or failed");
        }

        auto self = shared_from_this();
        try {
            owner->async([self, buffer, stamp, key, bytes]() {
                try {
                    if (!self->failed.load()) {
                        self->output(buffer, stamp, key);
                    }
                } catch (const std::exception &ex) {
                    self->failed = true;
                    ErrorL << "TS output failed: " << ex.what();
                } catch (...) {
                    self->failed = true;
                    ErrorL << "TS output failed: unknown exception";
                }
                self->pending.fetch_sub(1);
                self->pending_bytes.fetch_sub(bytes);
            }, false);
        } catch (...) {
            pending.fetch_sub(1);
            pending_bytes.fetch_sub(bytes);
            failed = true;
            throw;
        }
    }

    toolkit::EventPoller::Ptr owner;
    TsGapInput::OnPacket output;
    std::atomic<size_t> pending {0};
    std::atomic<size_t> pending_bytes {0};
    std::atomic<bool> failed {false};
};

// 只在补帧线程操作；不访问 HlsRecorder/HlsMakerImp。
class GapTsSink final : public MpegMuxer {
public:
    explicit GapTsSink(std::shared_ptr<TsDelivery> delivery)
        : _delivery(std::move(delivery)) {}

    ~GapTsSink() override {
        try {
            MpegMuxer::flush();
        } catch (const std::exception &ex) {
            WarnL << ex.what();
        }
    }

    bool addTrack(const Track::Ptr &track) override {
        if (track->getTrackType() == TrackVideo) {
            auto video = std::dynamic_pointer_cast<VideoTrack>(track);
            if (!video) {
                throw std::runtime_error("invalid TS video track");
            }
            _video_config = video->getConfigFrames();
        }

        if (track->getCodecId() == CodecAAC) {
            _aac = std::dynamic_pointer_cast<AACTrack>(track->clone());
            if (!_aac || !_aac->ready()) {
                throw std::runtime_error("AAC output config not ready");
            }
            _aac->addDelegate([this](const Frame::Ptr &frame) {
                return MpegMuxer::inputFrame(frame);
            });
        }

        return MpegMuxer::addTrack(track);
    }

    bool inputFrame(const Frame::Ptr &frame) override {
        if (frame->getCodecId() == CodecAAC &&
            frame->prefixSize() == 0) {
            if (!_aac) {
                throw std::runtime_error("missing AAC output track");
            }
            return _aac->inputFrame(frame);
        }

        if (frame->getTrackType() == TrackVideo && frame->keyFrame()) {
            for (const auto &config : _video_config) {
                auto buffer = std::make_shared<toolkit::BufferString>(
                    std::string(config->data(), config->size()));
                auto header = Factory::getFrameFromBuffer(
                    frame->getCodecId(), buffer,
                    frame->dts(), frame->pts());
                header->setIndex(frame->getIndex());
                MpegMuxer::inputFrame(header);
            }
        }
        return MpegMuxer::inputFrame(frame);
    }

    void resetTracks() override {
        MpegMuxer::flush();
        MpegMuxer::resetTracks();
        _aac.reset();
        _video_config.clear();
    }

private:
    void onWrite(toolkit::Buffer::Ptr buffer,
                 uint64_t stamp, bool key) override {
        _delivery->post(std::move(buffer), stamp, key);
    }

    std::shared_ptr<TsDelivery> _delivery;
    AACTrack::Ptr _aac;
    std::vector<Frame::Ptr> _video_config;
};

// 前端方法由流所属线程调用。
class TsGapInputImpl final : public TsGapInput {
public:
    TsGapInputImpl(uint32_t retention_ms, bool audio)
        : _retention_ms(retention_ms), _audio(audio) {}

    ~TsGapInputImpl() override {
        stop();
    }

    void bindOutput(const toolkit::EventPoller::Ptr &owner,
                    OnPacket output) override {
        if (_stopped) {
            throw std::runtime_error("TS gap input already stopped");
        }
        if (_delivery) {
            return; // 重复设置监听器不重建处理线程。
        }
        if (!owner || !output) {
            throw std::invalid_argument("invalid TS output binding");
        }
        _delivery = std::make_shared<TsDelivery>(
            owner, std::move(output));
        startIfReady();
    }

    bool addTrack(const Track::Ptr &track) override {
        if (_stopped) {
            return false;
        }
        if (_input) {
            return _input->addTrack(track);
        }
        _tracks.emplace_back(track->clone());
        return true;
    }

    void addTrackCompleted() override {
        if (_stopped) {
            return;
        }
        _tracks_ready = true;
        if (_input) {
            _input->addTrackCompleted();
        } else {
            startIfReady();
        }
    }

    bool inputFrame(const Frame::Ptr &frame) override {
        if (_stopped || (_delivery && _delivery->failed.load())) {
            return false;
        }
        if (_input) {
            return _input->inputFrame(frame);
        }

        const size_t max_bytes = 32 * 1024 * 1024;
        if (_pending.size() >= kStartupInputEvents ||
            frame->size() > max_bytes - _pending_bytes) {
            throw std::runtime_error("TS startup queue full");
        }
        _pending.emplace_back(Frame::getCacheAbleFrame(frame));
        _pending_bytes += frame->size();
        return true;
    }

    void resetTracks() override {
        if (_stopped) {
            return;
        }
        _tracks.clear();
        _pending.clear();
        _pending_bytes = 0;
        _tracks_ready = false;
        if (_input) {
            _input->resetTracks();
        }
    }

    void flush() override {
        if (_input) {
            _input->flush();
        }
    }

    void stop() noexcept override {
        if (_stopped) {
            return;
        }
        _stopped = true;
        // 现有 RecordGapFiller 的析构会停止并 join 工作线程。
        // 工作线程不会等待输出 EventPoller，因此没有互相等待。
        _input.reset();
        _tracks.clear();
        _pending.clear();
        _pending_bytes = 0;
        // 已排队任务持有自己的 delivery 和输出目标引用。
        _delivery.reset();
    }

private:
    void startIfReady() {
        if (_stopped || _input || !_delivery || !_tracks_ready) {
            return;
        }

        auto sink = std::make_shared<GapTsSink>(_delivery);
        auto input = wrapRecordGapFiller(
            sink, _retention_ms, _audio);

        for (const auto &track : _tracks) {
            if (!input->addTrack(track)) {
                throw std::runtime_error("TS input rejected track");
            }
        }
        input->addTrackCompleted();

        for (const auto &frame : _pending) {
            if (!input->inputFrame(frame)) {
                throw std::runtime_error("TS input queue full");
            }
        }

        _input = std::move(input);
        _tracks.clear();
        _pending.clear();
        _pending_bytes = 0;
    }

    uint32_t _retention_ms;
    bool _audio;
    bool _stopped = false;
    bool _tracks_ready = false;
    std::shared_ptr<TsDelivery> _delivery;
    MediaSinkInterface::Ptr _input;
    std::vector<Track::Ptr> _tracks;
    std::deque<Frame::Ptr> _pending;
    size_t _pending_bytes = 0;
};

} // namespace
#endif

TsGapInput::Ptr createTsGapInput(
    uint32_t retention_ms, bool audio_enabled) {
    if (!toolkit::mINI::Instance()
             ["record_gap_fill.enable"].as<bool>()) {
        return nullptr;
    }
#if defined(ENABLE_HLS) && defined(ENABLE_MP4) && defined(ENABLE_FFMPEG)
    return std::make_shared<TsGapInputImpl>(
        retention_ms, audio_enabled);
#else
    throw std::invalid_argument(
        "TS gap fill requires ENABLE_HLS, ENABLE_MP4 and ENABLE_FFMPEG");
#endif
}

} // namespace mediakit
