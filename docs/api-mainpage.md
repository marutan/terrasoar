# Terra Soar API {#mainpage}

API reference for the Terra Soar libraries, generated from the headers.

- **core** (`core/include/ts/`): the Qt-widget-free library — maths and
  WGS 84 geodesy, the camera and its navigation, the renderer backend and
  the globe renderer, third-party library handles, translation,
  performance measurement and OpenGL probing.
- **app** (`app/src/`): the desktop application shell.

Every public class states its thread-safety level:

- *Reentrant*: safe when each thread uses its own instances.
- *Thread-safe*: one instance may be shared between threads.
- *GUI thread only*: must be used from the thread that runs the
  QApplication.
- *Render thread only*: must be used where its OpenGL context is current
  (the renderer backend and GPU resources).

See `docs/architecture.md` for module boundaries and
`docs/coding-standards.md` for the rules this documentation follows.
