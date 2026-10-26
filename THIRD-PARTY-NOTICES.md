# Third-party software

Glance itself is licensed under the GNU General Public License, version 3 (see `LICENSE`).
The downloads include the following software by others.

## PDFium (`pdfium.dll`, Windows)

Chrome's PDF engine, prebuilt by [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)
(build chromium/8076). BSD-3-Clause licence, in `pdfium-LICENSE.txt`; the licences of the
libraries built into it are in `pdfium-licenses/`. Source: <https://pdfium.googlesource.com/pdfium/>.

## libmpv (`libmpv-2.dll`, Windows video add-on)

mpv's player engine, including FFmpeg and other libraries, prebuilt by
[shinchiro/mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake), release
`20261021`, file `mpv-dev-x86_64-20261021-git-e471496c56.7z`
(SHA-256 `efc719eff3dc5954b4a5efc800632808bcec01cc9034940dd14e5eb7fe9ea633`). This build is
licensed under the GNU General Public License.

Corresponding source:

- mpv, at the commit this build was made from: <https://github.com/mpv-player/mpv/tree/e471496c56>
- The build scripts, which fetch and record the exact versions of FFmpeg and every other library
  included: <https://github.com/shinchiro/mpv-winbuild-cmake> (release `20261021`)

## libmpv for the Mac (`Glance.app/Contents/Frameworks/`)

libmpv 0.41.0 with FFmpeg and the other libraries it uses (44 files, universal: Apple Silicon and
Intel), as prebuilt and published by [IINA](https://iina.io) at <https://iina.io/dylibs/universal/>
on 2026-10-26. Glance keeps its own copy as the `mac-deps-20261026` release asset
(SHA-256 `07ba5b95e191c530f87282562b360e6070502182b8905e1303eb63868b5cc9e6`, with per-file hashes
inside), and the Mac build checks it. These libraries are under the GNU GPL and LGPL.

Corresponding source:

- mpv 0.41.0: <https://github.com/mpv-player/mpv/tree/v0.41.0>
- FFmpeg and the other libraries, and how IINA builds them: <https://github.com/iina/iina> (see
  `other/download_libs.sh` and IINA's build documentation) and each library's own project.

## mpv client API headers (`windows/third_party/mpv/`, `Sources/CMpv/include/mpv/`)

`client.h`, `render.h` and `render_gl.h` from mpv, under the ISC licence stated at the top of each
file.
