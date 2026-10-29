/* Copyright (c) ZLMediaKit contributors. SPDX-License-Identifier: MIT */
#include "Extension/RecordGapFiller.h"
#include "Extension/Factory.h"
#include "Common/config.h"
#include "ext-codec/H264.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(__linux__) && !defined(ANDROID)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(ENABLE_MP4) && defined(ENABLE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

using namespace mediakit;
using namespace toolkit;

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Capture final : public MediaSinkInterface {
public:
    bool addTrack(const Track::Ptr &track) override {
        if (before_track) { before_track(); }
#if defined(__linux__) && !defined(ANDROID)
        worker_policy = sched_getscheduler(0);
        require(sched_getaffinity(0, sizeof(worker_cpus), &worker_cpus) == 0,
                "cannot inspect worker CPU affinity");
#endif
        tracks.emplace_back(track->clone());
        return true;
    }
    bool inputFrame(const Frame::Ptr &frame) override {
        frames.emplace_back(Frame::getCacheAbleFrame(frame));
        return true;
    }
    void resetTracks() override { ++resets; }
    void flush() override { ++flushes; }

    std::vector<Track::Ptr> tracks;
    std::vector<Frame::Ptr> frames;
    int resets = 0;
    int flushes = 0;
    std::function<void()> before_track;
#if defined(__linux__) && !defined(ANDROID)
    int worker_policy = -1;
    cpu_set_t worker_cpus{};
#endif
};

#if defined(ENABLE_MP4) && defined(ENABLE_FFMPEG)

std::string source() {
    const std::string hex =
        "000000016742c00adc426c0440000003004000000ca3c489e0"
        "0000000168ce0fc8"
        "0000016588843a26280008f649c9c9d75d75d75d75d75d75e0";
    std::string bytes;
    for (size_t index = 0; index < hex.size(); index += 2) {
        bytes.push_back(static_cast<char>(std::stoi(hex.substr(index, 2), nullptr, 16)));
    }
    return bytes;
}

Track::Ptr inputTrack() {
    auto track = Factory::getTrackByCodecId(CodecH264);
    auto bytes = source();
    splitH264(bytes.data(), bytes.size(), 4,
        [&](const char *data, size_t size, size_t) {
            track->inputFrame(Factory::getFrameFromPtr(CodecH264, data, size, 0, 0));
        });
    track->setIndex(0);
    require(track->ready(), "H.264 fixture is not ready");
    return track;
}

void feed(const MediaSinkInterface::Ptr &sink, int count) {
    auto bytes = source();
    auto begin = std::chrono::steady_clock::now();
    for (int index = 0; index < count; ++index) {
        splitH264(bytes.data(), bytes.size(), 4,
            [&](const char *data, size_t size, size_t) {
                auto buffer = std::make_shared<BufferString>(std::string(data, size));
                auto frame = Factory::getFrameFromBuffer(CodecH264, buffer, index * 40, index * 40);
                frame->setIndex(0);
                require(sink->inputFrame(frame), "normal input rejected");
            });
        std::this_thread::sleep_until(begin + std::chrono::milliseconds((index + 1) * 40));
    }
}

