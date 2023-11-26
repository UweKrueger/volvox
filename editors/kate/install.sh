#!/usr/bin/env bash

FILE_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"

mkdir -p "${HOME}/.local/share/katepart5/script/indentation"
status=$?
if [ $status != 0 ]; then
	exit $status
fi

cp -v "${FILE_DIR}/volvox.js" "${HOME}/.local/share/katepart5/script/indentation"
status=$?
if [ $status != 0 ]; then
	exit $status
fi

mkdir -p "${HOME}/.local/share/katepart5/syntax"
status=$?
if [ $status != 0 ]; then
	exit $status
fi


cp -v "${FILE_DIR}/volvox.xml" "${HOME}/.local/share/katepart5/syntax"
status=$?
if [ $status != 0 ]; then
	exit $status
fi
