/* Copyright (c) ZLMediaKit contributors. SPDX-License-Identifier: MIT */
#include "Extension/HlsArchive.h"
#include "Util/File.h"
#include "json/json.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace mediakit;
using namespace toolkit;
namespace {
void require(bool condition, const char *message) { if (!condition) { throw std::runtime_error(message); } }
void wait(const std::function<bool()> &test) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!test()) {
        if (std::chrono::steady_clock::now() > deadline) { throw std::runtime_error("archive test timed out"); }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
struct Temporary {
    std::string path = exeDir() + "hls-archive-test-" + makeRandStr(12, true);
    ~Temporary() { File::delete_file(path, false, false); }
    HlsArchive::Options options() const {
        HlsArchive::Options result;
        result.scan_root = path + "/recordings";
        result.spool_path = path + "/jobs";
        result.destination = "unit-test-storage/bucket";
        result.scan_interval_ms = 30;
        result.retry_delay_ms = 30;
        result.upload_timeout_ms = 1000;
        return result;
    }
};
std::string playlist(const std::vector<std::string> &segments, bool ended = true) {
    std::string result = "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-PLAYLIST-TYPE:EVENT\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n";
    for (const auto &path : segments) { result += "#EXTINF:1.25,\n" + path + "\n"; }
    if (ended) { result += "#EXT-X-ENDLIST\n"; }
    return result;
}
void write(const std::string &path, const std::string &data) {
    require(File::create_path(path, 0750) && File::saveFile(data, path), "cannot create archive fixture");
}
std::vector<Json::Value> tasks(const HlsArchive::Options &options) {
    std::vector<Json::Value> result;
    File::scanDir(options.spool_path, [&](const std::string &path, bool dir) {
        if (!dir && path.size() > 5 && path.substr(path.size() - 5) == ".json") {
            Json::Reader reader;
            Json::Value value;
            require(reader.parse(File::loadFile(path), value), "invalid task state");
            result.emplace_back(std::move(value));
        }
        return true;
    });
    return result;
}
bool ready(const HlsArchive::Options &options, size_t count = 1) {
    auto values = tasks(options);
    if (values.size() != count) { return false; }
    for (const auto &value : values) { if (value["status"].asString() != "ready") { return false; } }
    return true;
}
struct Storage {
    std::mutex mutex;
    std::vector<HlsArchive::Object> objects;
    bool fail_once = false;
    HlsArchive::Upload uploader() {
        return [this](HlsArchive::Object object, HlsArchive::Done done) {
            bool fail;
            {
                std::lock_guard<std::mutex> lock(mutex);
                fail = fail_once && object.key.find("second.ts") != std::string::npos;
                if (fail) { fail_once = false; }
                objects.emplace_back(std::move(object));
            }
            done(fail ? "HTTP 503 injected failure" : "");
        };
    }
    size_t size() { std::lock_guard<std::mutex> lock(mutex); return objects.size(); }
};

void testCompleteOrderAndRetry() {
    Temporary temp;
    auto options = temp.options();
    Storage storage;
    storage.fail_once = true;
    HlsArchive archive(options, storage.uploader());
    auto root = options.scan_root + "/round + 中文/live/test";
    const std::vector<std::string> paths {"2026-10-30/23/first.ts", "2026-10-31/00/second.ts"};
    write(root + "/" + paths[0], "first media bytes");
    write(root + "/" + paths[1], "second media bytes");
    write(root + "/hls.m3u8", playlist(paths, false));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(storage.size() == 0, "uploaded an unfinished playlist");
    auto final = playlist(paths);
    write(root + "/hls.m3u8", final);
    wait([&] { return ready(options); });
    archive.stop();
    require(storage.size() == 4, "unexpected retry/upload count");
    require(storage.objects[0].key == "hls/round + 中文/live/test/" + paths[0], "relative storage path changed");
    require(storage.objects[0].content == "first media bytes", "segment content changed");
    require(storage.objects[1].key == storage.objects[2].key, "retry changed object key");
    require(storage.objects.back().content == final, "playlist snapshot changed");
    require(storage.objects.back().content_type == "application/vnd.apple.mpegurl", "playlist MIME type missing");
    require(storage.objects.front().content_type == "video/mp2t", "TS MIME type missing");
    require(archive.ownsFile(root + "/" + paths[0]), "archive path not recognized");
    require(!archive.ownsFile(options.scan_root + "-other/a.ts"), "directory boundary incorrectly matched");
    auto count = storage.size();
    { HlsArchive restarted(options, storage.uploader()); std::this_thread::sleep_for(std::chrono::milliseconds(150)); }
    require(storage.size() == count, "ready archive uploaded again after restart");
    std::cout << "PASS: final playlist, preserved paths/content, retry, publication order and ready recovery\n";
}

void testInvalidAndMissingFiles() {
    Temporary temp;
    auto options = temp.options();
    Storage storage;
    HlsArchive archive(options, storage.uploader());
    write(options.scan_root + "/missing/live/test/hls.m3u8", playlist({"missing.ts"}));
    write(options.scan_root + "/escape/live/test/hls.m3u8", playlist({"../outside.ts"}));
    write(options.scan_root + "/key/live/test/a.ts", "media");
    auto encrypted = playlist({"a.ts"});
    encrypted.insert(encrypted.find("#EXTINF:"), "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n");
    write(options.scan_root + "/key/live/test/hls.m3u8", encrypted);
    auto rolling = playlist({"a.ts"});
    rolling.erase(rolling.find("#EXT-X-PLAYLIST-TYPE:EVENT\n"), std::string("#EXT-X-PLAYLIST-TYPE:EVENT\n").size());
    write(options.scan_root + "/rolling/live/test/hls.m3u8", rolling);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    require(storage.size() == 0, "invalid/missing source published an object");
    write(options.scan_root + "/missing/live/test/missing.ts", "late arriving file");
    wait([&] { return ready(options); });
    archive.stop();
    require(storage.size() == 2, "missing source did not recover");
    std::cout << "PASS: missing TS recovery, traversal, encryption and rolling playlist rejection\n";
}

void testStopAndResume() {
    Temporary temp;
    auto options = temp.options();
    auto root = options.scan_root + "/resume/live/test";
    std::mutex mutex;
    HlsArchive::Done late;
    std::vector<std::string> before;
    {
        HlsArchive archive(options, [&](HlsArchive::Object object, HlsArchive::Done done) {
            bool hold = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                before.push_back(object.key);
                hold = before.size() == 2;
                if (hold) { late = done; }
            }
            if (!hold) { done(""); }
        });
        write(root + "/a.ts", "first"); write(root + "/b.ts", "second");
        write(root + "/hls.m3u8", playlist({"a.ts", "b.ts"}));
        wait([&] { std::lock_guard<std::mutex> lock(mutex); return static_cast<bool>(late); });
        auto start = std::chrono::steady_clock::now();
        archive.stop();
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "shutdown waited for network timeout");
    }
    late(""); // Must not touch a destroyed archive.
    Storage storage;
    { HlsArchive restarted(options, storage.uploader()); wait([&] { return ready(options); }); }
    require(storage.size() == 2 && storage.objects.front().key.find("b.ts") != std::string::npos,
            "restart lost the successful checkpoint or skipped interrupted upload");
    require(storage.objects.back().key.find("hls.m3u8") != std::string::npos, "restart published playlist early");
    std::cout << "PASS: shutdown cancellation, late callback lifetime and persistent resume\n";
}

