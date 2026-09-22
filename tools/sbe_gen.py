#!/usr/bin/env python3
"""Minimal SBE (Simple Binary Encoding 1.0) code generator for FastMM.

Two sub-commands, standard library only:

  extract   Copy a subset of an SBE schema: the requested message templates plus every type,
            composite, enum and set they reference (transitively). Used to cut the committed
            CME MDP 3.0 subset (tools/sbe/mdp3_templates_subset.xml) out of CME's
            templates_FixBinary.xml.

  generate  Emit one C++20 header of zero-copy flyweights for a schema: explicit little-endian
            reads/writes through fastmm::codecs::sbe helpers (memcpy, never packed-struct
            casts), static_asserted block lengths and field offsets, repeating-group views for
            any dimension composite (groupSize / groupSize8Byte), optional (null) values,
            constant fields that are not on the wire, enums with validity checks, bitsets,
            decimal composites (mantissa + constant exponent), variable-length <data> fields
            after the groups (length-prefixed, read as std::string_view), implicit block
            lengths (sum of the fields when blockLength is omitted), constant fields with
            valueRef, and a writer for every message.
            --check compares against the existing file instead of writing it (exit 1 if stale).

Deliberately unsupported (the generator stops with an error): nested groups, <data> inside
groups, non-constant decimal exponents, array members in composites, big-endian schemas.
Schemas that carry the exponent in a separate field (Binance: `mbx:exponent`) get plain integer
mantissa accessors; the caller applies the exponent.

  python3 tools/sbe_gen.py generate --schema tools/sbe/mdp3_templates_subset.xml \\
      --out include/fastmm/codecs/mdp3/generated/mdp3_schema.hpp \\
      --namespace fastmm::codecs::mdp3::schema
  python3 tools/sbe_gen.py generate --schema tools/sbe/binance_spot_stream_1_0.xml \\
      --out include/fastmm/venues/binance/generated/binance_stream_sbe.hpp \\
      --namespace fastmm::venues::binance::sbe_stream
"""
import argparse
import hashlib
import re
import sys
import xml.etree.ElementTree as ET

SBE_NS = "http://www.fixprotocol.org/ns/simple/1.0"

PRIMS = {
    "char": ("char", 1),
    "int8": ("std::int8_t", 1),
    "uint8": ("std::uint8_t", 1),
    "int16": ("std::int16_t", 2),
    "uint16": ("std::uint16_t", 2),
    "int32": ("std::int32_t", 4),
    "uint32": ("std::uint32_t", 4),
    "int64": ("std::int64_t", 8),
    "uint64": ("std::uint64_t", 8),
}

CPP_KEYWORDS = {
    "alignas", "alignof", "and", "auto", "bool", "break", "case", "catch", "char", "class",
    "const", "continue", "default", "delete", "do", "double", "else", "enum", "explicit",
    "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int",
    "long", "mutable", "namespace", "new", "noexcept", "not", "operator", "or", "private",
    "protected", "public", "register", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "while", "xor",
}


def die(msg):
    sys.stderr.write("sbe_gen: error: %s\n" % msg)
    sys.exit(2)


def snake(name):
    s = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s)
    s = s.lower()
    return s + "_" if s in CPP_KEYWORDS else s


def camel(name):
    return name[0].upper() + name[1:]


def local(tag):
    return tag.split("}", 1)[1] if "}" in tag else tag


def one_line(text):
    return " ".join((text or "").split())


# ---------------------------------------------------------------------------------------------
# Schema model


class Encoded:
    """A <type> element (standalone or a composite member)."""

    def __init__(self, el):
        self.name = el.get("name")
        self.prim = el.get("primitiveType")
        if self.prim not in PRIMS:
            die("type %s: unsupported primitiveType %r" % (self.name, self.prim))
        self.length = int(el.get("length", "1"))
        self.presence = el.get("presence", "required")
        self.null_value = el.get("nullValue")
        self.constant = (el.text or "").strip() if self.presence == "constant" else None
        self.offset = int(el.get("offset")) if el.get("offset") is not None else None
        self.description = el.get("description", "")

    @property
    def cpp(self):
        return PRIMS[self.prim][0]

    @property
    def size(self):
        return 0 if self.presence == "constant" else PRIMS[self.prim][1] * self.length


class Composite:
    def __init__(self, el):
        self.name = el.get("name")
        self.description = el.get("description", "")
        self.members = []
        pos = 0
        for child in el:
            if local(child.tag) != "type":
                die("composite %s: only <type> members are supported" % self.name)
            m = Encoded(child)
            if m.presence != "constant":
                if m.offset is None:
                    m.offset = pos
                elif m.offset < pos:
                    die("composite %s: member %s overlaps" % (self.name, m.name))
                pos = m.offset + m.size
            self.members.append(m)
        self.size = pos

    def is_decimal(self):
        if [m.name for m in self.members] != ["mantissa", "exponent"]:
            return False
        if self.members[1].presence != "constant":
            die("composite %s: only constant exponents are supported" % self.name)
        return True


