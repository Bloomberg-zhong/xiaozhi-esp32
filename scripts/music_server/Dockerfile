FROM python:3.12-slim
WORKDIR /app
COPY music_server.py ./
COPY gateway_plugin.py ./
COPY xzmusic ./xzmusic
ENV PYTHONUNBUFFERED=1
EXPOSE 8090
USER nobody
CMD ["python3", "music_server.py", "--config", "/config/config.json"]
