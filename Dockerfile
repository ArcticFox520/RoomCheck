# syntax=docker/dockerfile:1

# ---------- build stage: Alpine (musl) produces a fully static binary ----------
FROM alpine:3.20 AS build

RUN apk add --no-cache build-base make file binutils
WORKDIR /src

COPY Makefile ./
COPY backend ./backend
COPY third_party ./third_party

RUN make CC=gcc LDFLAGS="-static -s -no-pie" build/RoomCheck \
    && file build/RoomCheck \
    && ! readelf -l build/RoomCheck | grep -q "INTERP" \
    && ! readelf -d build/RoomCheck | grep -q "NEEDED" \
    && ./build/RoomCheck --version

# ---------- runtime stage ----------
FROM alpine:3.20

RUN apk add --no-cache tzdata \
    && addgroup -S roomcheck \
    && adduser -S -G roomcheck -h /app roomcheck \
    && mkdir -p /data /app \
    && chown -R roomcheck:roomcheck /data /app

WORKDIR /app
COPY --from=build /src/build/RoomCheck /app/RoomCheck
COPY frontend /app/frontend

ENV TZ=Asia/Shanghai \
    PORT=8080 \
    ROOMCHECK_DATA_DIR=/data \
    ROOMCHECK_WEB_ROOT=/app/frontend

RUN chown -R roomcheck:roomcheck /app
USER roomcheck

EXPOSE 8080
VOLUME ["/data"]

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD wget -q -O /dev/null http://127.0.0.1:8080/api/health || exit 1

ENTRYPOINT ["/app/RoomCheck"]
