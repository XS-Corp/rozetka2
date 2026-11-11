#!/usr/bin/env python3
"""Generate typed native graphics imports from Khronos XML and Clang's SDL AST.

Inputs are retained in build/minecraft/graphics-abi. Generated C is checked in,
so a normal translator build does not require networking or SDL headers.
"""

from pathlib import Path
import json
import re
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build/minecraft/graphics-abi"
registry = ET.parse(WORK / "vk.xml").getroot()
types = {}
for node in registry.findall("types/type"):
    name = node.get("name") or node.findtext("name")
    if name:
        types[name] = node
scalar = {
    "void": "v",
    "char": "c",
    "signed char": "c",
    "unsigned char": "b",
    "int8_t": "c",
    "uint8_t": "b",
    "short": "s",
    "short int": "s",
    "unsigned short": "h",
    "unsigned short int": "h",
    "int16_t": "s",
    "uint16_t": "h",
    "int": "i",
    "unsigned int": "i",
    "int32_t": "i",
    "uint32_t": "i",
    "long": "l",
    "long int": "l",
    "unsigned long": "z",
    "unsigned long int": "z",
    "long long": "l",
    "long long int": "l",
    "unsigned long long": "z",
    "unsigned long long int": "z",
    "int64_t": "l",
    "uint64_t": "z",
    "intptr_t": "l",
    "uintptr_t": "z",
    "size_t": "z",
    "float": "f",
    "double": "d",
    "bool": "b",
    "_Bool": "b",
}


def vk_kind(name, seen=None):
    if name in scalar:
        return scalar[name]
    if name.startswith("PFN_"):
        return "p"
    seen = set() if seen is None else seen
    if name in seen:
        return None
    seen.add(name)
    node = types.get(name)
    if node is None:
        return None
    if node.get("alias"):
        return vk_kind(node.get("alias"), seen)
    cat = node.get("category")
    if cat == "handle":
        return "p" if node.findtext("type") == "VK_DEFINE_HANDLE" else "z"
    if cat == "enum":
        return "i"
    if cat == "funcpointer":
        return "p"
    text = "".join(node.itertext())
    if "*" in text and cat == "basetype":
        return "p"
    base = node.findtext("type")
    return vk_kind(base, seen) if base else None


commands = {}
for command in registry.findall("commands/command"):
    if command.get("api") and "vulkan" not in command.get("api").split(","):
        continue
    name = command.get("name") or command.findtext("proto/name")
    if name:
        commands[name] = command


def command_info(name, seen=None):
    node = commands[name]
    if node.get("alias"):
        return command_info(node.get("alias"))
    result = vk_kind(node.findtext("proto/type"))
    arguments = []
    callbacks = 0
    chains = 0
    unknown = result is None
    for index, param in enumerate(
        p
        for p in node.findall("param")
        if not p.get("api") or "vulkan" in p.get("api").split(",")
    ):
        text = "".join(param.itertext())
        base = param.findtext("type")
        kind = "p" if "*" in text or "[" in text else vk_kind(base)
        if (
            base == "VkAllocationCallbacks"
            or base.startswith("PFN_")
            or (
                types.get(base) is not None
                and types[base].get("category") == "funcpointer"
            )
        ):
            callbacks |= 1 << index
        record = types.get(base)
        if (
            record is not None
            and record.get("category") == "struct"
            and record.findtext("member/type") == "VkStructureType"
        ):
            chains |= 1 << index
        unknown |= kind is None
        arguments.append(kind or "p")
    return "".join(arguments), "!" if unknown else result, -1, callbacks, chains


vk_entries = [(name, *command_info(name)) for name in sorted(commands)]

ast = json.loads((WORK / "sdl3-ast.json").read_text())
typedefs = {}
functions = {}
callback_records = set()


