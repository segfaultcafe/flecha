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
    :: lib::std::io::errors
    :: lib::std::io::file
    :: lib::std::io::filesystem;

i32 main() {
    text, text_error := filesystem::read_file(\"${textPath}\");
    if text_error != none or text != \"flecha\\n\" {
        return 1;
    }
    text_info, text_metadata_error := filesystem::metadata(\"${textPath}\");
    if text_metadata_error != none or
        text_info.type != file::EntryType.File or text_info.size != 7 {
        return 32;
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

    if filesystem::write_file(\"${writePath}\", \"hi mom\") != none {
        return 35;
    }
    message, message_error := filesystem::read_file(\"${writePath}\");
    if message_error != none or message != \"hi mom\" {
        return 36;
    }

    [4]u8 bytes = ['A', 0, 'B', '\\n'];
    if filesystem::write_bytes(\"${writePath}\", bytes[:]) != none {
        return 5;
    }
    written, written_error := filesystem::read_file(\"${writePath}\");
    if written_error != none or written.len() != 4 or
        written[0] != 'A' or written[1] != '\\0' or
        written[2] != 'B' or written[3] != '\\n' {
        return 6;
    }

    [0]u8 no_bytes = none;
    if filesystem::write_bytes(\"${writePath}\", no_bytes[:]) != none {
        return 7;
    }
    truncated, truncated_error := filesystem::read_file(\"${writePath}\");
    if truncated_error != none or truncated.len() != 0 {
        return 8;
    }

    if filesystem::write_bytes(invalid_path.view(), no_bytes[:]) !=
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
    directory_info, directory_metadata_error := filesystem::metadata(
        \"${createdDirectory}\"
    );
    if directory_metadata_error != none or
        directory_info.type != file::EntryType.Directory or
        directory_info.size != 0 {
        return 33;
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

    if filesystem::write_bytes(\"${renameSource}\", bytes[:]) != none {
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
    if filesystem::remove_directory(\"${createdDirectory}\", false) !=
        errors::IoError.DirectoryNotEmpty {
        return 24;
    }
    if filesystem::remove_file(\"${renameDestination}\") != none {
        return 17;
    }
    if filesystem::remove_file(\"${renameDestination}\") != errors::IoError.NotFound {
        return 18;
    }
    if filesystem::remove_directory(\"${createdDirectory}\", false) != none {
        return 25;
    }
    present, exists_error = filesystem::exists(\"${createdDirectory}\");
    if exists_error != none or present {
        return 26;
    }
    if filesystem::remove_directory(\"${createdDirectory}\", false) !=
        errors::IoError.NotFound {
        return 27;
    }
    if filesystem::remove_directory(\"${createdDirectory}\", true) != none {
        return 28;
    }

    if filesystem::create_directory(\"${createdDirectory}\") != none or
        filesystem::write_bytes(\"${renameSource}\", bytes[:]) != none {
        return 29;
    }
    if filesystem::remove_directory(\"${createdDirectory}\", true) != none {
        return 30;
    }
    present, exists_error = filesystem::exists(\"${createdDirectory}\");
    if exists_error != none or present {
        return 31;
    }

    missing_info, missing_metadata_error := filesystem::metadata(\"${missingPath}\");
    if missing_metadata_error != errors::IoError.NotFound or
        missing_info.type != file::EntryType.Other or missing_info.size != 0 {
        return 34;
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
