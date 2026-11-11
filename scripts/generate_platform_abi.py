#!/usr/bin/env python3
"""Generate scalar macOS import signatures from the installed public SDK.

Aggregate returns and callback conversion require explicit bridge code.
CFRange parameters in this set occupy two available integer argument slots.
"""

import json
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parent.parent
work = root / "build/minecraft/platform-abi"
work.mkdir(parents=True, exist_ok=True)
wanted = set(
    """bzero ctime difftime fgets fileno getpeereid getutxent setutxent mincore proc_listpids
pthread_getschedparam pthread_setschedparam times wcsftime wcstombs fsync getppid utimes futimens utimensat
clonefile copyfile_state_alloc copyfile_state_free fcopyfile disconnectx fgetxattr flistxattr fremovexattr
fsetattrlist fsetxattr getline mknod msync rewind sendfile setattrlist
CFArrayAppendArray CFArrayAppendValue CFArrayCreateMutable CFDictionaryCreate CFDictionaryCreateMutable
CFNetworkCopyProxiesForURL CFNetworkCopySystemProxySettings CFRunLoopAddSource CFRunLoopGetCurrent
CFRunLoopRemoveSource CFRunLoopRunInMode CFRunLoopStop CFStringAppendCharacters CFURLCreateWithBytes
CFAttributedStringCreateMutable CFAttributedStringRemoveAttribute CFAttributedStringReplaceString
CFAttributedStringSetAttribute CFStringCompare CFStringCreateWithBytes CFStringCreateWithCharactersNoCopy
CFStringGetBytes CFStringHasPrefix CFStringHasSuffix CFURLCreateFromFileSystemRepresentation
CGDataProviderRelease CGDataProviderCopyData CGFontCopyGlyphNameForGlyph CGFontCopyPostScriptName CGFontCopyTableForTag
CGFontCreateWithDataProvider CGFontRelease CGFontRetain CTFontCopyAttribute CTFontCopyAvailableTables
CTFontCopyGraphicsFont CTFontCopyName CTFontCopyPostScriptName CTFontCopyVariation CTFontCreateCopyWithAttributes
CTFontCreatePathForGlyph CTFontCreateUIFontForLanguage CTFontCreateWithFontDescriptor CTFontCreateWithGraphicsFont
CTFontDescriptorCreateWithAttributes CTFontDescriptorCreateWithNameAndSize CTFontGetAdvancesForGlyphs
CTFontGetAscent CTFontGetDescent CTFontGetGlyphWithName CTFontGetGlyphsForCharacters CTFontGetLeading
CTFontGetSize CTFontGetVerticalTranslationsForGlyphs CTFontManagerCreateFontDescriptorsFromURL
CTGetCoreTextVersion CTLineGetGlyphRuns CTLineGetTrailingWhitespaceWidth CTRunGetAttributes CTRunGetGlyphCount
CTRunGetGlyphs CTRunGetGlyphsPtr CTRunGetPositions CTRunGetPositionsPtr CTRunGetStatus CTRunGetStringIndices
CTRunGetStringIndicesPtr CTRunGetTypographicBounds CTTypesetterCreateLine CTTypesetterCreateWithAttributedStringAndOptions""".split()
)
wanted.update(
    """objc_collectingEnabled objc_collectableZone objc_registerThreadWithCollector objc_unregisterThreadWithCollector objc_assertRegisteredThreadWithCollector objc_clear_stack objc_collect objc_setCollectionThreshold objc_setCollectionRatio objc_is_finalized objc_getClass objc_lookUpClass objc_getMetaClass objc_getProtocol objc_getClassList
class_getName class_getSuperclass class_isMetaClass class_getInstanceSize class_respondsToSelector class_conformsToProtocol
class_getInstanceMethod class_getClassMethod class_getInstanceVariable class_getProperty class_copyMethodList class_copyIvarList class_copyPropertyList
sel_getUid sel_getName sel_registerName sel_isEqual object_getClass object_getClassName object_isClass
method_getName method_getTypeEncoding method_getNumberOfArguments method_copyReturnType method_copyArgumentType method_getReturnType method_getArgumentType
ivar_getName ivar_getTypeEncoding ivar_getOffset property_getName property_getAttributes protocol_getName protocol_isEqual""".split()
)
wanted.update(
    """class_getVersion class_getWeakIvarLayout class_getIvarLayout class_setIvarLayout class_setSuperclass class_setVersion class_setWeakIvarLayout objc_allocateClassPair objc_copyProtocolList objc_getAssociatedObject objc_getFutureClass objc_getRequiredClass objc_registerClassPair objc_removeAssociatedObjects objc_setAssociatedObject object_copy object_dispose object_getIndexedIvars object_getInstanceVariable object_getIvar object_setClass object_setInstanceVariable object_setIvar protocol_conformsToProtocol protocol_copyMethodDescriptionList protocol_copyPropertyList protocol_copyProtocolList protocol_getProperty""".split()
)
wanted.update(
    """class_addIvar class_addProperty class_addProtocol class_copyProtocolList class_createInstance class_getClassVariable class_getImageName class_replaceProperty objc_allocateProtocol objc_constructInstance objc_copyClassList objc_copyClassNamesForImage objc_copyImageNames objc_destructInstance objc_disposeClassPair objc_loadWeak objc_registerProtocol objc_storeWeak property_copyAttributeList property_copyAttributeValue protocol_addMethodDescription protocol_addProperty protocol_addProtocol""".split()
)
headers = [
    "objc/objc-auto.h",
    "objc/runtime.h",
    "stdio.h",
    "stdlib.h",
    "string.h",
    "time.h",
    "wchar.h",
    "unistd.h",
    "pthread.h",
    "utmpx.h",
    "libproc.h",
    "copyfile.h",
    "sys/types.h",
    "sys/socket.h",
    "sys/mman.h",
    "sys/times.h",
    "sys/xattr.h",
    "sys/attr.h",
    "sys/stat.h",
    "sys/clonefile.h",
    "CoreFoundation/CoreFoundation.h",
    "CoreGraphics/CoreGraphics.h",
    "CoreText/CoreText.h",
    "CFNetwork/CFNetwork.h",
]
ast_file = work / "sdk-ast.json"
with ast_file.open("w") as output:
    subprocess.run(
        [
            "clang",
            "-target",
            "x86_64-apple-macos11",
            "-Wno-deprecated-declarations",
            "-x",
            "c",
            "-",
            "-Xclang",
            "-ast-dump=json",
            "-fsyntax-only",
        ],
        input="".join(f"#include <{name}>\n" for name in headers),
        text=True,
        stdout=output,
        check=True,
    )
