#!/usr/bin/env bash
#
# FretFight вЂ” local macOS build (JUCE 9.0.2).
#
# Replaces the GitHub Actions macOS job: builds a universal (arm64 + x86_64)
# Release bundle, re-brands it to FretFight, ad-hoc signs it and produces
# FretFight-<version>.dmg + FretFight-<version>.zip in docs/Output.
#
# The generated Xcode project references JUCE through ~/JUCE/modules, so JUCE
# must live there (the script clones tag 9.0.2 on the first run).
#
# Usage: see --help, or docs/BUILD_MACOS.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

JUCE_VERSION_PINNED="9.0.2"
JUCE_DIR="${JUCE_DIR:-$HOME/JUCE}"
OUT_DIR="$ROOT/docs/Output"
BUILD_DIR="$ROOT/.build/macos"
CONFIGURATION="Release"
ARCHS=""
VERSION=""
DO_RESAVE=1
DO_PACKAGE=1
PACKAGE_EXPLICIT=0
REBUILD_PROJUCER=0
ALLOW_JUCE_VERSION=0
VERBOSE=0

if [ -t 1 ]; then
  C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'; C_RED=$'\033[31m'
  C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RESET=$'\033[0m'
else
  C_BOLD=''; C_DIM=''; C_RED=''; C_GREEN=''; C_YELLOW=''; C_RESET=''
fi

info()  { printf '%s==>%s %s\n' "$C_BOLD" "$C_RESET" "$*"; }
ok()    { printf '%s  ok%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn()  { printf '%swarning:%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
die()   { printf '%serror:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
FretFight вЂ” Р»РѕРєР°Р»СЊРЅР°СЏ СЃР±РѕСЂРєР° РґР»СЏ macOS (JUCE 9.0.2)

РСЃРїРѕР»СЊР·РѕРІР°РЅРёРµ:
  GuitarBattle/scripts/build-macos.sh [РѕРїС†РёРё]

РћРїС†РёРё:
  --juce-dir DIR        РєР°С‚Р°Р»РѕРі JUCE 9.0.2 (РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ ~/JUCE)
  --out-dir DIR         РєСѓРґР° РїРѕР»РѕР¶РёС‚СЊ DMG/ZIP (РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ docs/Output)
  --build-dir DIR       РїСЂРѕРјРµР¶СѓС‚РѕС‡РЅС‹Рµ С„Р°Р№Р»С‹ Рё Р»РѕРіРё (РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ .build/macos)
  --version VER         РІРµСЂСЃРёСЏ Р°СЂС‚РµС„Р°РєС‚РѕРІ (РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ РёР· GuitarBattle.jucer)
  --debug               СЃРѕР±СЂР°С‚СЊ Debug (РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ Р±РµР· СѓРїР°РєРѕРІРєРё)
  --package             СѓРїР°РєРѕРІР°С‚СЊ РґР°Р¶Рµ Debug
  --no-package          С‚РѕР»СЊРєРѕ СЃРѕР±СЂР°С‚СЊ, Р±РµР· DMG/ZIP
  --no-resave           РЅРµ РїРµСЂРµРіРµРЅРµСЂРёСЂРѕРІР°С‚СЊ Xcode-РїСЂРѕРµРєС‚ С‡РµСЂРµР· Projucer
  --rebuild-projucer    РїРµСЂРµСЃРѕР±СЂР°С‚СЊ Projucer, РґР°Р¶Рµ РµСЃР»Рё Р±РёРЅР°СЂРЅРёРє СѓР¶Рµ РµСЃС‚СЊ
  --archs "arm64 x86_64"  Р°СЂС…РёС‚РµРєС‚СѓСЂС‹ (Release РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ universal)
  --allow-juce-version  РЅРµ РїР°РґР°С‚СЊ, РµСЃР»Рё РІРµСЂСЃРёСЏ JUCE РѕС‚Р»РёС‡Р°РµС‚СЃСЏ РѕС‚ 9.0.2
  --verbose             РїРѕРєР°Р·С‹РІР°С‚СЊ РїРѕР»РЅС‹Р№ РІС‹РІРѕРґ xcodebuild
  -h, --help            СЌС‚Р° СЃРїСЂР°РІРєР°

РџСЂРёРјРµСЂС‹:
  ./GuitarBattle/scripts/build-macos.sh
  ./GuitarBattle/scripts/build-macos.sh --debug --no-resave
  ./GuitarBattle/scripts/build-macos.sh --version 1.2.7 --out-dir ~/Desktop
EOF
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "РЅРµ РЅР°Р№РґРµРЅР° РєРѕРјР°РЅРґР° '$1'. $2"
}

run_logged() {
  local name="$1"; shift
  local log="$LOG_DIR/$name.log"

  info "$name"
  if [ "$VERBOSE" -eq 1 ]; then
    "$@" 2>&1 | tee "$log"
  elif ! "$@" >"$log" 2>&1; then
    printf '\n--- РїРѕСЃР»РµРґРЅРёРµ 60 СЃС‚СЂРѕРє %s ---\n' "$log" >&2
    tail -n 60 "$log" >&2
    die "$name Р·Р°РІРµСЂС€РёР»СЃСЏ СЃ РѕС€РёР±РєРѕР№ (РїРѕР»РЅС‹Р№ Р»РѕРі: $log)"
  fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --juce-dir)        [ $# -ge 2 ] || die "--juce-dir С‚СЂРµР±СѓРµС‚ РїСѓС‚СЊ"; JUCE_DIR="$2"; shift 2 ;;
    --out-dir)         [ $# -ge 2 ] || die "--out-dir С‚СЂРµР±СѓРµС‚ РїСѓС‚СЊ"; OUT_DIR="$2"; shift 2 ;;
    --build-dir)       [ $# -ge 2 ] || die "--build-dir С‚СЂРµР±СѓРµС‚ РїСѓС‚СЊ"; BUILD_DIR="$2"; shift 2 ;;
    --version)         [ $# -ge 2 ] || die "--version С‚СЂРµР±СѓРµС‚ Р·РЅР°С‡РµРЅРёРµ"; VERSION="$2"; shift 2 ;;
    --archs)           [ $# -ge 2 ] || die "--archs С‚СЂРµР±СѓРµС‚ Р·РЅР°С‡РµРЅРёРµ"; ARCHS="$2"; shift 2 ;;
    --debug)           CONFIGURATION="Debug"; shift ;;
    --package)         DO_PACKAGE=1; PACKAGE_EXPLICIT=1; shift ;;
    --no-package)      DO_PACKAGE=0; PACKAGE_EXPLICIT=1; shift ;;
    --no-resave)       DO_RESAVE=0; shift ;;
    --rebuild-projucer) REBUILD_PROJUCER=1; shift ;;
    --allow-juce-version) ALLOW_JUCE_VERSION=1; shift ;;
    --verbose)         VERBOSE=1; shift ;;
    -h|--help)         usage; exit 0 ;;
    *)                 die "РЅРµРёР·РІРµСЃС‚РЅР°СЏ РѕРїС†РёСЏ: $1 (СЃРј. --help)" ;;
  esac
