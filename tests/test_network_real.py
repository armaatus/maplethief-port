"""Real-asset checks for docs/re/network.md (#23) against #11's imports.json and #10's tags.

Skipped when work/ipa/ is absent (CI), and per test when the generated inputs are missing
(run tools/ghidra/import, then mtport.libtags and mtport.imports generate).
"""

from __future__ import annotations

import json

import pytest

from mtport import network as net
from repo_paths import REPO

pytestmark = pytest.mark.real_assets


def _json(rel: str) -> dict:
    path = REPO / rel
    if not path.is_file():
        pytest.skip(f"{rel} absent (run the RE pipeline: tools/ghidra/import, libtags, imports)")
    return json.loads(path.read_text(encoding="utf-8"))


@pytest.fixture(scope="module")
def sites() -> list[net.Site]:
    return net.collect(_json(net.IMPORTS_JSON), _json(net.TAGS_JSON))


def test_every_network_site_is_in_the_doc(sites):
    """Each site is named in the doc, or sits in a Chartboost function the doc lists."""
    missing = net.check_doc((REPO / net.DOC).read_text(encoding="utf-8"), sites)
    assert missing == [], "\n".join(net.site_rows(missing))


def test_every_network_import_is_used(sites):
    """The import list has no names that do not occur in the binary (typo guard)."""
    present = {r["name"] for r in _json(net.IMPORTS_JSON)["imports"]}
    assert set(net.NETWORK_IMPORTS) <= present


def test_site_counts(sites):
    """150 sites: 76 outside Chartboost (game, app shell, UIDevice category), 74 inside."""
    assert len(sites) == 150
    assert sum(not s.chartboost for s in sites) == 76
    # All socket and resolver calls are in the engine's CMSocket code.
    socket = [s for s in sites if s.what.startswith(("_CFSocket", "_inet_", "_gethost"))]
    assert socket and {s.tag for s in socket} == {"engine"}
    assert {s.function for s in socket} == {0x1BED5, 0x1C1C9, 0x1C419, 0x1C6B5, 0x1C7B5}
    # Reachability is Chartboost-only.
    reach = [s for s in sites if s.what.startswith("_SCNetworkReachability")]
    assert reach and all(s.chartboost for s in reach)


def test_names_are_applied_in_ghidra():
    """After tools/ghidra/import, every network.json function name is on its function."""
    functions = {int(f["address"], 16): f for f in _json("work/ghidra/functions.json")["functions"]}
    for address, kind, name, _comment in net.NAMES:
        if kind != "function":
            continue
        f = functions.get(int(address, 16))
        assert f is not None, (address, name)
        assert f["name"] == name and f["name_source"] == "names", (address, f["name"])


def test_named_functions_call_their_imports():
    """The CMSocket names sit on the functions that make the matching calls."""
    rows = {r["name"]: r for r in _json(net.IMPORTS_JSON)["imports"]}

    def callers(imp: str) -> set[int]:
        return {int(f, 16) for _site, f, _k in rows[imp]["sites"]}

    names = {n: int(a, 16) for a, _k, n, _c in net.NAMES}
    assert callers("_CFSocketConnectToAddress") == {names["CMSocket::Connect"]}
    assert callers("_gethostbyname") == {names["CMSocket::Connect"]}
    assert callers("_CFSocketSendData") == {names["CMSocket::FlushSendQueue"]}
    assert callers("_close") == {names["CMSocket::Disconnect"]}
