"""Unit tests for mtport.network (synthetic imports/tags documents, no game data)."""

import json
import subprocess
import sys

from mtport import names as nm
from mtport import network as net
from repo_paths import REPO


def synthetic_docs():
    imports_doc = {
        "imports": [
            {
                "name": "_CFSocketCreate",
                "sites": [["0x1C48A", "0x1C419", "call"]],
            },
            {
                "name": "_SCNetworkReachabilityGetFlags",
                "sites": [["0xCA110", "0xCA07D", "call"], ["0xCA1C2", "0xCA12D", "call"]],
            },
            {"name": "_strlen", "sites": [["0x1000", "0x2001", "call"]]},  # not network
        ],
        "sends": [
            {
                "site": "0xA8570",
                "function": "0xA8521",
                "selector": "openURL:",
                "class_method": None,
            },
            {
                "site": "0x95406",
                "function": "0x9462D",
                "selector": "sharedChartboost",
                "class_method": True,
            },
            {"site": "0x3000", "function": "0x2001", "selector": "init", "class_method": False},
        ],
        "functions": [
            {"function": "0x1C419", "name": "FUN_0001c418"},
            {"function": "0xCA07D", "name": "-[CBReachability connectionRequired]"},
        ],
    }
    tags_doc = {
        "functions": [
            {"address": "0x1C419", "tag": "engine"},
            {"address": "0xCA07D", "tag": "lib:chartboost"},
            {"address": "0xCA12D", "tag": "lib:chartboost"},
            {"address": "0xA8521", "tag": "app-objc"},
            {"address": "0x9462D", "tag": "engine"},
            {"address": "0x2001", "tag": "engine"},
        ]
    }
    return imports_doc, tags_doc


def test_collect_keeps_only_network_sites():
    sites = net.collect(*synthetic_docs())
    assert [(s.what, hex(s.site), s.tag) for s in sites] == [
        ("_CFSocketCreate", "0x1c48a", "engine"),
        ("+sharedChartboost", "0x95406", "engine"),
        ("-openURL:", "0xa8570", "app-objc"),
        ("_SCNetworkReachabilityGetFlags", "0xca110", "lib:chartboost"),
        ("_SCNetworkReachabilityGetFlags", "0xca1c2", "lib:chartboost"),
    ]
    assert sites[0].function_name == "FUN_0001c418"
    assert [s.chartboost for s in sites] == [False, False, False, True, True]


def test_check_doc_rules():
    sites = net.collect(*synthetic_docs())
    # Game sites need their own address; a Chartboost site is covered by its function.
    doc = "`0x1C48A` `0x95406` `0xA8570` and `0xCA07D` `0xCA12D`"
    assert net.check_doc(doc, sites) == []
    missing = net.check_doc("`0x1C419` (only the function) `0xCA07D`", sites)
    assert [hex(s.site) for s in missing] == ["0x1c48a", "0x95406", "0xa8570", "0xca1c2"]
    # A game site is not covered by its function's address.
    assert net.check_doc("0x1C419", sites[:1]) == sites[:1]


def test_doc_addresses_reads_hex_tokens():
    assert net.doc_addresses("at `0x1c48a`, 0xC3081 and 10 0x and x0x1") == {0x1C48A, 0xC3081}


def test_site_rows():
    sites = net.collect(*synthetic_docs())
    assert net.site_rows(sites[:1]) == [
        "| `0x1C48A` | `0x1C419` FUN_0001c418 | `_CFSocketCreate` | engine |"
    ]


def test_lists_have_no_duplicates():
    assert len(set(net.NETWORK_IMPORTS)) == len(net.NETWORK_IMPORTS)
    assert len(set(net.NETWORK_SELECTORS)) == len(net.NETWORK_SELECTORS)


def test_names_file_is_valid():
    doc = json.loads(net.names_json())
    source, entries = nm.parse(doc, "network.json")
    assert source == "network"
    assert len(entries) == len(net.NAMES)
    assert all(e.thumb for e in entries if e.kind == "function")  # the game is Thumb code
    assert {e.name for e in entries} >= {"CMSocket::Connect", "CDataMgr::OnServerPacket"}


def test_doc_names_every_named_function():
    text = (REPO / net.DOC).read_text(encoding="utf-8")
    for address, _kind, name, _comment in net.NAMES:
        assert int(address, 16) in net.doc_addresses(text), (address, name)


def test_cli_names(tmp_path):
    out = tmp_path / "names" / "network.json"
    env = {"PYTHONPATH": str(REPO / "tools")}
    subprocess.run(
        [sys.executable, "-m", "mtport.network", "names", str(out)],
        check=True,
        capture_output=True,
        env=env,
    )
    assert json.loads(out.read_text())["source"] == "network"
    res = subprocess.run(
        [sys.executable, "-m", "mtport.network", "check", "--repo", str(tmp_path)],
        capture_output=True,
        text=True,
        env=env,
    )
    assert res.returncode == 1 and "error:" in res.stderr
