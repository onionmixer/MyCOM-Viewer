# MYCOM archive builder and viewer

This project has two executable programs:

```text
MYCOM.ISO (read only)
  -> mycom-archive-build
  -> archive directory (normalized source + converted content + manifest)
  -> mycom-viewer
```

`mycom-archive-build` is the only data-processing command. It calls an
installed `7z` executable to read the ISO, normalizes the extracted files,
parses the MVB/DBF data, and writes portable content for the Qt5 viewer. `7z`
is an external tool dependency, not a separate project or manual stage.

## Build

```bash
cmake -S . -B build
cmake --build build
```

Qt 5 Core, Widgets, Multimedia, and MultimediaWidgets (5.12 or later) and a
C++17 compiler are required. The archive builder also requires `7z` to be on
`PATH`, or supplied with `--seven-zip`.

## Create an archive

The destination must be new or empty. The ISO is never modified.

```bash
./build/mycom-archive-build ../MYCOM.ISO ./mycom-archive
```

If `7z` has a nonstandard path:

```bash
./build/mycom-archive-build --seven-zip /path/to/7z ../MYCOM.ISO ./mycom-archive
```

`--min-bytes N` remains available for diagnostic/smoke conversions. Do not use
an elevated value for the normal archive because it intentionally omits short
recovered text runs.

The resulting directory has a stable contract:

```text
mycom-archive/
  manifest.json                 # mycom-archive/v1 registration and validation data
  normalized/
    mvb/                         # canonical uppercase MVB names
    dbf/MYDBF01.DBF
    assets/{bmp,wav,myavi}/      # canonical lowercase paths
  content/
    *.json                       # mycom-mvb-salvage/v1 book data
    *.html and topics/            # only when --review-html / --topic-pages are requested
```

`manifest.json` records the ISO identity, normalized source paths, all MVB and
asset files, DBF record count, every converted book, and the ISO SHA-256. Each
of the normalized MVB, DBF, BMP, WAV, and AVI files is also listed with its
canonical path, byte size, and SHA-256 in `normalized.files`. The viewer
accepts schema version 1 and uses this manifest rather than guessing a directory layout. Content pages refer to the
normalized assets inside the same archive, so assets are not duplicated under
`content/`.

To replace a valid archive after changing the builder, use the guarded rebuild
option. It refuses to delete a directory unless it contains a valid MYCOM
archive manifest.

```bash
./build/mycom-archive-build --rebuild ../MYCOM.ISO ./mycom-archive
```

The Qt5 viewer needs only the JSON content. Optional static review pages can be
generated when needed:

```bash
./build/mycom-archive-build --review-html --topic-pages ../MYCOM.ISO ./mycom-archive-html
```

The MVB conversion retains CP949 text fragments, Viewer navigation macros,
image/media references, original `|CATALOG` offsets, original `|TOPIC` header
chains, topic titles, and bounded body previews. For HEADA, article boundaries
are cross-checked against the 2,010-record DBF catalog. Five catalog-only DBF
entries have no independent MVB topic/body marker and deliberately remain
catalog-only: `92120690`, `94023200`, `9410080_`, `95113560`, and `95124060`.

## Open an archive

```bash
./build/mycom-viewer ./mycom-archive
```

The viewer accepts only a directory containing a valid `mycom-archive/v1`
manifest and verifies its content and normalized asset directories before
loading. It provides article and recovered-text browsing, archive search,
case-insensitive in-content search/highlighting, adjustable content font and
zoom, bookmarks, and Qt Multimedia playback for available WAV/AVI files.

Bookmarks and the chosen content font are stored in the user's
`mycom-viewer.ini`, separately from the archive itself.

## Convenience launcher

`run_mycom_viewer.sh` builds the project, creates `mycom_archive/` when its
manifest is absent, then opens that archive. Configure `ISO_PATH` in the script
if the ISO is stored elsewhere.

## Verification

```bash
ctest --test-dir build --output-on-failure
QT_QPA_PLATFORM=offscreen timeout 5s ./build/mycom-viewer ./mycom-archive
```

The automated tests cover bookmark persistence and manifest format/path
validation. A full ISO build additionally validates the normalized MVB count,
asset count, DBF records, converted-book count, and SHA-256 fields in
`manifest.json`. Set `MYCOM_ISO` to enable it in CTest/CI:

```bash
MYCOM_ISO=/absolute/path/to/MYCOM.ISO ctest --test-dir build -L iso --output-on-failure
```

Without `MYCOM_ISO`, the labeled integration test is reported as skipped and
the normal unit test suite remains runnable without the proprietary ISO.
