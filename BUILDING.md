# Building torrentinfo

The library and the `torrentinfo` program build on Linux and Windows from the
same sources, which are all in `src/`; build output goes in `build/`.
Platform differences are confined to `src/platform.c` (file access),
`src/net.c` (binding to a network interface), the certificate loading in
`src/http.c`, and a few `#ifdef _WIN32` lines in `src/main.c`. Lists use the bundled 4.4BSD `queue.h` on every platform.

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
.\torrentinfo.exe -D -I "Ethernet 2" file.torrent   # ... only through that network interface
.\torrentinfo.exe -D -i 42 file.torrent              # ... only piece 42
.\torrentinfo.exe -D -S file.torrent                 # download, then keep seeding until Ctrl-C
.\torrentinfo.exe -D -L 51413 file.torrent           # accept peers on port 51413 (default 6881)
.\torrentinfo.exe -V -O C:\Downloads file.torrent   # check what is on disk (offline)
```

To pass arguments when debugging in Visual Studio, use the `torrentinfo`
project's *Properties > Debugging > Command Arguments*.

### Notes

- **Choosing a network interface (`-I`)**, with `-t`, `-P` or `-D`, sends all
  tracker and peer traffic through one interface, for example a VPN. Give
  its name (`tun0`, `wg0`, `eth0` on Linux; the name shown in *Network
  Connections* such as `Ethernet 2` on Windows, in quotes if it has spaces)
  or one of its IP addresses. Each socket is bound to the interface's
  address and tied to the interface itself (`SO_BINDTODEVICE` on Linux,
  `IP_UNICAST_IF` on Windows), so nothing leaves another way; if the
  interface goes down, connections fail rather than falling back. Peers of
  an address family the interface lacks (IPv6 on an IPv4-only VPN) are
  skipped. DNS lookups for tracker host names still go through the system's
  resolver. On Linux kernels before 5.7, tying sockets to the interface
  needs root or `CAP_NET_RAW`; without it a warning is printed and only the
  address is bound, which Linux may route out of another interface.

- **Verifying (`-V`)** hashes the data on disk against the torrent without
  creating or changing anything, and needs no network. For each file it
  shows complete / partial / MISSING / NOT A FILE, the bytes verified, the
  piece range it spans and which of those pieces are missing (as ranges such
  as `7, 50-53, 120`), and warns when the size on disk differs from the
  torrent. A summary gives pieces and bytes verified and every missing piece.
  Exit status: 0 if complete, 2 if incomplete, 1 on error. A piece that spans
  two files counts against both, so a missing file can make its neighbour
  "partial".

- **Downloading (`-D`)** uses every peer the trackers return, up to 50
  connections at once. Pieces are chosen rarest-first; each peer keeps 16
  block requests in flight; a peer that sends a corrupt piece, stalls for
  30 seconds or sends blocks it was not asked for is dropped, and one that
  disconnects is retried after 30 seconds (three connections at most). Near
  the end, slow blocks are also requested from a second peer ("endgame").
  The download stops if no data arrives for 2 minutes, or if there is no
  peer at all for 1 minute. Progress is printed every 2 seconds. DHT and
  peer exchange are not implemented yet, so the trackers (and peers that
  connect to us) are the only source of peers.

- **Trackers** are announced to throughout the download: every tracker the
  torrent lists, all tiers at once, starting with "started" once the files
  have been checked (so "left" is accurate), then again at the interval
  each tracker asks for (60 seconds to 1 hour), reporting the bytes
  uploaded, downloaded and left. New peers from re-announces join the
  download. A tracker that fails is retried after 1, 2, 4... minutes (up
  to 30). When the download completes the trackers are told "completed";
  on Ctrl-C (or SIGTERM on Linux) they are told "stopped" (waiting at most
  5 seconds; press Ctrl-C again not to wait).

- **Uploading.** Peers are sent our bitfield and a "have" for each new
  piece, and their requests are answered from disk. Every 10 seconds the
  three interested peers that sent us the most (while seeding: that we sent
  the most) are unchoked, plus one more chosen at random every 30 seconds,
  so new peers get a chance; a free slot goes straight to a waiting peer.
  A peer connected to us twice (once each way) keeps one connection.

- **Incoming connections (`-L`).** `-D` listens on port 6881 for IPv4 and
  IPv6 (or the next free port up to 6890; `-L port` picks another, `-L 0`
  turns it off) and tells the trackers that port. With `-I` it listens on
  that interface only. Peers outside your network can reach you only if the
  port is open in your firewall and, behind a home router, forwarded to
  this computer; Windows Firewall asks the first time. Without that,
  downloading still works through outgoing connections.

- **Seeding (`-S`).** Without `-S`, `-D` exits once the download is complete.
  With it, the program keeps serving peers (dropping ones that are complete
  too) until Ctrl-C; with a complete download already on disk it seeds
  straight away.

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
  a project reference, add `src` to *C/C++ > Additional Include
  Directories*, enable vcpkg manifest mode, define
  `WIN32_LEAN_AND_MEAN`, and link the Windows libraries listed in
  `torrentinfo.vcxproj` (*Linker > Input*).
- **Other architectures:** only x64 is configured. To add ARM64, add an
  `ARM64` platform in *Build > Configuration Manager*; the code and vcpkg
  ports support it.
- Visual Studio 2026 can save the solution in the newer `.slnx` format
  (*File > Save As*) if you prefer it.
