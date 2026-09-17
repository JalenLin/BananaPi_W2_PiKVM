FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        openssh-client sshpass ca-certificates \
    && rm -rf /var/lib/apt/lists/*
