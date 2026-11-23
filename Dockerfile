# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS build-dependencies

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libasio-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

FROM build-dependencies AS build
COPY CMakeLists.txt ./
COPY src ./src
COPY web ./web

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_TSAN=OFF -DBUILD_TESTING=OFF \
    && cmake --build build --parallel 4

# Reproducible verification: docker build --target test -t coursework-tests .
FROM build-dependencies AS test
ARG ENABLE_TSAN=OFF
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 nodejs \
    && rm -rf /var/lib/apt/lists/*
COPY CMakeLists.txt ./
COPY src ./src
COPY web ./web
COPY tests ./tests
COPY tools ./tools
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_TSAN=${ENABLE_TSAN} -DBUILD_TESTING=ON \
    && cmake --build build --parallel 4
CMD ["ctest", "--test-dir", "build", "--output-on-failure"]

FROM ubuntu:24.04 AS runtime

# python3 runs the stdlib-only corpus generator; curl is used by the healthcheck.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl python3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home app \
    && mkdir -p /data/generated \
    && chown app:app /data/generated

WORKDIR /app
COPY --from=build /src/build/search_server /app/search_server
COPY --from=build /src/build/search /usr/local/bin/search
COPY tools/generate_dataset.py /app/tools/generate_dataset.py
COPY tools/start_server.py tools/prepare_model.py /app/tools/
COPY scripts/docker-entrypoint.sh /app/docker-entrypoint.sh
COPY web/vue.LICENSE.txt /app/licenses/vue.LICENSE.txt
RUN chmod 0755 /app/docker-entrypoint.sh

# The generated corpus lives on a volume so the first (slow) run is paid once.
# Declaring it also gives a plain `docker run` persistent storage.
VOLUME ["/data/generated"]

ENV PORT=8080 \
    DATASET_PROFILE=full \
    GENERATE_DATASET=true \
    AUTO_INDEX=true \
    OLLAMA_URL=http://ollama:11434 \
    OLLAMA_MODEL=qwen2.5:1.5b \
    PYTHONUNBUFFERED=1

EXPOSE 8080
USER app
ENTRYPOINT ["/app/docker-entrypoint.sh"]