done

[ "$(uname -s)" = "Darwin" ] || die "build-macos.sh Р·Р°РїСѓСЃРєР°РµС‚СЃСЏ С‚РѕР»СЊРєРѕ РЅР° macOS"

if [ -z "$ARCHS" ]; then
  if [ "$CONFIGURATION" = "Debug" ]; then
    ARCHS="$(uname -m)"
  else
    ARCHS="arm64 x86_64"
  fi
fi

if [ "$CONFIGURATION" = "Debug" ] && [ "$PACKAGE_EXPLICIT" -eq 0 ]; then
  DO_PACKAGE=0
fi

JUCE_DIR="${JUCE_DIR%/}"
OUT_DIR="${OUT_DIR%/}"
BUILD_DIR="${BUILD_DIR%/}"
LOG_DIR="$BUILD_DIR/logs"
JUCER="$ROOT/GuitarBattle/GuitarBattle.jucer"
XCODE_DIR="$ROOT/GuitarBattle/Builds/MacOSX"
PBXPROJ="$XCODE_DIR/GuitarBattle.xcodeproj/project.pbxproj"
APP_BUILD_DIR="$BUILD_DIR/$CONFIGURATION"
APP_PATH="$APP_BUILD_DIR/GuitarBattle.app"

mkdir -p "$LOG_DIR"

