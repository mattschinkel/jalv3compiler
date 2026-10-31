# Title: Makefile to create jal compiler pdf and html documentation using dita
# Author: Rob Jansen, Copyright (c) 2022..2023, all rights reserved.
# Adapted by:
# 
# This file is part of jallib (https://github.com/jallib/jallib)
# Released under the ZLIB license (http://www.opensource.org/licenses/zlib-license.html)
#
#

# Override on the command line or in the environment when DITA-OT lives
# elsewhere, e.g. make pdf DITA_DIR=$HOME/Tools/dita-ot-4.4.1
DITA_DIR?=$HOME/Tools/dita-ot-4.0.2
PDF_DIR=`pwd`/pdf
HTML_DIR=`pwd`/html
SOURCE_DIR=`pwd`/dita

clean: 
	rm -rf ${HTML_DIR}
	rm -rf ${PDF_DIR}

pdf:
	# Jalv3 documentation in pdf format
	${DITA_DIR}/bin/dita -project=${SOURCE_DIR}/jalv3/pdf.xml -o ${PDF_DIR}
	${DITA_DIR}/bin/dita -project=${SOURCE_DIR}/jalv3opt/pdf.xml -o ${PDF_DIR}
	${DITA_DIR}/bin/dita -project=${SOURCE_DIR}/jalv3pragma/pdf.xml -o ${PDF_DIR}

html:
	# Jalv3 documentation in html format
	${DITA_DIR}/bin/dita -project=${SOURCE_DIR}/jalv3/html.xml -o ${HTML_DIR}/jalv3
	${DITA_DIR}/bin/dita -project=${SOURCE_DIR}/jalv3opt/html.xml -o ${HTML_DIR}/jalv3opt
	${DITA_DIR}/bin/dita -project=${SOURCE_DIR}/jalv3pragma/html.xml -o ${HTML_DIR}/jalv3pragma

all: clean pdf html

