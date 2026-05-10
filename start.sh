#!/bin/sh

set -e

if [ ! -f ./preprocessed_data.bin ]; then
    echo "Generating preprocessed_data.bin..."

    # gunzip -c ./dataset/references.json.gz > references.json
    gcc -o ./src/preprocessor ./src/preprocessor.c
    ./src/preprocessor ./references.json preprocessed_data.bin
else
    echo "preprocessed_data.bin already exists."
fi
    
if [ ! -f ./hnsw_index.bin ]; then
    echo "Generating hnsw_index.bin..."

    gcc -o ./src/hnsw ./src/hnsw.c
    ./src/hnsw
else
    echo "hnsw_index.bin already exists."
fi

echo "Starting application..."

gcc -o ./src/main ./src/main.c ./src/validation.c ./src/normalization.c ./src/hnsw_search.c -lm