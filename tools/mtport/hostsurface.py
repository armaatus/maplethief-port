"""The game-reachable framework surface: every (class, selector) that game code sends to a host
class, the list the HLE framework issues implement (#49 Foundation, #51 drawing, #52 stubs,
#54 UIKit; ADR 0026: "each implements only the selectors that game code reaches").

Inputs: ``work/re/imports.json`` (#11, ``python3 -m mtport.imports generate``) and
``work/re/objc.json`` (#8, ``python3 -m mtport.objc generate``). A send counts when a *game*
function sends it (Chartboost never runs, ADR 0026) and that function is not dead code
(:data:`DEAD_FUNCTIONS`, from docs/re/objc-shell.md). Two kinds of send:

- **typed**: #11 knows the receiving class, and the method it reaches is a framework class's
  (``implemented_by``; ``[super dealloc]`` in an ``NSObject`` subclass reaches ``NSObject``).
  Except a send to ``self`` after ``self = [super init...]``: #11 types the result of an init
  (``how`` "init") as the class it was sent to, so ``self`` there looks like the app class's
  framework base. When that is the receiver and the sending method's own class implements the
  selector, the send reaches the app's method (``[self createGLes]`` in
  ``-[EAGLView initWithCoder:]``) and is no row;
- **untyped**: #11 does not know the receiver, and no app class or category implements the
  selector (Chartboost's classes may: they are never instantiated), so a framework class
  receives it. :data:`RECEIVERS` names those classes, by hand, from the sending functions;
  generation fails on an untyped selector it does not list, so a new analysis cannot drop a
  selector silently (and on a listed one no live game code sends, so the table stays exact).
  A selector some app class implements may reach either and is left out (``init``,
  ``release``, delegate methods).

Each row gets the *area* that implements it: Foundation for the Foundation and CoreFoundation
classes, UIKit for UIKit's (with the NSString drawing additions, :data:`AREA_OVERRIDES`), and
so on. ``docs/re/host-surface.md`` is the result. The runtime's test ``objc.host_surface``
(tests/cpp/host_surface_test.cpp) reads it and looks every row of the areas it checks up in
the registered host classes.

Command line::

    python3 -m mtport.hostsurface generate [--repo DIR]   # writes docs/re/host-surface.md
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

IMPORTS_JSON = "work/re/imports.json"
OBJC_JSON = "work/re/objc.json"
MARKDOWN_OUT = "docs/re/host-surface.md"
CLASS_SYMBOL = "_OBJC_CLASS_$_"

# Functions that never run (docs/re/objc-shell.md → How the shell works → Dead code): the movie
# player and its overlay, EAGLView's logo/timer path, MainMenuView's redraw animation, and the
# unreferenced ChooseCharacterView/TouchKeyPadView methods. Then the other methods objc-shell.md
# lists under "No selector reference" that send framework messages and are no framework
# callback (no delegate method, drawRect:, touches, layerClass): no code names their selector,
# not even as a button action, so nothing calls them.
DEAD_FUNCTIONS = frozenset(
    {
        "-[EAGLView startAppEnv]",
        "-[EAGLView drawMapleLogo__]",
        "-[EAGLView movieFinishedCallback:]",
        "-[MainMenuView redraw:]",
        "-[MainMenuView moveMapleTitle:]",
        "-[MainMenuView moveRobin:]",
        "-[MainMenuView movePanelMenu:]",
        "-[MainMenuView moveDetail:]",
        "-[MainMenuView moveBuilding:]",
        "-[ChooseCharacterView animateStatus:]",
        "-[TouchKeyPadView sampleButtonClicked:]",
        "-[TouchKeyPadView initKeyPadType5:]",
        # unreferenced, and no framework callback
        "-[ChooseCharacterView okAlert:]",
        "-[EAGLView reloadMainMenu:]",
        "-[GameCenterManager FindPlayerByid:]",
        "-[GameCenterManager FindScoreByid:]",
        "-[GameCenterManager ReloadHighScoreByCategory:]",
        "-[GameCenterManager ResetAchievements]",
        "-[InAppPurchaseManager getPurchasedReceiptLength]",
        "-[InAppPurchaseManager isPresentNonFinishedTransaction]",
        "-[InAppPurchaseManager makePurchase:]",
        "-[MainMenuView closePlay:]",
        "-[MainMenuView removeSubviews]",
        "-[TouchKeyPadView pressAttackButton:]",
        "-[TouchKeyPadView pressJumpButton:]",
        "-[TouchKeyPadView releaseAttackButton:]",
        "-[TouchKeyPadView releaseJumpButton:]",
    }
)
DEAD_PREFIXES = ("-[MoviePlayerController ", "-[MovieOverlayView ", "MoviePlayerController_")

# The receiving class(es) of each untyped game selector, from the sending functions (listed in
# imports.json → sends). A "+" prefix marks a class method. An empty tuple: the message goes to
# nil or to an object no framework class implements the selector for (a delegate the game
# never sets).
RECEIVERS: dict[str, tuple[str, ...]] = {
    # Foundation
    "UTF8String": ("NSString",),
    "addObject:": ("NSMutableArray",),
    "allObjects": ("NSSet",),  # the touches of touchesBegan:/Moved:/Ended:
    "alloc": ("+NSObject",),
    "appendFormat:": ("NSMutableString",),
    "availableData": ("NSFileHandle",),
    "bytes": ("NSData",),
    "closeFile": ("NSFileHandle",),
    "compare:options:": ("NSString",),  # the system version, NSNumericSearch
    "count": ("NSArray", "NSSet"),
    "countByEnumeratingWithState:objects:count:": ("NSArray", "NSSet"),
    "createFileAtPath:contents:attributes:": ("NSFileManager",),
    "fileExistsAtPath:": ("NSFileManager",),
    "getBytes:": ("NSData",),
    "isEqual:": ("NSString",),  # player ids
    "isEqualToString:": ("NSString",),
    "isKindOfClass:": ("NSObject",),
    "length": ("NSString", "NSData"),
    "localizedStringForKey:value:table:": ("NSBundle",),
    "objectAtIndex:": ("NSArray",),
    "objectForInfoDictionaryKey:": ("NSBundle",),
    "objectForKey:": ("NSDictionary", "NSLocale"),
    "pathForResource:ofType:inDirectory:": ("NSBundle",),
    "performSelector:withObject:": ("NSObject",),
    "performSelector:withObject:withObject:": ("NSObject",),
    "readDataOfLength:": ("NSFileHandle",),
    "removeAllObjects": ("NSMutableArray",),
    "removeItemAtPath:error:": ("NSFileManager",),
    "respondsToSelector:": ("NSObject",),
    "stringByAppendingPathComponent:": ("NSString",),
    "synchronizeFile": ("NSFileHandle",),
    "truncateFileAtOffset:": ("NSFileHandle",),
    "writeData:": ("NSFileHandle",),
    # NSString's UIKit additions (#51)
    "drawAtPoint:withFont:": ("NSString",),
    "sizeWithFont:": ("NSString",),
    # UIKit, CoreGraphics bridges and OpenGL ES (#51, #54, #117)
    "CGColor": ("UIColor",),
    "CGImage": ("UIImage",),
    "addSubview:": ("UIView",),
    "addTarget:action:forControlEvents:": ("UIButton",),
    "contentOffset": ("UIScrollView",),
    "dismissModalViewControllerAnimated:": ("UIViewController",),
    "drawInRect:": ("UIImage",),
    "fontWithSize:": ("UIFont",),
    "frame": ("UIView",),
    "layer": ("UIView",),
    "locationInView:": ("UITouch",),
    "model": ("UIDevice",),
    "openURL:": ("UIApplication",),
    "presentModalViewController:animated:": ("UIViewController",),
    "presentRenderbuffer:": ("EAGLContext",),
    "previousLocationInView:": ("UITouch",),
    "registerForRemoteNotificationTypes:": ("UIApplication",),
    "removeFromSuperview": ("UIView",),
    "renderbufferStorage:fromDrawable:": ("EAGLContext",),
    "setAlpha:": ("UIView",),
    "setApplicationIconBadgeNumber:": ("UIApplication",),
    "setBackgroundColor:": ("UIView",),
    "setBounds:": ("UIView",),
    "setCanCancelContentTouches:": ("UIScrollView",),
    "setCenter:": ("UIView",),
    "setClipsToBounds:": ("UIView",),
    "setContentSize:": ("UIScrollView",),
    "setCurrentPage:": ("UIPageControl",),
    "setDrawableProperties:": ("CAEAGLLayer",),
    "setEditable:": ("UITextView",),
    "setFont:": ("UILabel", "UITextView"),
    "setFrame:": ("UIView",),
    "setHidesWhenStopped:": ("UIActivityIndicatorView",),
    "setImage:forState:": ("UIButton",),
    "setImageEdgeInsets:": ("UIButton",),
    "setIndicatorStyle:": ("UIScrollView",),
    "setNeedsDisplay": ("UIView",),
    "setNumberOfLines:": ("UILabel",),
    "setNumberOfPages:": ("UIPageControl",),
    "setOpaque:": ("CAEAGLLayer",),
    "setPagingEnabled:": ("UIScrollView",),
    "setScrollEnabled:": ("UIScrollView",),
    "setShowsHorizontalScrollIndicator:": ("UIScrollView",),
    "setShowsVerticalScrollIndicator:": ("UIScrollView",),
    "setText:": ("UILabel", "UITextView"),
    "setTextAlignment:": ("UILabel",),
    "setTextColor:": ("UILabel",),
    "setTransform:": ("UIView",),
    "setUserInteractionEnabled:": ("UIView",),
    "setView:": ("UIViewController",),
    "size": ("UIImage",),
    "startAnimating": ("UIActivityIndicatorView",),
    "stopAnimating": ("UIActivityIndicatorView",),
    "subviews": ("UIView",),
    "systemName": ("UIDevice",),
    "systemVersion": ("UIDevice",),
    # AVFoundation
    "isPlaying": ("AVAudioPlayer",),
    "play": ("AVAudioPlayer",),
    "setCategory:error:": ("AVAudioSession",),
    "setCurrentTime:": ("AVAudioPlayer",),
    "setVolume:": ("AVAudioPlayer",),
    "stop": ("AVAudioPlayer",),
    # GameKit and StoreKit (#52)
    "addPayment:": ("SKPaymentQueue",),
    "addTransactionObserver:": ("SKPaymentQueue",),
    "authenticateWithCompletionHandler:": ("GKLocalPlayer",),
    "finishTransaction:": ("SKPaymentQueue",),
    "initWithIdentifier:": ("GKAchievement",),
    "invalidProductIdentifiers": ("SKProductsResponse",),
    "isAuthenticated": ("GKLocalPlayer",),
    "loadAchievementsWithCompletionHandler:": ("+GKAchievement",),
    "percentComplete": ("GKAchievement",),
    "playerID": ("GKPlayer",),
    "products": ("SKProductsResponse",),
    "removeTransactionObserver:": ("SKPaymentQueue",),
    "reportAchievementWithCompletionHandler:": ("GKAchievement",),
    "reportScoreWithCompletionHandler:": ("GKScore",),
    "setCategory:": ("GKLeaderboardViewController",),
    "setLeaderboardDelegate:": ("GKLeaderboardViewController",),
    "setPercentComplete:": ("GKAchievement",),
    "setTimeScope:": ("GKLeaderboardViewController",),
    "transactionReceipt": ("SKPaymentTransaction",),
    "transactionState": ("SKPaymentTransaction",),
    "identifier": ("GKAchievement",),
    # UIKit selectors that only Chartboost's classes implement among the image's
    "name": ("UIDevice",),
    "setTag:": ("UIView",),
    "tag": ("UIView",),
    "view": ("UITouch",),
    # -[InAppPurchaseManager failedTransaction:] tells its delegate, which the game never sets
    # the engine's Chartboost calls go to nil (+[Chartboost sharedChartboost] -> nil, ADR 0026)
    "setAppId:": (),
    "setAppSignature:": (),
    "showInterstitial": (),
    "startSession": (),
}

# Which issue's area a class belongs to, by the framework #11 binds it to.
AREAS = {
    "Foundation": "Foundation",
    "CoreFoundation": "Foundation",
    "UIKit": "UIKit",
    "QuartzCore": "UIKit",
    "OpenGLES": "OpenGLES",
    "AVFoundation": "AVFoundation",
    "GameKit": "GameKit",
    "StoreKit": "StoreKit",
    "MediaPlayer": "MediaPlayer",
}
# Classes the image never names (only untyped sends reach them) and their framework.
UNNAMED_CLASSES = {
    "NSLocale": "Foundation",
    "UITouch": "UIKit",
    "UIWindow": "UIKit",
    "SKPaymentTransaction": "StoreKit",
    "SKProductsResponse": "StoreKit",
}
# Selectors of one class implemented by another area: NSString's UIKit additions (#51).
AREA_OVERRIDES = {
    ("NSString", "drawAtPoint:withFont:"): "UIKit",
    ("NSString", "sizeWithFont:"): "UIKit",
}
OWNERS = {
    "Foundation": "#49",
    "UIKit": "#54 (drawing: #51)",
    "OpenGLES": "#117",
    "AVFoundation": "#117 (silent; sound: M6 #63)",
    "GameKit": "#52",
    "StoreKit": "#52",
    "MediaPlayer": "dead code",
}


@dataclass(frozen=True, order=True)
class Row:
    area: str
    cls: str
    selector: str  # "+name" or "-name"
    how: str  # "typed" or "untyped"
    sends: int


def is_dead(name: str) -> bool:
    return name in DEAD_FUNCTIONS or name.startswith(DEAD_PREFIXES)


def app_class_selectors(objc_doc: dict) -> dict[str, tuple[str | None, set[str]]]:
    """Each app class's framework base class (its first non-app ancestor) and its instance
    selectors, its app superclasses' included."""
    classes = {c["name"]: c for c in objc_doc["classes"] if c["group"] == "app"}
    out: dict[str, tuple[str | None, set[str]]] = {}
    for name in classes:
        sels: set[str] = set()
        seen: set[str] = set()
        base = None
        cls = classes.get(name)
        while cls is not None and cls["name"] not in seen:
            seen.add(cls["name"])
            sels.update(m["selector"] for m in cls["instance_methods"])
            parent = cls.get("superclass")
            base = parent.get("name") if isinstance(parent, dict) else None
            cls = classes.get(base)
        out[name] = (base, sels)
    return out


