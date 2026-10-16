/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "CloudUploader.h"
#include "Common/config.h"
#include "Http/HttpBody.h"
#include "Http/HttpRequester.h"
#include "Poller/EventPoller.h"
#include "Record/Recorder.h"
#include "Util/File.h"
#include "Util/NoticeCenter.h"
#include "Util/logger.h"
#include <cctype>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace std;
using namespace toolkit;

namespace mediakit {

static void *s_cloud_upload_tag = nullptr;
// 上传任务统一投递到同一个 poller 线程执行，保证 s_pending_requests 的访问线程唯一。
static EventPoller::Ptr s_upload_poller;
// 正在上传的请求对象，防止 HttpRequester 提前析构。
static std::set<HttpRequester::Ptr> s_pending_requests;

// 云上传配置键全部定义在本文件内，config.h/config.cpp 无需改动。
static const string kEnable = "cloud_upload.enable";
static const string kUrl = "cloud_upload.url";
static const string kMethod = "cloud_upload.method";
static const string kContentType = "cloud_upload.content_type";
static const string kTimeoutSec = "cloud_upload.timeout_sec";
static const string kDeleteAfterUpload = "cloud_upload.delete_after_upload";
static const string kMinioEnable = "cloud_upload.minio_enable";
static const string kMinioEndpoint = "cloud_upload.minio_endpoint";
static const string kMinioBucket = "cloud_upload.minio_bucket";
static const string kMinioAccessKey = "cloud_upload.minio_access_key";
static const string kMinioSecretKey = "cloud_upload.minio_secret_key";
static const string kMinioRegion = "cloud_upload.minio_region";
static const string kMinioObject = "cloud_upload.minio_object";

static onceToken s_cloud_defaults([]() {
    mINI::Instance()[kEnable] = false;
    mINI::Instance()[kUrl] = "";
    mINI::Instance()[kMethod] = "PUT";
    mINI::Instance()[kContentType] = "application/octet-stream";
    mINI::Instance()[kTimeoutSec] = 60;
    mINI::Instance()[kDeleteAfterUpload] = false;
    mINI::Instance()[kMinioEnable] = false;
    mINI::Instance()[kMinioEndpoint] = "";
    mINI::Instance()[kMinioBucket] = "";
    mINI::Instance()[kMinioAccessKey] = "";
    mINI::Instance()[kMinioSecretKey] = "";
    mINI::Instance()[kMinioRegion] = "us-east-1";
    mINI::Instance()[kMinioObject] = "{app}/{stream}/{file_name}";
});

// -------------------- SHA256 / HMAC-SHA256（自包含，无 OpenSSL 依赖） --------------------
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint32_t ep0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
static uint32_t ep1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
static uint32_t sig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
static uint32_t sig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

struct Sha256 {
    uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t bitlen = 0;
    uint8_t buf[64];
    size_t buflen = 0;

    void transform(const uint8_t data[64]) {
        uint32_t m[64];
        for (int i = 0, j = 0; i < 16; i++, j += 4) {
            m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) | ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
        }
        for (int i = 16; i < 64; i++) {
            m[i] = sig1(m[i - 2]) + m[i - 7] + sig0(m[i - 15]) + m[i - 16];
        }
        uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + ep1(e) + ch(e, f, g) + K[i] + m[i];
            uint32_t t2 = ep0(a) + maj(a, b, c);
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        st[0] += a; st[1] += b; st[2] += c; st[3] += d;
        st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    }

    void update(const uint8_t *p, size_t n) {
        bitlen += (uint64_t)n * 8;
        while (n) {
            size_t take = 64 - buflen;
            if (take > n) take = n;
            memcpy(buf + buflen, p, take);
            buflen += take; p += take; n -= take;
            if (buflen == 64) { transform(buf); buflen = 0; }
        }
    }

