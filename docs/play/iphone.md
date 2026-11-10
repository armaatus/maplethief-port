# Playing on your iPhone

The port runs on an iPhone with iOS 16 or later, as an app you build yourself and install from a Mac with Xcode. This is called sideloading. There is no App Store version and no download for iOS.

> **Checked on one iPhone so far.** On an iPhone 12 with iOS 26.6.1 the app installs, starts and reaches the first map by touch ([#83](https://github.com/armaatus/maplethief-port/issues/83)). If you try another model or iOS version, please tell us how it goes.

## What you need

- **A Mac with Xcode** from the App Store. The Command Line Tools alone are not enough. Open Xcode once so it can finish installing, and add iOS under Xcode > Settings > Components if it asks.
- **Your own copy of the game:** the file `MapleThief-1.5.ipa`, in `original/` inside this repository. We can't give it to you. [Your game file](your-ipa.md) explains which file it has to be.
- **A checkout of this repository**, and the tools in the [build guide](../build/README.md): CMake and Python.
- **An Apple account.** A free one works; a paid developer account is more convenient.
- **Your iPhone and a cable**, the first time.

| | Free Apple account | Paid developer account |
|---|---|---|
| Cost | Free | Yearly fee |
| App keeps working for | 7 days, then build and install it again | 1 year |
| Apps installed this way at once | 3 | No limit that matters here |
| Bundle identifier | Usually needs one of your own (below) | Works as it is |

## 1. Prepare the Mac

Tell the Mac to use Xcode's tools, once:

```sh
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

Add your Apple account in Xcode: Xcode > Settings > Accounts, then the **+** button. Your **team id** is the ten letters and digits shown there as the team's id, for example `ABCDE12345`. If Xcode doesn't show one for a free account, skip the team here and pick your team in step 3 instead.

`scripts/build doctor` lists what's still missing and how to fix it.

## 2. Build the app

From the repository's folder, with your team id:

```sh
MAPLETHIEF_IOS_TEAM=ABCDE12345 scripts/build ios
```

With a free account also give the app a bundle identifier of your own, since the default one can only be registered by one account:

```sh
MAPLETHIEF_IOS_TEAM=ABCDE12345 MAPLETHIEF_IOS_BUNDLE_ID=nl.yourname.maplethief scripts/build ios
```

This extracts your IPA, recompiles the game and builds the app for iOS. The first run takes a while. The game's files from your IPA are copied into the app here, on your own Mac. They never go anywhere else, so keep the app to your own devices.

Without a team id the build still finishes, unsigned. Your iPhone won't install that app, so the command tells you what to do next.

## 3. Install it with Xcode

1. Connect your iPhone with a cable, unlock it, and tap **Trust** if it asks.
2. Turn on **Developer Mode** (iOS 16 and later): Settings > Privacy & Security > Developer Mode, then restart the phone when it asks.
3. Open the project Xcode can run:

   ```sh
   open build/ios-app/MapleThief.xcodeproj
   ```

4. In the bar at the top, choose the **MapleThief** scheme and your iPhone as the destination.
5. Click the **MapleThief** project in the sidebar, then the **MapleThief** target. On the **Signing & Capabilities** tab make sure **Automatically manage signing** is on and your team is selected. If Xcode reports a problem with the bundle identifier, use your own as in step 2.
6. Press **Run** (the triangle).

With a free account, the first start fails with a message that the developer isn't trusted. On the iPhone open Settings > General > VPN & Device Management, tap your account under Developer App and choose **Trust**. Then press Run again.

After a build with a free account you have 7 days. When the app stops opening, build and install it again. Your saves stay on the phone as long as you don't delete the app.

## Saves

The game keeps its saves in the app's own folder. To copy them off the phone or back onto it, use the Files app (On My iPhone > MapleThief) or the Finder's file sharing. To bring your saves over from the original game, see [Saves from your iPhone](iphone-saves.md).

## Playing

The game runs in landscape, either way round, and the status bar stays hidden. You can play with the on-screen key pad or with a controller paired in the iPhone's Bluetooth settings. See [Controls](controls.md).

## Sound

The game shares the phone's sound as the original did: music from another app keeps playing with it, and the Ring/Silent switch silences the game. To hear the game with the switch on silent, set **Audio session** to `own` in the settings menu's Audio tab and start the game again. Other apps' music is then turned down while it runs.

A call, Siri or an alarm stops the sound, and it comes back when they're done. None of this has been checked on a real iPhone yet ([#197](https://github.com/armaatus/maplethief-port/issues/197)).

## When something goes wrong

- **`Xcode isn't installed or selected`:** do step 1, and check that Xcode opened once.
- **`Xcode has no iOS SDK`:** Xcode > Settings > Components, add iOS.
- **`Your team has no devices` in step 2:** a new Apple account has no registered iPhone yet. Connect yours and press Run once as in step 3, which registers it, then run the command from step 2 again.
- **A signing error in Xcode:** check that your Apple account is added and your team is selected. With a free account, use a bundle identifier of your own, and remove an older app of yours if you already have three.
- **The iPhone refuses the app:** turn on Developer Mode, and trust your account under VPN & Device Management.

For anything else, see [Troubleshooting](troubleshooting.md).
