"""The names contract (mtport.names): schema, merging and the Ghidra symbol mapping."""

import json

import pytest

from mtport import names


def write(tmp_path, file_name, doc):
    path = tmp_path / file_name
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path


def entry(address, name, kind="function", **extra):
    return {"address": address, "name": name, "kind": kind, **extra}


@pytest.mark.parametrize(
    ("name", "expected"),
    [
        ("-[Class sel:with:]", ((), "-[Class_sel:with:]")),
        ("+[Class new]", ((), "+[Class_new]")),
        (
            "-[UIDevice(IdentifierAddition) macaddress]",
            ((), "-[UIDevice(IdentifierAddition)_macaddress]"),
        ),
        ("Class::vf3", (("Class",), "vf3")),
        ("ns::Class::~Class", (("ns", "Class"), "~Class")),
        ("ns::Tpl<a::b>::f", (("ns", "Tpl<a::b>"), "f")),
        ("Class::operator<", (("Class",), "operator<")),
        ("Class::operator()", (("Class",), "operator()")),
        ("Class::operator<<", (("Class",), "operator<<")),
        ("std::map<int, Foo*>::find", (("std", "map<int,_Foo*>"), "find")),
        ("_main", ((), "_main")),
        ("loop head", ((), "loop_head")),
        ("a   b", ((), "a_b")),  # a run of invalid characters becomes one "_"
    ],
)
def test_ghidra_symbol(name, expected):
    assert names.ghidra_symbol(name) == expected


def test_symbols_never_contain_invalid_characters():
    for name in ("-[A b:c:]", "x::y z::w", "tab sep"):
        namespaces, symbol = names.ghidra_symbol(name)
        for part in (*namespaces, symbol):
            assert part and not any(names.is_invalid_char(ch) for ch in part)


def test_entry_location_and_mode():
    thumb = names.NameEntry(0x2A3C1, "f", "function")
    arm = names.NameEntry(0x27E0, "start", "function")
    label = names.NameEntry(0x2A3C5, "l", "label")
    data = names.NameEntry(0x117E45, "d", "data")
    assert (thumb.location, thumb.thumb) == (0x2A3C0, True)
    assert (arm.location, arm.thumb) == (0x27E0, False)
    assert (label.location, label.thumb) == (0x2A3C4, False)
    assert (data.location, data.thumb) == (0x117E45, False)


def test_hexaddr_round_trips_through_parse_address():
    assert names.hexaddr(0x2A3C1) == "0x2A3C1"
    assert names.hexaddr(0) == "0x0"
    assert names.parse_address(names.hexaddr(0xFFFFFFFF)) == 0xFFFFFFFF


@pytest.mark.parametrize("text", ["2A3C1", "0x2a3c1", "0X0002A3C1", "0x2A3C1"])
def test_parse_hex_is_lenient(text):
    assert names.parse_hex(text) == 0x2A3C1


@pytest.mark.parametrize("text", ["", "0x", "x1", " 0x1", "0x_1", "0x1g", 0x1])
def test_parse_hex_rejects_non_hex(text):
    with pytest.raises(ValueError):
        names.parse_hex(text)


@pytest.mark.parametrize("text", ["2A3C1", "0x123456789", "0x", 7])
def test_parse_address_is_strict(text):
    with pytest.raises(ValueError):
        names.parse_address(text)


def test_other_names_skips_the_callers_own_file(tmp_path):
    write(tmp_path, "a.json", {"source": "a", "names": [entry("0x101", "f"), entry("0x200", "d")]})
    write(tmp_path, "b.json", {"source": "b", "names": [entry("0x101", "g")]})
    write(tmp_path, "mine.json", {"source": "mine", "names": [entry("0x300", "mine")]})
    assert names.other_names(tmp_path, "mine.json") == {0x100: "f", 0x200: "d"}


