# Makefile – ipfw_addtable
#
# Applies patches to a FreeBSD 15/stable source tree and builds the
# affected components (ipfw kernel module + ipfw(8) userspace tool).
#
# Usage:
#   make SRCDIR=/usr/src apply     # copy new files + apply patches
#   make SRCDIR=/usr/src kernel    # rebuild ipfw.ko
#   make SRCDIR=/usr/src userspace # rebuild sbin/ipfw
#   make SRCDIR=/usr/src all       # apply + kernel + userspace
#   make SRCDIR=/usr/src clean     # remove staged copies
#
# SRCDIR defaults to /usr/src.  Set it to the root of your FreeBSD
# source tree if it lives elsewhere.

SRCDIR	?= /usr/src

KERNEL_IPFW_DIR	= ${SRCDIR}/sys/netpfil/ipfw
KERNEL_MOD_DIR	= ${SRCDIR}/sys/modules/ipfw
USERSPACE_DIR	= ${SRCDIR}/sbin/ipfw

NEW_KERNEL_FILES = \
	src/sys/netpfil/ipfw/ip_fw_addtable.h \
	src/sys/netpfil/ipfw/ip_fw_addtable.c

PATCHES = \
	patches/0001-ip_fw.h-add-O_ADDTABLE.patch \
	patches/0002-ip_fw2.c-handle-O_ADDTABLE.patch \
	patches/0003-kernel-Makefile.patch \
	patches/0004-ipfw2.h-add-TOK_ADDTABLE.patch \
	patches/0005-ipfw2.c-userspace-parser.patch

# -------------------------------------------------------------------------
# Targets
# -------------------------------------------------------------------------

.PHONY: all apply kernel userspace clean check-srcdir

all: apply kernel userspace

check-srcdir:
	@test -d "${SRCDIR}/sys/netpfil/ipfw" || \
	    { echo "ERROR: FreeBSD source not found at SRCDIR=${SRCDIR}"; exit 1; }

## apply – copy new source files and apply all patches
apply: check-srcdir
	@echo ">>> Copying new source files..."
	cp src/sys/netpfil/ipfw/ip_fw_addtable.h ${KERNEL_IPFW_DIR}/
	cp src/sys/netpfil/ipfw/ip_fw_addtable.c ${KERNEL_IPFW_DIR}/
	@echo ">>> Applying patches..."
	@for p in ${PATCHES}; do \
	    echo "  patch: $$p"; \
	    patch -d ${SRCDIR} -p1 --forward --fuzz=3 < $$p || exit 1; \
	done
	@echo ">>> Done. Source tree at ${SRCDIR} is ready to build."

## kernel – rebuild only the ipfw kernel module
kernel: check-srcdir
	@echo ">>> Building ipfw.ko..."
	make -C ${KERNEL_MOD_DIR} obj
	make -C ${KERNEL_MOD_DIR}

## userspace – rebuild only ipfw(8)
userspace: check-srcdir
	@echo ">>> Building ipfw(8)..."
	make -C ${USERSPACE_DIR}

## clean – remove the files we copied into the source tree
clean: check-srcdir
	rm -f ${KERNEL_IPFW_DIR}/ip_fw_addtable.h
	rm -f ${KERNEL_IPFW_DIR}/ip_fw_addtable.c
	@echo "NOTE: patches applied to ${SRCDIR} must be reverted manually."
	@echo "      Use: patch -d ${SRCDIR} -p1 -R < patches/<patch>"
