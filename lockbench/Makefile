# The application that is statically linked into the kernel image.
#
# This file is included by the top-level Makefile, which compiles these sources
# with the kernel's own flags and links the resulting objects into the kernel.
# It is not meant to be run on its own. To build the kernel with this app:
#
#     make
#
# (See test/Makefile for the alternative test application, built with
#  `make app=tests`.)

app-objects += lockbench/osv-lockbench.o
app-objects += lockbench/tst-futex.o

posix-lockbench: posix-lockbench.cc
	g++ -O3 -std=c++20 -pthread $< -o $@ -ggdb  -DPOSIX -I../ -I../include

posix-futex: tst-futex.cc
	g++ -O3 -std=c++20 -pthread $< -o $@ -ggdb  -DPOSIX -I../include -I..

.PHONY: posix-lockbench # Make it always