class Enum:
    def __init__(self, el, schema):
        self.name = el.get("name")
        self.encoding = el.get("encodingType")
        self.values = [(v.get("name"), (v.text or "").strip(), v.get("description", ""))
                       for v in el if local(v.tag) == "validValue"]
        enc = schema.types.get(self.encoding)
        if enc is None:
            if self.encoding not in PRIMS:
                die("enum %s: unknown encodingType %s" % (self.name, self.encoding))
            self.prim, self.null_value = self.encoding, None
        else:
            self.prim = enc.prim
            self.null_value = enc.null_value if enc.presence == "optional" else None
        self.size = PRIMS[self.prim][1]

    @property
    def cpp(self):
        return PRIMS[self.prim][0]


class SetType:
    def __init__(self, el, schema):
        self.name = el.get("name")
        self.encoding = el.get("encodingType")
        self.choices = [(c.get("name"), int((c.text or "").strip()), c.get("description", ""))
                        for c in el if local(c.tag) == "choice"]
        enc = schema.types.get(self.encoding)
        self.prim = enc.prim if enc is not None else self.encoding
        if self.prim not in PRIMS:
            die("set %s: unknown encodingType %s" % (self.name, self.encoding))
        self.size = PRIMS[self.prim][1]

    @property
    def cpp(self):
        return PRIMS[self.prim][0]


class Field:
    def __init__(self, el, schema):
        self.name = el.get("name")
        self.id = el.get("id")
        self.type_name = el.get("type")
        self.offset = int(el.get("offset")) if el.get("offset") is not None else None
        self.since = int(el.get("sinceVersion", "0"))
        self.description = el.get("description", "")
        self.kind, self.type = schema.resolve(self.type_name)
        # presence="constant" on the field itself (valueRef="enum.Value"): not on the wire.
        self.value_ref = el.get("valueRef") if el.get("presence") == "constant" else None
        if self.value_ref is not None and self.kind != "enum":
            die("field %s: valueRef constants are supported for enums only" % self.name)
        self.size = 0 if self.value_ref is not None else self.type.size

    @property
    def constant(self):
        return self.value_ref is not None or (
            self.kind == "type" and self.type.presence == "constant")

    @property
    def accessor(self):
        return snake(self.name)


def layout(owner, fields, block_length):
    """Assigns offsets; returns the block length (the fields' end when block_length is None)."""
    pos = 0
    for f in fields:
        if f.size == 0:
            continue
        if f.offset is None:
            f.offset = pos
        elif f.offset < pos:
            die("%s: field %s at offset %d overlaps the previous field" % (owner, f.name, f.offset))
        pos = f.offset + f.size
    if block_length is None:
        return pos
    if pos > block_length:
        die("%s: fields end at %d beyond blockLength %d" % (owner, pos, block_length))
    return block_length


def optional_int(el, attr):
    v = el.get(attr)
    return int(v) if v is not None else None


class Group:
    def __init__(self, el, schema, since):
        self.name = el.get("name")
        self.id = el.get("id")
        self.block_length = optional_int(el, "blockLength")
        self.dimension = el.get("dimensionType", "groupSizeEncoding")
        self.description = el.get("description", "")
        self.since = max(since, int(el.get("sinceVersion", "0")))
        if self.dimension not in schema.composites:
            die("group %s: unknown dimensionType %s" % (self.name, self.dimension))
        self.fields = []
        for child in el:
            t = local(child.tag)
            if t != "field":
                die("group %s: <%s> not supported (nested groups / data)" % (self.name, t))
            self.fields.append(Field(child, schema))
        self.block_length = layout(self.name, self.fields, self.block_length)


class DataField:
    """A variable-length <data> field: a composite of a length prefix and varData bytes."""

    def __init__(self, el, schema):
        self.name = el.get("name")
        self.id = el.get("id")
        self.type_name = el.get("type")
        self.description = el.get("description", "")
        c = schema.composites.get(self.type_name)
        if c is None or [m.name for m in c.members] != ["length", "varData"]:
            die("data %s: type %s must be a composite of length + varData" % (
                self.name, self.type_name))
        self.length = c.members[0]
        if self.length.prim not in ("uint8", "uint16", "uint32"):
            die("data %s: unsupported length type %s" % (self.name, self.length.prim))


class Message:
    def __init__(self, el, schema):
        self.name = el.get("name")
        self.id = int(el.get("id"))
        self.block_length = optional_int(el, "blockLength")
        self.since = int(el.get("sinceVersion", "0"))
        self.description = el.get("description", "")
        self.fields, self.groups, self.datas = [], [], []
        for child in el:
            t = local(child.tag)
            if t == "field":
                if self.groups or self.datas:
                    die("message %s: field after a group or data" % self.name)
                self.fields.append(Field(child, schema))
            elif t == "group":
                if self.datas:
                    die("message %s: group after data" % self.name)
                self.groups.append(Group(child, schema, self.since))
            elif t == "data":
                self.datas.append(DataField(child, schema))
            else:
                die("message %s: <%s> not supported" % (self.name, t))
        self.block_length = layout(self.name, self.fields, self.block_length)


