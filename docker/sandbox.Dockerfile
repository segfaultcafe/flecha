FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        binutils \
        ca-certificates \
        cmake \
        file \
        g++-14 \
        gcc-14 \
        llvm-18-dev \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

ENV CC=/usr/bin/gcc-14
ENV CXX=/usr/bin/g++-14

WORKDIR /src/flecha
COPY . .

ARG FLECHA_BUILD_VERSION

RUN cmake -S . -B /tmp/flecha-build -G Ninja \
        -DBUILD_TESTING=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/flecha \
        "-DFLECHA_BUILD_VERSION=${FLECHA_BUILD_VERSION}" \
    && cmake --build /tmp/flecha-build --parallel \
    && cmake --install /tmp/flecha-build

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        bash \
        binutils \
        build-essential \
        ca-certificates \
        clang-18 \
        curl \
        git \
        libc6-dev \
        luarocks \
    && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH
ARG NEOVIM_VERSION=0.12.6

RUN case "${TARGETARCH}" in \
        amd64) \
            neovim_archive="nvim-linux-x86_64.tar.gz"; \
            neovim_sha256="474430d53e6264f6d6dd18db42d6dc9df3a1b56ca9e88a325bbf860e1a811d87" \
            ;; \
        arm64) \
            neovim_archive="nvim-linux-arm64.tar.gz"; \
            neovim_sha256="8f1f64a0bdb97247034038c3823c6cbad5bdf9ecd5751b85494b71c3ee04c815" \
            ;; \
        *) \
            echo "Unsupported Docker architecture: ${TARGETARCH}" >&2; \
            exit 1 \
            ;; \
    esac \
    && curl --fail --location --silent --show-error \
        "https://github.com/neovim/neovim/releases/download/v${NEOVIM_VERSION}/${neovim_archive}" \
        --output /tmp/nvim.tar.gz \
    && echo "${neovim_sha256}  /tmp/nvim.tar.gz" | sha256sum --check \
    && mkdir --parents /opt/neovim \
    && tar --extract --gzip --file /tmp/nvim.tar.gz \
        --directory /opt/neovim --strip-components=1 \
    && ln --symbolic /opt/neovim/bin/nvim /usr/local/bin/nvim \
    && rm /tmp/nvim.tar.gz

COPY --from=builder /opt/flecha /opt/flecha
COPY docker/sandbox/init.vim /etc/flecha-sandbox/init.vim

ENV PATH=/opt/flecha/bin:${PATH}
ENV FLECHA_LINKER=/usr/bin/clang++-18

RUN useradd --create-home --shell /bin/bash flecha \
    && install -d -o flecha -g flecha \
        /home/flecha/.config/nvim \
        /home/flecha/.local/share/nvim/site/pack/flecha/start \
        /workspace \
    && cp /etc/flecha-sandbox/init.vim /home/flecha/.config/nvim/init.vim \
    && ln -s /opt/flecha/share/flecha/editors/neovim \
        /home/flecha/.local/share/nvim/site/pack/flecha/start/flecha.nvim \
    && chown -R flecha:flecha /home/flecha/.config /home/flecha/.local

USER flecha
WORKDIR /workspace

RUN flecha --version \
    && longbow --version \
    && nvim --version \
    && longbow new hello \
    && cd hello \
    && longbow check \
    && longbow build \
    && nvim --headless -i NONE -n \
        -c "edit /workspace/hello/src/main.fl" \
        -c "if &filetype !=# 'flecha' | cquit | endif" \
        -c "if !exists('b:current_syntax') | cquit | endif" \
        -c quit

CMD ["sleep", "infinity"]