    string finalRaw() {
        uint64_t bitlen_save = bitlen;
        uint8_t pad = 0x80; update(&pad, 1);
        uint8_t zero = 0; while (buflen != 56) update(&zero, 1);
        uint8_t lenb[8];
        for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bitlen_save >> (56 - i * 8));
        update(lenb, 8);
        string out(32, '\0');
        for (int i = 0; i < 8; i++) {
            out[i * 4]     = (char)(st[i] >> 24);
            out[i * 4 + 1] = (char)(st[i] >> 16);
            out[i * 4 + 2] = (char)(st[i] >> 8);
            out[i * 4 + 3] = (char)(st[i]);
        }
        return out;
    }
};

static string toHex(const string &raw) {
    static const char *d = "0123456789abcdef";
    string r;
    r.reserve(raw.size() * 2);
    for (unsigned char c : raw) { r.push_back(d[c >> 4]); r.push_back(d[c & 15]); }
    return r;
}

static string sha256Raw(const string &msg) {
    Sha256 s;
    s.update((const uint8_t *)msg.data(), msg.size());
    return s.finalRaw();
}

static string sha256Hex(const string &msg) { return toHex(sha256Raw(msg)); }

static string hmacSha256Raw(const string &key, const string &msg) {
    string k = key;
    if (k.size() > 64) k = sha256Raw(k);
    k.resize(64, '\0');
    string ipad(64, 0x36), opad(64, 0x5c);
    for (int i = 0; i < 64; i++) { ipad[i] ^= k[i]; opad[i] ^= k[i]; }
    string inner = sha256Raw(ipad + msg);
    return sha256Raw(opad + inner);
}

// -------------------- 通用工具 --------------------
static string replaceAll(string str, const string &from, const string &to) {
    if (from.empty()) return str;
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
    return str;
}

static string makeUploadUrl(const string &url, const RecordInfo &info) {
    string ret = url;
    ret = replaceAll(ret, "{vhost}", info.vhost);
    ret = replaceAll(ret, "{app}", info.app);
    ret = replaceAll(ret, "{stream}", info.stream);
    ret = replaceAll(ret, "{params}", info.params);
    ret = replaceAll(ret, "{file_path}", info.file_path);
    ret = replaceAll(ret, "{file_name}", info.file_name);
    return ret;
}

