"""Small classfile emitter; fixtures never require a host ARM JVM."""

import struct


def class_strings(data):
    """Read String constants from a real consumer class, without a host JVM."""
    if data[:4] != bytes.fromhex("cafebabe"):
        raise ValueError("Not a Java class")
    pool = {}
    references = []
    position = 10
    index = 1
    count = int.from_bytes(data[8:10])
    sizes = {
        3: 4,
        4: 4,
        5: 8,
        6: 8,
        7: 2,
        9: 4,
        10: 4,
        11: 4,
        12: 4,
        15: 3,
        16: 2,
        17: 4,
        18: 4,
        19: 2,
        20: 2,
    }
    while index < count:
        tag = data[position]
        position += 1
        if tag == 1:
            length = int.from_bytes(data[position : position + 2])
            position += 2
            pool[index] = data[position : position + length].decode(
                "utf-8", errors="replace"
            )
            position += length
        elif tag == 8:
            references.append(int.from_bytes(data[position : position + 2]))
            position += 2
        else:
            position += sizes[tag]
            if tag in (5, 6):
                index += 1
        index += 1
    return [pool[index] for index in references]


class JavaClass:
    def __init__(self, name, parent):
        self.pool = []
        self.methods = []
        self.name = name
        self.this = self.cls(name)
        self.parent = self.cls(parent)
        self.code = self.utf("Code")

    def entry(self, data):
        self.pool.append(data)
        return len(self.pool)

    def utf(self, text):
        data = text.encode()
        return self.entry(b"\1" + struct.pack(">H", len(data)) + data)

    def cls(self, text):
        return self.entry(b"\7" + struct.pack(">H", self.utf(text)))

    def reference(self, tag, owner, name, descriptor):
        owner = self.cls(owner)
        n, d = self.utf(name), self.utf(descriptor)
        signature = self.entry(b"\x0c" + struct.pack(">HH", n, d))
        return self.entry(bytes([tag]) + struct.pack(">HH", owner, signature))

    def ref(self, tag, owner, name, descriptor):
        return struct.pack(">H", self.reference(tag, owner, name, descriptor))

    def string(self, text):
        return struct.pack(">H", self.entry(b"\10" + struct.pack(">H", self.utf(text))))

    def long(self, value):
        index = self.entry(b"\5" + struct.pack(">q", value))
        self.entry(b"")
        return struct.pack(">H", index)

    def method(self, flags, name, descriptor, stack, locals_count, code):
        n, d = self.utf(name), self.utf(descriptor)
        attribute = (
            struct.pack(">HHI", stack, locals_count, len(code))
            + code
            + struct.pack(">HH", 0, 0)
        )
        self.methods.append(
            struct.pack(">HHHHHI", flags, n, d, 1, self.code, len(attribute))
            + attribute
        )

    def write(self, directory):
        data = (
            bytes.fromhex("cafebabe")
            + struct.pack(">HHH", 0, 49, len(self.pool) + 1)
            + b"".join(self.pool)
        )
        data += (
            struct.pack(
                ">HHHHHH", 0x21, self.this, self.parent, 0, 0, len(self.methods)
            )
            + b"".join(self.methods)
            + b"\0\0"
        )
        (directory / (self.name + ".class")).write_bytes(data)
