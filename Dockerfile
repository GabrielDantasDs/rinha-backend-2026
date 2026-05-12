# Build
FROM gcc:13 AS builder

WORKDIR /app
COPY src/ .
COPY dataset/ ./dataset/
COPY hnsw_index.bin ./
COPY preprocessed_data.bin ./

RUN gcc -O2 -o preprocessor preprocessor.c
RUN gcc -O2 -o hnsw hnsw.c
RUN gunzip -c ./dataset/references.json.gz > references.json 
RUN ./preprocessor references.json preprocessed_data.bin
RUN ./hnsw
RUN gcc -O2 -static -o app main.c validation.c normalization.c hnsw_search.c -lm

# Runtime mínimo
FROM alpine:3.20

WORKDIR /app

COPY --from=builder /app/app .
COPY --from=builder /app/dataset/ ./dataset/
COPY --from=builder /app/preprocessed_data.bin .

##Delete after
COPY --from=builder /app/hnsw_index.bin .


CMD ["./app"]