def collect(node):
    kind = node.get("kind")
    name = node.get("name", "")
    if kind == "TypedefDecl":
        typedefs[name] = node.get("type", {}).get(
            "desugaredQualType", node.get("type", {}).get("qualType", "")
        )
    if kind == "FunctionDecl" and (
        name.startswith("SDL_") or re.match(r"(gl|CGL)[A-Z]", name)
    ):
        functions[name] = node
    if kind == "RecordDecl" and name:
        for field in node.get("inner", []):
            qual = field.get("type", {}).get(
                "desugaredQualType", field.get("type", {}).get("qualType", "")
            )
            if field.get("kind") == "FieldDecl" and "(*" in qual:
                callback_records.add(name)
    for child in node.get("inner", []):
        collect(child)


collect(ast)


def sdl_kind(qual, seen=None):
    qual = re.sub(r"\b(const|volatile|restrict|_Nonnull|_Nullable)\b", "", qual).strip()
    if "*" in qual or "[" in qual:
        return "p"
    if qual in scalar:
        return scalar[qual]
    if qual.startswith("enum "):
        return "i"
    seen = set() if seen is None else seen
    if qual in seen:
        return None
    seen.add(qual)
    return sdl_kind(typedefs[qual], seen) if qual in typedefs else None


def ast_entries():
    values = []
    for name, node in sorted(functions.items()):
        result_qual = node["type"]["qualType"].split("(", 1)[0].strip()
        result = sdl_kind(result_qual)
        arguments = []
        callbacks = 0
        unknown = result is None
        for index, param in enumerate(
            x for x in node.get("inner", []) if x["kind"] == "ParmVarDecl"
        ):
            qual = param["type"].get("desugaredQualType", param["type"]["qualType"])
            kind = sdl_kind(qual)
            original = param["type"]["qualType"]
            if "va_list" in original or "va_list" in qual:
                unknown = True
            record = re.sub(r"\bconst\b|\*", "", original).strip()
            if "(*" in qual or record in callback_records:
                callbacks |= 1 << index
            unknown |= kind is None
            arguments.append(kind or "p")
        fmt = -1
        if node.get("variadic"):
            fmt = (
                0
                if name in ("SDL_SetError", "SDL_Log")
                else (
                    2
                    if name == "SDL_LogMessage"
                    else 1 if name.startswith("SDL_Log") else -1
                )
            )
            if fmt < 0:
                unknown = True
        # Function pointer results need a typed guest stub, never a raw ARM address.
        underlying = typedefs.get(result_qual, "")
        if "(*" in underlying and name not in (
            "SDL_Vulkan_GetVkGetInstanceProcAddr",
            "SDL_GL_GetProcAddress",
            "SDL_EGL_GetProcAddress",
            "SDL_GetFunctionPointer",
            "SDL_GetDefaultLogOutputFunction",
        ):
            unknown = True
        values.append(
            (name, "".join(arguments), "!" if unknown else result, fmt, callbacks, 0)
        )
    return values


sdl_entries = ast_entries()

gl_ast = WORK / "opengl-ast.json"
if not gl_ast.is_file():
    headers = "".join(
        f"#include <OpenGL/{name}.h>\n"
        for name in ("OpenGL", "gl", "glext", "gl3", "gl3ext")
    )
    with gl_ast.open("w") as output:
        subprocess.run(
            [
                "clang",
                "-target",
                "x86_64-apple-macos11",
                "-Wno-deprecated-declarations",
                "-Wno-#warnings",
                "-x",
                "c",
                "-",
                "-Xclang",
                "-ast-dump=json",
                "-fsyntax-only",
            ],
            input=headers,
            text=True,
            stdout=output,
            check=True,
        )
typedefs = {}
functions = {}
callback_records = set()
collect(json.loads(gl_ast.read_text()))
gl_entries = ast_entries()


