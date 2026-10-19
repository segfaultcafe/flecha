set(dataDirectory "${FLECHA_OUTPUT}_data")
file(MAKE_DIRECTORY "${dataDirectory}")
set(textPath "${dataDirectory}/text.txt")
set(emptyPath "${dataDirectory}/empty.bin")
set(missingPath "${dataDirectory}/missing.txt")
set(writePath "${dataDirectory}/written.bin")
set(createdDirectory "${dataDirectory}/created")
set(renameSource "${createdDirectory}/source.bin")
set(renameDestination "${createdDirectory}/destination.bin")
set(missingParentDirectory "${createdDirectory}/missing/child")
set(missingRenameSource "${createdDirectory}/missing.bin")
file(REMOVE_RECURSE "${createdDirectory}")
file(WRITE "${textPath}" "flecha\n")
file(WRITE "${emptyPath}" "")
file(WRITE "${writePath}" "stale trailing contents")

set(sourceDirectory "${FLECHA_OUTPUT}_project")
file(MAKE_DIRECTORY "${sourceDirectory}")
set(source "${sourceDirectory}/main.fl")
file(WRITE "${source}"
"module main;

@imports
    :: lib::std::io::filesystem
    :: lib::std::io::errors;

i32 main() {
    text, text_error := filesystem::read_file(\"${textPath}\");
    if text_error != none or text != \"flecha\\n\" {
        return 1;
    }

    empty, empty_error := filesystem::read_file(\"${emptyPath}\");
    if empty_error != none or empty.len() != 0 {
        return 2;
    }

    missing, missing_error := filesystem::read_file(\"${missingPath}\");
    if missing_error != errors::IoError.NotFound or missing.len() != 0 {
        return 3;
    }

    string invalid_path = \"bad\" + '\\0'.string() + \"path\";
    invalid, invalid_error := filesystem::read_file(invalid_path.view());
    if invalid_error != errors::IoError.InvalidPath or invalid.len() != 0 {
        return 4;
    }

    [4]u8 bytes = [65, 0, 66, 10];
    if filesystem::write_file(\"${writePath}\", bytes[:]) != none {
        return 5;
    }
    written, written_error := filesystem::read_file(\"${writePath}\");
    if written_error != none or written.len() != 4 or
        written[0] != 'A' or written[1] != '\\0' or
        written[2] != 'B' or written[3] != '\\n' {
        return 6;
    }

    [0]u8 no_bytes = none;
    if filesystem::write_file(\"${writePath}\", no_bytes[:]) != none {
        return 7;
    }
    truncated, truncated_error := filesystem::read_file(\"${writePath}\");
    if truncated_error != none or truncated.len() != 0 {
        return 8;
    }

    if filesystem::write_file(invalid_path.view(), no_bytes[:]) !=
        errors::IoError.InvalidPath {
        return 9;
    }

    present, exists_error := filesystem::exists(\"${missingPath}\");
    if exists_error != none or present {
        return 10;
    }
    present, exists_error = filesystem::exists(\"${textPath}\");
    if exists_error != none or !present {
        return 19;
    }
    if filesystem::create_directory(\"${textPath}\") !=
        errors::IoError.AlreadyExists {
        return 20;
    }
    if filesystem::create_directory(\"${createdDirectory}\") != none {
        return 11;
    }
    if filesystem::create_directory(\"${createdDirectory}\") !=
        errors::IoError.AlreadyExists {
        return 12;
    }
    if filesystem::create_directory(\"${missingParentDirectory}\") !=
        errors::IoError.NotFound {
        return 21;
    }
    present, exists_error = filesystem::exists(\"${createdDirectory}\");
    if exists_error != none or !present {
        return 13;
    }

    if filesystem::write_file(\"${renameSource}\", bytes[:]) != none {
        return 14;
    }
    if filesystem::rename(\"${renameSource}\", \"${renameDestination}\") != none {
        return 15;
    }
    if filesystem::rename(\"${missingRenameSource}\", \"${renameSource}\") !=
        errors::IoError.NotFound {
        return 22;
    }
    source_present, source_exists_error := filesystem::exists(\"${renameSource}\");
    destination_present, destination_exists_error := filesystem::exists(
        \"${renameDestination}\"
    );
    if source_exists_error != none or source_present or
        destination_exists_error != none or !destination_present {
        return 16;
    }
    renamed, renamed_error := filesystem::read_file(\"${renameDestination}\");
    if renamed_error != none or renamed.len() != 4 or
        renamed[0] != 'A' or renamed[1] != '\\0' or
        renamed[2] != 'B' or renamed[3] != '\\n' {
        return 23;
    }
    if filesystem::remove_file(\"${renameDestination}\") != none {
        return 17;
    }
    if filesystem::remove_file(\"${renameDestination}\") != errors::IoError.NotFound {
        return 18;
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
    message(FATAL_ERROR "filesystem compilation failed: ${compileError}")
endif()
execute_process(COMMAND "${FLECHA_OUTPUT}" RESULT_VARIABLE runResult TIMEOUT 10)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR "filesystem program returned ${runResult}, expected 42")
endif()
file(REMOVE_RECURSE "${createdDirectory}")