typedefs = {}
enums = {}
functions = {}


def collect(node):
    name = node.get("name", "")
    kind = node.get("kind")
    if kind == "TypedefDecl":
        typedefs[name] = node.get("type", {}).get(
            "desugaredQualType", node.get("type", {}).get("qualType", "")
        )
    if kind == "EnumDecl":
        enums["enum " + name] = node.get("fixedUnderlyingType", {}).get(
            "qualType", "int"
        )
    if kind == "FunctionDecl" and name in wanted:
        functions[name] = node
    for child in node.get("inner", []):
        collect(child)


collect(json.loads(ast_file.read_text()))
scalar = {
    "void": "v",
    "char": "c",
    "signed char": "c",
    "unsigned char": "b",
    "short": "s",
    "unsigned short": "h",
    "int": "i",
    "unsigned int": "i",
    "long": "l",
    "unsigned long": "z",
    "long long": "l",
    "unsigned long long": "z",
    "float": "f",
    "double": "d",
    "_Bool": "b",
    "bool": "b",
}


def kind(qual, seen=None):
    qual = re.sub(r"\b(const|volatile|restrict|_Nonnull|_Nullable)\b", "", qual).strip()
    if "*" in qual or "[" in qual:
        return "p"
    if qual in scalar:
        return scalar[qual]
    seen = set() if seen is None else seen
    if qual in seen:
        return None
    seen.add(qual)
    base = typedefs.get(qual) or enums.get(qual)
    return kind(base, seen) if base else None


entries = []
missing = wanted - functions.keys()
if missing:
    raise SystemExit("Missing SDK declarations: " + ", ".join(sorted(missing)))
for name, node in sorted(functions.items()):
    result = kind(node["type"]["qualType"].split("(", 1)[0].strip())
    arguments = []
    for parameter in (
        x for x in node.get("inner", []) if x.get("kind") == "ParmVarDecl"
    ):
        qual = parameter["type"]["qualType"]
        value = (
            "ll"
            if qual == "CFRange"
            else kind(parameter["type"].get("desugaredQualType", qual))
        )
        if value is None:
            raise SystemExit(
                f"Aggregate parameter needs explicit ABI conversion: {name}: {qual}"
            )
        arguments.append(value)
    if result is None or node.get("variadic"):
        raise SystemExit("Explicit ABI conversion required: " + name)
    entries.append(f'    D("{name}", "{"".join(arguments)}", \'{result}\'),\n')
(root / "src/platform_abi.inc").write_text(
    "/* Generated from the public macOS SDK by scripts/generate_platform_abi.py. */\n"
    + "".join(entries)
)
print("Generated", len(entries), "scalar/platform signatures")
