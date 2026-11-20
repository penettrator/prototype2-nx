"""Prepare the opt-in integer tail of sub_8299D618 after standard Switch codegen."""
from hashlib import sha256
from pathlib import Path
import re
import sys

PROJECT = Path(__file__).resolve().parents[1] / "project"
TARGET = "generated/default/prototype2_recomp.109.cpp"
FUNCTION = "DEFINE_REX_FUNC(sub_8299D618) {"
START = "loc_8299DAB8:"
END = "loc_8299DB5C:"
GUARD = "#if defined(P2_COLLISION_FILTER_LOCALS) // terminal pair filter\n"
ORIGINAL_HASH = "56807f1c89e3ffca669b7015c61055e128129cccf43d9371275a68aefe66e363"
REGISTERS = range(3, 12)


def checked_replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"unexpected collision filter: {old!r}")
    return text.replace(old, new, 1)


def localize(original):
    if sha256(original.encode("utf-8")).hexdigest() != ORIGINAL_HASH:
        raise ValueError("collision filter changed since the differential tests")
    # Every temporary is assigned before its first use on each iteration.
    # Commit all of them before publication or when the last pair is rejected.
    local = re.sub(r"\bctx\.r(3|4|5|6|7|8|9|10|11)\b", r"local\1", original)

    def commit(indent):
        return "".join(f"{indent}ctx.r{r}.u64 = local{r}.u64;\n" for r in REGISTERS)

    local = checked_replace(
        local,
        "\tif (cr6.eq) goto loc_8299DB50;",
        "\tif (cr6.eq) {\n\t\tif (r29.u32 == 1) {\n"
        + commit("\t\t\t")
        + "\t\t}\n\t\tgoto loc_8299DB50;\n\t}",
    )
    call = "\tctx.lr = 0x8299DB50;"
    local = checked_replace(local, call, commit("\t") + call)
    declaration = "\tPPCRegister " + ", ".join(f"local{r}{{}}" for r in REGISTERS) + ";\n"
    return "\t{\n" + declaration + local + "\t}\n"


def prepare(path):
    data = path.read_bytes()
    source = data.decode("utf-8")
    eol = "\r\n" if "\r\n" in source else "\n"
    source = source.replace("\r\n", "\n")
    fn = source.index(FUNCTION)
    end = source.index(END, fn)
    if GUARD in source[fn:end]:
        start = source.index(GUARD, fn)
        wrapped = source[start:end]
        _, sep, disabled = wrapped.partition("#else\n")
        if not sep or not disabled.endswith("#endif\n"):
            raise ValueError("collision filter guard changed")
        original = disabled.removesuffix("#endif\n")
        if wrapped != GUARD + localize(original) + "#else\n" + original + "#endif\n":
            raise ValueError("guarded collision filter does not match the tested rewrite")
        print("collision filter: already prepared (verified)")
        return
    start = source.index(START, fn)
    original = source[start:end]
    wrapped = GUARD + localize(original) + "#else\n" + original + "#endif\n"
    updated = source[:start] + wrapped + source[end:]
    if updated.replace(wrapped, original, 1) != source:
        raise ValueError("disabled branch does not reproduce the original source")
    path.write_bytes(updated.replace("\n", eol).encode("utf-8"))
    print("collision filter: guarded rewrite; disabled branch unchanged")


if __name__ == "__main__":
    project = Path(sys.argv[1]) if len(sys.argv) > 1 else PROJECT
    prepare(project / TARGET)
