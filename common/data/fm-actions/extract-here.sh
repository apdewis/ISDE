#!/bin/sh
# Extract archive(s) into the same directory as the archive.
for f in "$@"; do
    dir=$(dirname "$f")
    case "$f" in
        *.tar)                tar xf  "$f" -C "$dir" ;;
        *.tar.gz|*.tgz)       tar xzf "$f" -C "$dir" ;;
        *.tar.bz2|*.tbz2)     tar xjf "$f" -C "$dir" ;;
        *.tar.xz|*.txz)       tar xJf "$f" -C "$dir" ;;
        *.tar.zst)            tar --zstd -xf "$f" -C "$dir" ;;
        *.zip)                unzip -o "$f" -d "$dir" ;;
        *.7z)                 7z x -y -o"$dir" "$f" ;;
    esac
done