info "FretFight macOS build"
printf '    repo:       %s\n' "$ROOT"
printf '    config:     %s (%s)\n' "$CONFIGURATION" "$ARCHS"
printf '    JUCE:       %s\n' "$JUCE_DIR"
printf '    logs:       %s\n' "$LOG_DIR"

# --- Toolchain ---------------------------------------------------------------

need_cmd xcodebuild "РЈСЃС‚Р°РЅРѕРІРёС‚Рµ Xcode РёР· App Store."
need_cmd xcrun      "РЈСЃС‚Р°РЅРѕРІРёС‚Рµ Xcode РёР· App Store."
need_cmd git        "РЈСЃС‚Р°РЅРѕРІРёС‚Рµ Xcode Command Line Tools: xcode-select --install"
need_cmd ditto      "РЎРёСЃС‚РµРјРЅР°СЏ СѓС‚РёР»РёС‚Р° macOS, РґРѕР»Р¶РЅР° Р±С‹С‚СЊ РІ /usr/bin."
need_cmd hdiutil    "РЎРёСЃС‚РµРјРЅР°СЏ СѓС‚РёР»РёС‚Р° macOS, РґРѕР»Р¶РЅР° Р±С‹С‚СЊ РІ /usr/bin."
need_cmd codesign   "РЎРёСЃС‚РµРјРЅР°СЏ СѓС‚РёР»РёС‚Р° macOS, РґРѕР»Р¶РЅР° Р±С‹С‚СЊ РІ /usr/bin."
need_cmd shasum     "РЎРёСЃС‚РµРјРЅР°СЏ СѓС‚РёР»РёС‚Р° macOS, РґРѕР»Р¶РЅР° Р±С‹С‚СЊ РІ /usr/bin."
[ -x /usr/libexec/PlistBuddy ] || die "РЅРµС‚ /usr/libexec/PlistBuddy (РЅСѓР¶РµРЅ Xcode)."

DEVELOPER_DIR_PATH="$(xcode-select -p 2>/dev/null || true)"
case "$DEVELOPER_DIR_PATH" in
  *.app/Contents/Developer) ;;
  *)
    die "Р°РєС‚РёРІРµРЅ РЅРµ Xcode, Р° '$DEVELOPER_DIR_PATH'.
       РЈСЃС‚Р°РЅРѕРІРёС‚Рµ РїРѕР»РЅС‹Р№ Xcode Рё РІС‹РїРѕР»РЅРёС‚Рµ:
         sudo xcode-select -s /Applications/Xcode.app/Contents/Developer"
    ;;
esac

ok "Xcode $(xcodebuild -version | head -n 1 | sed 's/^Xcode //')"
ok "macOS SDK $(xcrun --sdk macosx --show-sdk-version)"

# --- JUCE --------------------------------------------------------------------

juce_detect_version() {
  local dir="$1" ver=""

  if [ -f "$dir/CMakeLists.txt" ]; then
    ver="$(sed -n 's/^project(JUCE VERSION \([0-9][0-9.]*\).*/\1/p' "$dir/CMakeLists.txt" | head -n 1)"
  fi

  if [ -z "$ver" ] && [ -d "$dir/.git" ]; then
    ver="$(git -C "$dir" describe --tags --always 2>/dev/null || true)"
    ver="${ver#v}"
  fi

  printf '%s' "$ver"
}