def test_load_dir_merges_sorted_files(tmp_path):
    write(tmp_path, "b.json", {"source": "rtti", "names": [entry("0x2A3C1", "Class::vf3")]})
    write(
        tmp_path,
        "a.json",
        {
            "source": "objc",
            "names": [
                entry("0x2A3C1", "Class::vf3", comment="from a"),
                entry("0x117E44", "cfstr_x", "data"),
                entry("0x2A3D1", "inner", "label"),
            ],
        },
    )
    write(
        tmp_path,
        "c.json",
        {"source": "flow", "names": [entry("0x2A3C1", "Class::vf3", comment="from c")]},
    )
    (tmp_path / "notes.txt").write_text("ignored")
    merged = names.load_dir(tmp_path)
    assert [m.entry.location for m in merged] == [0x2A3C0, 0x2A3D0, 0x117E44]
    first = merged[0]
    assert first.sources == ["objc", "rtti", "flow"]
    assert first.comment == "from a\nfrom c"


def test_missing_dir_has_no_names(tmp_path):
    assert names.load_dir(tmp_path / "absent") == []


@pytest.mark.parametrize(
    ("other", "message"),
    [
        (entry("0x2A3C1", "Other::name"), "a.json says function 'Class::vf3'"),
        (entry("0x2A3C0", "Class::vf3"), "(0x2A3C0)"),  # same location, ARM instead of Thumb
        (entry("0x2A3C0", "Class::vf3", "data"), "b.json says data"),
    ],
)
def test_conflicts_are_errors(tmp_path, other, message):
    write(tmp_path, "a.json", {"source": "objc", "names": [entry("0x2A3C1", "Class::vf3")]})
    write(tmp_path, "b.json", {"source": "rtti", "names": [other]})
    with pytest.raises(names.NamesError) as e:
        names.load_dir(tmp_path)
    assert message in str(e.value)


@pytest.mark.parametrize(
    ("doc", "message"),
    [
        ([], "top level must be an object"),
        ({"names": []}, "'source' must be a non-empty string"),
        ({"source": "x", "names": {}}, "'names' must be a list"),
        ({"source": "x", "names": [], "extra": 1}, "unknown top-level keys"),
        ({"source": "x", "names": [entry("2A3C1", "f")]}, "address must be a hex string"),
        ({"source": "x", "names": [entry("0x123456789", "f")]}, "address must be a hex string"),
        ({"source": "x", "names": [entry("0x10", "f", "code")]}, "kind must be one of"),
        ({"source": "x", "names": [entry("0x10", "")]}, "non-empty string"),
        ({"source": "x", "names": [entry("0x10", " f")]}, "leading or trailing whitespace"),
        ({"source": "x", "names": [entry("0x10", "a\nb")]}, "control character"),
        ({"source": "x", "names": [entry("0x10", "x" * 2001)]}, "longer than 2000"),
        ({"source": "x", "names": [entry("0x10", "FUN_0002a3c0")]}, "reserved default names"),
        ({"source": "x", "names": [entry("0x10", "A::LAB_1234")]}, "reserved default names"),
        ({"source": "x", "names": [entry("0x10", "A::::b")]}, "empty '::' component"),
        ({"source": "x", "names": [entry("0x10", "f", comment=3)]}, "comment must be a string"),
        ({"source": "x", "names": [{**entry("0x10", "f"), "size": 4}]}, "unknown keys"),
    ],
)
def test_validation(tmp_path, doc, message):
    write(tmp_path, "bad.json", doc)
    with pytest.raises(names.NamesError) as e:
        names.load_dir(tmp_path)
    assert message in str(e.value)
    assert str(e.value).startswith("bad.json")


def test_all_problems_are_reported(tmp_path):
    write(tmp_path, "a.json", {"source": "x", "names": [entry("bad", "f"), entry("0x10", "")]})
    (tmp_path / "b.json").write_text("{not json", encoding="utf-8")
    with pytest.raises(names.NamesError) as e:
        names.load_dir(tmp_path)
    assert len(e.value.problems) == 3


def test_cli(tmp_path, capsys):
    write(tmp_path, "a.json", {"source": "objc", "names": [entry("0x2A3C1", "-[A b:]")]})
    assert names.main(["show", str(tmp_path)]) == 0
    out = capsys.readouterr()
    assert "0x0002A3C1 function -[A_b:]  [objc]" in out.out
    write(tmp_path, "z.json", {"source": "x", "names": [entry("0x10", "")]})
    assert names.main(["check", str(tmp_path)]) == 1
