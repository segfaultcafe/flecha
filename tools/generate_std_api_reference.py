#!/usr/bin/env python3
"""Generate the checked-in Flecha v0.3 standard-library API reference."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
STD_ROOT = ROOT / "std" / "src"
MANIFEST = ROOT / "docs" / "standard_library_modules.json"
OUTPUT = ROOT / "docs" / "standard_library.md"


@dataclass(frozen=True)
class SourceModule:
    name: str
    files: tuple[Path, ...]
    declarations: tuple[str, ...]
    methods: tuple[str, ...]


def scrub(source: str) -> str:
    """Replace comments and string contents while preserving layout."""
    result: list[str] = []
    index = 0
    block_comment = False
    quote: str | None = None
    while index < len(source):
        current = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if block_comment:
            if current == "*" and following == "/":
                result.extend("  ")
                index += 2
                block_comment = False
            else:
                result.append("\n" if current == "\n" else " ")
                index += 1
            continue
        if quote is not None:
            if current == "\\" and following:
                result.extend("  ")
                index += 2
            elif current == quote:
                result.append(" ")
                index += 1
                quote = None
            else:
                result.append("\n" if current == "\n" else " ")
                index += 1
            continue
        if current == "/" and following == "*":
            result.extend("  ")
            index += 2
            block_comment = True
        elif current == "/" and following == "/":
            while index < len(source) and source[index] != "\n":
                result.append(" ")
                index += 1
        elif current in {'"', "'"}:
            result.append(" ")
            index += 1
            quote = current
        else:
            result.append(current)
            index += 1
    return "".join(result)


def compact_signature(value: str) -> str:
    value = re.sub(r"\s+", " ", value.strip())
    value = re.sub(r"\s+([,;)])", r"\1", value)
    value = re.sub(r"([(<])\s+", r"\1", value)
    return value


def matching_brace(masked: str, opening: int) -> int:
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise ValueError("unclosed declaration block")


def public_structure_declaration(value: str) -> str:
    """Remove private storage from an exported structure declaration."""
    lines = value.strip().splitlines()
    if not lines or not re.match(r"export\s+structure\b", lines[0].strip()):
        return value.strip()

    rendered: list[str] = [lines[0]]
    pending: list[str] = []
    private_depth = 0
    private_field = False
    for line in lines[1:]:
        stripped = line.strip()
        if private_depth:
            private_depth += line.count("{") - line.count("}")
            continue
        if private_field:
            if ";" in line:
                private_field = False
            continue
        if stripped.startswith("private {"):
            pending.clear()
            private_depth = line.count("{") - line.count("}")
            continue
        if stripped.startswith("private "):
            pending.clear()
            private_field = ";" not in line
            continue
        if not stripped or stripped.startswith("//"):
            pending.append(line)
            continue
        rendered.extend(pending)
        pending.clear()
        rendered.append(line)
    return "\n".join(rendered)


def declaration_end(masked: str, start: int) -> tuple[int, bool]:
    parens = brackets = 0
    for index in range(start, len(masked)):
        character = masked[index]
        if character == "(":
            parens += 1
        elif character == ")":
            parens -= 1
        elif character == "[":
            brackets += 1
        elif character == "]":
            brackets -= 1
        elif character == ";" and parens == brackets == 0:
            return index + 1, False
        elif character == "{" and parens == brackets == 0:
            prefix = masked[start:index]
            block_declaration = bool(
                re.match(
                    r"export\s+(structure|interface|enum|typeset|decoration)\b",
                    prefix.strip(),
                )
                or re.match(r"export\s+error\s+[A-Z]\w*\s*$", prefix.strip())
            )
            if block_declaration:
                end = matching_brace(masked, index) + 1
                while end < len(masked) and masked[end].isspace():
                    end += 1
                if end < len(masked) and masked[end] == ";":
                    end += 1
                return end, True
            return index, False
    raise ValueError("unterminated exported declaration")


def exported_declarations(source: str) -> tuple[tuple[str, ...], set[str]]:
    masked = scrub(source)
    declarations: list[str] = []
    exported_types: set[str] = set()
    for match in re.finditer(r"(?m)^export\s+", masked):
        start = match.start()
        end, block = declaration_end(masked, start)
        original = source[start:end]
        if block:
            rendered = public_structure_declaration(original)
            type_match = re.match(
                r"export\s+(?:structure|interface|enum|error|typeset|decoration)\s+([A-Za-z_]\w*)",
                compact_signature(original[: original.find("{")]),
            )
            if type_match:
                exported_types.add(type_match.group(1))
        else:
            rendered = compact_signature(original)
            if not rendered.endswith(";"):
                rendered += ";"
        declarations.append(rendered)
    return tuple(declarations), exported_types


def public_methods(source: str, exported_types: set[str]) -> tuple[str, ...]:
    masked = scrub(source)
    methods: list[str] = []
    depth = 0
    boundary = 0
    index = 0
    while index < len(masked):
        character = masked[index]
        if character == "{" and depth == 0:
            header = masked[boundary:index].strip()
            compact = compact_signature(header)
            owner = re.search(r"\b([A-Za-z_]\w*)(?:<[^{};]+>)?::[A-Za-z_]\w*[!?]?\s*\(", compact)
            if (
                owner
                and owner.group(1) in exported_types
                and not re.search(r"\bprivate\b", compact)
                and not compact.startswith("export ")
            ):
                methods.append(compact + ";")
            depth = 1
        elif character == "{" and depth > 0:
            depth += 1
        elif character == "}" and depth > 0:
            depth -= 1
            if depth == 0:
                boundary = index + 1
        elif character == ";" and depth == 0:
            boundary = index + 1
        index += 1
    return tuple(methods)


def logical_module_name(path: Path, source: str) -> str:
    """Return the public module identity, honoring namespace attachment."""
    masked = scrub(source)
    module_match = re.search(r"(?m)^module\s+([A-Za-z_]\w*)\s*;", masked)
    if module_match is None:
        raise ValueError(f"{path.relative_to(ROOT)} has exports but no module declaration")

    attachment_match = re.search(
        r"(?m)^under\s+namespace\s+(root(?:::[A-Za-z_]\w*)*)\s*;",
        masked,
    )
    if attachment_match is not None:
        namespace = attachment_match.group(1).split("::")[1:]
        return "std::" + "::".join((*namespace, module_match.group(1)))

    relative = path.relative_to(STD_ROOT)
    return "std::" + "::".join((*relative.parts[:-1], module_match.group(1)))


def scan_modules() -> dict[str, SourceModule]:
    grouped: dict[str, list[Path]] = {}
    for path in sorted(STD_ROOT.rglob("*.fl")):
        if path.name.endswith("_test.fl") or path.name in {"runtime.fl", "std.fl"}:
            continue
        source = path.read_text(encoding="utf-8")
        if not re.search(r"(?m)^export\s+", source):
            continue
        relative = path.relative_to(STD_ROOT)
        name = logical_module_name(path, source)
        grouped.setdefault(name, []).append(relative)

    modules: dict[str, SourceModule] = {}
    for name, files in grouped.items():
        declarations: list[str] = []
        methods: list[str] = []
        for relative in files:
            source = (STD_ROOT / relative).read_text(encoding="utf-8")
            file_declarations, exported_types = exported_declarations(source)
            declarations.extend(file_declarations)
            methods.extend(public_methods(source, exported_types))
        modules[name] = SourceModule(
            name=name,
            files=tuple(files),
            declarations=tuple(declarations),
            methods=tuple(methods),
        )
    return modules


def load_manifest() -> dict[str, dict[str, str]]:
    raw = json.loads(MANIFEST.read_text(encoding="utf-8"))
    entries = raw.get("modules")
    if not isinstance(entries, list):
        raise ValueError("standard_library_modules.json must contain a modules array")
    result: dict[str, dict[str, str]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ValueError("each module entry must have a string name")
        name = entry["name"]
        if name in result:
            raise ValueError(f"duplicate module metadata for {name}")
        for field in ("summary", "contract", "reference", "example"):
            if not isinstance(entry.get(field), str) or not entry[field].strip():
                raise ValueError(f"{name} is missing {field}")
        result[name] = entry
    return result


def relative_link(target: str) -> str:
    if target.startswith(("http://", "https://", "#")):
        return target
    if target.startswith("docs/"):
        return target.removeprefix("docs/")
    return "../" + target


def render(modules: dict[str, SourceModule], metadata: dict[str, dict[str, str]]) -> str:
    discovered = set(modules)
    documented = set(metadata)
    if discovered != documented:
        missing = sorted(discovered - documented)
        stale = sorted(documented - discovered)
        details = []
        if missing:
            details.append("missing metadata: " + ", ".join(missing))
        if stale:
            details.append("stale metadata: " + ", ".join(stale))
        raise ValueError("; ".join(details))

    lines = [
        "# Flecha 0.3 standard library",
        "",
        "This is the public API index for the standard library shipped with Flecha",
        "0.3.0. Import names, declarations, and examples below are release contracts.",
        "The declaration appendix is generated directly from `std/src`; CI rejects a",
        "changed public surface until this checked-in reference and its module metadata",
        "are updated together.",
        "",
        "Applications import these modules through the `std` dependency alias:",
        "",
        "```flecha",
        "@imports",
        "    :: lib::std::containers::collections",
        "    :: lib::std::fmt;",
        "```",
        "",
        "The installed, runnable [`standard_library`](../examples/standard_library/README.md)",
        "example imports every module in this index. The topic guides carry the full",
        "ownership, allocation, error, blocking, determinism, and platform contracts.",
        "The [library API rules](library_api_style.md) govern naming and compatibility.",
        "",
        "## Module index",
        "",
        "| Module | Purpose | Operational contract | Reference | Focused example |",
        "| --- | --- | --- | --- | --- |",
    ]
    for name in sorted(modules):
        entry = metadata[name]
        reference = relative_link(entry["reference"])
        example = relative_link(entry["example"])
        lines.append(
            f"| `{name}` | {entry['summary']} | {entry['contract']} | "
            f"[guide]({reference}) | [example/test]({example}) |"
        )

    lines.extend(
        [
            "",
            "## Reading the declaration appendix",
            "",
            "`export` declarations are listed exactly as public top-level declarations.",
            "Public methods belonging to exported types follow them. Function bodies are",
            "intentionally omitted; enum variants and public structure fields remain visible.",
            "Private implementation details and native runtime symbols are not public API.",
            "",
            "## Declaration appendix",
            "",
        ]
    )
    for name in sorted(modules):
        module = modules[name]
        entry = metadata[name]
        anchor = name.replace("::", "-")
        lines.extend(
            [
                f'<a id="{anchor}"></a>',
                f"### `{name}`",
                "",
                entry["summary"] + ".",
                "",
                "Sources: "
                + ", ".join(
                    f"[`std/src/{path.as_posix()}`](../std/src/{path.as_posix()})"
                    for path in module.files
                ),
                "",
                "```flecha",
            ]
        )
        lines.extend(module.declarations)
        if module.methods:
            lines.extend(["", "// Public methods"])
            lines.extend(module.methods)
        lines.extend(["```", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check", action="store_true", help="fail if the checked-in reference is stale"
    )
    arguments = parser.parse_args()
    try:
        generated = render(scan_modules(), load_manifest())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"standard-library reference: {error}", file=sys.stderr)
        return 1

    if arguments.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != generated:
            print(
                "docs/standard_library.md is stale; run "
                "tools/generate_std_api_reference.py",
                file=sys.stderr,
            )
            return 1
        return 0

    OUTPUT.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
