/* Copyright (c) ZLMediaKit contributors. SPDX-License-Identifier: MIT */
#include "HlsArchive.h"
#include "Util/File.h"
#include "Util/MD5.h"
#include "Util/logger.h"
#include "Util/mini.h"
#include "Util/onceToken.h"
#include "json/json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace toolkit;
namespace mediakit {
namespace {

constexpr size_t kMaxPlaylist = 8 * 1024 * 1024;
constexpr size_t kMaxSegment = 64 * 1024 * 1024;
constexpr size_t kMaxSegments = 100000;
onceToken defaults([] {
    auto &ini = mINI::Instance();
    ini["hls_archive.enable"] = false;
    ini["hls_archive.scan_root"] = "/var/lib/zlm/www/hls-archive";
    ini["hls_archive.spool_path"] = "/var/lib/zlm/archive-jobs";
    ini["hls_archive.object_prefix"] = "hls";
    ini["hls_archive.scan_interval_sec"] = 3;
    ini["hls_archive.retry_delay_sec"] = 3;
    ini["hls_archive.retry_max"] = 10;
    ini["hls_archive.delete_local_after_archive"] = false;
});

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string digest(const std::string &data) { return MD5(data).hexdigest(); }

std::string absolute(const std::string &path) {
    if (path.empty() || path.find('\0') != std::string::npos) { throw std::runtime_error("empty/invalid path"); }
    auto result = File::absolutePath(path, path.front() == '/' ? "/" : exeDir(), true);
    while (result.size() > 1 && result.back() == '/') { result.pop_back(); }
    return result;
}

bool inside(const std::string &path, const std::string &root) {
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/';
}

std::string realPath(const std::string &path) {
    char resolved[PATH_MAX];
#if defined(_WIN32)
    if (!_fullpath(resolved, path.c_str(), sizeof(resolved))) { throw std::runtime_error("cannot resolve " + path); }
#else
    if (!realpath(path.c_str(), resolved)) { throw std::runtime_error("cannot resolve " + path); }
#endif
    return resolved;
}

bool symlink(const std::string &path) {
#if !defined(_WIN32)
    struct stat info{};
    return lstat(path.c_str(), &info) != 0 || S_ISLNK(info.st_mode);
#else
    return false;
#endif
}

std::string read(const std::string &path, size_t limit) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) { throw std::runtime_error("cannot read " + path); }
    auto size = input.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > limit) { throw std::runtime_error("file exceeds archive limit: " + path); }
    std::string data(static_cast<size_t>(size), '\0');
    input.seekg(0);
    if (!data.empty() && !input.read(&data[0], data.size())) { throw std::runtime_error("short read: " + path); }
    return data;
}

void atomicWrite(const std::string &path, const std::string &data) {
    auto tmp = path + ".tmp";
    FILE *file = File::create_file(tmp, "wb");
    if (!file) { throw std::runtime_error("cannot create archive state: " + tmp); }
    bool ok = fwrite(data.data(), 1, data.size(), file) == data.size() && fflush(file) == 0;
#if !defined(_WIN32)
    if (ok) { ok = fsync(fileno(file)) == 0; }
#endif
    if (fclose(file) != 0) { ok = false; }
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        throw std::runtime_error("cannot save archive state: " + path);
    }
}

bool relativePath(const std::string &path) {
    if (path.empty() || path.front() == '/' || path.find('\0') != std::string::npos ||
        path.find_first_of("\\?#%:\r\n") != std::string::npos) { return false; }
    for (auto &part : split(path, "/")) {
        if (part.empty() || part == "." || part == "..") { return false; }
    }
    return true;
}

