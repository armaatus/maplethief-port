# Credits

The game has no staff credits: no screen, image, table, or script lists the people who made it. What it says about its makers is on the main menu and its About screen ([re/objc-shell.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/objc-shell.md), `-[MainMenuView clickAbout:]` `0xA8A0D`).

## About screen

The About button of the main menu opens a scrolling panel with:

- a heading and one paragraph that describe the game as a side-scrolling role-playing game with world maps and quests;
- a support line that points to the mobile games page of Nexon America's MapleStory site;
- the homepages of Nexon Mobile and Nexon America, and the homepage of the online MapleStory;
- four link buttons that open those addresses in Safari, and a build label button (a format with a number and the letters NR).

The addresses are listed on the [Online features](about/online.html#links) page; all of them are dead links today ([re/network.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/network.md#urls)).

## Copyright line and logos

- **Copyright line.** The main menu shows an image (`MainMenuCopyRight.png`) with a copyright notice for NEXON Korea Corp. and NEXON America Inc., dated 2012.
- **Launch image.** iOS shows `Default.png`, the Nexon logo, while the game starts. The game itself never draws it.
- **Company logo.** `MainMenuCompanyLogo.png`, a Nexon Mobile logo, ships in the bundle, but the main menu's `companyLogo` view is never given an image ([Unused content](about/unused.html#unused-root-images)).

## Help

The Help button shows one picture page in English and two in Korean (`MainMenuHelp.png`; `MainMenuHelp_kor.png` and `MainMenuHelpGradeInfo.png`). They explain the controls; they name no makers.
