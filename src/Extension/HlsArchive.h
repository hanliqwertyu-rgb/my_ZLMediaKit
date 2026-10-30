/* Copyright (c) ZLMediaKit contributors. SPDX-License-Identifier: MIT */
#ifndef ZLMEDIAKIT_HLS_ARCHIVE_H
#define ZLMEDIAKIT_HLS_ARCHIVE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mediakit {

// Storage-independent archiver for completed, unencrypted TS media playlists.
// The uploader owns the supplied bytes and must report success only after PUT
// completed. Its callback may run on any thread, including after stop().
class HlsArchive {
public:
    struct Options {
        std::string scan_root;
        std::string spool_path;
        std::string object_prefix = "hls";
        std::string destination; // Endpoint/bucket identity, never credentials.
        uint32_t scan_interval_ms = 3000;
        uint32_t retry_delay_ms = 3000;
        uint32_t retry_max = 10;
        uint32_t upload_timeout_ms = 65000;
        bool delete_local_after_archive = false;
    };
    struct Object {
        std::string key;
        std::string content_type;
        std::string content;
    };
    using Done = std::function<void(const std::string &error)>;
    using Upload = std::function<void(Object object, Done done)>;

    static bool configured();
    static Options configuredOptions(const std::string &destination);

    HlsArchive(Options options, Upload upload);
    ~HlsArchive();
    HlsArchive(const HlsArchive &) = delete;
    HlsArchive &operator=(const HlsArchive &) = delete;
    void stop();
    bool ownsFile(const std::string &path) const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace mediakit
#endif