def sending_class(function_name: str) -> str | None:
    """The class of an instance method name ``-[Class selector]``, else None."""
    if function_name.startswith("-[") and " " in function_name:
        return function_name[2:].split(" ", 1)[0]
    return None


def app_selectors(objc_doc: dict) -> set[str]:
    """Selectors an app class or a category of the image implements (``objc.json``)."""
    out: set[str] = set()
    for c in objc_doc["classes"]:
        if c["group"] == "app":
            out.update(m["selector"] for m in c["instance_methods"] + c["class_methods"])
    for c in objc_doc["categories"]:
        out.update(m["selector"] for m in c["instance_methods"] + c["class_methods"])
    return out


def surface(doc: dict, objc_doc: dict) -> list[Row]:
    """The rows of ``imports.json``'s game sends (see the module docstring)."""
    app = app_selectors(objc_doc)
    own = app_class_selectors(objc_doc)
    frameworks = {
        i["name"][len(CLASS_SYMBOL) :]: i["framework"]
        for i in doc["imports"]
        if i["name"].startswith(CLASS_SYMBOL)
    }
    frameworks.update(UNNAMED_CLASSES)
    names = {f["function"]: f["name"] for f in doc["functions"]}
    typed: Counter = Counter()
    untyped: Counter = Counter()
    for s in doc["sends"]:
        sel = s["selector"]
        if s["class"] != "game" or sel is None or is_dead(names.get(s["function"], "")):
            continue
        if s["implemented_by"] is not None and not s["in_image"]:
            base, sels = own.get(sending_class(names.get(s["function"], "")) or "", (None, set()))
            if s.get("how") == "init" and s.get("receiver") == base and sel in sels:
                continue  # [self sel] after self = [super init...]: the app's own method
            typed[(s["implemented_by"], ("+" if s["class_method"] else "-") + sel)] += 1
        elif s["implemented_by"] is None and (not s["in_image"] or sel not in app):
            untyped[sel] += 1
    missing = sorted(set(untyped) - set(RECEIVERS))
    if missing:
        raise ValueError(
            "untyped game selectors without a receiver in hostsurface.RECEIVERS: "
            + ", ".join(missing)
        )
    unused = sorted(set(RECEIVERS) - set(untyped))
    if unused:
        raise ValueError(
            "hostsurface.RECEIVERS lists selectors no live game code sends untyped: "
            + ", ".join(unused)
        )
    rows: dict[tuple[str, str], Row] = {}

    def add(cls: str, selector: str, how: str, sends: int) -> None:
        if cls not in frameworks:
            raise ValueError(f"no framework known for class {cls}")
        area = AREA_OVERRIDES.get((cls, selector[1:]), AREAS[frameworks[cls]])
        old = rows.get((cls, selector))
        if old is not None:
            how = old.how if old.how == how else "typed, untyped"
            sends += old.sends
        rows[(cls, selector)] = Row(area, cls, selector, how, sends)

    for (cls, selector), n in typed.items():
        add(cls, selector, "typed", n)
    for sel, n in untyped.items():
        for receiver in RECEIVERS[sel]:
            cls, prefix = (receiver[1:], "+") if receiver.startswith("+") else (receiver, "-")
            add(cls, prefix + sel, "untyped", n)
    return sorted(rows.values(), key=lambda r: (r.area, r.cls, r.selector))


