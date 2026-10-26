"""Unit tests for the viewer page scaffolding (mtport.htmlpage)."""

from __future__ import annotations

import shutil
import subprocess

import pytest

from mtport import htmlpage as hp


def test_esc():
    assert hp.esc('<a href="x">&\'') == "&lt;a href=&quot;x&quot;&gt;&amp;&#x27;"
    assert hp.esc(-3) == "-3"


def test_cells_and_link_escape():
    assert hp.text("<b>", "ko") == hp.Cell("&lt;b&gt;", None, "ko")
    assert hp.num(7, "<x>") == hp.Cell('7 <span class="label">&lt;x&gt;</span>', 7)
    want = '<a href="a.html#&quot;" title="t&amp;">&lt;n&gt;</a>'
    assert hp.link('a.html#"', "<n>", "t&") == want


def test_table():
    cols = [hp.Column("id", "the <key>", numeric=True), hp.Column("name")]
    rows = [[hp.num(2), hp.text("b")], [hp.num(1), hp.text("<a>", "ko")]]
    out = hp.table(cols, rows, ["r2", "r1"])
    assert '<th class="num" title="the &lt;key&gt;" tabindex="0">id</th>' in out
    assert '<th tabindex="0">name</th>' in out
    assert '<tr data-i="0" id="r2"><td class="num" data-k="2">2</td><td>b</td></tr>' in out
    assert '<tr data-i="1" id="r1"><td class="num" data-k="1">1</td>' in out
    assert '<td lang="ko">&lt;a&gt;</td>' in out
    assert out.count("<tr data-i=") == 2
    with pytest.raises(ValueError, match="row 0 has 1 cells"):
        hp.table(cols, [[hp.num(1)]])


def test_page_is_self_contained():
    out = hp.page("A & B", "<p>x</p>\n", "<a href='index.html'>Index</a>")
    assert out.startswith("<!doctype html>\n")
    assert '<meta charset="utf-8">' in out
    assert "<title>A &amp; B</title>" in out and "<h1>A &amp; B</h1>" in out
    assert out.count("<script>") == 1 and out.count("<style>") == 1
    # No network: nothing is loaded from anywhere.
    assert "http" not in out and "src=" not in out and "<link" not in out
    assert out == hp.page("A & B", "<p>x</p>\n", "<a href='index.html'>Index</a>")


def test_write_dir_and_diff_dir(tmp_path):
    out = tmp_path / "v"
    files = {"index.html": b"i", "icons/a/1.png": b"p"}
    assert hp.diff_dir(out, files) == [f"{out}: missing (run the build command)"]
    hp.write_dir(out, files, "maps")
    assert (out / "icons/a/1.png").read_bytes() == b"p"
    assert hp.diff_dir(out, files) == []
    (out / "index.html").write_bytes(b"changed")
    (out / "icons/a/1.png").unlink()
    (out / "x.json").write_bytes(b"{}")
    assert hp.diff_dir(out, files) == [
        "icons/a/1.png: missing",
        "index.html: stale (differs from a fresh build)",
        "x.json: not part of a fresh build",
    ]
    hp.write_dir(out, files, "maps")  # owned: replaced
    assert hp.diff_dir(out, files) == []
    with pytest.raises(FileExistsError):
        hp.write_dir(out, files, "sprites")  # another viewer's directory
    foreign = tmp_path / "foreign"
    foreign.mkdir()
    (foreign / "keep.txt").write_text("k", encoding="utf-8")
    with pytest.raises(FileExistsError):
        hp.write_dir(foreign, files, "maps")
    assert (foreign / "keep.txt").exists()
    empty = tmp_path / "empty"
    empty.mkdir()
    hp.write_dir(empty, files, "maps")
    assert (empty / "index.html").exists()


# A minimal fake DOM: enough of the API that SCRIPT uses to sort and filter one table.
DOM_HARNESS = r"""
const fs = require("fs");
function el(attrs, text) {
  const a = Object.assign({}, attrs || {}), ls = {};
  return {
    textContent: text || "", hidden: false,
    getAttribute: (k) => (k in a ? a[k] : null),
    setAttribute: (k, v) => { a[k] = String(v); },
    removeAttribute: (k) => { delete a[k]; },
    classList: { contains: (c) => (a.class || "").split(" ").includes(c) },
    addEventListener: (ev, f) => { ls[ev] = f; },
    fire: (ev, e) => ls[ev](e || {}),
  };
}
const data = [["3", "b"], ["10", "a"], ["2", "c"], ["10", "d"]];
const rows = data.map((d, i) => {
  const r = el({ "data-i": String(i) });
  r.cells = [el({ "data-k": d[0] }, d[0] + " x"), el({}, d[1])];
  r.textContent = d.join(" ");
  return r;
});
const body = { rows: rows.slice(),
  appendChild(r) { this.rows.splice(this.rows.indexOf(r), 1); this.rows.push(r); } };
const heads = [el({ class: "num" }, "n"), el({}, "t")];
const table = { tHead: { rows: [{ cells: heads }] }, tBodies: [body] };
const box = el({}); box.value = "";
const shown = el({});
const viewer = { querySelector: (s) =>
  (s === "table.sortable" ? table : s === ".filter" ? box : shown) };
global.document = { querySelectorAll: () => [viewer] };
eval(fs.readFileSync(process.argv[2], "utf8"));
const order = () => body.rows.map((r) => r.getAttribute("data-i")).join(",");
const out = [shown.textContent];
const mark = () => order() + " " + heads[0].getAttribute("aria-sort");
heads[0].fire("click"); out.push(mark());
heads[0].fire("click"); out.push(mark());
heads[1].fire("keydown", { key: "Enter" }); out.push(mark());
box.value = "10"; box.fire("input"); out.push(shown.textContent + " " + rows.map((r) => r.hidden));
console.log(out.join("\n"));
"""


@pytest.mark.skipif(shutil.which("node") is None, reason="node not installed")
def test_script_sorts_and_filters(tmp_path):
    (tmp_path / "script.js").write_text(hp.SCRIPT, encoding="utf-8")
    (tmp_path / "harness.js").write_text(DOM_HARNESS, encoding="utf-8")
    result = subprocess.run(
        ["node", str(tmp_path / "harness.js"), str(tmp_path / "script.js")],
        capture_output=True,
        text=True,
        check=True,
    )
    assert result.stdout.splitlines() == [
        "4 of 4 rows",
        "2,0,1,3 ascending",  # numbers by value, ties in original order
        "1,3,0,2 descending",
        "1,0,2,3 null",  # text column; the other header loses its mark
        "2 of 4 rows true,false,true,false",
    ]
