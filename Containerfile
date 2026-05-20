FROM fedora

RUN dnf install -y python3 python3-uv git
RUN uv pip install --system -U https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip

WORKDIR /work
# RUN --mount=type=bind,dst=/work,rw pio pkg install --no-save

# ENTRYPOINT ["pio"]
# CMD ["run", "-e", "default"]