class Schema:
    def __init__(self, path):
        raw = open(path, "rb").read()
        self.sha256 = hashlib.sha256(raw).hexdigest()
        r = ET.fromstring(raw)
        if r.get("byteOrder", "littleEndian") != "littleEndian":
            die("only littleEndian schemas are supported")
        self.package = r.get("package")
        self.id = int(r.get("id"))
        self.version = int(r.get("version"))
        self.semantic_version = r.get("semanticVersion", "")
        self.description = r.get("description", "")
        self.types, self.composites, self.enums, self.sets = {}, {}, {}, {}
        self.type_order = []
        types_el = [e for e in r if local(e.tag) == "types"]
        for tel in types_el:
            for el in tel:
                t = local(el.tag)
                if t == "type":
                    self.types[el.get("name")] = Encoded(el)
                elif t == "composite":
                    self.composites[el.get("name")] = Composite(el)
                self.type_order.append(el)
        for tel in types_el:
            for el in tel:
                t = local(el.tag)
                if t == "enum":
                    self.enums[el.get("name")] = Enum(el, self)
                elif t == "set":
                    self.sets[el.get("name")] = SetType(el, self)
        self.messages = [Message(el, self) for el in r if local(el.tag) == "message"]

    def resolve(self, name):
        if name in self.types:
            return "type", self.types[name]
        if name in self.composites:
            return "composite", self.composites[name]
        if name in self.enums:
            return "enum", self.enums[name]
        if name in self.sets:
            return "set", self.sets[name]
        if name in PRIMS:
            return "type", Encoded(ET.Element("type", {"name": name, "primitiveType": name}))
        die("unknown type %s" % name)
        return None


# ---------------------------------------------------------------------------------------------
# extract


def cmd_extract(args):
    raw = open(args.schema, "rb").read()
    root = ET.fromstring(raw)
    wanted = {int(x) for x in args.templates.split(",")}
    msgs = [m for m in root if local(m.tag) == "message" and int(m.get("id")) in wanted]
    missing = wanted - {int(m.get("id")) for m in msgs}
    if missing:
        die("templates not in schema: %s" % sorted(missing))
    types_el = [e for e in root if local(e.tag) == "types"]
    by_name = {el.get("name"): el for tel in types_el for el in tel}
    needed = set()
    stack = ["messageHeader"]
    for m in msgs:
        for el in m.iter():
            if local(el.tag) == "field":
                stack.append(el.get("type"))
            elif local(el.tag) == "group":
                stack.append(el.get("dimensionType", "groupSizeEncoding"))
    while stack:
        n = stack.pop()
        if n in needed or n not in by_name:
            continue
        needed.add(n)
        if by_name[n].get("encodingType"):
            stack.append(by_name[n].get("encodingType"))
    # ElementTree reserves "ns<digits>" prefixes; serialise with a placeholder and rename it to
    # CME's "ns2" afterwards so the subset reads like the original file.
    ET.register_namespace("sbeschema", SBE_NS)
    ET.register_namespace("xsi", "http://www.w3.org/2001/XMLSchema-instance")
    out_root = ET.Element(root.tag, dict(root.attrib))
    out_types = ET.SubElement(out_root, "types")
    for tel in types_el:
        for el in tel:
            if el.get("name") in needed:
                out_types.append(el)
    for m in msgs:
        out_root.append(m)
    ET.indent(out_root, space="    ")
    body = ET.tostring(out_root, encoding="unicode")
    body = body.replace("<sbeschema:", "<ns2:").replace("</sbeschema:", "</ns2:")
    body = body.replace("xmlns:sbeschema=", "xmlns:ns2=")
    note_lines = [line.replace("--", "- -") for line in args.note.split("\\n")] if args.note else []
    header = '<?xml version="1.0" encoding="UTF-8"?>\n<!--\n'
    header += "  Subset of an SBE schema, cut by: python3 tools/sbe_gen.py extract\n"
    header += "  Source file sha256: %s\n" % hashlib.sha256(raw).hexdigest()
    header += "  Templates: %s\n" % ", ".join(m.get("name") for m in msgs)
    header += "".join("  %s\n" % line for line in note_lines)
    header += "-->\n"
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(header + body + "\n")


# ---------------------------------------------------------------------------------------------
# generate


def cpp_literal(prim, text):
    if prim == "char":
        if len(text) != 1:
            die("char constant %r must be one character" % text)
        return "'%s'" % ("\\'" if text == "'" else text)
    v = int(text)
    if prim == "uint64":
        return "%dULL" % v
    if prim == "int64":
        return "INT64_MIN" if v == -(2 ** 63) else "%dLL" % v
    if prim.startswith("uint"):
        return "%dU" % v
    return "%d" % v


