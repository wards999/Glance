# Third-party software

Glance itself is licensed under the GNU General Public License, version 3 (see `LICENSE`).
The Windows downloads include the following software by others.

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

## mpv client API headers (`windows/third_party/mpv/`)

`client.h` and `render.h` from mpv, under the ISC licence stated at the top of each file.
