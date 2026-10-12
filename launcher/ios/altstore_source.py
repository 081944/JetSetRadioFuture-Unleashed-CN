#!/usr/bin/env python3
"""
Put a released .ipa into the AltStore / SideStore source (altstore/source.json).

    launcher/ios/altstore_source.py <the .ipa> [--notes "What changed"] [--date 2026-10-12T16:05:25Z]

The version, build and minimum iOS come from the app's own Info.plist inside
the .ipa, the size from the file, and the download link is the release
asset's: .../releases/download/v<version>/JSRF-Unleashed-iOS.ipa -- so run it
for the .ipa that is attached to that release, then commit the source. The
newest version goes first (AltStore takes the first entry as the latest);
an entry for the same version is replaced, not repeated.

A source is how AltStore and SideStore find an app and its updates: players
add its URL once (Sources > +) and install from there.
"""
import argparse
import datetime
import json
import os
import plistlib
import sys
import zipfile

REPO = "JuMatt/JetSetRadioFuture-Unleashed"
ASSET = "JSRF-Unleashed-iOS.ipa"
HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "altstore", "source.json")


def ipa_info(path):
    with zipfile.ZipFile(path) as z:
        plists = [n for n in z.namelist()
                  if n.startswith("Payload/") and n.endswith(".app/Info.plist") and n.count("/") == 2]
        if not plists:
            sys.exit("no Payload/<app>.app/Info.plist in " + path)
        return plistlib.loads(z.read(plists[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ipa")
    ap.add_argument("--notes", default="", help="what changed, shown in AltStore")
    ap.add_argument("--date", default="", help="ISO 8601; default now (UTC)")
    a = ap.parse_args()

    info = ipa_info(a.ipa)
    version = info["CFBundleShortVersionString"]
    entry = {
        "version": version,
        "buildVersion": info["CFBundleVersion"],
        "date": a.date or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "localizedDescription": a.notes or "JSRF Unleashed " + version + ".",
        "downloadURL": "https://github.com/%s/releases/download/v%s/%s" % (REPO, version, ASSET),
        "size": os.path.getsize(a.ipa),
        "minOSVersion": info.get("MinimumOSVersion", "15.0"),
    }
    with open(SOURCE) as f:
        src = json.load(f)
    app = next(x for x in src["apps"] if x["bundleIdentifier"] == info["CFBundleIdentifier"])
    app["versions"] = [entry] + [v for v in app["versions"] if v["version"] != version]
    with open(SOURCE, "w") as f:
        json.dump(src, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("source: %s %s (%d bytes) -> %s" % (app["name"], version, entry["size"], entry["downloadURL"]))


if __name__ == "__main__":
    main()