ensure_juce() {
  if [ -d "$JUCE_DIR/modules/juce_core" ]; then
    local ver
    ver="$(juce_detect_version "$JUCE_DIR")"
    if [ -n "$ver" ] && [ "$ver" != "$JUCE_VERSION_PINNED" ]; then
      if [ "$ALLOW_JUCE_VERSION" -eq 1 ]; then
        warn "РІ $JUCE_DIR JUCE $ver РІРјРµСЃС‚Рѕ $JUCE_VERSION_PINNED (РїСЂРѕРґРѕР»Р¶Р°СЋ РїРѕ --allow-juce-version)"
      else
        die "РЅСѓР¶РµРЅ JUCE $JUCE_VERSION_PINNED, Р° РІ $JUCE_DIR Р»РµР¶РёС‚ $ver.
       РџРѕСЃС‚Р°РІСЊС‚Рµ РЅСѓР¶РЅСѓСЋ РІРµСЂСЃРёСЋ РёР»Рё Р·Р°РїСѓСЃС‚РёС‚Рµ СЃ --allow-juce-version."
      fi
    fi
    ok "JUCE ${ver:-unknown} РІ $JUCE_DIR"
    return 0
  fi

  if [ -e "$JUCE_DIR" ] && [ -n "$(ls -A "$JUCE_DIR" 2>/dev/null || true)" ]; then
    die "$JUCE_DIR СЃСѓС‰РµСЃС‚РІСѓРµС‚, РЅРѕ РЅРµ РїРѕС…РѕР¶ РЅР° JUCE (РЅРµС‚ modules/juce_core).
       РЈРєР°Р¶РёС‚Рµ РґСЂСѓРіРѕР№ РєР°С‚Р°Р»РѕРі С‡РµСЂРµР· --juce-dir РёР»Рё СѓР±РµСЂРёС‚Рµ СЌС‚РѕС‚."
  fi

  info "РљР»РѕРЅРёСЂСѓСЋ JUCE $JUCE_VERSION_PINNED РІ $JUCE_DIR (РѕРґРёРЅ СЂР°Р·, ~200 РњР‘)"
  mkdir -p "$(dirname "$JUCE_DIR")"
  git clone --depth 1 --branch "$JUCE_VERSION_PINNED" \
    https://github.com/juce-framework/JUCE.git "$JUCE_DIR"
  [ -d "$JUCE_DIR/modules/juce_core" ] || die "РєР»РѕРЅ JUCE РЅРµ СЃРѕРґРµСЂР¶РёС‚ modules/juce_core"
  ok "JUCE $JUCE_VERSION_PINNED СЃРєР»РѕРЅРёСЂРѕРІР°РЅ"
}

# The generated Xcode project resolves JUCE via ~/JUCE/modules. A different
# --juce-dir only works if ~/JUCE points at it (symlink) or Projucer rewrites
# the project (checked after --resave).
check_juce_location() {
  if [ "$JUCE_DIR" = "$HOME/JUCE" ]; then
    return 0
  fi

  if [ -e "$HOME/JUCE" ] && [ "$(cd "$HOME/JUCE" && pwd -P)" = "$(cd "$JUCE_DIR" && pwd -P)" ]; then
    return 0
  fi

  if [ "$DO_RESAVE" -eq 0 ]; then
    die "Xcode-РїСЂРѕРµРєС‚ РёС‰РµС‚ JUCE РІ ~/JUCE, Р° РІС‹Р±СЂР°РЅ $JUCE_DIR.
       РЎРґРµР»Р°Р№С‚Рµ СЃСЃС‹Р»РєСѓ:  ln -s \"$JUCE_DIR\" \"$HOME/JUCE\"
       Р»РёР±Рѕ Р·Р°РїСѓСЃС‚РёС‚Рµ СЃ --resave (РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ), С‡С‚РѕР±С‹ Projucer РїРµСЂРµРїРёСЃР°Р» РїСѓС‚СЊ."
  fi
}