void testCleanupAndMutation() {
    Temporary temp;
    auto options = temp.options();
    options.delete_local_after_archive = true;
    auto root = options.scan_root + "/cleanup/live/test";
    Storage storage;
    {
        HlsArchive archive(options, storage.uploader());
        write(root + "/a.ts", "media"); write(root + "/hls.m3u8", playlist({"a.ts"}));
        wait([&] { return ready(options) && !File::fileExist(root + "/hls.m3u8"); });
    }
    require(!File::fileExist(root + "/a.ts"), "cleanup retained completed TS");
    options.delete_local_after_archive = false;
    options.retry_max = 2;
    auto changed = options.scan_root + "/mutation/live/test";
    {
        HlsArchive archive(options, [&](HlsArchive::Object object, HlsArchive::Done done) {
            if (object.content_type == "video/mp2t") { write(changed + "/hls.m3u8", playlist({"replaced.ts"})); }
            done("");
        });
        write(changed + "/a.ts", "original"); write(changed + "/hls.m3u8", playlist({"a.ts"}));
        wait([&] {
            for (auto &task : tasks(options)) { if (task["status"].asString() == "failed") { return true; } }
            return false;
        });
    }
    require(File::fileExist(changed + "/a.ts"), "failed archive deleted source");
    std::cout << "PASS: cleanup only after ready and rejection of reused recording directory\n";
}
} // namespace

int main() {
    try {
        testCompleteOrderAndRetry();
        testInvalidAndMissingFiles();
        testStopAndResume();
        testCleanupAndMutation();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "HLS archive test failed: " << error.what() << std::endl;
        return 1;
    }
}