def null_literal(prim, null_value):
    if null_value is not None:
        return cpp_literal(prim, null_value)
    # SBE 1.0 default null values for optional fields without an explicit nullValue.
    return {"char": "'\\0'", "int8": "INT8_MIN", "uint8": "UINT8_MAX", "int16": "INT16_MIN",
            "uint16": "UINT16_MAX", "int32": "INT32_MIN", "uint32": "UINT32_MAX",
            "int64": "INT64_MIN", "uint64": "UINT64_MAX"}[prim]


class Gen:
    def __init__(self, schema, namespace):
        self.s = schema
        self.ns = namespace
        self.out = []

    def w(self, text=""):
        self.out.append(text)

    def used_type_names(self):
        used = set()
        for m in self.s.messages:
            used.update(f.type_name for f in m.fields)
            for g in m.groups:
                used.add(g.dimension)
                used.update(f.type_name for f in g.fields)
        return used

    # ---- types ------------------------------------------------------------------------------
    def emit_enum(self, e):
        w = self.w
        w("// enum %s (encoding %s)" % (e.name, e.encoding))
        w("enum class %s : %s {" % (e.name, e.cpp))
        for name, val, desc in e.values:
            w("  %s = %s,  // %s" % (name, cpp_literal(e.prim, val), one_line(desc)))
        if e.null_value is not None:
            w("  NullValue = %s,  // encoding null" % cpp_literal(e.prim, e.null_value))
        w("};")
        w("[[nodiscard]] constexpr bool is_valid(%s v) noexcept {" % e.name)
        w("  switch (v) {")
        for name, _, _ in e.values:
            w("    case %s::%s:" % (e.name, name))
        w("      return true;")
        w("    default:")
        w("      return false;")
        w("  }")
        w("}")
        w("[[nodiscard]] constexpr std::string_view to_string(%s v) noexcept {" % e.name)
        w("  switch (v) {")
        for name, _, _ in e.values:
            w("    case %s::%s:" % (e.name, name))
            w('      return "%s";' % name)
        w("    default:")
        w('      return "?";')
        w("  }")
        w("}")
        w()

    def emit_set(self, st):
        w = self.w
        w("// set %s (encoding %s)" % (st.name, st.encoding))
        w("struct %s {" % st.name)
        w("  %s bits = 0;" % st.cpp)
        for name, bit, desc in st.choices:
            acc = snake(name)
            w("  // bit %d: %s" % (bit, one_line(desc)))
            w("  [[nodiscard]] constexpr bool %s() const noexcept {" % acc)
            w("    return ((static_cast<unsigned>(bits) >> %dU) & 1U) != 0;" % bit)
            w("  }")
            w("  constexpr void set_%s(bool on) noexcept {" % acc.rstrip("_"))
            w("    const unsigned b = static_cast<unsigned>(bits);")
            expr = "on ? (b | (1U << %dU)) : (b & ~(1U << %dU))" % (bit, bit)
            if st.cpp == "std::uint32_t":  # already unsigned: a cast would be -Wuseless-cast
                w("    bits = %s;" % expr)
            else:
                w("    bits = static_cast<%s>(%s);" % (st.cpp, expr))
            w("  }")
        w("  constexpr bool operator==(const %s&) const noexcept = default;" % st.name)
        w("};")
        w("static_assert(sizeof(%s) == %d);" % (st.name, st.size))
        w()

    def emit_dimension(self, c):
        w = self.w
        bl = c.members[0]
        ng = [x for x in c.members if x.name == "numInGroup"]
        if bl.name != "blockLength" or not ng:
            die("dimension %s must have blockLength and numInGroup" % c.name)
        ng = ng[0]
        w("// composite %s (group dimension): %s" % (c.name, one_line(c.description)))
        w("struct %s {" % camel(c.name))
        w("  static constexpr std::size_t kSize = %d;" % c.size)
        w("  static constexpr std::size_t kBlockLengthOffset = %d;" % bl.offset)
        w("  using BlockLength = %s;" % bl.cpp)
        w("  static constexpr std::size_t kNumInGroupOffset = %d;" % ng.offset)
        w("  using NumInGroup = %s;" % ng.cpp)
        w("};")
        w()

    def emit_composite(self, c):
        w = self.w
        name = camel(c.name)
        if c.is_decimal():
            man, exp = c.members
            opt = man.presence == "optional"
            w("// composite %s: %s" % (c.name, one_line(c.description)))
            w("using %s = sbe::Decimal<%s, %s, %s, %s>;" % (
                name, man.cpp, exp.constant, "true" if opt else "false",
                null_literal(man.prim, man.null_value) if opt else "0"))
            w("static_assert(%s::kSize == %d);" % (name, c.size))
            w()
            return
        w("// composite %s: %s" % (c.name, one_line(c.description)))
        w("struct %s {" % name)
        w("  static constexpr std::size_t kSize = %d;" % c.size)
        for m in c.members:
            if m.presence == "constant":
                w("  static constexpr %s k%s = %s;" % (m.cpp, camel(m.name),
                                                     cpp_literal(m.prim, m.constant)))
                continue
            if m.length != 1:
                die("composite %s: array members are not supported" % c.name)
            if m.presence == "optional":
                w("  static constexpr %s k%sNull = %s;" % (m.cpp, camel(m.name),
                                                         null_literal(m.prim, m.null_value)))
        for m in c.members:
            if m.presence != "constant":
                w("  %s %s = 0;  // offset %d" % (m.cpp, snake(m.name), m.offset))
        w("  [[nodiscard]] static %s load(const std::byte* p) noexcept {" % name)
        w("    %s v;" % name)
        for m in c.members:
            if m.presence != "constant":
                w("    v.%s = sbe::load_le<%s>(p + %d);" % (snake(m.name), m.cpp, m.offset))
        w("    return v;")
        w("  }")
        w("  void store(std::byte* p) const noexcept {")
        for m in c.members:
            if m.presence != "constant":
                w("    sbe::store_le<%s>(p + %d, %s);" % (m.cpp, m.offset, snake(m.name)))
        w("  }")
        w("  constexpr bool operator==(const %s&) const noexcept = default;" % name)
        w("};")
        w()

    # ---- fields -----------------------------------------------------------------------------
    def field_getter(self, f, owner_since, indent):
        acc = f.accessor
        k, t = f.kind, f.type
        lines = ["// %s (tag %s): %s" % (f.name, f.id, one_line(f.description))]
        if f.value_ref is not None:
            enum_name, _, value = f.value_ref.partition(".")
            if enum_name != t.name or value not in [v[0] for v in t.values]:
                die("field %s: bad valueRef %s" % (f.name, f.value_ref))
            lines.append("// constant, not on the wire")
            lines.append("[[nodiscard]] static constexpr %s %s() noexcept { return %s::%s; }"
                         % (t.name, acc, t.name, value))
            return [indent + x for x in lines]
        if f.constant:
            lines.append("// constant, not on the wire")
            lines.append("[[nodiscard]] static constexpr %s %s() noexcept { return %s; }"
                         % (t.cpp, acc, cpp_literal(t.prim, t.constant)))
            return [indent + x for x in lines]
        # Fields added after the owning block's first version can be absent from older blocks.
        guard = None
        if f.since > owner_since:
            guard = "version_ < %d || block_length_ < %d" % (f.since, f.offset + f.size)
        if k == "type" and t.length > 1:
            if t.prim != "char":
                die("field %s: non-char arrays are not supported" % f.name)
            ret, null = "std::string_view", "std::string_view{}"
            body = "sbe::load_chars(p_ + %d, %d)" % (f.offset, t.length)
        elif k == "type":
            ret = t.cpp
            body = "sbe::load_le<%s>(p_ + %d)" % (t.cpp, f.offset)
            null = "0"
            if t.presence == "optional":
                cname = "k%sNull" % camel(f.name)
                lines.append("static constexpr %s %s = %s;" % (t.cpp, cname,
                                                             null_literal(t.prim, t.null_value)))
                lines.append("[[nodiscard]] bool has_%s() const noexcept { return %s() != %s; }"
                             % (acc.rstrip("_"), acc, cname))
                null = cname
        elif k == "enum":
            ret = t.name
            body = "static_cast<%s>(sbe::load_le<%s>(p_ + %d))" % (t.name, t.cpp, f.offset)
            null = ("%s::NullValue" % t.name) if t.null_value is not None else ("%s{}" % t.name)
        elif k == "set":
            ret = t.name
            body = "%s{sbe::load_le<%s>(p_ + %d)}" % (t.name, t.cpp, f.offset)
            null = "%s{}" % t.name
        else:
            ret = camel(t.name)
            body = "%s::load(p_ + %d)" % (ret, f.offset)
            null = ("%s::null()" % ret) if t.is_decimal() else ("%s{}" % ret)
        lines.append("[[nodiscard]] %s %s() const noexcept {" % (ret, acc))
        if guard:
            lines.append("  if (%s) return %s;" % (guard, null))
        lines.append("  return %s;" % body)
        lines.append("}")
        return [indent + x for x in lines]

    def field_setter(self, f, indent):
        k, t, acc = f.kind, f.type, f.accessor.rstrip("_")
        if f.constant:
            return []
        if k == "type" and t.length > 1:
            s = "void set_%s(std::string_view v) noexcept { sbe::store_chars(p_ + %d, %d, v); }" % (
                acc, f.offset, t.length)
        elif k == "type":
            s = "void set_%s(%s v) noexcept { sbe::store_le<%s>(p_ + %d, v); }" % (
                acc, t.cpp, t.cpp, f.offset)
        elif k == "enum":
            s = ("void set_%s(%s v) noexcept { sbe::store_le<%s>(p_ + %d, static_cast<%s>(v)); }"
                 % (acc, t.name, t.cpp, f.offset, t.cpp))
        elif k == "set":
            s = "void set_%s(%s v) noexcept { sbe::store_le<%s>(p_ + %d, v.bits); }" % (
                acc, t.name, t.cpp, f.offset)
        else:
            s = "void set_%s(const %s& v) noexcept { v.store(p_ + %d); }" % (
                acc, camel(t.name), f.offset)
        return [indent + s]

    @staticmethod
    def required_length(fields, since):
        end = 0
        for f in fields:
            if not f.constant and f.since <= since:
                end = max(end, f.offset + f.size)
        return end

    def asserts(self, cls, fields, block_length):
        out = ["static_assert(%s::kBlockLength == %d);" % (cls, block_length),
               "static_assert(%s::kRequiredBlockLength <= %s::kBlockLength);" % (cls, cls)]
        for f in fields:
            if not f.constant:
                out.append("static_assert(%d + %d <= %s::kBlockLength);  // %s @%d"
                           % (f.offset, f.size, cls, f.name, f.offset))
        return out

    def emit_entry(self, g):
        w = self.w
        cls = camel(g.name)
        w("  // group %s (tag %s, %s): %s" % (g.name, g.id, g.dimension, one_line(g.description)))
        w("  class %s {" % cls)
        w("   public:")
        w("    using Dimension = %s;" % camel(g.dimension))
        w("    static constexpr std::uint16_t kBlockLength = %d;" % g.block_length)
        w("    static constexpr std::uint16_t kRequiredBlockLength = %d;"
          % self.required_length(g.fields, g.since))
        w("    %s() noexcept = default;" % cls)
        w("    %s(const std::byte* p, std::uint16_t block_length, std::uint16_t version) noexcept"
          % cls)
        w("        : p_(p), block_length_(block_length), version_(version) {}")
        w("    [[nodiscard]] std::uint16_t acting_block_length() const noexcept { return block_length_; }")
        w("    [[nodiscard]] std::uint16_t acting_version() const noexcept { return version_; }")
        for f in g.fields:
            for line in self.field_getter(f, g.since, "    "):
                w(line)
        w("   private:")
        w("    const std::byte* p_ = nullptr;")
        w("    std::uint16_t block_length_ = 0;")
        w("    std::uint16_t version_ = 0;")
        w("  };")
        w("  class %sWriter {" % cls)
        w("   public:")
        w("    %sWriter() noexcept = default;" % cls)
        w("    explicit %sWriter(std::byte* p) noexcept : p_(p) {}" % cls)
        for f in g.fields:
            for line in self.field_setter(f, "    "):
                w(line)
        w("   private:")
        w("    std::byte* p_ = nullptr;")
        w("  };")

    def emit_message(self, m):
        w = self.w
        cls = m.name
        w("// ---- %s: template %d, blockLength %d, sinceVersion %d"
          % (m.name, m.id, m.block_length, m.since))
        w("// %s" % one_line(m.description))
        w("class %s {" % cls)
        w(" public:")
        w("  static constexpr std::uint16_t kTemplateId = %d;" % m.id)
        w("  static constexpr std::uint16_t kBlockLength = %d;" % m.block_length)
        w("  static constexpr std::uint16_t kRequiredBlockLength = %d;"
          % self.required_length(m.fields, m.since))
        w("  static constexpr std::uint16_t kSinceVersion = %d;" % m.since)
        w('  static constexpr std::string_view kName = "%s";' % m.name)
        for g in m.groups:
            self.emit_entry(g)
        w()
        w("  %s() noexcept = default;" % cls)
        w("  // `body` starts right after the SBE message header; block_length and version are the")
        w("  // acting values from that header (they may differ from this schema's).")
        w("  %s(std::span<const std::byte> body, std::uint16_t block_length, "
          "std::uint16_t version) noexcept" % cls)
        w("      : p_(body.data()), size_(body.size()), block_length_(block_length), "
          "version_(version) {}")
        w("  [[nodiscard]] std::uint16_t acting_block_length() const noexcept { return block_length_; }")
        w("  [[nodiscard]] std::uint16_t acting_version() const noexcept { return version_; }")
        for f in m.fields:
            for line in self.field_getter(f, m.since, "  "):
                w(line)
        for i, g in enumerate(m.groups):
            gcls = camel(g.name)
            view = "sbe::GroupView<%s, %s::Dimension>" % (gcls, gcls)
            w("  [[nodiscard]] %s %s() const noexcept {" % (view, snake(g.name)))
            if i == 0:
                w("    const std::size_t at = block_length_;")
            else:
                w("    const auto prev = %s();" % snake(m.groups[i - 1].name))
                w("    if (!prev.valid()) return {};")
                w("    const std::size_t at = prev.end_offset();")
            w("    if (p_ == nullptr || at > size_) return {};")
            w("    return %s(p_ + at, size_ - at, at, version_, %s::kRequiredBlockLength);"
              % (view, gcls))
            w("  }")
        for i, d in enumerate(m.datas):
            acc = snake(d.name)
            w("  // data %s (id %s, %s): %s" % (d.name, d.id, d.type_name, one_line(d.description)))
            w("  [[nodiscard]] sbe::VarData %s_data() const noexcept {" % acc)
            w("    if (p_ == nullptr || size_ < block_length_) return {};")
            if i > 0:
                w("    const auto prev = %s_data();" % snake(m.datas[i - 1].name))
                w("    if (!prev.valid()) return {};")
                w("    const std::size_t at = prev.end_offset();")
            elif m.groups:
                w("    const auto last = %s();" % snake(m.groups[-1].name))
                w("    if (!last.valid()) return {};")
                w("    const std::size_t at = last.end_offset();")
            else:
                w("    const std::size_t at = block_length_;")
            w("    return sbe::VarData::load<%s>(p_, size_, at);" % d.length.cpp)
            w("  }")
            w("  [[nodiscard]] std::string_view %s() const noexcept { return %s_data().value(); }"
              % (acc, acc))
        w("  // Encoded length of the root block and all %s; 0 when truncated or malformed."
          % ("groups and data" if m.datas else "groups"))
        w("  [[nodiscard]] std::size_t size_bytes() const noexcept {")
        w("    if (p_ == nullptr || size_ < block_length_ || block_length_ < kRequiredBlockLength) {")
        w("      return 0;")
        w("    }")
        if m.datas:
            w("    const auto last = %s_data();" % snake(m.datas[-1].name))
            w("    return last.valid() ? last.end_offset() : 0;")
        elif m.groups:
            w("    const auto last = %s();" % snake(m.groups[-1].name))
            w("    return last.valid() ? last.end_offset() : 0;")
        else:
            w("    return block_length_;")
        w("  }")
        tail = ""
        if m.datas:
            tail = " && %s_data().valid()" % snake(m.datas[-1].name)
        elif m.groups:
            tail = " && %s().valid()" % snake(m.groups[-1].name)
        w("  [[nodiscard]] bool valid() const noexcept {")
        w("    return p_ != nullptr && size_ >= block_length_ && block_length_ >= kRequiredBlockLength%s;"
          % tail)
        w("  }")
        w()
        w(" private:")
        w("  const std::byte* p_ = nullptr;")
        w("  std::size_t size_ = 0;")
        w("  std::uint16_t block_length_ = 0;")
        w("  std::uint16_t version_ = 0;")
        w("};")
        for line in self.asserts(cls, m.fields, m.block_length):
            w(line)
        for g in m.groups:
            for line in self.asserts("%s::%s" % (cls, camel(g.name)), g.fields, g.block_length):
                w(line)
        w()
        w("class %sWriter {" % cls)
        w(" public:")
        w("  using Reader = %s;" % cls)
        w("  // Writes the root block at the start of `body` (the bytes after the SBE message")
        w("  // header) and zero-fills it. Groups must be appended in schema order.")
        w("  explicit %sWriter(std::span<std::byte> body) noexcept" % cls)
        w("      : p_(body.data()), cap_(body.size()), used_(Reader::kBlockLength) {")
        w("    if (cap_ < used_) {")
        w("      ok_ = false;")
        w("      used_ = 0;")
        w("      return;")
        w("    }")
        w("    std::memset(p_, 0, used_);")
        w("  }")
        w("  [[nodiscard]] bool ok() const noexcept { return ok_; }")
        w("  [[nodiscard]] std::size_t size_bytes() const noexcept { return used_; }")
        w("  [[nodiscard]] static constexpr sbe::MessageHeader header() noexcept {")
        w("    return sbe::MessageHeader{Reader::kBlockLength, Reader::kTemplateId, kSchemaId, "
          "kSchemaVersion};")
        w("  }")
        if m.fields:
            for f in m.fields:
                for line in self.field_setter(f, "  "):
                    w(line.replace("p_ + ", "root() + "))
        for g in m.groups:
            gcls = camel(g.name)
            gw = "sbe::GroupWriter<Reader::%sWriter, Reader::%s::Dimension>" % (gcls, gcls)
            w("  // Appends the %s group with `count` zero-filled entries." % g.name)
            w("  [[nodiscard]] %s %s(std::size_t count) noexcept {" % (gw, snake(g.name)))
            w("    auto g = %s::create(ok_ ? p_ + used_ : nullptr, ok_ ? cap_ - used_ : 0, "
              "Reader::%s::kBlockLength, count);" % (gw, gcls))
            w("    if (g.ok()) {")
            w("      used_ += g.size_bytes();")
            w("    } else {")
            w("      ok_ = false;")
            w("    }")
            w("    return g;")
            w("  }")
        for d in m.datas:
            w("  // Appends the %s data field (after every group, in schema order)." % d.name)
            w("  bool set_%s(std::string_view v) noexcept {" % snake(d.name).rstrip("_"))
            w("    const std::size_t n = ok_ ? sbe::VarData::store<%s>(p_ + used_, cap_ - used_, v) : 0;"
              % d.length.cpp)
            w("    if (n == 0) {")
            w("      ok_ = false;")
            w("      return false;")
            w("    }")
            w("    used_ += n;")
            w("    return true;")
            w("  }")
        w()
        w(" private:")
        w("  [[nodiscard]] std::byte* root() noexcept { return ok_ ? p_ : scratch_; }")
        w("  std::byte* p_;")
        w("  std::size_t cap_;")
        w("  std::size_t used_;")
        w("  bool ok_ = true;")
        w("  std::byte scratch_[Reader::kBlockLength > 0 ? Reader::kBlockLength : 1] = {};")
        w("};")
        w()

    def run(self, label):
        s, w = self.s, self.w
        used = self.used_type_names()
        w("// clang-format off")
        w("// GENERATED by tools/sbe_gen.py - DO NOT EDIT.")
        w("// Source schema: %s (sha256 %s)" % (label, s.sha256))
        w("// Schema: package=%s id=%d version=%d semanticVersion=%s description=%s"
          % (s.package, s.id, s.version, s.semantic_version, s.description))
        w("// Regenerate: python3 tools/sbe_gen.py generate --schema %s --out <this file> --namespace %s"
          % (label, self.ns))
        w("// NOLINTBEGIN(readability-*,modernize-*,cppcoreguidelines-*,misc-*,bugprone-reserved-identifier)")
        w("#pragma once")
        w('#include "fastmm/codecs/sbe/sbe.hpp"')
        w()
        w("#include <cstddef>")
        w("#include <cstdint>")
        w("#include <cstring>")
        w("#include <span>")
        w("#include <string_view>")
        w()
        w("namespace %s {" % self.ns)
        if not self.ns.startswith("fastmm::codecs"):
            w()
            w("namespace sbe = ::fastmm::codecs::sbe;")
        w()
        w("inline constexpr std::uint16_t kSchemaId = %d;" % s.id)
        w("inline constexpr std::uint16_t kSchemaVersion = %d;" % s.version)
        w()
        hdr = s.composites.get("messageHeader")
        if hdr is None:
            die("schema has no messageHeader composite")
        got = [(m.name, m.prim, m.offset) for m in hdr.members]
        expect = [("blockLength", "uint16", 0), ("templateId", "uint16", 2),
                  ("schemaId", "uint16", 4), ("version", "uint16", 6)]
        if got != expect:
            die("messageHeader layout %s is not the standard SBE header" % got)
        w("// messageHeader is the standard SBE header: see sbe::MessageHeader.")
        w("static_assert(sbe::MessageHeader::kSize == %d);" % hdr.size)
        w()
        dims = {g.dimension for m in s.messages for g in m.groups}
        for name in [el.get("name") for el in s.type_order]:
            if name not in used:
                continue
            if name in dims:
                self.emit_dimension(s.composites[name])
            elif name in s.composites:
                self.emit_composite(s.composites[name])
        for name in list(s.enums):
            if name in used:
                self.emit_enum(s.enums[name])
        for name in list(s.sets):
            if name in used:
                self.emit_set(s.sets[name])
        for m in s.messages:
            self.emit_message(m)
        w("// Template ids of every message in this schema.")
        w("inline constexpr std::uint16_t kTemplateIds[] = {%s};"
          % ", ".join(str(m.id) for m in s.messages))
        w("[[nodiscard]] constexpr std::string_view template_name(std::uint16_t id) noexcept {")
        w("  switch (id) {")
        for m in s.messages:
            w("    case %d:" % m.id)
            w('      return "%s";' % m.name)
        w("    default:")
        w('      return "?";')
        w("  }")
        w("}")
        w()
        w("}  // namespace %s" % self.ns)
        w("// NOLINTEND(readability-*,modernize-*,cppcoreguidelines-*,misc-*,bugprone-reserved-identifier)")
        w("// clang-format on")
        return "\n".join(line.rstrip() for line in self.out) + "\n"


