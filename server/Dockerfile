FROM python:3.12-slim

# Use the standard repositories by default; an optional mirror is a build choice.
ARG DEBIAN_MIRROR=https://deb.debian.org/debian
RUN if [ -n "$DEBIAN_MIRROR" ]; then \
      sed -i "s|http://deb.debian.org/debian|${DEBIAN_MIRROR}|g" /etc/apt/sources.list.d/debian.sources; \
    fi \
 && apt-get update \
 && apt-get install -y --no-install-recommends ffmpeg ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
ARG PIP_INDEX_URL=https://pypi.org/simple
RUN pip install --no-cache-dir --index-url "$PIP_INDEX_URL" -r requirements.txt

COPY server.py public_feeds.py listening_store.py source_manager.py access_store.py manage_access.py runtime_release.py device_covers.py config.json ./
COPY static/ ./static/

VOLUME ["/app/data", "/app/media"]
EXPOSE 8899

HEALTHCHECK --interval=60s --timeout=10s --start-period=30s --retries=3 \
  CMD python -c "import urllib.request;urllib.request.urlopen('http://127.0.0.1:8899/healthz').read()" || exit 1

CMD ["gunicorn", "--bind", "0.0.0.0:8899", "--workers", "1", "--threads", "8", \
     "--timeout", "120", "--access-logfile", "-", "server:app"]
