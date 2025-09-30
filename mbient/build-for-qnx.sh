#! /bin/bash
set -e
srcdir=`dirname $0`
origdir=`pwd`
CMAKE_INSTALL_PREFIX=$1
QNX_AP_BASE_DIR=$2
CMAKE_BINARY_DIR=$3
LAYER_PLATFORM_SERVICES=$4

cd `dirname "$srcdir"`

echo "Patching Makefile.am"
sed -i -e "s:\$(srcdir)/src/liboconfig/scanner.l:src/liboconfig/scanner.l:" \
       -e "s:\$(srcdir)/src/liboconfig/parser.y:src/liboconfig/parser.y:" \
        Makefile.am

./build.sh

export CPPFLAGS="-I${CMAKE_INSTALL_PREFIX}/include/ \
                 -I${CMAKE_INSTALL_PREFIX}/include/dlt/ \
                 -I${QNX_AP_BASE_DIR}/install/usr/include/"

export LDFLAGS="-L${CMAKE_INSTALL_PREFIX}/lib/ \
                -L${QNX_AP_BASE_DIR}/install/aarch64le/lib \
                -L${CMAKE_BINARY_DIR}/${LAYER_PLATFORM_SERVICES}/dlt-daemon/src/lib/"

# Patch the generated "configure" file since libtool has a bug (see https://debbugs.gnu.org/cgi/bugreport.cgi?bug=21137)
sed -i -e "s:test x-L = \"\$p\":test x-L = \"x\$p\":" \
       -e "s:test x-R = \"\$p\":test x-R = \"x\$p\":" \
        configure

./configure        --host aarch64-unknown-nto-qnx7.1.0 \
                   --disable-all-plugins \
                   --disable-werror \
                   --with-fp-layout=nothing \
                   --with-java=no \
                   --with-libperl=no \
                   --with-libdlt=yes \
                   --enable-write_dlt \
                   --enable-network \
                   --enable-thermal \
                   --enable-logfile \
                   --enable-cpu \
                   --enable-gpu \
                   --enable-processes \
                   --enable-memory \
                   --enable-match-regex \
                   --enable-npu \
                   --enable-netstat_qnx \
                   --prefix=${CMAKE_INSTALL_PREFIX}

make -j1 install

cd $origdir