ensure_juce
check_juce_location

# --- Projucer ----------------------------------------------------------------

PROJUCER_BUILD_DIR="$JUCE_DIR/extras/Projucer/Builds/MacOSX"

find_projucer() {
  local candidate

  for candidate in \
    "$PROJUCER_BUILD_DIR/build/Release/Projucer.app/Contents/MacOS/Projucer" \
    "$PROJUCER_BUILD_DIR/build/Release/Projucer" \
    "$PROJUCER_BUILD_DIR/build/Debug/Projucer.app/Contents/MacOS/Projucer" \
    "$PROJUCER_BUILD_DIR/Projucer.app/Contents/MacOS/Projucer"; do
    if [ -x "$candidate" ]; then
      printf '%s' "$candidate"
      return 0
    fi
  done

  candidate="$(find "$PROJUCER_BUILD_DIR" -type f -name Projucer -perm -u+x 2>/dev/null | head -n 1 || true)"
  if [ -n "$candidate" ] && [ -x "$candidate" ]; then
    printf '%s' "$candidate"
    return 0
  fi

  return 1
}

PROJUCER_BIN=""
if [ "$REBUILD_PROJUCER" -eq 0 ]; then
  PROJUCER_BIN="$(find_projucer || true)"
fi

if [ -z "$PROJUCER_BIN" ]; then
  [ -d "$PROJUCER_BUILD_DIR/Projucer.xcodeproj" ] || die "РЅРµС‚ $PROJUCER_BUILD_DIR/Projucer.xcodeproj"
  info "РЎРѕР±РёСЂР°СЋ Projucer (РѕРґРёРЅ СЂР°Р·, 1-2 РјРёРЅ)"
  # Ad-hoc signing keeps the JUCE "Sign Target" phase happy: it reads
  # CODE_SIGN_IDENTITY and fails on an unset variable.
  run_logged projucer-build xcodebuild \
    -project "$PROJUCER_BUILD_DIR/Projucer.xcodeproj" \
    -configuration Release \
    CODE_SIGN_IDENTITY=- \
    build
  PROJUCER_BIN="$(find_projucer || true)"
  [ -n "$PROJUCER_BIN" ] || die "РЅРµ РЅР°С€С‘Р» СЃРѕР±СЂР°РЅРЅС‹Р№ Projucer РІ $PROJUCER_BUILD_DIR"
  ok "Projucer СЃРѕР±СЂР°РЅ"
fi

# --- Regenerate the Xcode project -------------------------------------------

if [ "$DO_RESAVE" -eq 1 ]; then
  [ -f "$JUCER" ] || die "РЅРµС‚ $JUCER"
  info "РџРµСЂРµРіРµРЅРµСЂРёСЂСѓСЋ Xcode-РїСЂРѕРµРєС‚ РёР· .jucer"
  run_logged projucer-resave "$PROJUCER_BIN" --resave "$JUCER"
  ok "Xcode-РїСЂРѕРµРєС‚ РѕР±РЅРѕРІР»С‘РЅ"

  if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    if [ -n "$(git -C "$ROOT" status --porcelain -- GuitarBattle/Builds/MacOSX 2>/dev/null)" ]; then
      warn "Projucer РёР·РјРµРЅРёР» С„Р°Р№Р»С‹ РІ GuitarBattle/Builds/MacOSX вЂ” РїСЂРѕРІРµСЂСЊС‚Рµ 'git diff' РїРµСЂРµРґ РєРѕРјРјРёС‚РѕРј"
    fi
  fi
else
  info "РџСЂРѕРїСѓСЃРєР°СЋ --resave (--no-resave): РёСЃРїРѕР»СЊР·СѓСЋ Р·Р°РєРѕРјРјРёС‡РµРЅРЅС‹Р№ Xcode-РїСЂРѕРµРєС‚"
