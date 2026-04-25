#!/bin/sh
# Extract archive(s) into a subfolder named after the archive.
for f in "$@"; do
    base=$(basename "$f")
    # Strip all archive extensions to get the folder name
    folder=$(echo "$base" | sed 's/\.tar\.gz$//;s/\.tar\.bz2$//;s/\.tar\.xz$//;s/\.tar\.zst$//;s/\.tgz$//;s/\.tbz2$//;s/\.txz$//;s/\.tar$//;s/\.zip$//;s/\.7z$//')
    dest="$(dirname "$f")/$folder"
    mkdir -p "$dest"
    case "$f" in
        *.tar)                tar xf  "$f" -C "$dest" ;;
        *.tar.gz|*.tgz)       tar xzf "$f" -C "$dest" ;;
        *.tar.bz2|*.tbz2)     tar xjf "$f" -C "$dest" ;;
        *.tar.xz|*.txz)       tar xJf "$f" -C "$dest" ;;
        *.tar.zst)            tar --zstd -xf "$f" -C "$dest" ;;
        *.zip)                unzip -o "$f" -d "$dest" ;;
        *.7z)                 7z x -y -o"$dest" "$f" ;;
    esac
done