def to_markdown(rows: list[Row]) -> str:
    areas = Counter(r.area for r in rows)
    classes = {a: len({r.cls for r in rows if r.area == a}) for a in areas}
    out = [
        "# Host surface",
        "",
        "<!-- Generated by `python3 -m mtport.hostsurface generate` (tools/mtport/hostsurface.py)"
        " from `work/re/imports.json`. Do not edit by hand: rerun the generator. -->",
        "",
        "Every (class, selector) that game code sends to a framework class: what the host "
        "classes must implement ([ADR 0026](../adr/0026-objective-c-strategy.md)). Sends from "
        "Chartboost and from dead code ([objc-shell.md](objc-shell.md#how-the-shell-works)) "
        "are left out. *typed*: [#11](imports.md#objective-c-messages) knows the receiving "
        "class (or the framework class whose method an image class's send reaches); "
        "*untyped*: the receiver is unknown and no app class or category implements the "
        "selector, so `mtport.hostsurface.RECEIVERS` names the receiving class by hand. "
        "*Sends* counts the call sites.",
        "",
        "`objc.host_surface` (tests/cpp/host_surface_test.cpp) looks every row of the areas it "
        "checks up in the runtime's registered host classes.",
        "",
        "| Area | Classes | Selectors | Implemented by |",
        "|---|---|---|---|",
    ]
    for a in sorted(areas):
        out.append(f"| {a} | {classes[a]} | {areas[a]} | {OWNERS.get(a, '?')} |")
    for a in sorted(areas):
        out += ["", f"## {a}", "", "| Class | Selector | Sends | Receiver |", "|---|---|---|---|"]
        for r in rows:
            if r.area == a:
                out.append(f"| `{r.cls}` | `{r.selector}` | {r.sends} | {r.how} |")
    return "\n".join(out) + "\n"


def generate(repo: Path) -> list[Row]:
    from mtport import ghidra_common as gc

    doc = json.loads((repo / IMPORTS_JSON).read_text(encoding="utf-8"))
    objc_doc = json.loads((repo / OBJC_JSON).read_text(encoding="utf-8"))
    rows = surface(doc, objc_doc)
    gc.write_outputs(repo, {MARKDOWN_OUT: to_markdown(rows)})
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.hostsurface", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    gen = sub.add_parser(
        "generate", help=f"write {MARKDOWN_OUT} from {IMPORTS_JSON} and {OBJC_JSON}"
    )
    gen.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    try:
        rows = generate(args.repo)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        if isinstance(e, FileNotFoundError):
            print(
                "run python3 -m mtport.objc generate and python3 -m mtport.imports generate first",
                file=sys.stderr,
            )
        return 1
    for area, n in sorted(Counter(r.area for r in rows).items()):
        print(f"  {area:<14} {n} selectors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
