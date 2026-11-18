# libGLX

GLX for X programs, one of the libraries of the X11 server for the compositor
(`xserver`): the image carries `/lib/libGLX.so` whenever the server is selected.
OpenGL itself is `libGL.so` ([userland/desktop/libGL](../../libGL/)), which holds no
`glX*` function. An X program that uses GLX links both, `-lGL -lGLX`; a program
ported from a system whose `libGL` also provides GLX needs `-lGLX` added to its link.
