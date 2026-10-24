#!/usr/bin/env python3
"""Check an arm64 Dusk bundle's Mach-O metadata and closure without running code."""

import argparse
import json
import mmap
import os
from pathlib import Path
import plistlib
import posixpath
import re
import struct
from xml.parsers.expat import ExpatError


THIN_MAGICS = {
    b"\xce\xfa\xed\xfe": ("<", 28),
    b"\xcf\xfa\xed\xfe": ("<", 32),
    b"\xfe\xed\xfa\xce": (">", 28),
    b"\xfe\xed\xfa\xcf": (">", 32),
}
FAT_MAGICS = {
    b"\xca\xfe\xba\xbe": (">", False),
    b"\xbe\xba\xfe\xca": ("<", False),
    b"\xca\xfe\xba\xbf": (">", True),
    b"\xbf\xba\xfe\xca": ("<", True),
}
LOAD_DYLIB_COMMANDS = {0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023}
CPU_NAMES = {7: "i386", 0x1000007: "x86_64", 12: "arm", 0x100000C: "arm64"}


class InvalidMachO(ValueError):
    pass


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", value):
        raise ValueError("expected a numeric macOS version such as 11.0")
    parts = tuple(int(part) for part in value.split("."))
    parts += (0,) * (3 - len(parts))
    if not 0 < parts[0] <= 65535 or any(part > 255 for part in parts[1:]):
        raise ValueError("macOS version is outside the Mach-O version range")
    return parts


def version_text(parts):
    return ".".join(str(part) for part in parts[:2 if parts[2] == 0 else 3])


def packed_version(value):
    return (value >> 16, (value >> 8) & 255, value & 255)


def parse_slice(data, start, size):
    magic = bytes(data[start:start + 4])
    if magic not in THIN_MAGICS:
        raise InvalidMachO("architecture slice has no Mach-O header")
    endian, header_size = THIN_MAGICS[magic]
    if size < header_size:
        raise InvalidMachO("truncated Mach-O header")
    cpu, subtype, filetype, count, command_bytes = struct.unpack_from(endian + "5I", data, start + 4)
    if count > 65536 or command_bytes > 16 * 1024 * 1024:
        raise InvalidMachO("load-command table exceeds verification limits")
    if command_bytes > size - header_size or count > command_bytes // 8:
        raise InvalidMachO("truncated load-command table")
    cursor = start + header_size
    end = cursor + command_bytes
    minimums = []
    dependencies = []
    for _ in range(count):
        if cursor + 8 > end:
            raise InvalidMachO("truncated load command")
        command, length = struct.unpack_from(endian + "2I", data, cursor)
        if length < 8 or length % 4 or cursor + length > end:
            raise InvalidMachO("invalid load-command size")
        if command == 0x32:
            if length < 24:
                raise InvalidMachO("truncated LC_BUILD_VERSION")
            platform, minimum, _, tools = struct.unpack_from(endian + "4I", data, cursor + 8)
            if platform != 1:
                raise InvalidMachO(f"LC_BUILD_VERSION platform {platform} is not macOS")
            if length != 24 + tools * 8:
                raise InvalidMachO("invalid LC_BUILD_VERSION tool table")
            minimums.append(packed_version(minimum))
        elif command == 0x24:
            if length != 16:
                raise InvalidMachO("invalid LC_VERSION_MIN_MACOSX size")
            minimums.append(packed_version(struct.unpack_from(endian + "I", data, cursor + 8)[0]))
        elif command in LOAD_DYLIB_COMMANDS:
            if length < 24:
                raise InvalidMachO("truncated dylib load command")
            name_offset = struct.unpack_from(endian + "I", data, cursor + 8)[0]
            if not 24 <= name_offset < length:
                raise InvalidMachO("invalid dylib name offset")
            raw_name = bytes(data[cursor + name_offset:cursor + length])
            if b"\0" not in raw_name:
                raise InvalidMachO("unterminated dylib name")
            try:
                name = raw_name.split(b"\0", 1)[0].decode("utf-8")
            except UnicodeDecodeError as error:
                raise InvalidMachO("dylib name is not UTF-8") from error
            if not name:
                raise InvalidMachO("empty dylib name")
            dependencies.append(name)
        cursor += length
    if cursor != end:
        raise InvalidMachO("load-command count does not cover its table")
    if len(minimums) != 1 or minimums[0][0] == 0:
        raise InvalidMachO("expected exactly one nonzero macOS minimum-version command")
    return {"cpu": cpu, "subtype": subtype, "filetype": filetype,
            "minimum": minimums[0], "dependencies": dependencies}


