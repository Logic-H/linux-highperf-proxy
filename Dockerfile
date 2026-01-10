FROM ubuntu:22.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    libssl-dev \
    liburing-dev \
    linux-libc-dev \
    pkg-config \
    python3 \
    zlib1g-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build -j \
  && strip build/proxy_server


FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    liburing2 \
    zlib1g \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/proxy_server /app/proxy_server
COPY --from=builder /src/config/proxy.conf /app/config/proxy.conf

RUN mkdir -p /config /logs \
  && cp /app/config/proxy.conf /config/proxy.conf

VOLUME ["/config", "/logs"]

EXPOSE 8080/tcp
EXPOSE 8080/udp

ENTRYPOINT ["/app/proxy_server"]
CMD ["-c", "/config/proxy.conf"]
