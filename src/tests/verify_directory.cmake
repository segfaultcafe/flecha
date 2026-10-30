set(dataDirectory "${FLECHA_OUTPUT}_data")
set(filePath "${dataDirectory}/alpha.txt")
set(nestedDirectory "${dataDirectory}/nested")
set(missingPath "${dataDirectory}/missing")
set(linkPath "${dataDirectory}/alpha.link")
file(REMOVE_RECURSE "${dataDirectory}")
file(MAKE_DIRECTORY "${dataDirectory}" "${nestedDirectory}")
file(WRITE "${filePath}" "alpha")

set(expectedEntryCount 2)
set(linkCase "")
set(linkRequirement "")
if(UNIX)
    file(CREATE_LINK "${filePath}" "${linkPath}" SYMBOLIC RESULT linkResult)
    if(linkResult STREQUAL "0")
        set(expectedEntryCount 3)
        set(linkRequirement " or !foundLink")
        set(linkCase " else if entry.name() == \"alpha.link\" {
            if entry.kind() != file::EntryType.SymbolicLink or foundLink {
                return 16;
            }
            foundLink = true;
       }")
    endif()
endif()

set(sourceDirectory "${FLECHA_OUTPUT}_project")
file(MAKE_DIRECTORY "${sourceDirectory}")
set(source "${sourceDirectory}/main.fl")
file(WRITE "${source}"
"module main;

@imports
    :: lib::std::io::errors
    :: lib::std::io::file;

i32 iterate_known_directory() {
    directory, openError := file::Directory::open(\"${dataDirectory}\");
    if openError != none or !directory.isOpen() {
        return 1;
    }

    moved := directory;
    bool foundFile = false;
    bool foundDirectory = false;
    bool foundLink = false;
    usize count = 0;

    while true {
        entry, hasEntry, nextError := moved.next();
        if nextError != none {
            return 2;
        }
        if !hasEntry {
            break;
        }

        if entry.entryName != entry.name() or entry.entryKind != entry.kind() {
            return 3;
        }
        if entry.name() == \"alpha.txt\" {
            if entry.kind() != file::EntryType.File or foundFile {
                return 4;
            }
            foundFile = true;
        } else if entry.name() == \"nested\" {
            if entry.kind() != file::EntryType.Directory or foundDirectory {
                return 5;
            }
            foundDirectory = true;
       }${linkCase} else {
            return 6;
        }
        count++;
    }

    eofEntry, hasEofEntry, eofError := moved.next();
    if eofError != none or hasEofEntry or eofEntry.name() != \"\" or
        eofEntry.kind() != file::EntryType.Other {
        return 7;
    }
    if count != ${expectedEntryCount} or !foundFile or !foundDirectory${linkRequirement} {
        return 8;
    }
    if moved.close() != none or moved.isOpen() {
        return 9;
    }
    closedEntry, hasClosedEntry, closedError := moved.next();
    if closedError != errors::IoError.Closed or hasClosedEntry or
        closedEntry.name() != \"\" {
        return 10;
    }
    if moved.close() != errors::IoError.Closed {
        return 11;
    }
    return 42;
}

i32 empty_directory_reaches_eof() {
    directory, openError := file::Directory::open(\"${nestedDirectory}\");
    if openError != none {
        return 12;
    }
    entry, hasEntry, nextError := directory.next();
    if nextError != none or hasEntry or entry.name() != \"\" {
        return 13;
    }
    return 42;
}

i32 entry_survives_directory_close() {
    directory, openError := file::Directory::open(\"${dataDirectory}\");
    if openError != none {
        return 17;
    }
    entry, hasEntry, nextError := directory.next();
    if nextError != none or !hasEntry or entry.name() == \"\" {
        return 18;
    }
    if directory.close() != none {
        return 19;
    }
    movedEntry := entry;
    if movedEntry.name() == \"\" {
        return 20;
    }
    return 42;
}

i32 implicit_close() {
    directory, openError := file::Directory::open(\"${dataDirectory}\");
    if openError != none or !directory.isOpen() {
        return 21;
    }
    return 42;
}

i32 main() {
    result := iterate_known_directory();
    if result != 42 {
        return result;
    }
    result = empty_directory_reaches_eof();
    if result != 42 {
        return result;
    }
    result = entry_survives_directory_close();
    if result != 42 {
        return result;
    }
    result = implicit_close();
    if result != 42 {
        return result;
    }

    missing, missingError := file::Directory::open(\"${missingPath}\");
    if missingError != errors::IoError.NotFound or missing.isOpen() {
        return 14;
    }
    ordinaryFile, fileError := file::Directory::open(\"${filePath}\");
    if fileError != errors::IoError.NotFound or ordinaryFile.isOpen() {
        return 15;
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
    message(FATAL_ERROR "directory compilation failed: ${compileError}")
endif()
execute_process(COMMAND "${FLECHA_OUTPUT}" RESULT_VARIABLE runResult TIMEOUT 10)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR "directory program returned ${runResult}, expected 42")
endif()

file(REMOVE_RECURSE "${dataDirectory}" "${sourceDirectory}")