fi

[ -f "$PBXPROJ" ] || die "РЅРµС‚ $PBXPROJ вЂ” Р·Р°РїСѓСЃС‚РёС‚Рµ Р±РµР· --no-resave"

if ! grep -q 'JUCE/modules/juce_core' "$PBXPROJ"; then
  die "РІ $PBXPROJ РЅРµ РЅР°Р№РґРµРЅ РїСѓС‚СЊ Рє JUCE вЂ” РїРµСЂРµРіРµРЅРµСЂРёСЂСѓР№С‚Рµ РїСЂРѕРµРєС‚ (СѓР±РµСЂРёС‚Рµ --no-resave)"
fi

if [ "$JUCE_DIR" != "$HOME/JUCE" ] && ! grep -qF "$JUCE_DIR/modules/juce_core" "$PBXPROJ"; then
  die "Xcode-РїСЂРѕРµРєС‚ РІСЃС‘ РµС‰С‘ СЃСЃС‹Р»Р°РµС‚СЃСЏ РЅР° ~/JUCE/modules, Р° РЅРµ РЅР° $JUCE_DIR/modules.
       РЎРґРµР»Р°Р№С‚Рµ СЃСЃС‹Р»РєСѓ:  ln -s \"$JUCE_DIR\" \"$HOME/JUCE\""
fi

# --- Version ----------------------------------------------------------------

if [ -z "$VERSION" ]; then
  VERSION="$(sed -n 's/.*<JUCERPROJECT[^>]*version="\([^"]*\)".*/\1/p' "$JUCER" | head -n 1)"
  [ -n "$VERSION" ] || die "РЅРµ СѓРґР°Р»РѕСЃСЊ РїСЂРѕС‡РёС‚Р°С‚СЊ version РёР· $JUCER (Р·Р°РґР°Р№С‚Рµ --version)"
fi
ok "РІРµСЂСЃРёСЏ: $VERSION"

# --- Build ------------------------------------------------------------------

mkdir -p "$APP_BUILD_DIR"

info "РЎРѕР±РёСЂР°СЋ $CONFIGURATION ($ARCHS)"
run_logged xcodebuild-build xcodebuild \
  -project "$XCODE_DIR/GuitarBattle.xcodeproj" \
  -target "GuitarBattle - App" \
  -configuration "$CONFIGURATION" \
  ARCHS="$ARCHS" \
  ONLY_ACTIVE_ARCH=NO \
  CONFIGURATION_BUILD_DIR="$APP_BUILD_DIR" \
  CODE_SIGN_IDENTITY=- \
  build

[ -d "$APP_PATH" ] || die "СЃР±РѕСЂРєР° РїСЂРѕС€Р»Р°, РЅРѕ РЅРµС‚ $APP_PATH"

ARCH_INFO="$(lipo -archs "$APP_PATH/Contents/MacOS/GuitarBattle" 2>/dev/null || true)"
[ -n "$ARCH_INFO" ] || die "РЅРµ СѓРґР°Р»РѕСЃСЊ РїСЂРѕС‡РёС‚Р°С‚СЊ Р°СЂС…РёС‚РµРєС‚СѓСЂС‹ $APP_PATH"
ok "GuitarBattle.app: $ARCH_INFO"

if [ "$CONFIGURATION" = "Release" ]; then
  case " $ARCH_INFO " in
    *" arm64 "*) ;;
    *) die "РІ Release-СЃР±РѕСЂРєРµ РЅРµС‚ СЃР»Р°Р№СЃР° arm64 ($ARCH_INFO)" ;;
  esac
  case " $ARCH_INFO " in
    *" x86_64 "*) ;;
    *) die "РІ Release-СЃР±РѕСЂРєРµ РЅРµС‚ СЃР»Р°Р№СЃР° x86_64 ($ARCH_INFO)" ;;
  esac
fi

