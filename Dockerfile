FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Enable installation of man pages
RUN rm -f /etc/dpkg/dpkg.cfg.d/excludes

# Install development and debugging tools
RUN apt-get update && apt-get install -y \
    build-essential gdb valgrind strace ltrace \
    libx11-dev x11-apps x11-utils xauth \
    man-db manpages-dev manpages-posix-dev \
    git make nano vim procps psmisc less file \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /work

# Start Bash and remove generated build artifacts when the shell/container exits.
CMD ["/bin/bash", "-c", "trap 'make clean' EXIT; /bin/bash"]
