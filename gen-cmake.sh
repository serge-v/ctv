SRCDIR=$HOME/src/xtree/ctv
BDIR=$HOME/b/ctv
OS=$(uname)

if [ "_$OS" == "_Darwin" ] ; then
	rm -rf ${BDIR}x
	mkdir -p ${BDIR}x
	cd ${BDIR}x
	cmake -G Xcode -DCMAKE_BUILD_TYPE=DEBUG -DCMAKE_TOOLCHAIN_FILE=$SRCDIR/macports.cmake $SRCDIR

	rm -rf ${BDIR}b
	mkdir -p ${BDIR}b
	cd ${BDIR}b
	cmake -DCMAKE_BUILD_TYPE=DEBUG -DCMAKE_TOOLCHAIN_FILE=$SRCDIR/macports.cmake $SRCDIR

#	rm -rf ${BDIR}-rpi
#	mkdir -p ${BDIR}-rpi
#	cd ${BDIR}-rpi
#	cmake -DCMAKE_BUILD_TYPE=DEBUG -DCMAKE_TOOLCHAIN_FILE=$SRCDIR/rpi-cross.cmake $SRCDIR
else
	rm -rf ${BDIR}b
	mkdir -p ${BDIR}b
	cd ${BDIR}b
	cmake -DCMAKE_BUILD_TYPE=DEBUG $SRCDIR
fi
