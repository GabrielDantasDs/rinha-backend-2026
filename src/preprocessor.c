#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

typedef struct
{
    float vector[14];
    unsigned char label;
} item_t;

typedef enum {
    S_OUTSIDE,
    S_KEY,
    S_AFTER_KEY,
    S_VECTOR,
    S_NUMBER,
    S_LABEL
} state_t;

int preprocess_data(const char *input_file, const char *output_file) {
    FILE *in = fopen(input_file, "r");
    if (!in) { perror("fopen input"); return -1; }

    FILE *out = fopen(output_file, "wb");
    if (!out) { perror("fopen output"); fclose(in); return -1; }

    state_t state = S_OUTSIDE;

    item_t item;
    int vec_idx = 0;

    char key[32] = {0};
    int key_pos = 0;

    char numbuf[64] = {0};
    int num_pos = 0;

    char labelbuf[16] = {0};
    int label_pos = 0;

    int c;
    while ((c = fgetc(in)) != EOF) {

        switch (state) {

        case S_OUTSIDE:
            if (c == '"') {
                state = S_KEY;
                key_pos = 0;
            }
            break;

        case S_KEY:
            if (c == '"') {
                key[key_pos] = '\0';
                state = S_AFTER_KEY;
            } else if (key_pos < (int)sizeof(key)-1) {
                key[key_pos++] = (char)c;
            }
            break;

        case S_AFTER_KEY:
            if (c == '[' && strcmp(key, "vector") == 0) {
                state = S_VECTOR;
                vec_idx = 0;
            } else if (c == '"' && strcmp(key, "label") == 0) {
                state = S_LABEL;
                label_pos = 0;
            }
            break;

        case S_VECTOR:
            if (isdigit(c) || c == '-' || c == '.') {
                state = S_NUMBER;
                num_pos = 0;
                numbuf[num_pos++] = (char)c;
            } else if (c == ']') {
                state = S_OUTSIDE;
            }
            break;

        case S_NUMBER:
            if (isdigit(c) || c == '.' || c == 'e' || c == 'E' || c == '-') {
                if (num_pos < (int)sizeof(numbuf)-1)
                    numbuf[num_pos++] = (char)c;
            } else {
                numbuf[num_pos] = '\0';

                if (vec_idx < 14) {
                    item.vector[vec_idx++] = strtof(numbuf, NULL);
                }

                state = (c == ']') ? S_OUTSIDE : S_VECTOR;
            }
            break;

        case S_LABEL:
            if (c == '"') {
                labelbuf[label_pos] = '\0';

                if (strcmp(labelbuf, "legit") == 0)
                    item.label = 0;
                else
                    item.label = 1;

                // item completo → escreve
                fwrite(&item, sizeof(item_t), 1, out);

                state = S_OUTSIDE;
            } else if (label_pos < (int)sizeof(labelbuf)-1) {
                labelbuf[label_pos++] = (char)c;
            }
            break;
        }
    }

    fclose(in);
    fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s input.json output.bin\n", argv[0]);
        return 1;
    }

    return preprocess_data(argv[1], argv[2]);
}