PLIST_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
  "$APP_PATH/Contents/Info.plist" 2>/dev/null || true)"
if [ -n "$PLIST_VERSION" ] && [ "$PLIST_VERSION" != "$VERSION" ]; then
  warn "РІРµСЂСЃРёСЏ РІ Info.plist ($PLIST_VERSION) РЅРµ СЃРѕРІРїР°РґР°РµС‚ СЃ $VERSION вЂ” РїР°РєСѓСЋ РєР°Рє $VERSION"
fi

# --- Package ----------------------------------------------------------------

plist_set() {
  local plist="$1" key="$2" value="$3"

  /usr/libexec/PlistBuddy -c "Set :$key $value" "$plist" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :$key string $value" "$plist"
}

write_helpers() {
  local work="$1"

  cat > "$work/README.txt" <<'EOF'
FretFight РґР»СЏ macOS

1. РџРµСЂРµС‚Р°С‰РёС‚Рµ FretFight РІ РїР°РїРєСѓ Applications.
2. РџСЂРёР»РѕР¶РµРЅРёРµ РЅРµ РЅРѕС‚Р°СЂРёР·РѕРІР°РЅРѕ Apple, РїРѕСЌС‚РѕРјСѓ РїСЂРё РїРµСЂРІРѕРј Р·Р°РїСѓСЃРєРµ macOS РµРіРѕ
   Р±Р»РѕРєРёСЂСѓРµС‚. Р”РІР°Р¶РґС‹ С‰С‘Р»РєРЅРёС‚Рµ В«First launch.commandВ» РІ СЌС‚РѕРј РѕРєРЅРµ вЂ” РѕРЅ СЃРЅРёРјРµС‚
   РєР°СЂР°РЅС‚РёРЅ Рё РѕС‚РєСЂРѕРµС‚ FretFight.
   Р›РёР±Рѕ РІС‹РїРѕР»РЅРёС‚Рµ РІ РўРµСЂРјРёРЅР°Р»Рµ:
       xattr -dr com.apple.quarantine /Applications/FretFight.app
   Рё Р·Р°РїСѓСЃС‚РёС‚Рµ FretFight РёР· РїР°РїРєРё Applications.
3. Р•СЃР»Рё macOS РїРёС€РµС‚ В«РќРµ СѓРґР°С‘С‚СЃСЏ РѕС‚РєСЂС‹С‚СЊ РїСЂРѕРіСЂР°РјРјСѓВ»: РЎРёСЃС‚РµРјРЅС‹Рµ РЅР°СЃС‚СЂРѕР№РєРё в†’
   РљРѕРЅС„РёРґРµРЅС†РёР°Р»СЊРЅРѕСЃС‚СЊ Рё Р±РµР·РѕРїР°СЃРЅРѕСЃС‚СЊ в†’ РїСЂРѕРєСЂСѓС‚РёС‚Рµ РІРЅРёР· в†’ В«Р’СЃС‘ СЂР°РІРЅРѕ РѕС‚РєСЂС‹С‚СЊВ».

FretFight for macOS

1. Drag FretFight into Applications.
2. The app is not notarized by Apple, so macOS blocks the first launch.
   Double-click "First launch.command" in this window: it removes the
   quarantine flag and opens FretFight.
   Alternatively, run in Terminal:
       xattr -dr com.apple.quarantine /Applications/FretFight.app
   and launch FretFight from Applications.
3. If macOS says "The application cannot be opened": System Settings в†’
   Privacy & Security в†’ scroll down в†’ "Open Anyway".
EOF

  cat > "$work/First launch.command" <<'EOF'
#!/bin/bash
# FretFight is ad-hoc signed and not notarized, so macOS quarantines it.
# This helper removes the quarantine flag and opens the installed app.
APP="/Applications/FretFight.app"

if [ ! -d "$APP" ]; then
  echo "РЎРЅР°С‡Р°Р»Р° РїРµСЂРµС‚Р°С‰РёС‚Рµ FretFight РІ РїР°РїРєСѓ Applications."
  echo "Drag FretFight into Applications first."
  echo
  read -n 1 -s -r -p "РќР°Р¶РјРёС‚Рµ Р»СЋР±СѓСЋ РєР»Р°РІРёС€Сѓ... / Press any key..."
  exit 1
fi

xattr -dr com.apple.quarantine "$APP" 2>/dev/null
open "$APP"
EOF

  chmod +x "$work/First launch.command"
}