def parse_macho(data):
    magic = bytes(data[:4])
    if magic in THIN_MAGICS:
        return [parse_slice(data, 0, len(data))]
    endian, fat64 = FAT_MAGICS[magic]
    if len(data) < 8:
        raise InvalidMachO("truncated universal header")
    count = struct.unpack_from(endian + "I", data, 4)[0]
    entry_size = 32 if fat64 else 20
    table_end = 8 + count * entry_size
    if not 1 <= count <= 64 or table_end > len(data):
        raise InvalidMachO("invalid universal architecture table")
    slices = []
    intervals = []
    identities = set()
    for index in range(count):
        values = struct.unpack_from(endian + ("2I2Q2I" if fat64 else "5I"),
                                    data, 8 + index * entry_size)
        cpu, subtype, offset, size, alignment = values[:5]
        if alignment > 63 or offset % (1 << alignment):
            raise InvalidMachO("invalid universal slice alignment")
        if offset < table_end or size < 4 or offset + size > len(data):
            raise InvalidMachO("universal architecture slice is outside the file")
        if any(offset < other_end and other_start < offset + size
               for other_start, other_end in intervals):
            raise InvalidMachO("overlapping universal architecture slices")
        intervals.append((offset, offset + size))
        parsed = parse_slice(data, offset, size)
        if (parsed["cpu"], parsed["subtype"]) != (cpu, subtype):
            raise InvalidMachO("universal architecture does not match its slice header")
        if (cpu, subtype) in identities:
            raise InvalidMachO("duplicate universal architecture")
        identities.add((cpu, subtype))
        slices.append(parsed)
    return slices


def contained(path, bundle):
    return path == bundle or bundle in path.parents


