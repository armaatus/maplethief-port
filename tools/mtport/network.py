"""Network paths of the armv7 slice (#23): names, and a coverage check of docs/re/network.md.

The analysis itself is written up in ``docs/re/network.md``. This module keeps the parts of it
that a machine can check:

* :data:`NETWORK_IMPORTS` and :data:`NETWORK_SELECTORS`: which imports and ObjC messages count
  as network paths (sockets, name resolution, reachability, URL loading, URL opening, web
  views, push registration, the device identifier that is sent, and the game's calls into
  Chartboost).
* :func:`collect`: every code site of those, from #11's ``work/re/imports.json`` (import call
  sites and ``objc_msgSend`` sends), each with the #10 tag of its function
  (``work/re/function-tags.json``).
* :func:`check_doc`: every collected site must be named in the doc by its address, except
  that a site inside a ``lib:chartboost`` function may instead be covered by that function's
  address (the doc's Chartboost table): stubbing Chartboost at its entry points makes those
  unreachable. Both come from :mod:`mtport.sitecheck`, which #21's ``mtport.timing`` shares.
* :data:`NAMES`: Ghidra names for the game's network functions (``CMSocket`` and the
  ``CDataMgr`` code that drives it), written to ``work/re/names/network.json``.

Command line::

    python3 -m mtport.network names [OUT]      # work/re/names/network.json
    python3 -m mtport.network sites [--repo D] # every network site, as Markdown rows
    python3 -m mtport.network check [--repo D] # exit 1 if the doc misses a site
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

from mtport import ghidra_common as gc
from mtport import sitecheck

NAMES_OUT = "work/re/names/network.json"
IMPORTS_JSON = sitecheck.IMPORTS_JSON
TAGS_JSON = sitecheck.TAGS_JSON
DOC = "docs/re/network.md"

CHARTBOOST_TAG = sitecheck.CHARTBOOST_TAG

# Imports that open, resolve, probe or describe a network path. NSURL is included because it
# is how every opened URL is built; its file-URL sites are listed in the doc as local.
NETWORK_IMPORTS: tuple[str, ...] = (
    "_CFSocketConnectToAddress",
    "_CFSocketCreate",
    "_CFSocketCreateRunLoopSource",
    "_CFSocketGetNative",
    "_CFSocketInvalidate",
    "_CFSocketSendData",
    "_CFRunLoopAddSource",  # only the CMSocket run-loop source
    "_close",  # only the CMSocket descriptor
    "_gethostbyname",
    "_inet_addr",
    "_inet_ntoa",
    "_setsockopt",
    "_if_nametoindex",
    "_sysctl",
    "_SCNetworkReachabilityCreateWithAddress",
    "_SCNetworkReachabilityCreateWithName",
    "_SCNetworkReachabilityGetFlags",
    "_SCNetworkReachabilityScheduleWithRunLoop",
    "_SCNetworkReachabilitySetCallback",
    "_SCNetworkReachabilityUnscheduleFromRunLoop",
    "_OBJC_CLASS_$_NSURLConnection",
    "_OBJC_METACLASS_$_NSURLConnection",
    "_OBJC_CLASS_$_NSURLRequest",
    "_OBJC_CLASS_$_NSMutableURLRequest",
    "_OBJC_CLASS_$_NSURLCredential",
    "_NSURLAuthenticationMethodServerTrust",
    "_OBJC_CLASS_$_UIWebView",
    "_OBJC_METACLASS_$_UIWebView",
    "_OBJC_CLASS_$_NSURL",
)

# Messages that load or open a URL, register for push, read the device id that is sent, or
# drive Chartboost (whose classes are in the image, so their sends are not import sites).
NETWORK_SELECTORS: tuple[str, ...] = (
    "openURL:",
    "canOpenURL:",
    "loadRequest:",
    "loadHTMLString:baseURL:",
    "initWithRequest:delegate:startImmediately:",
    "requestWithURL:",
    "URLWithString:",
    "dataWithContentsOfURL:",
    "registerForRemoteNotificationTypes:",
    "uniqueGlobalDeviceIdentifier",
    "uniqueDeviceIdentifier",
    "macaddress",
    "reachabilityForInternetConnection",
    "reachabilityWithHostName:",
    "reachabilityWithAddress:",
    "currentReachabilityStatus",
    "sharedChartboost",
    "setAppId:",
    "setAppSignature:",
    "startSession",
    "showInterstitial",
)

# -- Ghidra names (work/re/names/network.json) ----------------------------------------------

# (address, kind, name, comment). Function pointers carry the Thumb bit.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    ("0x1BBB5", "function", "CMSocket::Swap16", "htons for the sockaddr_in port"),
    ("0x1BBE1", "function", "CMSocket::OnSocketEvent", "connect (4) + write (8) -> connected"),
    ("0x1BC4D", "function", "CMSocket::GetStatus", "+0x14: 1 connected, -1 not connected"),
    ("0x1BC61", "function", "CMSocket::ReadU16", "static (buf, off), little-endian"),
    ("0x1BCB1", "function", "CMSocket::WriteU16", "static (buf, off, value), little-endian"),
    ("0x1BCD5", "function", "CMSocket::ReadU32", "static (buf, off), little-endian"),
    ("0x1BD09", "function", "CMSocket::WriteU32", "static (buf, off, value), little-endian"),
    ("0x1BD41", "function", "CMSocket::CopyBytes", "static (src, soff, dst, doff, n)"),
    ("0x1BDB1", "function", "CMSocket::ReadString", "static (buf, off, n): new[] n+1, NUL"),
    ("0x1BE31", "function", "CMSocket::WriteFixedString", "static (buf, off, s, width), NUL pad"),
    ("0x1BED5", "function", "CMSocket::Disconnect", "(this, notify): close; notify -> 0x20"),
    ("0x1C1C9", "function", "CMSocket::FlushSendQueue", "CFSocketSendData; 3 errors -> fail"),
    ("0x1C31D", "function", "CMSocket::Send", "frame [u32 len+6][u16 0xFFFF][payload]"),
    ("0x1C419", "function", "CMSocket::Connect", "(this, host, port, byName): TCP, 10 s"),
    ("0x1C651", "function", "CMSocket_Callback", "CFSocketCallBack: 3 data, 4 connect, 8 write"),
    ("0x1C8B5", "function", "CMSocket::CMSocket", "send queue of 30, 2 KiB receive buffer"),
    ("0x1C935", "function", "CMSocket::Instance", "singleton, operator new(0x84C)"),
    ("0x1CA79", "function", "CMSocket::OnData", "reassemble frames -> OnServerPacket"),
    ("0x371ED", "function", "CDataMgr::SendServerPacket", "(this, op): 0, 2, 4, 0xB4"),
    ("0x37B55", "function", "CDataMgr::ConnectServer", '"50.17.236.149":20001'),
    ("0x3ED19", "function", "CDataMgr::OnServerPacket", "server ops 1, 3, 5 (IAP verify)"),
    ("0x22955", "function", "OpenPortalURLAndExit", "no-op NGB hook, then exit(1)"),
    ("0x11AA94", "data", "CMSocket::s_pInstance", "CMSocket::Instance() singleton"),
)


def names_json() -> str:
    return gc.names_json("network", NAMES)


# -- sites (the check itself lives in mtport.sitecheck, shared with mtport.timing) ----------

Site = sitecheck.Site
doc_addresses = sitecheck.doc_addresses
check_doc = sitecheck.check_doc
site_rows = sitecheck.site_rows


def collect(imports_doc: dict, tags_doc: dict) -> list[Site]:
    """Every site of :data:`NETWORK_IMPORTS` and :data:`NETWORK_SELECTORS`, sorted."""
    return sitecheck.collect(imports_doc, tags_doc, NETWORK_IMPORTS, NETWORK_SELECTORS)


# -- command line ---------------------------------------------------------------------------


def _load(repo: Path) -> list[Site]:
    return sitecheck.load_sites(repo, NETWORK_IMPORTS, NETWORK_SELECTORS)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.network", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    names = sub.add_parser("names", help=f"write the Ghidra names file (default {NAMES_OUT})")
    names.add_argument("out", nargs="?", default=NAMES_OUT)
    default_repo = Path(__file__).resolve().parents[2]
    for cmd, text in (("sites", "print every network site"), ("check", f"check {DOC}")):
        p = sub.add_parser(cmd, help=text)
        p.add_argument("--repo", type=Path, default=default_repo)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "names":
            out = Path(args.out)
            gc.write_text(out, names_json())
            print(f"wrote {out} ({len(NAMES)} names)")
            return 0
        sites = _load(args.repo)
        if args.cmd == "sites":
            print("\n".join(site_rows(sites)))
            return 0
        missing = check_doc((args.repo / DOC).read_text(encoding="utf-8"), sites)
        game = sum(1 for s in sites if not s.chartboost)
        print(f"{len(sites)} network sites ({game} outside Chartboost), {len(missing)} missing")
        for row in site_rows(missing):
            print("  " + row)
        return 1 if missing else 0
    except (OSError, ValueError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        if isinstance(e, FileNotFoundError):
            hint = "run tools/ghidra/import, mtport.libtags and mtport.imports first"
            print(hint, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