std::vector<std::string> parsePlaylist(const std::string &text) {
    std::vector<std::string> segments;
    std::set<std::string> unique;
    std::istringstream input(text);
    std::string line;
    bool first = true, ended = false, duration = false, full = false, target = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty()) { continue; }
        if (first) {
            if (line != "#EXTM3U") { throw std::runtime_error("invalid EXTM3U header"); }
            first = false;
            continue;
        }
        if (ended) { throw std::runtime_error("content after ENDLIST"); }
        if (line == "#EXT-X-ENDLIST") { ended = true; continue; }
        if (line.compare(0, 8, "#EXTINF:") == 0) {
            size_t end = 0;
            double value = std::stod(line.substr(8), &end);
            if (duration || !std::isfinite(value) || value <= 0 || line.substr(8 + end, 1) != ",") {
                throw std::runtime_error("invalid segment duration");
            }
            duration = true;
        } else if (line == "#EXT-X-PLAYLIST-TYPE:EVENT" || line == "#EXT-X-PLAYLIST-TYPE:VOD") {
            full = true;
        } else if (line.compare(0, 22, "#EXT-X-MEDIA-SEQUENCE:") == 0) {
            if (line != "#EXT-X-MEDIA-SEQUENCE:0") { throw std::runtime_error("rolling playlist is not a complete archive"); }
        } else if (line.compare(0, 22, "#EXT-X-TARGETDURATION:") == 0) {
            target = true;
        } else if (line.front() == '#') {
            // Reject features with additional objects or different segment semantics.
            if (line.compare(0, 7, "#EXT-X-") == 0 &&
                line.compare(0, 15, "#EXT-X-VERSION:") != 0 &&
                line.compare(0, 19, "#EXT-X-ALLOW-CACHE:") != 0 &&
                line != "#EXT-X-DISCONTINUITY" &&
                line.compare(0, 30, "#EXT-X-DISCONTINUITY-SEQUENCE:") != 0 &&
                line.compare(0, 25, "#EXT-X-PROGRAM-DATE-TIME:") != 0 &&
                line != "#EXT-X-INDEPENDENT-SEGMENTS") {
                throw std::runtime_error("unsupported HLS tag: " + line.substr(0, line.find(':')));
            }
        } else {
            if (!duration || !relativePath(line) || line.size() < 3 || line.substr(line.size() - 3) != ".ts") {
                throw std::runtime_error("only relative TS segment paths are supported");
            }
            if (!unique.insert(line).second || segments.size() >= kMaxSegments) { throw std::runtime_error("duplicate or excessive segments"); }
            segments.emplace_back(line);
            duration = false;
        }
    }
    if (first || !ended || !full || !target || duration || segments.empty()) {
        throw std::runtime_error("incomplete TS media playlist (use hls.segNum=0)");
    }
    return segments;
}

uint32_t secondsOption(const char *key) {
    double seconds = mINI::Instance()[key].as<double>();
    if (!std::isfinite(seconds) || seconds < 0.05 || seconds > 3600) { throw std::invalid_argument(std::string("invalid ") + key); }
    return static_cast<uint32_t>(seconds * 1000);
}

} // namespace

class HlsArchive::Impl {
public:
    Impl(Options options, Upload upload) : _options(std::move(options)), _upload(std::move(upload)) {
        if (!_upload || _options.destination.empty() || _options.scan_interval_ms == 0 ||
            _options.retry_delay_ms == 0 || _options.retry_max == 0 || _options.upload_timeout_ms == 0) {
            throw std::invalid_argument("invalid HLS archive options/uploader");
        }
        _options.scan_root = absolute(_options.scan_root);
        _options.spool_path = absolute(_options.spool_path);
        while (!_options.object_prefix.empty() && _options.object_prefix.back() == '/') { _options.object_prefix.pop_back(); }
        if (!relativePath(_options.object_prefix)) { throw std::invalid_argument("invalid HLS archive object prefix"); }
        if (!File::create_path(_options.scan_root + "/", 0750) || !File::create_path(_options.spool_path + "/", 0750)) {
            throw std::runtime_error("cannot create HLS archive directories");
        }
        _options.scan_root = realPath(_options.scan_root);
        _options.spool_path = realPath(_options.spool_path);
        _thread = std::thread([this] { run(); });
    }
    ~Impl() { stop(); }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(_control->mutex);
            _control->stopping = true;
        }
        _control->condition.notify_all();
        if (_thread.joinable()) { _thread.join(); }
    }
    bool ownsFile(const std::string &path) const {
        return !path.empty() && inside(absolute(path), _options.scan_root);
    }

