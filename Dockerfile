# Build
FROM gcc:13 AS builder

WORKDIR /app
COPY . .

RUN gcc -O2 -static -o app main.c

# Runtime mínimo
FROM alpine:3.20

WORKDIR /app

COPY --from=builder /app/app .

CMD ["./app"]