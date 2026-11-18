#!/bin/sh
# Archives, signs with Developer ID (Xcode cloud signing), notarizes and installs
# "Xmac NTFS.app" into /Applications. Removes the intermediate copies afterwards: duplicate
# registrations of the same bundle id stop FSKit from enabling the module.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Volumes/XcodeExternal/Applications/Xcode.app/Contents/Developer}
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
WORK=$(mktemp -d -t xmac-ntfs-release)
DERIVED="$WORK/derived"
trap 'rm -rf "$WORK"' EXIT

cd "$ROOT"
xcodegen generate -q
cat > "$WORK/ExportOptions.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>method</key><string>developer-id</string>
<key>teamID</key><string>4DU4BTV46U</string>
<key>signingStyle</key><string>automatic</string>
<key>destination</key><string>upload</string>
</dict></plist>
EOF
xcodebuild -quiet -project XmacNTFS.xcodeproj -scheme XmacNTFS -configuration Release \
  -derivedDataPath "$DERIVED" -archivePath "$WORK/XmacNTFS.xcarchive" -allowProvisioningUpdates archive
xcodebuild -quiet -exportArchive -archivePath "$WORK/XmacNTFS.xcarchive" \
  -exportOptionsPlist "$WORK/ExportOptions.plist" -exportPath "$WORK/upload" -allowProvisioningUpdates
echo "Uploaded; waiting for notarization..."
i=0
until xcodebuild -exportNotarizedApp -archivePath "$WORK/XmacNTFS.xcarchive" -exportPath "$WORK/notarized" >/dev/null 2>&1; do
  i=$((i + 1)); [ "$i" -lt 60 ] || { echo "notarization timed out" >&2; exit 1; }
  sleep 15
done
spctl -a "$WORK/notarized/XmacNTFS.app"

for app in "$WORK"/derived/Build/Intermediates.noindex/ArchiveIntermediates/*/InstallationBuildProductsLocation/Applications/*.app \
           "$WORK"/XmacNTFS.xcarchive/Products/Applications/*.app "$WORK"/XmacNTFS.xcarchive/Submissions/*/*.app \
           "$WORK"/upload/*.app "$WORK"/notarized/*.app; do
  [ -e "$app" ] && "$LSREGISTER" -u "$app" 2>/dev/null || true
done
rm -rf "/Applications/Xmac NTFS.app"
ditto "$WORK/notarized/XmacNTFS.app" "/Applications/Xmac NTFS.app"
"$LSREGISTER" -f -R "/Applications/Xmac NTFS.app"
open "/Applications/Xmac NTFS.app"
echo "Installed: $(codesign -dvv "/Applications/Xmac NTFS.app" 2>&1 | grep -m1 '^Authority=')"