private:
    struct Control {
        std::mutex mutex;
        std::condition_variable condition;
        bool stopping = false;
    };
    struct Segment { std::string path, md5; uint64_t size = 0; };
    struct Job {
        std::string id, relative, playlist, status = "pending", error;
        std::vector<Segment> segments;
        size_t next = 0;
        uint32_t attempts = 0;
        int64_t retry_at = 0;
        bool cleanup = false;
    };
    bool stopping() const {
        std::lock_guard<std::mutex> lock(_control->mutex);
        return _control->stopping;
    }
    std::string jobId(const std::string &relative) const {
        return digest(_options.destination + "\n" + _options.object_prefix + "\n" + _options.scan_root + "\n" + relative);
    }
    std::string statePath(const Job &job) const { return _options.spool_path + "/" + job.id + ".json"; }
    std::string directory(const Job &job) const { return File::parentDir(_options.scan_root + "/" + job.relative); }

    void save(const Job &job) {
        Json::Value value;
        value["version"] = 1;
        value["id"] = job.id;
        value["scan_root"] = _options.scan_root;
        value["destination"] = _options.destination;
        value["object_prefix"] = _options.object_prefix;
        value["playlist_path"] = job.relative;
        value["playlist"] = job.playlist;
        value["status"] = job.status;
        value["next"] = Json::UInt64(job.next);
        value["attempts"] = job.attempts;
        value["retry_at"] = Json::Int64(job.retry_at);
        value["error"] = job.error;
        value["cleanup_done"] = job.cleanup;
        value["playlist_object"] = _options.object_prefix + "/" + job.relative;
        value["segments"] = Json::Value(Json::arrayValue);
        for (const auto &segment : job.segments) {
            Json::Value item;
            item["path"] = segment.path;
            item["size"] = Json::UInt64(segment.size);
            item["md5"] = segment.md5;
            value["segments"].append(item);
        }
        atomicWrite(statePath(job), value.toStyledString());
    }

    void recover() {
        File::scanDir(_options.spool_path, [this](const std::string &path, bool is_dir) {
            if (is_dir || path.size() < 5 || path.substr(path.size() - 5) != ".json") { return true; }
            try {
                Json::Value value;
                Json::Reader reader;
                if (!reader.parse(read(path, 64 * 1024 * 1024), value) || value["version"].asInt() != 1) { throw std::runtime_error("invalid task JSON"); }
                if (value["scan_root"].asString() != _options.scan_root || value["destination"].asString() != _options.destination ||
                    value["object_prefix"].asString() != _options.object_prefix) { return true; }
                Job job;
                job.relative = value["playlist_path"].asString();
                if (!relativePath(job.relative) || value["id"].asString() != jobId(job.relative)) { throw std::runtime_error("invalid task identity"); }
                job.id = value["id"].asString();
                job.playlist = value["playlist"].asString();
                auto paths = parsePlaylist(job.playlist);
                if (!value["segments"].isArray() || value["segments"].size() != paths.size()) { throw std::runtime_error("invalid task inventory"); }
                for (size_t i = 0; i < paths.size(); ++i) {
                    auto &item = value["segments"][static_cast<Json::ArrayIndex>(i)];
                    Segment segment;
                    segment.path = item["path"].asString();
                    segment.size = item["size"].asUInt64();
                    segment.md5 = item["md5"].asString();
                    if (segment.path != paths[i] || segment.size == 0 || segment.size > kMaxSegment || segment.md5.size() != 32) { throw std::runtime_error("invalid task segment"); }
                    job.segments.emplace_back(std::move(segment));
                }
                job.status = value["status"].asString();
                job.next = value["next"].asUInt64();
                job.attempts = value["attempts"].asUInt();
                job.retry_at = value["retry_at"].asInt64();
                job.error = value["error"].asString();
                job.cleanup = value["cleanup_done"].asBool();
                if (job.next > job.segments.size() + 1 || (job.status != "pending" && job.status != "retry_wait" &&
                    job.status != "ready" && job.status != "failed")) { throw std::runtime_error("invalid task progress"); }
                if ((job.status == "ready") != (job.next == job.segments.size() + 1)) {
                    throw std::runtime_error("task completion does not match its checkpoint");
                }
                _known[job.relative] = job.id;
                _fingerprints[job.relative] = digest(job.playlist);
                if (job.status != "ready" || (_options.delete_local_after_archive && !job.cleanup)) {
                    _jobs.emplace(job.id, std::move(job));
                }
            } catch (const std::exception &error) {
                ErrorL << "hls_archive: cannot recover " << path << ": " << error.what();
            }
            return !stopping();
        });
    }

    std::string segmentContent(const Job &job, const Segment &segment) const {
        auto root = realPath(directory(job));
        auto path = root + "/" + segment.path;
        if (!inside(realPath(path), root) || symlink(path)) { throw std::runtime_error("segment escapes recording directory"); }
        auto content = read(path, kMaxSegment);
        if (content.size() != segment.size || digest(content) != segment.md5) { throw std::runtime_error("segment changed after recording: " + segment.path); }
        return content;
    }

    void discover(const std::string &path) {
        const auto relative = path.substr(_options.scan_root.size() + 1);
        try {
            auto text = read(path, kMaxPlaylist);
            if (text.find("#EXT-X-ENDLIST") == std::string::npos) { return; }
            if (_known.count(relative)) {
                if (_fingerprints.at(relative) != digest(text) && _warnings[relative] != "recording directory reused") {
                    _warnings[relative] = "recording directory reused";
                    ErrorL << "hls_archive: recording directory reused; use a unique customized_path: " << path;
                }
                return;
            }
            auto paths = parsePlaylist(text);
            auto fingerprint = digest(text);
            if (_candidates[relative] != fingerprint) { _candidates[relative] = fingerprint; return; }
            Job job;
            job.id = jobId(relative);
            job.relative = relative;
            job.playlist = std::move(text);
            const auto root = realPath(File::parentDir(path));
            if (!inside(root, _options.scan_root)) { throw std::runtime_error("recording directory escapes scan root"); }
            for (const auto &uri : paths) {
                auto file = root + "/" + uri;
                if (!inside(realPath(file), root) || symlink(file)) { throw std::runtime_error("segment escapes recording directory"); }
                auto data = read(file, kMaxSegment);
                if (data.empty()) { throw std::runtime_error("empty segment: " + uri); }
                Segment item;
                item.path = uri;
                item.size = data.size();
                item.md5 = digest(data);
                job.segments.emplace_back(std::move(item));
                if (stopping()) { return; }
            }
            // A producer still changing the playlist is not a completed task.
            if (read(path, kMaxPlaylist) != job.playlist) { _candidates.erase(relative); return; }
            save(job);
            InfoL << "hls_archive: queued id=" << job.id << ", playlist=" << path << ", segments=" << job.segments.size();
            _known[relative] = job.id;
            _fingerprints[relative] = fingerprint;
            _jobs.emplace(job.id, std::move(job));
            _candidates.erase(relative);
            _warnings.erase(relative);
        } catch (const std::exception &error) {
            if (_warnings[relative] != error.what()) {
                _warnings[relative] = error.what();
                WarnL << "hls_archive: waiting/rejected " << path << ": " << error.what();
            }
        }
    }

    void scan(const std::string &directory, size_t depth = 0) {
        if (depth > 16 || stopping()) { return; }
        File::scanDir(directory, [this, depth](const std::string &path, bool is_dir) {
            if (stopping()) { return false; }
            if (symlink(path) || path == _options.spool_path) { return true; }
            if (is_dir) { scan(path, depth + 1); }
            else if (path.size() >= 9 && path.substr(path.size() - 9) == "/hls.m3u8") { discover(path); }
            return !stopping();
        });
    }

    bool upload(Object object, std::string &error) {
        struct Result { bool done = false; std::string error; };
        auto result = std::make_shared<Result>();
        auto control = _control;
        _upload(std::move(object), [result, control](const std::string &value) {
            {
                std::lock_guard<std::mutex> lock(control->mutex);
                if (result->done) { return; }
                result->done = true;
                result->error = value;
            }
            control->condition.notify_all();
        });
        std::unique_lock<std::mutex> lock(control->mutex);
        bool completed = control->condition.wait_for(lock, std::chrono::milliseconds(_options.upload_timeout_ms), [&] {
            return control->stopping || result->done;
        });
        if (control->stopping) { return false; }
        error = completed ? result->error : "upload callback timed out";
        return true;
    }

    void cleanup(Job &job) {
        if (!_options.delete_local_after_archive || job.cleanup || stopping()) { return; }
        auto playlist_path = _options.scan_root + "/" + job.relative;
        if (!File::fileExist(playlist_path)) { job.cleanup = true; save(job); return; }
        if (read(playlist_path, kMaxPlaylist) != job.playlist) { throw std::runtime_error("refusing cleanup of a reused recording directory"); }
        for (const auto &segment : job.segments) {
            auto file = directory(job) + segment.path;
            if (!File::fileExist(file)) { continue; }
            segmentContent(job, segment);
            if (std::remove(file.c_str()) != 0) { throw std::runtime_error("cannot remove archived segment"); }
        }
        if (std::remove(playlist_path.c_str()) != 0) { throw std::runtime_error("cannot remove archived playlist"); }
        job.cleanup = true;
        save(job);
    }

    bool advance(Job &job) {
        if (job.status == "ready") {
            try { cleanup(job); }
            catch (const std::exception &error) {
                if (_warnings[job.relative] != error.what()) {
                    _warnings[job.relative] = error.what();
                    WarnL << "hls_archive: cleanup deferred id=" << job.id << ", error=" << error.what();
                }
            }
            return false;
        }
        if (job.status == "failed" || job.retry_at > nowMs() || stopping()) { return false; }
        try {
            if (read(_options.scan_root + "/" + job.relative, kMaxPlaylist) != job.playlist) { throw std::runtime_error("recording directory changed before archive completed"); }
            Object object;
            if (job.next < job.segments.size()) {
                const auto &segment = job.segments[job.next];
                object.key = _options.object_prefix + "/" + File::parentDir(job.relative) + segment.path;
                object.content_type = "video/mp2t";
                object.content = segmentContent(job, segment);
            } else {
                object.key = _options.object_prefix + "/" + job.relative;
                object.content_type = "application/vnd.apple.mpegurl";
                object.content = job.playlist;
            }
            std::string error;
            if (!upload(std::move(object), error)) { return false; }
            if (!error.empty()) { throw std::runtime_error(error); }
            const auto checkpoint = job.next;
            ++job.next;
            job.attempts = 0;
            job.retry_at = 0;
            job.error.clear();
            job.status = job.next == job.segments.size() + 1 ? "ready" : "pending";
            try { save(job); }
            catch (...) { job.next = checkpoint; job.status = "pending"; throw; }
            if (job.status == "ready") {
                InfoL << "hls_archive: ready id=" << job.id << ", object=" << _options.object_prefix + "/" + job.relative;
            }
            return true;
        } catch (const std::exception &error) {
            job.error = error.what();
            ++job.attempts;
            job.status = job.attempts >= _options.retry_max ? "failed" : "retry_wait";
            job.retry_at = nowMs() + std::min<uint64_t>(60000,
                static_cast<uint64_t>(_options.retry_delay_ms) * (1ULL << std::min<uint32_t>(job.attempts - 1, 10)));
            save(job);
            WarnL << "hls_archive: " << job.status << " id=" << job.id << ", attempt=" << job.attempts << ", error=" << job.error;
            return false;
        }
    }

    void run() noexcept {
        setThreadName("hls archive");
        try {
            recover();
            int64_t next_scan = 0;
            while (!stopping()) {
                if (nowMs() >= next_scan) { scan(_options.scan_root); next_scan = nowMs() + _options.scan_interval_ms; }
                bool progressed = false;
                int64_t wake = next_scan;
                std::vector<std::string> retired;
                for (auto &entry : _jobs) {
                    if (stopping()) { break; }
                    progressed = advance(entry.second) || progressed;
                    if (entry.second.status == "retry_wait") { wake = std::min(wake, entry.second.retry_at); }
                    if (entry.second.status == "ready" && (!_options.delete_local_after_archive || entry.second.cleanup)) {
                        retired.emplace_back(entry.first);
                    }
                }
                // Keep only a small fingerprint in memory for finished jobs;
                // their inventories and snapshots remain in the durable ledger.
                for (const auto &id : retired) { _jobs.erase(id); }
                if (progressed) { continue; }
                std::unique_lock<std::mutex> lock(_control->mutex);
                _control->condition.wait_for(lock, std::chrono::milliseconds(std::max<int64_t>(10, wake - nowMs())), [this] { return _control->stopping; });
            }
        } catch (const std::exception &error) {
            ErrorL << "hls_archive: worker stopped; pending tasks retained: " << error.what();
        }
    }

    Options _options;
    Upload _upload;
    std::shared_ptr<Control> _control = std::make_shared<Control>();
    std::thread _thread;
    std::map<std::string, Job> _jobs;
    std::map<std::string, std::string> _known, _fingerprints, _candidates, _warnings;
};

bool HlsArchive::configured() { return mINI::Instance()["hls_archive.enable"].as<bool>(); }
HlsArchive::Options HlsArchive::configuredOptions(const std::string &destination) {
    auto &ini = mINI::Instance();
    Options options;
    options.scan_root = ini["hls_archive.scan_root"];
    options.spool_path = ini["hls_archive.spool_path"];
    options.object_prefix = ini["hls_archive.object_prefix"];
    options.scan_interval_ms = secondsOption("hls_archive.scan_interval_sec");
    options.retry_delay_ms = secondsOption("hls_archive.retry_delay_sec");
    options.retry_max = ini["hls_archive.retry_max"].as<uint32_t>();
    options.delete_local_after_archive = ini["hls_archive.delete_local_after_archive"].as<bool>();
    options.destination = destination;
    return options;
}
HlsArchive::HlsArchive(Options options, Upload upload) : _impl(new Impl(std::move(options), std::move(upload))) {}
HlsArchive::~HlsArchive() = default;
void HlsArchive::stop() { _impl->stop(); }
bool HlsArchive::ownsFile(const std::string &path) const { return _impl->ownsFile(path); }

} // namespace mediakit
