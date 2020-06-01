# libCODY: COmpiler DYnamism<a href="#1"><sup>1</sup></a>

Copyright (C) 2020 Nathan Sidwell, nathan@acm.org

libCODY is an implementation of the C++20 module protocol introduced
by https://wg21.link/p1184.  It has evolved from the r1 version, and
an r2 version will produced at some point.

**WARNING:**  This is preliminary software.

In addition to supporting C++modules, this may also support LTO
requirements and could also feed deal with generated #include files
and feed the compiler with prepruned include paths and whatnot.  (The
system calls involved in include searches can be quite expensive on
some build infrastuctures.)

* Client and Server objects
* Direct connection for in-process use
* Testing with Joust (that means nothing to you, doesn't it!)



<a name="1">1</a>: or a small town in Wyoming
