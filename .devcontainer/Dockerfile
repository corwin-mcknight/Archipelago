# Pinned by digest: rebuilds stay on the 3.24 branch (clang 22, QEMU 11,
# Python 3.14) instead of silently jumping major versions with :latest. Bump
# the tag and digest together, deliberately. apk still tracks the branch at
# patch level -- Alpine drops superseded package versions from its mirrors, so
# exact apk pins rot into build failures; full toolchain freeze means
# publishing this built image to a registry once CI exists.
FROM alpine:3.24.1@sha256:28bd5fe8b56d1bd048e5babf5b10710ebe0bae67db86916198a6eec434943f8b

RUN apk upgrade --no-cache && apk add --no-cache bash htop file binutils g++ clang clang-extra-tools compiler-rt llvm lld lldb gdb-multiarch make git xorriso qemu-system-x86_64 qemu-system-riscv64 nasm mtools ripgrep openssh py3-yaml fzf curl npm xz bubblewrap

RUN echo 'export PATH="$HOME/.local/bin:$PATH"' >> /root/.bashrc
RUN echo "alias plume='python3 -m plume'" >> /root/.bashrc

# Shell Environment
WORKDIR /workspaces
ENV SHELL=/bin/bash
ENTRYPOINT [ "/bin/bash" ]
