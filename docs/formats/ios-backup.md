# iOS device backups (`Manifest.db`, `Manifest.mbdb`)

A Finder, iTunes or Apple Devices backup of an iPhone or iPod touch is a folder of files named by hash, plus a manifest that maps each hash to the app (the **domain**) and path it came from. This is the only way to get the game's save files off a device that isn't jailbroken: the game's `Info.plist` has no `UIFileSharingEnabled`, so neither the Files app nor iTunes file sharing can see its `Documents/` ([save.md](save.md)). This spec covers what the port reads from a backup: Apple's format, not the game's. How to make a backup and import it is in [saves/ios-backup.md](../saves/ios-backup.md).

- **Parser:** [`tools/mtport/iosbackup.py`](../../tools/mtport/iosbackup.py) (#149). It finds the game's `Documents/` files, checks them with [`savecheck.py`](../../tools/mtport/savecheck.py), and hands them to the port's import (`MapleThief --import-saves`, [ADR 0078](../adr/0078-save-export-import-backups-and-checks.md)). It also writes synthetic backups for the tests (`encode_mbdb`, `write_synthetic_backup`).
- **Tests:** [`tests/test_iosbackup.py`](../../tests/test_iosbackup.py) (CI) on synthetic backups of both manifest formats: encrypted, hostile and incomplete backups, plus an mbdb round-trip, a byte layout check and a fuzz test. CTest `saves.ios_backup_import` imports both formats through the real program. No real backup is in the repository. A local "parse every shipped file" test doesn't apply, because the IPA holds no backup.
- **Status:** specified from Apple's published behaviour and the open-source readers (libimobiledevice's `idevicebackup2`, iphone-backup tools). Every rule below has a synthetic test. **No real backup has been read yet** (#149, `needs-human`).

## The folder

| File | Since | What the port uses |
|---|---|---|
| `Manifest.plist` | iOS 4 | `IsEncrypted` (bool), `Applications` (a dict keyed by bundle id: is the game in this backup?), `Lockdown.DeviceName`, `Lockdown.ProductVersion`, `Date`. Binary or XML plist. Encrypted backups also hold `BackupKeyBag` and, from iOS 10.2, `ManifestKey`. |
| `Info.plist` | iOS 4 | `Device Name`, `Product Version`, `Last Backup Date` (only to list backups). It can be several MB, because it holds app icons. |
| `Status.plist` | iOS 5 | `SnapshotState`: `"finished"` once the backup completed. Anything else is reported as a warning. |
| `Manifest.db` | iOS 10 | SQLite: the file list (below). |
| `Manifest.mbdb` | iOS 5 (iOS 4 with `Manifest.mbdx`) | the file list in binary records (below). Replaced by `Manifest.db` in iOS 10. |
| `<id[:2]>/<id>` | iOS 10 | a file's contents, in a folder named by the first two hex digits of its ID |
| `<id>` | iOS 4–9 | a file's contents, flat in the backup folder |
| `*.mdinfo`, `*.mddata` | iOS ≤ 3 | the older format, not read (the tool says so) |

When both manifests are present, `Manifest.db` wins.

**The file ID** of every file is the SHA-1, in lowercase hex, of `<domain>-<relativePath>` in UTF-8. An app's files are in the domain `AppDomain-<bundle id>`, and their paths are relative to the app's home. The game's bundle id is `com.nexonmobile.maplestory.thief` ([ANALYSIS.md](../ANALYSIS.md)). Its saves are therefore in `AppDomain-com.nexonmobile.maplestory.thief` under `Documents/`:

| Path | File ID |
|---|---|
| `Documents/theif0` | `cae5c77bdc2c044c07abbbe215c128fd70720274` |
| `Documents/quest0` | `45a129dc684a02611475f7c541e2038958826ced` |
| `Documents/config` | `597f7576d892c5eb53ae7becab90b5cf08f96b1f` |

So on iOS 10+, `theif0` is at `ca/cae5c77bdc2c044c07abbbe215c128fd70720274`. Its bytes are the file as the game wrote it, unchanged (unencrypted backups store contents as they are).

## `Manifest.db` (iOS 10+)

An SQLite 3 database, starting with `SQLite format 3\0`:

```sql
CREATE TABLE Files (fileID TEXT PRIMARY KEY, domain TEXT, relativePath TEXT, flags INTEGER, file BLOB);
CREATE TABLE Properties (key TEXT PRIMARY KEY, value BLOB);
-- plus indexes on Files(domain), Files(relativePath) and Files(flags)
```

| Column | Meaning |
|---|---|
| `fileID` | the file ID above (40 hex digits) |
| `domain` | `AppDomain-<bundle id>`, `HomeDomain`, `CameraRollDomain`, … |
| `relativePath` | the path within the domain; `""` is the domain's root |
| `flags` | 1 file, 2 folder, 4 symbolic link |
| `file` | an `NSKeyedArchiver` binary plist of an `MBFile` (`Size`, `Mode`, `UserID`, `GroupID`, `LastModified`, `ProtectionClass`, …). Not read by the port. |

The port opens it read-only and immutable (`file:…?mode=ro&immutable=1`, with `query_only` on and `trusted_schema` off), so it never writes a journal into the backup. It reads only `SELECT fileID, relativePath, flags FROM Files WHERE domain = ?`.

**Encrypted backups:** since iOS 10.2 `Manifest.db` is itself encrypted with `ManifestKey`, so the file doesn't start with the SQLite header. The port treats that as an encrypted backup.

## `Manifest.mbdb` (iOS 4–9)

A 6-byte header, `6D 62 64 62 05 00` (`"mbdb"`, version 5.0; iOS 4's is read as 4.0), then records back to back until the end of the file. There is no count and no index: iOS 4's `Manifest.mbdx` index isn't needed, because the file ID can be computed from the record. All integers are **big-endian**.

A **string** is a `u16` length followed by that many bytes (UTF-8 for text). The length `FF FF` means "no string", which reads as empty.

| Field | Type | Meaning |
|---|---|---|
| domain | string | `AppDomain-com.nexonmobile.maplestory.thief`, … |
| path | string | relative path (no string for the domain's root) |
| link target | string | a symbolic link's target, else none |
| data hash | string | 20 bytes: the SHA-1 of the contents (none for folders) |
| encryption key | string | none in an unencrypted backup; a wrapped key (about 40 bytes) in an encrypted one |
| mode | u16 | `st_mode`: `0x8000` file, `0x4000` folder, `0xA000` link, plus the permission bits |
| inode | u64 | |
| user id, group id | u32 ×2 | 501 (`mobile`) |
| modified, accessed, created | u32 ×3 | Unix seconds |
| size | u64 | the contents' size in bytes |
| protection class | u8 | data protection class (0 for folders) |
| property count | u8 | |
| properties | count × (string name, string value) | extended attributes; the values are bytes |

The record of `Documents/config` (a 6-byte file) in a synthetic backup:

```
0000  6d 62 64 62 05 00 00 2a 41 70 70 44 6f 6d 61 69   mbdb..."AppDomai
0010  6e 2d 63 6f 6d 2e 6e 65 78 6f 6e 6d 6f 62 69 6c   n-com.nexonmobil
0020  65 2e 6d 61 70 6c 65 73 74 6f 72 79 2e 74 68 69   e.maplestory.thi
0030  65 66 00 10 44 6f 63 75 6d 65 6e 74 73 2f 63 6f   ef..Documents/co
0040  6e 66 69 67 ff ff 00 14 00 00 00 00 00 00 00 00   nfig.... (no link) hash...
0050  00 00 00 00 00 00 00 00 00 00 00 00 ff ff 81 a4   ...(no key) mode 0o100644
0060  00 00 00 00 00 00 00 09 00 00 01 f5 00 00 01 f5   inode 9, uid 501, gid 501
0070  53 8b 16 40 53 8b 16 40 53 8b 16 40 00 00 00 00   three times, size...
0080  00 00 00 06 03 00                                 ...6, class 3, 0 properties
```

**Encrypted backups:** `Manifest.mbdb` itself stays readable, but each file record carries an encryption key and the contents are AES-encrypted. The port treats a game file with a key as an encrypted backup.

## What the port trusts

Nothing. A backup comes from outside, so the parser:

- refuses an encrypted backup (`IsEncrypted`, a `Manifest.db` that isn't SQLite, or a record with an encryption key), and says how to make an unencrypted one;
- writes only names from the game's own list (`savecheck.is_game_file`: `theif0`–`2`, `quest0`–`2`, `config`, `cupon`, `CTerms`, the receipt lists and the port's `maplethief-cashshop<n>`), taken from a `relativePath` that is exactly `Documents/<name>`. Any other path, including one with `..`, a subfolder or a backslash, is listed as ignored and never becomes a name;
- requires a file ID of exactly 40 lowercase hex digits, and refuses a data file (or its two-digit folder) that is a link, isn't a regular file, is over 16 MiB (the import's cap), or (mbdb) doesn't match the record's size;
- refuses a game file listed twice, or listed as a folder or link;
- bounds-checks every mbdb read (a cut-short or corrupted file gives an error naming the byte, and no other kind of exception), caps `Manifest.mbdb` at 512 MiB and each plist at 64 MiB, and reads plists with `plistlib` (XML entity declarations are refused);
- then checks the set with `savecheck` (the same rules as the program's `mt_saves_check`), and the program's `--import-saves` checks it again before anything replaces a save.
