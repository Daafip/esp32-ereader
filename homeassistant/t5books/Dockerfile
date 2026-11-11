# T5 book server (docs/book-server.md). The Supervisor builds this folder.
# bookprep (tools/bookprep) is in it when `pixi run ha-addon` made it (a local add-on:
# exactly the code you have); from the add-on repository it comes from the GitHub
# release of this version (tag v<version>).
FROM python:3.12-slim-bookworm

ARG BUILD_VERSION=dev
ENV T5BOOKS_VERSION=${BUILD_VERSION} \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

COPY . /tmp/addon/
RUN set -eu; \
    src=/tmp/addon/bookprep; \
    if [ ! -d "$src" ]; then \
      url="https://github.com/Daafip/esp32-ereader/archive/refs/tags/v${BUILD_VERSION}.tar.gz"; \
      python -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' \
        "$url" /tmp/src.tar.gz; \
      tar -xzf /tmp/src.tar.gz -C /tmp; \
      src=$(echo /tmp/esp32-ereader-*/tools/bookprep); \
    fi; \
    pip install -r /tmp/addon/requirements.txt "$src"; \
    python -m bookprep.bookserver --fetch-fonts /opt/t5books/fonts; \
    install -m 755 /tmp/addon/run.py /run.py; \
    rm -rf /tmp/addon /tmp/src.tar.gz /tmp/esp32-ereader-*

CMD ["python", "/run.py"]
