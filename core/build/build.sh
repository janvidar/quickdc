#!/bin/sh

# echo `basename $0`;

svn up ..

# detect svn version.
SVN_VERSION=`svn --version | head -n 1 | cut -f 3 -d " " | cut -f 1-2 -d "."`;

if [ ${SVN_VERSION} = "1.1" ]; then
	BUILD=`cat ../.svn/entries  | grep revision | tr -d [:alpha:][:punct:][:space:]`
else
	BUILD=`head -n 4 ../.svn/entries | tail -n 1`
fi

echo "Build #" ${BUILD}

make clean
make BUILD=${BUILD} -j 2 all
make autotest

