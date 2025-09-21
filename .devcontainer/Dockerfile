FROM alpine:latest

RUN apk add --no-cache bash htop file binutils g++ clang clang-extra-tools lld lldb gdb-multiarch make git bear xorriso qemu-system-x86_64 nasm ripgrep openssh

# Shell Environment
WORKDIR /workspaces
ENV SHELL=/bin/bash
ENTRYPOINT [ "/bin/bash" ]
