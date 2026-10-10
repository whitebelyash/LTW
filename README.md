## Large Thin Wrapper
A thin OpenGL core-to-OpenGL ES wrapper, primarily intended for running Minecraft.

# Building
`./gradlew :ltw:assembleRelease`

After completion, an AAR with native libraries will be available in `ltw/build/outputs/aar/ltw-release.aar`

# Fixed-function / compatibility mode
Besides the usual core-profile translation this wrapper can emulate the OpenGL
1.x/2.0 fixed-function pipeline so that classic desktop GL applications run on
top of the GLES3 backend. Set the environment variable `LTW_COMPAT=1` to make the
context report a 2.1 compatibility profile (`GL_VERSION`, `GL_CONTEXT_PROFILE_MASK`,
`GL_SHADING_LANGUAGE_VERSION`). The fixed-function entry points are implemented
regardless of that flag, so an application that ignores the reported version and
calls them directly still works.

Emulated features include immediate mode (`glBegin`/`glEnd`), client vertex
arrays, the matrix stacks, per-vertex lighting and materials, fog, alpha test,
texture environment/coordinate generation for up to four texture units, clip
planes and display lists. It is a pragmatic subset intended to get GL1.0/2.0
applications drawing, not a bit-exact implementation.