def verify(bundle, maximum):
    errors = []
    images = {}
    advertised = maximum
    report = {"status": "FAIL", "bundle": str(bundle), "max_macos": version_text(maximum),
              "required_architecture": "arm64",
              "macho_files": 0, "architecture_slices": 0, "dependency_edges": 0,
              "images": [], "errors": errors}
    try:
        bundle = bundle.resolve()
        report["bundle"] = str(bundle)
    except (OSError, RuntimeError) as error:
        errors.append(f"cannot resolve bundle: {error}")
        return report
    if not bundle.is_dir():
        errors.append("bundle directory does not exist")
        return report
    try:
        with (bundle / "Contents/Info.plist").open("rb") as handle:
            info = plistlib.load(handle)
        executable = info["CFBundleExecutable"]
        if not isinstance(executable, str) or not executable or Path(executable).name != executable:
            raise ValueError("CFBundleExecutable must be a file name")
        advertised = version(info["LSMinimumSystemVersion"])
        if advertised > maximum:
            errors.append(f"Info.plist advertises macOS {version_text(advertised)}, above {version_text(maximum)}")
    except (OSError, ValueError, KeyError, TypeError, ExpatError) as error:
        errors.append(f"Contents/Info.plist: {error}")
        executable = "DuskStudio"
    limit = min(maximum, advertised)
    report["advertised_macos"] = version_text(advertised)

    def walk_error(error):
        errors.append(f"cannot enumerate bundle: {error}")

    for directory, subdirs, files in os.walk(bundle, followlinks=False, onerror=walk_error):
        for name in sorted(subdirs + files):
            path = Path(directory) / name
            label = str(path.relative_to(bundle))
            try:
                resolved = path.resolve(strict=True)
                if not contained(resolved, bundle):
                    errors.append(f"{label}: symbolic link escapes the bundle")
                    continue
                if not resolved.is_file() or resolved in images:
                    continue
                with resolved.open("rb") as handle:
                    if handle.read(4) not in THIN_MAGICS.keys() | FAT_MAGICS.keys():
                        continue
                    with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
                        images[resolved] = parse_macho(data)
            except (OSError, RuntimeError, InvalidMachO) as error:
                errors.append(f"{label}: {error}")

    for required in (bundle / "Contents/MacOS" / executable,
                     bundle / "Contents/MacOS/dusk-studio-plugin-host"):
        try:
            required_images = images.get(required.resolve(strict=True))
            if required_images is None:
                errors.append(f"{required.relative_to(bundle)}: required executable is not a valid Mach-O")
            elif any(image["filetype"] != 2 for image in required_images):
                errors.append(f"{required.relative_to(bundle)}: required executable must use MH_EXECUTE in every slice")
        except (OSError, RuntimeError) as error:
            errors.append(f"{required.relative_to(bundle)}: required executable is missing ({error})")
    if not images:
        errors.append("no valid Mach-O images found")
    executable_dir = bundle / "Contents/MacOS"
    for path, slices in sorted(images.items()):
        label = str(path.relative_to(bundle))
        if not any(image["cpu"] == 0x100000C for image in slices):
            errors.append(f"{label}: bundled Mach-O has no arm64 slice")
        if path.parent != executable_dir and any(image["filetype"] == 2 for image in slices):
            errors.append(f"{label}: unsupported nested executable; @executable_path requires the Contents/MacOS context")
        for image in slices:
            architecture = CPU_NAMES.get(image["cpu"], f"cpu-{image['cpu']}")
            architecture += f"/subtype-{image['subtype']}"
            report["images"].append({"path": label, "architecture": architecture,
                                     "minimum_macos": version_text(image["minimum"]),
                                     "filetype": image["filetype"]})
            if image["minimum"] > limit:
                errors.append(f"{label} [{architecture}]: minimum macOS {version_text(image['minimum'])} exceeds {version_text(limit)}")
            for dependency in image["dependencies"]:
                report["dependency_edges"] += 1
                normalized = posixpath.normpath(dependency)
                if normalized.startswith(("/usr/lib/", "/System/Library/")):
                    continue
                if dependency.startswith("@loader_path/"):
                    target = path.parent / dependency[len("@loader_path/"):]
                elif dependency.startswith("@executable_path/"):
                    target = executable_dir / dependency[len("@executable_path/"):]
                else:
                    errors.append(f"{label} [{architecture}]: unresolved or external dependency {dependency!r}; only bundled @loader_path and @executable_path are supported")
                    continue
                try:
                    target = target.resolve(strict=True)
                    if not contained(target, bundle):
                        raise ValueError("resolved target escapes the bundle")
                    if target not in images:
                        raise ValueError("target is not a valid bundled Mach-O")
                    matching = [other for other in images[target] if other["cpu"] == image["cpu"]]
                    if not matching:
                        raise ValueError("target has no matching CPU architecture")
                    if any(other["filetype"] != 6 for other in matching):
                        raise ValueError("dependency target must use MH_DYLIB")
                except (OSError, RuntimeError, ValueError) as error:
                    errors.append(f"{label} [{architecture}]: dependency {dependency!r}: {error}")
    report["macho_files"] = len(images)
    report["architecture_slices"] = len(report["images"])
    report["status"] = "FAIL" if errors else "PASS"
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path, help="staged or extracted .app directory")
    parser.add_argument("--max-macos", default="11.0", help="highest permitted minimum version (default: 11.0)")
    args = parser.parse_args()
    try:
        maximum = version(args.max_macos)
    except ValueError as error:
        parser.error(str(error))
    report = verify(args.app, maximum)
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
