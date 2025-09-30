#!/bin/sh

DEFAULT_VERSION="5.12.0.git"

thisdir=$(dirname $0)

if [ -d ${thisdir}/.git ]; then
	VERSION="`git -C ${thisdir} describe --dirty=+ --abbrev=7 --exclude=release* 2> /dev/null \
	| sed \
	  -e '/^collectd-/d' \
		-e 's/^v//' \
		-e 's///' \
		-e 's/-/./1'`"
fi

if test -z "$VERSION"; then
	VERSION="$DEFAULT_VERSION"
fi

printf "%s" "$VERSION"
