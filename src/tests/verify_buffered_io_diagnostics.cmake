file(MAKE_DIRECTORY "${FLECHA_OUTPUT}")

function(reject_buffered name code body)
    set(source "${FLECHA_OUTPUT}/${name}.fl")
    set(object "${FLECHA_OUTPUT}/${name}.o")
    file(WRITE "${source}"
        "module ${name};\n"
        "@imports\n"
        "    :: lib::std::io::buffered\n"
        "    :: lib::std::io::file\n"
        "    :: lib::std::io::stream;\n"
        "${body}\n")
    file(REMOVE "${object}")
    execute_process(
        COMMAND "${FLECHA_COMPILER}" --emit-object "${source}"
            --lib "std=${FLECHA_STD}" -o "${object}"
        RESULT_VARIABLE result ERROR_VARIABLE errors TIMEOUT 30)
    if(result EQUAL 0 OR EXISTS "${object}" OR NOT errors MATCHES "${code}")
        message(FATAL_ERROR
            "${name}: expected ${code} without object emission: ${errors}")
    endif()
endfunction()

reject_buffered(buffered_move_reuse FLECHA-S0059 [=[
void main() {
    reader := stream::Reader::init(
        [](destination) => { return (none, none); }
    );
    value, failure := buffered::BufferedReader::withCapacity(reader, 4);
    moved := value;
    [1]u8 destination = none;
    value.read(destination[:]);
}
]=])

reject_buffered(stream_move_reuse FLECHA-S0059 [=[
void main() {
    writer := stream::Writer::init(
        [](source) => { return (source.len(), none); }
    );
    value, failure := buffered::BufferedWriter::withCapacity(writer, 4);
    [1]u8 source = [1];
    writer.write(source[:]);
}
]=])

reject_buffered(file_access_during_buffered_borrow FLECHA-S0029 [=[
void main() {
    opened, failure := file::File::open(
        "buffered.tmp",
        file::OpenOptions::readOnly()
    );
    reader := opened.reader();
    value, allocationFailure :=
        buffered::BufferedReader::withCapacity(reader, 4);
    [1]u8 destination = none;
    opened.read(destination[:]);
    value.read(destination[:]);
}
]=])

reject_buffered(overlapping_file_adapters FLECHA-S0029 [=[
void main() {
    opened, failure := file::File::open(
        "buffered.tmp",
        file::OpenOptions::readOnly()
    );
    reader := opened.reader();
    value, allocationFailure :=
        buffered::BufferedReader::withCapacity(reader, 4);
    writer := opened.writer();
    [1]u8 destination = none;
    value.read(destination[:]);
}
]=])

reject_buffered(local_file_escape FLECHA-S0058 [=[
buffered::BufferedReader make_reader() {
    opened, failure := file::File::open(
        "buffered.tmp",
        file::OpenOptions::readOnly()
    );
    reader := opened.reader();
    value, allocationFailure :=
        buffered::BufferedReader::withCapacity(reader, 4);
    return value;
}
]=])
