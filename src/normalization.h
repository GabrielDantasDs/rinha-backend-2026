#ifndef NORMALIZATION_H
#define NORMALIZATION_H

/* Load constants + MCC risk table into memory.
 * Call once at startup before any create_vector_from_request().
 * Returns 0 on success, -1 if a file is missing (defaults are used). */
int normalization_init(const char *constants_path, const char *mcc_risk_path);

int create_vector_from_request(char *request, float *vector);
int normalize_vector(float *vector, int size);

#endif // NORMALIZATION_H