static string readFileAll(const string &path) {
    ifstream ifs(path, ios::binary);
    if (!ifs) return "";
    ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

static bool isUnreserved(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static string uriEncodePath(const string &p) {
    static const char *hex = "0123456789ABCDEF";
    string r;
    for (unsigned char c : p) {
        if (c == '/') { r += '/'; }
        else if (isUnreserved(c)) { r += (char)c; }
        else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
    }
    return r;
}

static void parseEndpoint(const string &endpoint, string &scheme, string &host) {
    scheme = "http";
    host = endpoint;
    auto pos = endpoint.find("://");
    if (pos != string::npos) {
        scheme = endpoint.substr(0, pos);
        host = endpoint.substr(pos + 3);
    }
    while (!host.empty() && host.back() == '/') host.pop_back();
}

static string amzDate() {
    time_t t = time(nullptr);
    static std::mutex s_mtx;
    std::lock_guard<std::mutex> lck(s_mtx);
    struct tm *tm = gmtime(&t);
    char buf[20] = {0};
    strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", tm);
    return buf;
}

static string awsAuthorization(const string &method, const string &canonical_uri, const string &query,
                               const vector<pair<string, string>> &headers, const string &payload_hash,
                               const string &access_key, const string &secret_key, const string &region,
                               const string &service, const string &amzdate, const string &datestamp) {
    string canonical_headers, signed_headers;
    for (size_t i = 0; i < headers.size(); i++) {
        canonical_headers += headers[i].first + ":" + headers[i].second + "\n";
        signed_headers += (i ? ";" : "") + headers[i].first;
    }
    string canonical_request = method + "\n" + canonical_uri + "\n" + query + "\n" +
                               canonical_headers + "\n" + signed_headers + "\n" + payload_hash;
    string scope = datestamp + "/" + region + "/" + service + "/aws4_request";
    string string_to_sign = "AWS4-HMAC-SHA256\n" + amzdate + "\n" + scope + "\n" + sha256Hex(canonical_request);

    string k_date = hmacSha256Raw("AWS4" + secret_key, datestamp);
    string k_region = hmacSha256Raw(k_date, region);
    string k_service = hmacSha256Raw(k_region, service);
    string k_signing = hmacSha256Raw(k_service, "aws4_request");
    string signature = toHex(hmacSha256Raw(k_signing, string_to_sign));

    return "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope +
           ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
}

// -------------------- 上传后端接口与实现 --------------------
class IUploader {
public:
    using Ptr = std::shared_ptr<IUploader>;
    virtual ~IUploader() = default;
    // 判断该后端当前是否可用（由配置决定）
    virtual bool enabled() = 0;
    // 执行上传
    virtual void upload(const RecordInfo &info) = 0;
};

static void startUpload(const string &url, const string &method, const vector<pair<string, string>> &headers,
                        const string &file_path, bool delete_after, float timeout_sec) {
    s_upload_poller->async([url, method, headers, file_path, delete_after, timeout_sec]() {
        auto requester = std::make_shared<HttpRequester>();
        requester->setMethod(method);
        for (auto &h : headers) {
            requester->addHeader(h.first, h.second);
        }
        requester->setBody(std::make_shared<HttpFileBody>(file_path));

        s_pending_requests.insert(requester);
        std::weak_ptr<HttpRequester> weak = requester;
        requester->startRequester(
            url,
            [weak, file_path, delete_after](const SockException &ex, const Parser &response) {
                // 只有连接成功且 HTTP 状态码为 2xx 才算上传成功。
                // MinIO 等对象存储失败时通常返回 4xx/5xx 响应，但连接本身是成功的，
                // 若不检查状态码，会把失败误判为成功，并在 delete_after_upload 开启时误删本地录像。
                auto status = response.status();
                bool success = !ex && status.size() == 3 && status[0] == '2';
                if (!success) {
                    auto body = response.content();
                    if (body.size() > 1024) {
                        body.resize(1024);
                    }
                    ErrorL << "云存储上传失败 " << file_path << "："
                           << (ex ? ex.what() : "HTTP " + status + " " + response.statusStr())
                           << ", body: " << body;
                } else {
                    InfoL << "云存储上传成功 " << file_path << " (HTTP " << status << ")";
                    if (delete_after) File::delete_file(file_path.data());
                }
                if (auto strong = weak.lock()) s_pending_requests.erase(strong);
            },
            timeout_sec);
    });
}

// 通用 HTTP 模板上传后端
class HttpUploader : public IUploader {
public:
    bool enabled() override {
        GET_CONFIG(bool, minio_enable, kMinioEnable);
        if (minio_enable) return false;
        GET_CONFIG(string, url, kUrl);
        return !url.empty();
    }

    void upload(const RecordInfo &info) override {
        GET_CONFIG(string, url, kUrl);
        GET_CONFIG(string, method, kMethod);
        GET_CONFIG(string, content_type, kContentType);
        GET_CONFIG(float, timeout_sec, kTimeoutSec);
        GET_CONFIG(bool, delete_after, kDeleteAfterUpload);

        vector<pair<string, string>> headers;
        if (!content_type.empty()) headers.emplace_back("Content-Type", content_type);
        startUpload(makeUploadUrl(url, info), method.empty() ? "PUT" : method, headers,
                    info.file_path, delete_after, timeout_sec);
    }
};

// MinIO 直传后端（AWS Signature V4）
class MinioUploader : public IUploader {
public:
    bool enabled() override {
        GET_CONFIG(bool, minio_enable, kMinioEnable);
        if (!minio_enable) return false;
        GET_CONFIG(string, endpoint, kMinioEndpoint);
        GET_CONFIG(string, bucket, kMinioBucket);
        GET_CONFIG(string, access_key, kMinioAccessKey);
        GET_CONFIG(string, secret_key, kMinioSecretKey);
        return !endpoint.empty() && !bucket.empty() && !access_key.empty() && !secret_key.empty();
    }

    void upload(const RecordInfo &info) override {
        GET_CONFIG(string, endpoint, kMinioEndpoint);
        GET_CONFIG(string, bucket, kMinioBucket);
        GET_CONFIG(string, access_key, kMinioAccessKey);
        GET_CONFIG(string, secret_key, kMinioSecretKey);
        GET_CONFIG(string, region, kMinioRegion);
        GET_CONFIG(string, object_tpl, kMinioObject);
        GET_CONFIG(string, content_type, kContentType);
        GET_CONFIG(float, timeout_sec, kTimeoutSec);
        GET_CONFIG(bool, delete_after, kDeleteAfterUpload);

        if (endpoint.empty() || bucket.empty() || access_key.empty() || secret_key.empty()) {
            ErrorL << "MinIO 上传配置不完整";
            return;
        }

        string scheme, host;
        parseEndpoint(endpoint, scheme, host);

        string object_key = makeUploadUrl(object_tpl, info);
        string canonical_uri = "/" + bucket + "/" + uriEncodePath(object_key);
        string payload_hash = sha256Hex(readFileAll(info.file_path));

        string amzdate = amzDate();
        string datestamp = amzdate.substr(0, 8);
        string content_type_val = content_type.empty() ? "application/octet-stream" : content_type;

        vector<pair<string, string>> sign_headers;
        sign_headers.emplace_back("content-type", content_type_val);
        sign_headers.emplace_back("host", host);
        sign_headers.emplace_back("x-amz-content-sha256", payload_hash);
        sign_headers.emplace_back("x-amz-date", amzdate);

        string authorization = awsAuthorization("PUT", canonical_uri, "", sign_headers, payload_hash,
                                                access_key, secret_key, region, "s3", amzdate, datestamp);

        string upload_url = scheme + "://" + host + "/" + bucket + "/" + object_key;

        vector<pair<string, string>> send_headers;
        send_headers.emplace_back("Content-Type", content_type_val);
        send_headers.emplace_back("x-amz-date", amzdate);
        send_headers.emplace_back("x-amz-content-sha256", payload_hash);
        send_headers.emplace_back("Authorization", authorization);

        startUpload(upload_url, "PUT", send_headers, info.file_path, delete_after, timeout_sec);
    }
};

// -------------------- 上传后端注册表 --------------------
static vector<IUploader::Ptr> s_uploaders;

static void onRecord(const RecordInfo &info) {
    GET_CONFIG(bool, enable, kEnable);
    if (!enable) return;

    for (auto &uploader : s_uploaders) {
        if (uploader->enabled()) {
            uploader->upload(info);
            return;
        }
    }
}

void installCloudUploader() {
    s_upload_poller = EventPollerPool::Instance().getPoller(false);

    // 注册上传后端；顺序即优先级。
    s_uploaders.emplace_back(std::make_shared<MinioUploader>());
    s_uploaders.emplace_back(std::make_shared<HttpUploader>());

    NoticeCenter::Instance().addListener(&s_cloud_upload_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        onRecord(info);
    });
    NoticeCenter::Instance().addListener(&s_cloud_upload_tag, Broadcast::kBroadcastRecordTs, [](BroadcastRecordTsArgs) {
        onRecord(info);
    });
    InfoL << "云存储上传模块已安装";
}

void uninstallCloudUploader() {
    NoticeCenter::Instance().delListener(&s_cloud_upload_tag);
    s_uploaders.clear();
    s_pending_requests.clear();
    s_upload_poller = nullptr;
}

} // namespace mediakit
