# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libasio-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_TSAN=OFF \
    && cmake --build build --parallel "$(nproc)"

FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home app

WORKDIR /app
COPY --from=build /src/build/search_server /app/search_server

ENV PORT=8080 \
    DATA_DIR=/data/docs \
    OLLAMA_URL=http://ollama:11434 \
    OLLAMA_MODEL=qwen2.5:1.5b

EXPOSE 8080
USER app
ENTRYPOINT ["/app/search_server"]