package_app() {
  local work="$BUILD_DIR/package"
  local app="$work/FretFight.app"
  local plist="$app/Contents/Info.plist"
  local dmg="$OUT_DIR/FretFight-$VERSION.dmg"
  local zip="$OUT_DIR/FretFight-$VERSION.zip"

  rm -rf "$work"
  mkdir -p "$work" "$OUT_DIR"

  info "Р“РѕС‚РѕРІР»СЋ FretFight.app"
  ditto "$APP_PATH" "$app"

  # The bundle is renamed and re-branded after the build, so the ad-hoc
  # signature has to be renewed afterwards.
  plist_set "$plist" CFBundleName FretFight
  plist_set "$plist" CFBundleDisplayName FretFight
  plist_set "$plist" CFBundleIdentifier online.fretfight.client
  plist_set "$plist" CFBundleShortVersionString "$VERSION"
  plist_set "$plist" CFBundleVersion "$VERSION"

  codesign --force --deep --sign - "$app" >/dev/null 2>&1 \
    || die "codesign РЅРµ СЃРјРѕРі РїРѕРґРїРёСЃР°С‚СЊ $app"
  codesign --verify --deep --verbose=2 "$app" 2>&1 | sed 's/^/    /'
  ok "bundle РїРѕРґРїРёСЃР°РЅ (ad-hoc)"

  write_helpers "$work"

  # A mounted FretFight volume would make hdiutil fail with "Resource busy".
  hdiutil detach "/Volumes/FretFight" >/dev/null 2>&1 || true

  info "РЎРѕР·РґР°СЋ DMG"
  ln -sfn /Applications "$work/Applications"
  if ! hdiutil create -volname "FretFight" -srcfolder "$work" -ov -format UDZO \
      "$dmg" >"$LOG_DIR/hdiutil.log" 2>&1; then
    tail -n 40 "$LOG_DIR/hdiutil.log" >&2
    die "hdiutil create РЅРµ СЃРјРѕРі СЃРѕР±СЂР°С‚СЊ $dmg"
  fi
  rm -f "$work/Applications"

  info "РЎРѕР·РґР°СЋ ZIP"
  ditto -c -k --sequesterRsrc --keepParent "$app" "$zip"

  [ -s "$dmg" ] || die "DMG РїСѓСЃС‚РѕР№: $dmg"
  [ -s "$zip" ] || die "ZIP РїСѓСЃС‚РѕР№: $zip"

  printf '\n'
  ls -lh "$dmg" "$zip" | sed 's/^/    /'
  printf '\n%sSHA256%s\n' "$C_BOLD" "$C_RESET"
  shasum -a 256 "$dmg" "$zip" | sed 's/^/    /'
  printf '\n'
  ok "DMG: $dmg"
  ok "ZIP: $zip"
}

if [ "$DO_PACKAGE" -eq 1 ]; then
  package_app
else
  info "РЈРїР°РєРѕРІРєР° РїСЂРѕРїСѓС‰РµРЅР°; РїСЂРёР»РѕР¶РµРЅРёРµ: $APP_PATH"
fi

printf '\n%sР“РѕС‚РѕРІРѕ.%s РђСЂС‚РµС„Р°РєС‚С‹ Р»РµР¶Р°С‚ РІ %s (РєР°С‚Р°Р»РѕРі РІ .gitignore).\n' \
  "$C_BOLD$C_GREEN" "$C_RESET" "$OUT_DIR"

