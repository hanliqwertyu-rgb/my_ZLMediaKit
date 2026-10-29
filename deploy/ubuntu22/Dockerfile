ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE} AS runtime-base

ENV DEBIAN_FRONTEND=noninteractive TZ=Asia/Shanghai
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates tzdata python3 ffmpeg libssl3 libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && ln -snf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime \
    && echo Asia/Shanghai > /etc/timezone

FROM runtime-base AS build
ARG BUILD_JOBS=4
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config libssl-dev \
        libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ZLMVersion.h.ini default.pem DejaVuSans.ttf ./
COPY cmake/ cmake/
COPY 3rdpart/ 3rdpart/
COPY src/ src/
COPY server/ server/
COPY ext-codec/ ext-codec/
COPY srt/ srt/
COPY tests/ tests/
COPY www/ www/
COPY deploy/ubuntu22/config.ini.example conf/config.ini
RUN cmake -S . -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_SERVER=ON -DENABLE_FFMPEG=ON -DENABLE_MP4=ON -DENABLE_HLS=ON \
        -DENABLE_OPENSSL=ON -DENABLE_SRT=ON -DENABLE_RTPPROXY=ON \
        -DENABLE_WEBRTC=OFF -DENABLE_PYTHON=OFF -DENABLE_PLAYER=OFF \
        -DENABLE_API=OFF -DENABLE_TESTS=ON -DDISABLE_REPORT=ON \
    && python3 -c 'from pathlib import Path; import re; c=Path("/build/CMakeCache.txt").read_text(); m=re.search(r"^MK_COMPILE_DEFINITIONS:INTERNAL=(.*)$",c,re.M); assert m and {"ENABLE_FFMPEG","ENABLE_MP4","ENABLE_HLS","ENABLE_OPENSSL"} <= set(m[1].split(";")), "Required build feature disabled"' \
    && cmake --build /build --target MediaServer test_record_gap_fill -j "${BUILD_JOBS}" \
    && ctest --test-dir /build -R '^test_record_gap_fill$' --output-on-failure \
    && sha256sum release/linux/Release/MediaServer src/Extension/RecordGapFiller.cpp \
        src/Record/Recorder.cpp src/Record/HlsRecorder.h > /src/BUILD_INFO

FROM runtime-base AS runtime
ARG SOURCE_REVISION=workspace
LABEL org.opencontainers.image.title="ZLMediaKit with MP4/HLS gap filling" \
      org.opencontainers.image.revision="${SOURCE_REVISION}"
RUN groupadd -g 10001 zlm && useradd -u 10001 -g 10001 -M -s /usr/sbin/nologin zlm \
    && mkdir -p /opt/zlm /etc/zlm /var/lib/zlm/www /var/log/zlm \
    && chown -R 10001:10001 /var/lib/zlm /var/log/zlm
COPY --from=build /src/release/linux/Release/MediaServer /src/release/linux/Release/MediaServer.debug /opt/zlm/
COPY --from=build /src/BUILD_INFO /opt/zlm/BUILD_INFO
COPY --from=build /src/www/ /opt/zlm/www/
COPY --from=build /src/DejaVuSans.ttf /opt/zlm/DejaVuSans.ttf
COPY deploy/ubuntu22/healthcheck.py /opt/zlm/healthcheck.py
COPY tools/test_record_api.py /opt/zlm/tools/test_record_api.py
RUN ffmpeg -hide_banner -encoders > /tmp/encoders.txt 2>&1 \
    && python3 -c 'from pathlib import Path; t=Path("/tmp/encoders.txt").read_text(); assert "libx264" in t and " aac " in t, "Required output encoder unavailable"' \
    && rm /tmp/encoders.txt \
    && /opt/zlm/MediaServer --version
WORKDIR /var/lib/zlm
USER 10001:10001
EXPOSE 8080/tcp 1935/tcp 8554/tcp
STOPSIGNAL SIGINT
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD ["python3", "/opt/zlm/healthcheck.py"]
ENTRYPOINT ["/opt/zlm/MediaServer"]
CMD ["-c", "/etc/zlm/config.ini", "-s", "/etc/zlm/certs", "--log-dir", "/var/log/zlm", "--log-size", "20", "--log-slice", "21", "-l", "2", "--affinity", "0"]
