#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ROUND_COUNT = 10000000,
    COLLECTION_COUNT = 4000000,
    COLLECTION_GROWTH_COUNT = 1000000,
    UTF8_GROUPS = 8192,
    UTF8_REPETITIONS = 128,
    FNV_BYTES = 262144,
    FNV_REPETITIONS = 64,
};

static uint64_t rotate_left(uint64_t value, unsigned count) {
    return (value << count) | (value >> (64U - count));
}

static uint64_t rounds(void) {
    uint64_t left = UINT64_C(81985529216486895);
    uint64_t right = UINT64_C(18364758544493064720);
    for (size_t index = 0; index < ROUND_COUNT; ++index) {
        left = rotate_left(left, 7) ^ right;
        right = rotate_left(right, 13) + left + (uint64_t)index;
        left ^= rotate_left(right, 17);
    }
    return left ^ right;
}

static uint64_t collections_workload(void) {
    uint64_t* values = malloc(COLLECTION_COUNT * sizeof(*values));
    if (values == NULL) {
        exit(4);
    }
    for (size_t index = 0; index < COLLECTION_COUNT; ++index) {
        values[index] = (uint64_t)index;
    }
    uint64_t total = 0;
    for (size_t index = 0; index < COLLECTION_COUNT; ++index) {
        total += values[index];
    }
    free(values);
    return total;
}

static uint64_t collections_growth_workload(void) {
    size_t length = 0;
    size_t capacity = 0;
    uint64_t* values = NULL;
    for (size_t index = 0; index < COLLECTION_GROWTH_COUNT; ++index) {
        if (length == capacity) {
            size_t next = capacity == 0 ? 4 : capacity * 2;
            uint64_t* replacement = realloc(values, next * sizeof(*values));
            if (replacement == NULL) {
                free(values);
                exit(4);
            }
            values = replacement;
            capacity = next;
        }
        values[length++] = (uint64_t)index;
    }
    uint64_t total = 0;
    for (size_t index = 0; index < length; ++index) total += values[index];
    free(values);
    return total;
}

static int count_valid_utf8(const unsigned char* bytes, size_t length,
                            uint64_t* count) {
    size_t offset = 0;
    uint64_t total = 0;
    while (offset < length) {
        const unsigned char first = bytes[offset];
        size_t width = 1;
        if (first <= 127) {
            width = 1;
        } else if (first >= 194 && first <= 223) {
            width = 2;
            if (length - offset < width || bytes[offset + 1] < 128 ||
                bytes[offset + 1] > 191)
                return 0;
        } else if (first >= 224 && first <= 239) {
            width = 3;
            if (length - offset < width || bytes[offset + 1] < 128 ||
                bytes[offset + 1] > 191 || bytes[offset + 2] < 128 ||
                bytes[offset + 2] > 191 ||
                (first == 224 && bytes[offset + 1] < 160) ||
                (first == 237 && bytes[offset + 1] > 159))
                return 0;
        } else if (first >= 240 && first <= 244) {
            width = 4;
            if (length - offset < width || bytes[offset + 1] < 128 ||
                bytes[offset + 1] > 191 || bytes[offset + 2] < 128 ||
                bytes[offset + 2] > 191 || bytes[offset + 3] < 128 ||
                bytes[offset + 3] > 191 ||
                (first == 240 && bytes[offset + 1] < 144) ||
                (first == 244 && bytes[offset + 1] > 143))
                return 0;
        } else {
            return 0;
        }
        ++total;
        offset += width;
    }
    *count = total;
    return 1;
}

static uint64_t utf8_workload(void) {
    static const unsigned char group[10] = {
        65, 194, 162, 226, 130, 172, 240, 159, 152, 128,
    };
    const size_t length = UTF8_GROUPS * sizeof(group);
    unsigned char* bytes = malloc(length);
    if (bytes == NULL) {
        exit(4);
    }
    for (size_t index = 0; index < UTF8_GROUPS; ++index) {
        memcpy(bytes + index * sizeof(group), group, sizeof(group));
    }

    uint64_t total = 0;
    for (size_t repetition = 0; repetition < UTF8_REPETITIONS; ++repetition) {
        uint64_t count = 0;
        if (!count_valid_utf8(bytes, length, &count)) exit(4);
        total += count;
    }
    free(bytes);
    return total;
}

static uint64_t fnv_workload(void) {
    unsigned char* bytes = malloc(FNV_BYTES);
    if (bytes == NULL) {
        exit(4);
    }
    for (size_t index = 0; index < FNV_BYTES; ++index) {
        bytes[index] = (unsigned char)(index * 31U + 7U);
    }

    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t repetition = 0; repetition < FNV_REPETITIONS; ++repetition) {
        for (size_t index = 0; index < FNV_BYTES; ++index) {
            hash = (hash ^ bytes[index]) * UINT64_C(1099511628211);
        }
    }
    free(bytes);
    return hash;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    uint64_t checksum = 0;
    if (strcmp(argv[1], "rounds") == 0) {
        checksum = rounds();
    } else if (strcmp(argv[1], "collections") == 0) {
        checksum = collections_workload();
    } else if (strcmp(argv[1], "collections_growth") == 0) {
        checksum = collections_growth_workload();
    } else if (strcmp(argv[1], "utf8") == 0) {
        checksum = utf8_workload();
    } else if (strcmp(argv[1], "fnv") == 0) {
        checksum = fnv_workload();
    } else {
        return 3;
    }
    printf("%llu\n", (unsigned long long)checksum);
    return 0;
}
