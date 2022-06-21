// https://www.duskborn.com/posts/simple-c-command-line-parser
#ifndef OPTION
#error "Must define OPTION macro"
#endif

OPTION(HELP,    h, help,        0, "print all the options and exit")
OPTION(OUT,     o, output,      1, "set the output path")
OPTION(INCLUDE, I, include,     1, "add include directory")
OPTION(PREPROC, P, preprocess,  0, "preprocess file and output to stdout")
OPTION(RUN,     r, run,         1, "execute compiled program")
OPTION(LIB,     l, lib,         1, "add library to compilation unit")

#undef OPTION