def cmd_generate(args):
    schema = Schema(args.schema)
    text = Gen(schema, args.namespace).run(args.schema_label or args.schema)
    if args.check:
        try:
            with open(args.out, encoding="utf-8") as f:
                current = f.read()
        except OSError:
            current = None
        if current != text:
            sys.stderr.write("sbe_gen: %s is out of date; rerun without --check\n" % args.out)
            return 1
        return 0
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return 0


def main():
    ap = argparse.ArgumentParser(description="Minimal SBE code generator for FastMM")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ex = sub.add_parser("extract", help="cut a template subset out of a full schema")
    ex.add_argument("--schema", required=True)
    ex.add_argument("--templates", required=True, help="comma-separated template ids")
    ex.add_argument("--out", required=True)
    ex.add_argument("--note", default="", help="provenance note (\\n separates lines)")
    ge = sub.add_parser("generate", help="emit the C++ flyweight header")
    ge.add_argument("--schema", required=True)
    ge.add_argument("--schema-label", default=None, help="schema path printed in the header")
    ge.add_argument("--out", required=True)
    ge.add_argument("--namespace", required=True)
    ge.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if args.cmd == "extract":
        cmd_extract(args)
        return 0
    return cmd_generate(args)


if __name__ == "__main__":
    sys.exit(main())
