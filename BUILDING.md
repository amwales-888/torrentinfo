# Building torrentinfo

The library and the `torrentinfo` program build on Linux and Windows from the
same sources. Platform differences are confined to `platform.c` (file access),
the certificate loading in `http.c`, and a few `#ifdef _WIN32` lines in
`main.c`. Lists use the bundled 4.4BSD `queue.h` on every platform.

Dependencies: **libuv** and **OpenSSL 3**.

## Linux (GNU make)

```sh
sudo apt install build-essential pkg-config libuv1-dev libssl-dev   # Debian/Ubuntu
make                 # build/release/torrentinfo (and ./torrentinfo link)
make debug           # AddressSanitizer + UBSan build in build/debug
make install PREFIX=$HOME/.local
```

Usage is the same as on Windows (below), for example
`./torrentinfo -D -O ~/Downloads file.torrent`.

## Windows (Visual Studio 2026)

The solution uses platform toolset **v145** (Visual Studio 2026) and gets
libuv and OpenSSL through **vcpkg manifest mode** (`vcpkg.json`). Visual
Studio 2026 includes vcpkg.

### One-time setup

1. In the Visual Studio Installer, make sure the **Desktop development with
   C++** workload is installed. Its **vcpkg package manager** component is
   included by default.
2. Open **Developer PowerShell for VS 2026** (from the Start menu or
   *Tools > Command Line*) and run:

   ```powershell
   vcpkg integrate install
   ```

   This lets MSBuild use vcpkg for every project. You only do it once per
   machine.

### Build

1. Open `torrentinfo.sln`.
2. Choose **Release | x64** (or Debug) and **Build > Build Solution**.

The first build downloads and compiles libuv and OpenSSL into
`vcpkg_installed\`, which takes several minutes; later builds reuse them.
Output goes to `build\x64\Release\torrentinfo.exe`, with the libuv and OpenSSL
DLLs copied next to it.

From the command line (Developer PowerShell):

```powershell
msbuild torrentinfo.sln /p:Configuration=Release /p:Platform=x64 /m
```

### Running

```powershell
cd build\x64\Release
.\torrentinfo.exe file.torrent
.\torrentinfo.exe -o out.torrent C:\path\to\file1 C:\path\to\file2
.\torrentinfo.exe -t file.torrent
.\torrentinfo.exe -P file.torrent      # announce, then contact the peers: handshake,
                                        # pieces each one has, and whether it unchokes us
.\torrentinfo.exe -D -O C:\Downloads file.torrent   # download from all peers (re-run to resume)
.\torrentinfo.exe -D -i 42 file.torrent              # ... only piece 42
```

To pass arguments when debugging in Visual Studio, use the `torrentinfo`
project's *Properties > Debugging > Command Arguments*.

### Notes

- **Downloading (`-D`)** uses every peer the trackers return, up to 50
  connections at once. Pieces are chosen rarest-first; each peer keeps 16
  block requests in flight; a peer that sends a corrupt piece, stalls for
  30 seconds or sends blocks it was not asked for is dropped, and one that
  disconnects is retried after 30 seconds (three connections at most). Near
  the end, slow blocks are also requested from a second peer ("endgame").
  The download stops if no data arrives for 2 minutes. Progress is printed
  every 2 seconds. DHT and peer exchange are not implemented yet, so the
  trackers are the only source of peers.

- **Trusted certificates for HTTPS trackers** come from the Windows
  certificate store (the "Trusted Root Certification Authorities"), loaded
  once at start-up. `-C file.pem` or the `SSL_CERT_FILE` environment variable
  use a PEM file instead.
- **Unicode file names** work: the library converts UTF-8 paths to UTF-16 for
  the Win32 file functions, and `torrentinfo.manifest` makes the program's
  command-line arguments UTF-8 (Windows 10 version 1903 or later).
- **File names Windows cannot use** are adjusted when a torrent is saved:
  `< > : " | ? *` and control characters become `_`, a trailing `.` or space
  becomes `_`, and reserved names such as `CON` or `LPT1` get a `_` prefix.
- **Using the library in your own project:** add `libtorrentinfo.vcxproj` as
  a project reference, enable vcpkg manifest mode, define
  `WIN32_LEAN_AND_MEAN`, and link the Windows libraries listed in
  `torrentinfo.vcxproj` (*Linker > Input*).
- **Other architectures:** only x64 is configured. To add ARM64, add an
  `ARM64` platform in *Build > Configuration Manager*; the code and vcpkg
  ports support it.
- Visual Studio 2026 can save the solution in the newer `.slnx` format
  (*File > Save As*) if you prefer it.