size_t countBlackFrames(const Capture &capture) {
    auto codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    std::unique_ptr<AVCodecContext, std::function<void(AVCodecContext *)>> decoder(
        avcodec_alloc_context3(codec), [](AVCodecContext *value) { avcodec_free_context(&value); });
    require(decoder && avcodec_open2(decoder.get(), codec, nullptr) == 0, "open output decoder");
    std::string headers;
    auto video = std::dynamic_pointer_cast<VideoTrack>(capture.tracks.at(0));
    for (const auto &config : video->getConfigFrames()) {
        headers.append(config->data(), config->size());
    }
    size_t black = 0, pictures = 0;
    auto receive = [&]() {
        AVFrame *frame = av_frame_alloc();
        while (avcodec_receive_frame(decoder.get(), frame) == 0) {
            ++pictures;
            if (frame->data[0][0] < 30) { ++black; }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
    };
    FrameMerger merger(FrameMerger::h264_prefix);
    bool first = true;
    auto decode = [&](uint64_t, uint64_t, const Buffer::Ptr &buffer, bool) {
        std::string bytes = first ? headers : std::string();
        first = false;
        bytes.append(buffer->data(), buffer->size());
        AVPacket *packet = av_packet_alloc();
        require(av_new_packet(packet, bytes.size()) == 0, "allocate output packet");
        std::memcpy(packet->data, bytes.data(), bytes.size());
        auto result = avcodec_send_packet(decoder.get(), packet);
        av_packet_free(&packet);
        require(result >= 0, "decode output packet");
        receive();
    };
    for (const auto &frame : capture.frames) {
        if (frame->getTrackType() == TrackVideo) {
            merger.inputFrame(frame, decode);
        }
    }
    merger.flush();
    require(avcodec_send_packet(decoder.get(), nullptr) == 0, "drain output decoder");
    receive();
    require(pictures >= 10, "too few decoded output pictures");
    return black;
}

void testContinuousLowFrameRate() {
    auto &ini = mINI::Instance();
    ini["record_gap_fill.detect_ms"] = 200;
    auto capture = std::make_shared<Capture>();
    auto sink = wrapRecordGapFiller(capture, 2000, false);
    sink->addTrack(inputTrack());
    sink->addTrackCompleted();
    auto bytes = source();
    auto begin = std::chrono::steady_clock::now();
    for (int index = 0; index < 12; ++index) {
        // SPS advertises 25 fps, but receive and media cadence are 12.5 fps.
        // A real source may have stale FPS metadata or short frame gaps.
        splitH264(bytes.data(), bytes.size(), 4,
            [&](const char *data, size_t size, size_t) {
                auto buffer = std::make_shared<BufferString>(std::string(data, size));
                auto frame = Factory::getFrameFromBuffer(CodecH264, buffer, index * 80, index * 80);
                frame->setIndex(0);
                require(sink->inputFrame(frame), "low frame rate input rejected");
            });
        std::this_thread::sleep_until(begin + std::chrono::milliseconds((index + 1) * 80));
    }
    sink.reset();
    require(countBlackFrames(*capture) == 0, "continuous low frame rate flashed black");
    ini["record_gap_fill.detect_ms"] = 80;
}

void testCachedGopRemapping() {
    auto &ini = mINI::Instance();
    ini["record_gap_fill.detect_ms"] = 200;
    auto capture = std::make_shared<Capture>();
    auto sink = wrapRecordGapFiller(capture, 2000, false);
    sink->addTrack(inputTrack());
    sink->addTrackCompleted();
    std::string key;
    auto bytes = source();
    splitH264(bytes.data(), bytes.size(), 4,
        [&](const char *data, size_t size, size_t) {
            if (Factory::getFrameFromPtr(CodecH264, data, size, 0, 0)->keyFrame()) {
                key.assign(data, size);
            }
        });
    require(!key.empty(), "missing cached GOP fixture");
    auto send = [&](int index) {
        auto frame = Factory::getFrameFromBuffer(CodecH264,
            std::make_shared<BufferString>(key), index * 40, index * 40);
        frame->setIndex(0);
        require(sink->inputFrame(frame), "cached GOP or following live frame rejected");
    };
    // API recording replays several seconds of GOP media within milliseconds.
    // Pictures mapped before a clock reanchor must not accumulate indefinitely.
    for (int index = 0; index < 180; ++index) {
        send(index);
        if (index % 20 == 19) {
            // Retaining metadata is fast but not instantaneous. Reanchoring
            // several milliseconds after startup must still cover output t=0.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    auto begin = std::chrono::steady_clock::now();
    for (int index = 0; index < 16; ++index) {
        send(180 + index);
        std::this_thread::sleep_until(begin + std::chrono::milliseconds((index + 1) * 40));
    }
    sink.reset();
    require(countBlackFrames(*capture) == 0, "cached GOP remapping stopped or flashed black");
    ini["record_gap_fill.detect_ms"] = 80;
}

void testStartupInputBurst() {
    auto &ini = mINI::Instance();
    ini["record_gap_fill.detect_ms"] = 200;
    auto capture = std::make_shared<Capture>();
    std::promise<void> opening, release;
    auto opened = opening.get_future();
    auto resume = release.get_future().share();
    capture->before_track = [&opening, resume] {
        opening.set_value();
        resume.wait();
    };
    auto sink = wrapRecordGapFiller(capture, 2000, false);
    // Always release the blocked worker before the sink is destroyed.
    auto unblock = std::shared_ptr<void>(nullptr, [&release](void *) { release.set_value(); });
    sink->addTrack(inputTrack());
    sink->addTrackCompleted();
    std::string key;
    auto bytes = source();
    splitH264(bytes.data(), bytes.size(), 4,
        [&](const char *data, size_t size, size_t) {
            if (Factory::getFrameFromPtr(CodecH264, data, size, 0, 0)->keyFrame()) {
                key.assign(data, size);
            }
        });
    require(!key.empty(), "missing startup burst fixture");
    auto send = [&](int index) {
        auto frame = Factory::getFrameFromBuffer(CodecH264,
            std::make_shared<BufferString>(key), index * 40, index * 40);
        frame->setIndex(0);
        return sink->inputFrame(frame);
    };
    require(send(0) && send(1), "startup trigger rejected");
    require(opened.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "output initialization did not start");
    // Simulate input GOP replay while codec/downstream initialization is busy.
    // A temporary startup burst must not be mistaken for sustained overload.
    bool accepted = true;
    for (int index = 2; index < 600; ++index) { accepted = send(index) && accepted; }
    unblock.reset();
    require(accepted, "valid startup GOP burst rejected");
    auto begin = std::chrono::steady_clock::now();
    for (int index = 0; index < 16; ++index) {
        require(send(600 + index), "live input after startup burst rejected");
        std::this_thread::sleep_until(begin + std::chrono::milliseconds((index + 1) * 40));
    }
    sink.reset();
    require(countBlackFrames(*capture) == 0, "startup burst stopped or flashed black");
    ini["record_gap_fill.detect_ms"] = 80;
}

#if defined(__linux__) && !defined(ANDROID)
void testWorkerScheduling() {
    std::promise<void> result;
    auto done = result.get_future();
    std::thread parent([&result] {
        try {
            cpu_set_t allowed;
            require(sched_getaffinity(0, sizeof(allowed), &allowed) == 0,
                    "cannot inspect parent CPU affinity");
            cpu_set_t pinned;
            CPU_ZERO(&pinned);
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (CPU_ISSET(cpu, &allowed)) { CPU_SET(cpu, &pinned); break; }
            }
            require(pthread_setaffinity_np(pthread_self(), sizeof(pinned), &pinned) == 0,
                    "cannot pin test parent thread");
            // SCHED_BATCH is available without root and exercises inherited
            // scheduling policy. Production pollers can instead use FIFO/99.
            sched_param priority{};
            require(pthread_setschedparam(pthread_self(), SCHED_BATCH, &priority) == 0,
                    "cannot set test parent scheduling policy");
            auto capture = std::make_shared<Capture>();
            auto sink = wrapRecordGapFiller(capture, 2000, false);
            sink->addTrack(inputTrack());
            sink->addTrackCompleted();
            feed(sink, 4);
            sink.reset();
            require(capture->worker_policy == SCHED_OTHER,
                    "encoding worker inherited parent scheduling policy");
            if (CPU_COUNT(&allowed) > 1) {
                require(CPU_COUNT(&capture->worker_cpus) > 1,
                        "encoding worker inherited single-core poller affinity");
            }
            result.set_value();
        } catch (...) { result.set_exception(std::current_exception()); }
    });
    parent.join();
    done.get();
}
#endif

void testLimitAndReset() {
    auto &ini = mINI::Instance();
    ini["record_gap_fill.max_ms"] = 400;
    auto capture = std::make_shared<Capture>();
    auto sink = wrapRecordGapFiller(capture, 2000, false);
    sink->addTrack(inputTrack());
    sink->addTrackCompleted();
    feed(sink, 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    feed(sink, 4); // Expired timeline must not silently resume.
    sink->resetTracks();
    sink->addTrack(inputTrack());
    sink->addTrackCompleted();
    feed(sink, 4);
    sink.reset(); // All Capture access below happens after join().
    require(capture->resets == 1, "track reset not forwarded exactly once");
    require(capture->tracks.size() == 2, "reset did not establish a new output track");
    require(capture->flushes > 0, "output was not drained");
    uint64_t previous = 0;
    int rollbacks = 0;
    size_t previous_epoch_frames = 0;
    for (size_t index = 0; index < capture->frames.size(); ++index) {
        const auto &frame = capture->frames[index];
        if (frame->dts() < previous) {
            ++rollbacks;
            previous_epoch_frames = index;
        }
        previous = frame->dts();
    }
    require(rollbacks == 1, "only explicit reset may restart timestamps");
    require(previous_epoch_frames > 0, "first recording epoch is empty");
    require(capture->frames[previous_epoch_frames - 1]->dts() < 800,
            "idle cap failed to stop the expired timeline");
    ini["record_gap_fill.max_ms"] = 2000;
}

#if defined(ENABLE_HLS)
void testTsStartupAndStop() {
    auto sink = createTsGapInput(2000, false);
    require(sink != nullptr, "enabled TS adapter missing");
    sink->addTrack(inputTrack());
    sink->addTrackCompleted();
    feed(sink, 4); // Startup/GOP frames may precede listener binding.

    auto poller = EventPollerPool::Instance().getPoller();
    auto packets = std::make_shared<std::atomic<size_t>>(0);
    auto invalid = std::make_shared<std::atomic<bool>>(false);
    sink->bindOutput(poller, [packets, invalid](Buffer::Ptr buffer, uint64_t, bool) {
        if (!buffer) {
            return;
        }
        if (buffer->size() % 188 != 0) {
            *invalid = true;
        }
        for (size_t offset = 0; offset < buffer->size(); offset += 188) {
            if (static_cast<unsigned char>(buffer->data()[offset]) != 0x47) {
                *invalid = true;
            }
        }
        ++*packets;
    });
    feed(sink, 4);
    sink->resetTracks();
    sink->addTrack(inputTrack());
    sink->addTrackCompleted();
    feed(sink, 4);
    sink->stop();
    sink->stop();
    require(!sink->addTrack(inputTrack()), "stopped adapter accepted a track");

    auto barrier = std::make_shared<std::promise<void>>();
    auto done = barrier->get_future();
    poller->async([barrier]() { barrier->set_value(); }, false);
    require(done.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "TS delivery did not drain");
    require(packets->load() > 0 && !invalid->load(), "invalid or missing MPEG-TS output");
}
#endif
#endif

} // namespace

int main() {
    try {
        EventPollerPool::setPoolSize(1);
        auto &ini = mINI::Instance();
        ini["record_gap_fill.enable"] = false;
        auto capture = std::make_shared<Capture>();
        require(wrapRecordGapFiller(capture, 2000, true) == capture,
                "disabled wrapper changed the sink");
        require(!createTsGapInput(2000, true), "disabled TS adapter is not null");
        ini["record_gap_fill.enable"] = true;
        ini["record_gap_fill.detect_ms"] = 80;
        ini["record_gap_fill.max_ms"] = 2000;
        ini["record_gap_fill.fps"] = 25;
#if defined(ENABLE_MP4) && defined(ENABLE_FFMPEG)
        bool invalid = false;
        try {
            wrapRecordGapFiller(capture, 0, false);
        } catch (const std::invalid_argument &) {
            invalid = true;
        }
        require(invalid, "zero retention was not rejected");
#if defined(__linux__) && !defined(ANDROID)
        testWorkerScheduling();
#endif
        testContinuousLowFrameRate();
        testCachedGopRemapping();
        testStartupInputBurst();
        testLimitAndReset();
#if defined(ENABLE_HLS)
        testTsStartupAndStop();
#endif
#else
        bool unavailable = false;
        try {
            wrapRecordGapFiller(capture, 2000, false);
        } catch (const std::invalid_argument &) {
            unavailable = true;
        }
        require(unavailable, "enabled feature without codecs must fail explicitly");
#endif
        std::cout << "record gap fill regressions passed\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "record gap fill regression failed: " << ex.what() << '\n';
        return 1;
    }
}
