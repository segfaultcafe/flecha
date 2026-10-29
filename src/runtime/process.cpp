#include <cstddef>
#include <cstdint>
#include <cstring>

#include "runtime/runtime_abi.h"

namespace {

size_t argumentCount = 0;
const char* const* argumentValues = nullptr;

}  // namespace

extern "C" {

void flecha_runtime_v1_process_arguments_initialize(int32_t count,
                                                    const char* const* values) {
    if (count <= 0 || values == nullptr) {
        argumentCount = 0;
        argumentValues = nullptr;
        return;
    }

    argumentCount = static_cast<size_t>(count);
    argumentValues = values;
}

size_t flecha_runtime_v1_process_argument_count(void) { return argumentCount; }

int32_t flecha_runtime_v1_process_argument_capture(FlechaRuntimeString* output,
                                                   size_t index) {
    if (output == nullptr) return FLECHA_RUNTIME_PROCESS_ARGUMENT_INVALID_INDEX;
    *output = {nullptr, 0, 0};

    if (index >= argumentCount || argumentValues == nullptr ||
        argumentValues[index] == nullptr)
        return FLECHA_RUNTIME_PROCESS_ARGUMENT_INVALID_INDEX;

    const char* value = argumentValues[index];
    const size_t length = std::strlen(value);
    const int32_t status = flecha_runtime_v1_string_try_from_bytes(
        output, reinterpret_cast<const uint8_t*>(value), length);
    return status == FLECHA_RUNTIME_STRING_OK
               ? FLECHA_RUNTIME_PROCESS_ARGUMENT_OK
               : FLECHA_RUNTIME_PROCESS_ARGUMENT_OUT_OF_MEMORY;
}

}  // extern "C"
