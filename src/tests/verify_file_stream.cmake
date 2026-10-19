set(dataPath "${FLECHA_OUTPUT}_data.bin")
file(REMOVE "${dataPath}")

set(sourceDirectory "${FLECHA_OUTPUT}_project")
file(MAKE_DIRECTORY "${sourceDirectory}")
set(source "${sourceDirectory}/main.fl")
file(WRITE "${source}"
"module main;

@imports
    :: lib::std::io::file
    :: lib::std::io::errors;

i32 main() {
    writer, openError := file::File::open(
        \"${dataPath}\",
        file::OpenOptions::writeOnly()
    );
    if openError != none or !writer.isOpen() {
        return 1;
    }

    [4]u8 bytes = [65, 0, 66, 10];
    written, writeError := writer.write(bytes[:]);
    if writeError != none or written != 4 {
        return 2;
    }
    if writer.flush() != none or writer.close() != none or writer.isOpen() {
        return 3;
    }

    exclusiveOptions := build file::OpenOptions {
        self.write = true;
        self.createNew = true;
    };
    exclusive, exclusiveError := file::File::open(
        \"${dataPath}\",
        exclusiveOptions
    );
    if exclusiveError != errors::IoError.AlreadyExists or exclusive.isOpen() {
        return 4;
    }

    reader, readOpenError := file::File::open(
        \"${dataPath}\",
        file::OpenOptions::readOnly()
    );
    if readOpenError != none {
        return 5;
    }
    [4]u8 output = none;
    count, readError := reader.read(output[:]);
    if readError != none or count != 4 or
        output[0] != 65 or output[1] != 0 or
        output[2] != 66 or output[3] != 10 {
        return 6;
    }
    position, seekError := reader.seek(-2, file::SeekFrom.End);
    if seekError != none or position != 2 {
        return 7;
    }
    [2]u8 tail = none;
    count, readError = reader.read(tail[:]);
    if readError != none or count != 2 or tail[0] != 66 or tail[1] != 10 {
        return 8;
    }
    if reader.close() != none {
        return 9;
    }
    return 42;
}
")

execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${source}" -o "${FLECHA_OUTPUT}"
        --lib "std=${FLECHA_STD}"
    RESULT_VARIABLE compileResult ERROR_VARIABLE compileError TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "file-stream compilation failed: ${compileError}")
endif()
execute_process(COMMAND "${FLECHA_OUTPUT}" RESULT_VARIABLE runResult TIMEOUT 10)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR "file-stream program returned ${runResult}, expected 42")
endif()

file(REMOVE "${dataPath}")
