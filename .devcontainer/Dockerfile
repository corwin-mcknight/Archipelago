FROM alpine:latest

RUN apk upgrade --no-cache && apk add --no-cache bash htop file binutils g++ clang clang-extra-tools lld lldb gdb-multiarch make git xorriso qemu-system-x86_64 nasm ripgrep openssh py3-yaml fzf curl

RUN curl -fsSL https://claude.ai/install.sh | bash
RUN echo 'export PATH="$HOME/.local/bin:$PATH"' >> /root/.bashrc
RUN echo "alias plume='python3 -m plume'" >> /root/.bashrc

# Shell Environment
WORKDIR /workspaces
ENV SHELL=/bin/bash
ENTRYPOINT [ "/bin/bash" ]