# pNext structures that embed callbacks are rejected until callback conversion
# is implemented. This also covers vendor extension callbacks absent from old SDKs.
def structure_value(name):
    for node in registry.findall(".//enum"):
        if node.get("name") != name:
            continue
        if node.get("alias"):
            return structure_value(node.get("alias"))
        if node.get("value"):
            try:
                return int(node.get("value"), 0)
            except ValueError:
                continue
        if node.get("offset"):
            extension = node.get("extnumber")
            if extension is None:
                for ext in registry.findall("extensions/extension"):
                    if any(x is node for x in ext.findall(".//enum")):
                        extension = ext.get("number")
                        break
            if extension:
                return (
                    1000000000 + (int(extension) - 1) * 1000 + int(node.get("offset"))
                ) * (-1 if node.get("dir") == "-" else 1)
    return None


callback_nodes = []
for name, node in types.items():
    if node.get("category") != "struct":
        continue
    members = node.findall("member")
    if not members or members[0].findtext("type") != "VkStructureType":
        continue
    if any((member.findtext("type") or "").startswith("PFN_") for member in members):
        value = structure_value(members[0].get("values", ""))
        if value is not None:
            callback_nodes.append((name, value))


def entries(label, values):
    out = [f"static const r2_graphics_abi {label}[] = {{\n"]
    for name, args, result, fmt, callbacks, chains in values:
        out.append(
            '    {{"%s", "%s", \'%s\', %d}, UINT32_C(0x%x), UINT32_C(0x%x)},\n'
            % (name, args, result, fmt, callbacks, chains)
        )
    return "".join(out) + "};\n"


out = """/* Generated by scripts/generate_graphics_abi.py.
 * Vulkan registry: KhronosGroup/Vulkan-Headers 9fbc2a782ee3f8a4db64e7d991e23da75e893f44
 * Registry license: Apache-2.0 OR MIT. SDL declarations: installed SDL 3.4.16 (zlib).
 * OpenGL/CGL declarations: installed macOS SDK public OpenGL headers.
 * Unknown aggregate ABIs remain diagnostic traps. Callback arguments are guarded.
 */\n"""
out += (
    entries("vulkan_abis", vk_entries)
    + "\n"
    + entries("sdl3_abis", sdl_entries)
    + "\n"
    + entries("opengl_abis", gl_entries)
)
out += "\nstatic const uint32_t vulkan_callback_structures[] = {\n"
for name, value in callback_nodes:
    out += f"    UINT32_C({value}), /* {name} */\n"
out += "};\n"
# Every pNext extension to VkInstanceCreateInfo in this registry consists of
# scalar words and pointers. Compute the shared Darwin 64-bit layout rather
# than copying an unknown-size node or modifying guest memory in place.
out += (
    "\nstatic const struct { uint32_t type, size; } vulkan_instance_chain_sizes[] = {\n"
)
for name, node in types.items():
    if "VkInstanceCreateInfo" not in node.get("structextends", "").split(","):
        continue
    offset = 0
    alignment = 1
    for member in node.findall("member"):
        text = "".join(member.itertext())
        kind = "p" if "*" in text else vk_kind(member.findtext("type"))
        size = {
            "p": 8,
            "z": 8,
            "l": 8,
            "i": 4,
            "f": 4,
            "d": 8,
            "b": 1,
            "c": 1,
            "s": 2,
            "h": 2,
        }.get(kind)
        if size is None or "[" in text:
            raise SystemExit("Explicit instance-chain layout required: " + name)
        alignment = max(alignment, size)
        offset = (offset + size - 1) // size * size + size
    size = (offset + alignment - 1) // alignment * alignment
    value = structure_value(node.find("member").get("values", ""))
    out += f"    {{UINT32_C({value}), {size}}}, /* {name} */\n"
out += "};\n"

(ROOT / "src/graphics_abi.inc").write_text(out)
print(
    "Generated",
    len(vk_entries),
    "Vulkan and",
    len(sdl_entries),
    "SDL signatures;",
    len(callback_nodes),
    "callback structure kinds",
)
print("SDL unsupported aggregate signatures:", sum(x[2] == "!" for x in sdl_entries))
print(
    "Generated",
    len(gl_entries),
    "OpenGL/CGL signatures;",
    sum(x[2] == "!" for x in gl_entries),
    "unsupported aggregate signatures",
)
