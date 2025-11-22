#!/usr/bin/env bash

FILE_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"

mkdir -p "${HOME}/.local/share/libgedit-gtksourceview-300/language-specs"
status=$?
if [ $status != 0 ]; then
	exit $status
fi

cp -v "${FILE_DIR}/volvox.lang" "${HOME}/.local/share/libgedit-gtksourceview-300/language-specs"
status=$?
if [ $status != 0 ]; then
	exit $status
fi
