/* Copyright (c) ZLMediaKit contributors. SPDX-License-Identifier: MIT */
#ifndef ZLMEDIAKIT_RECORD_GAP_FILLER_H
#define ZLMEDIAKIT_RECORD_GAP_FILLER_H

#include <functional>
#include "Common/MediaSink.h"
#include "Poller/EventPoller.h"

namespace mediakit {

// Wrap an encoded-frame sink. The disabled path returns the original sink.
MediaSinkInterface::Ptr wrapRecordGapFiller(
    MediaSinkInterface::Ptr recorder,
    uint32_t retention_ms,
    bool audio_enabled);

class TsGapInput : public MediaSinkInterface {
public:
    using Ptr = std::shared_ptr<TsGapInput>;
    using OnPacket = std::function<void(
        toolkit::Buffer::Ptr, uint64_t, bool)>;

    // Frontend operations run on the stream owner. Buffer startup frames until
    // tracks and the output binding are ready. The first binding is retained.
    virtual void bindOutput(
        const toolkit::EventPoller::Ptr &owner,
        OnPacket output) = 0;

    // Join the encoding worker; already posted packet callbacks remain async.
    virtual void stop() noexcept = 0;
};

TsGapInput::Ptr createTsGapInput(
    uint32_t retention_ms,
    bool audio_enabled);

} // namespace mediakit
#endif
