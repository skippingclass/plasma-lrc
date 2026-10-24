#!/usr/bin/env bash
#
# Release helper script for plasma-lrc.
#
# Usage:
#   ./scripts/release.sh patch    # 1.4.0 -> 1.4.1
#   ./scripts/release.sh minor    # 1.4.0 -> 1.5.0
#   ./scripts/release.sh major    # 1.4.0 -> 2.0.0
#   ./scripts/release.sh 1.4.2    # Explicit version
#
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <patch|minor|major|X.Y.Z>" >&2
    exit 1
fi

BUMP_TYPE="$1"

# 1. Check for uncommitted changes
if [[ -n "$(git status --porcelain)" ]]; then
    echo "Error: Working directory has uncommitted changes. Commit or stash them first." >&2
    git status -s
    exit 1
fi

# 2. Run unit tests
echo "==> Running unit tests..."
cmake -S . -B build -DPLASMA_LRC_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure

# 3. Read current version from CMakeLists.txt
CURRENT_VER=$(grep -Po 'project\(plasma-lrc VERSION \K[0-9.]+' CMakeLists.txt)
echo "Current version: $CURRENT_VER"

IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT_VER"
PATCH="${PATCH:-0}"

if [[ "$BUMP_TYPE" == "patch" ]]; then
    NEW_VER="${MAJOR}.${MINOR}.$((PATCH + 1))"
elif [[ "$BUMP_TYPE" == "minor" ]]; then
    NEW_VER="${MAJOR}.$((MINOR + 1)).0"
elif [[ "$BUMP_TYPE" == "major" ]]; then
    NEW_VER="$((MAJOR + 1)).0.0"
elif [[ "$BUMP_TYPE" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    NEW_VER="$BUMP_TYPE"
else
    echo "Error: Invalid version or bump type '$BUMP_TYPE'. Expected patch, minor, major, or X.Y.Z." >&2
    exit 1
fi

TAG="v${NEW_VER}"
echo "==> Bumping version to $NEW_VER (tag: $TAG)"

# 4. Update files
# CMakeLists.txt
sed -i -E "s/project\(plasma-lrc VERSION [0-9.]+/project(plasma-lrc VERSION ${NEW_VER}/" CMakeLists.txt

# metadata.json
sed -i -E "s/\"Version\": \"[0-9.]+\"/\"Version\": \"${NEW_VER}\"/" metadata.json

# PKGBUILD
sed -i -E "s/pkgver=[0-9.]+\.r0/pkgver=${NEW_VER}.r0/" PKGBUILD

# Provider User-Agents
sed -i -E "s|plasma-lrc/[0-9.]+|plasma-lrc/${NEW_VER}|" lrclib.cpp amllprovider.cpp lrcredprovider.cpp spicylyrics.cpp

echo "==> Updated CMakeLists.txt, metadata.json, PKGBUILD, and provider source files"

# 5. Commit and tag
git add CMakeLists.txt metadata.json PKGBUILD lrclib.cpp amllprovider.cpp lrcredprovider.cpp spicylyrics.cpp
if [[ -f CHANGELOG.md ]]; then
    git add CHANGELOG.md || true
fi

git commit -m "chore(release): bump version to ${TAG}"
git tag -a "${TAG}" -m "Release ${TAG}"

echo ""
echo "Successfully created release commit and tag ${TAG}!"
echo "To publish, run:"
echo "    git push origin main --tags"
