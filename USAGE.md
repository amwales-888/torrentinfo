# TORRENTINFO(1)

## NAME

**torrentinfo** — look inside, create, check and download BitTorrent files

## SYNOPSIS

```
torrentinfo file.torrent | magnet-link
torrentinfo -o new.torrent [-a url]... [-w url]... [-c text] [-n name] [-l size] [-p] file...
torrentinfo -t [-T seconds] [-C certs.pem] [-I interface] file.torrent
torrentinfo -P [-T seconds] [-C certs.pem] [-I interface] file.torrent
torrentinfo -D [-S] [-L port] [-N] [-X] [-W] [-R] [-E mode] [-d speed] [-u speed] [-f list] [-F pattern]...
               [-O folder] [-i piece] [-T seconds] [-C certs.pem] [-I interface]
               file.torrent | magnet-link ...
torrentinfo -V [-O folder] [-f list] [-F pattern]... file.torrent
torrentinfo -m file.torrent
```

On Windows the program is called `torrentinfo.exe`; everything else is the
same.

## DESCRIPTION

**torrentinfo** is a small BitTorrent client that runs in a terminal. A
*.torrent* file describes one or more files that people share with each
other: their names and sizes, and a fingerprint of every *piece* (a small
slice of the data), so that whatever you download can be checked. The
torrent also lists *trackers*: servers that introduce you to the other
people, or *peers*, who are sharing the same files.

The program works in one of six ways, chosen by the first option.

**Show a torrent** (no option)
: Prints what is inside a torrent file: its name, the files it contains
  and their sizes, the trackers, the piece size and so on. Given a magnet
  link, it shows what the link contains.

**Create a torrent** (`-o`)
: Makes a new torrent file from files on your computer, ready to share.

**Ask the trackers** (`-t`)
: Contacts every tracker in the torrent and lists the peers they report,
  with the number of people sharing and downloading.

**Contact the peers** (`-P`)
: Asks the trackers for peers, then knocks on each peer's door to see
  whether it answers, which client it runs, how much of the data it has,
  and whether it is willing to send.

**Download** (`-D`)
: Downloads the torrent. The program gets peers from the trackers, fetches
  pieces from many of them at once, checks every piece, and saves the files.
  While it downloads, it also sends pieces it already has to others, as
  BitTorrent expects. It keeps the trackers informed as it goes, and finds
  new peers when they report some.
: If you stop a download (Ctrl-C) or it is interrupted, simply run the same
  command again: the program checks what is already on disk and carries on
  from there.
: A progress line appears every two seconds, showing how far along the
  download is, the download and upload speeds, and how many peers are
  connected.
: Give several torrents (or magnet links, or both) to download them all at
  once. They share one port, the DHT and the speed limits; each line of
  output then starts with the torrent's number (`[1]`, `[2]`, ...), in the
  order given. The program ends when every torrent is done (with `-S`,
  when you press Ctrl-C). `-f`, `-F` and `-i` work with one torrent only.

**Magnet links**
: Instead of a torrent file, `-D` accepts a *magnet link*, the
  `magnet:?xt=urn:btih:...` links websites offer. Put it in quotes. The
  program first asks peers for the torrent's details (this usually takes a
  few seconds), checks them, keeps them (see FILES), and then downloads as
  usual. Running the same link again starts straight away. Links that name
  no tracker work too: the peers are found through the DHT. Magnet links
  cannot be used for *private* torrents (those of many private trackers):
  their peers will not send the details, so you need the .torrent file.

**Make a magnet link** (`-m`)
: Prints the magnet link for a torrent file, with its name, trackers and
  web seeds, ready to paste into a web page, a message or another client.
  For a private torrent it also warns that the link will not work.

**Check a download** (`-V`)
: Checks the files on disk against the torrent, without using the network
  and without changing anything. On a large torrent this can take a while;
  progress is shown every two seconds. For each file it says whether it is
  complete, partly there or missing, how much of it is good, and which
  pieces are missing.

## OPTIONS

### Creating a torrent

`-o new.torrent`
: Create a torrent from the files named after the options and save it as
  *new.torrent*. Give one file, or several (they are shared together as a
  folder).

`-a url`
: Add a tracker. Repeat to add more; they are tried as alternatives.

`-w url`
: Add a web seed: a web address the files can also be downloaded from.

`-c text`
: Add a comment.

`-n name`
: The name the download will have. By default this is the file's name or,
  for several files, the torrent's name without *.torrent*.

`-l size`
: The piece size, such as `256K` or `1M`. Normally best left to the
  program, which picks one to suit the size of the files.

`-p`
: Mark the torrent *private*: clients should then find peers only through
  its trackers.

### Downloading

`-D`
: Download the torrent (see DESCRIPTION).

`-O folder`
: Where to save the download, or with `-V` where to look for it. The
  default is the current folder.

`-S`
: Seed: once the download is complete, keep running and keep sharing with
  others until you press Ctrl-C. If the files are already complete, seeding
  starts straight away. Without `-S`, the program finishes as soon as the
  download is complete.

`-L port`
: The port other peers can use to connect to you. The default is 6881; if
  it is busy, the next free one up to 6890 is used. `-L 0` turns this off:
  the program then only makes connections itself, which still works for
  downloading. See NOTES about firewalls.

`-X`
: Don't use the DHT. Normally the program also finds peers through the DHT,
  a worldwide network of BitTorrent users that works without trackers (but
  never for private torrents).

`-N`
: Don't ask the router to forward the port. Normally the program asks the
  router (using UPnP or NAT-PMP) to let other peers reach you, and removes
  the forwarding again when it ends.

`-f list`
: Download only some of the files, given by their numbers in the `#`
  column of the torrent's file list (`torrentinfo file.torrent` shows it):
  `-f 2`, `-f 1,3`, `-f 4-9,12`. With `-V`, check only those files. See
  "Choosing files" under NOTES.

`-F pattern`
: Download only the files whose name matches the pattern, where `*` stands
  for any text and `?` for any one character: `-F '*.mkv'`. Upper and lower
  case are treated alike. A pattern with a `/` in it is matched against the
  whole path inside the torrent (`-F 'Season 1/*'`). Give `-F` several
  times for several patterns; `-f` and `-F` can be combined, and every file
  either one picks is downloaded. Put the pattern in quotes so that the
  shell does not expand it. With a magnet link, use `-F`: the file numbers
  are not known until the torrent has been fetched.

`-R`
: Check every piece already on disk, as if starting afresh, instead of
  trusting the resume file kept from last time (see "Fast resume" under
  NOTES). Use it if you think the files were changed behind the program's
  back.

`-W`
: Do not download from web seeds (see "Web seeds" under NOTES).

`-E mode`
: Protocol encryption. `prefer` (the default) encrypts connections with
  peers that support it and falls back to an ordinary connection with
  those that do not. `require` talks only to peers that encrypt, and turns
  the others away; you will find fewer peers. `off` never encrypts and
  turns away peers that insist on it. See "Encryption" under NOTES.

`-d speed`
: Download no faster than this. Give it in KiB per second (`-d 500`) or
  with a unit: `500k`, `1.5M`, `2G`. `0` means no limit, the default. The
  limit is for everything the program downloads, shared by all the peers;
  with several torrents, those that are downloading share it equally.

`-u speed`
: Upload no faster than this (same units). The allowance is shared evenly
  between the torrents uploading and between the peers each uploads to.
  Useful to leave a seed running without slowing down the rest of your
  connection.

`-i piece`
: Download just one piece, given by its number (starting at 0). Mostly
  useful for testing.

### Network

`-T seconds`
: How long to wait for a tracker or a peer to answer. The default is 15.

`-I interface`
: Send all traffic through one network connection, for example a VPN. Give
  the connection's name (such as `tun0` or `wg0` on Linux and FreeBSD,
  `en0` or `utun3` on macOS, or
  the name shown in Windows' *Network Connections*, such as
  `"Ethernet 2"`), or its IP address. Nothing is sent any other way; if that
  connection goes down, the program cannot reach anyone until it comes
  back. Looking up a tracker's name (DNS) still uses your normal
  connection. On FreeBSD the program can only use the connection's
  address, and the system's routes decide which way traffic goes; it says
  `[address only]` when it starts (see *BUILDING.md* for how to route
  everything through a VPN there).

`-C certs.pem`
: For trackers whose address starts with `https://`, trust the
  certificates in this file instead of the system's own list. Rarely
  needed.

### Other

`-V`
: Check a download against the torrent (see DESCRIPTION). With `-f` or
  `-F`, only the chosen files are checked and reported, and the download
  counts as complete when they are.

`-h`
: Show a short summary of the options.

## STOPPING

Press **Ctrl-C** to stop a download or end seeding. The program tells the
trackers that you are leaving, which takes a few seconds at most, and prints
a summary (one per torrent). Press Ctrl-C a second time to quit without
waiting.

Nothing is lost by stopping: pieces already saved are kept, and running the
same command again continues the download.

## EXIT STATUS

`0`
: Success. For `-D`, the download is complete (or seeding ended normally).
  For `-V`, every piece is present and correct.

`1`
: Something went wrong, or the download was stopped before it finished.

`2`
: `-V` only: the check worked, but the download is incomplete.

## NOTES

**Firewalls and routers.** Other peers can connect to you only if the port
(6881 unless you chose another with `-L`) is allowed through your firewall
and, on a home network, forwarded by your router to your computer. The
program asks the router to do this itself and says whether it worked, for
example "router 192.168.1.1 forwards port 6881 to us (UPnP)". If your router
has this switched off, you can turn on UPnP in its settings or forward the
port by hand. Windows asks about the firewall the first time. Without any of
this, downloading still works; you just meet fewer peers and can share with
fewer people.

**Finding peers.** Peers come from the torrent's trackers, from the DHT,
from people who connect to you, and from the peers themselves, which tell
each other about the other peers they know ("peer exchange"). Private
torrents only use their trackers.

**Fast resume.** When a download stops (finished, interrupted with Ctrl-C,
or given up), and every 30 seconds while it runs, the program notes which
pieces are on disk, the size and time of last change of each file, and, at
the end, the parts of pieces it had only half downloaded. The next start
reads that instead of checking all the data again, so even a very large
download carries on at once ("fast resume: N pieces taken from the resume
file without checking"). Any file that has been changed since (its size
or time differs) is checked as before, and so is everything if the resume
file is missing or damaged. `-R` checks everything regardless; `-V` always
checks everything.

**Web seeds.** Some torrents (and magnet links, with `ws=`) list web
servers that hold the same files, such as a project's download mirror or
the Internet Archive. The program downloads from them as well as from
peers: they never go away, so a torrent with few peers can still download
at full speed. Everything they send is checked like data from peers. A
server that fails is tried again after a while (15 seconds, then longer),
and given up after five failures in a row or two corrupt pieces. The
progress shows a `web seeds:` line with how many are in use and how much
they have sent. `-d` limits peers and web seeds together. The torrent's
listing shows its web seeds.

**Encryption.** BitTorrent's protocol encryption scrambles the
connection between you and a peer so that it does not look like
BitTorrent. Some internet providers slow down or cut BitTorrent traffic
they recognise, and some peers only accept encrypted connections. It is
not real security: it does not hide which torrents you share, or who you
share them with. Each connection is reported as `encrypted`, as
`handshake encrypted` (the peer asked to encrypt only the start of the
connection), or with nothing for an ordinary one. With `prefer`, a peer
that does not support encryption is retried at once without it; one that
simply does not answer takes about five seconds.

**Choosing files.** With `-f` or `-F` only the chosen files are
downloaded; the others are not created at all. The download is complete
when the chosen files are, and with `-S` the program then shares what it
has. Data is checked in pieces, and a piece can straddle the end of one
file and the start of the next. When only one of the two files is chosen,
the whole piece is still downloaded (it cannot be checked otherwise), and
the part that belongs to the file you did not choose is kept in a hidden
file next to the download, `.name.parts`, a few pieces in size. Choosing
more files later, or the whole torrent, just downloads what is missing; the
`.parts` file is deleted once every file is on disk. A file you did not
choose but that is already on disk is left as it is and used.

**Blocking addresses.** Put addresses you never want to deal with in
`blocklist.txt` (see FILES), one per line: a single address (`203.0.113.7`),
a block (`10.0.0.0/8`, `2001:db8::/32`) or a range
(`198.51.100.0 - 198.51.100.255`). Lines starting with `#` are comments.
Published lists in the PeerGuardian (`.p2p`) and eMule `ipfilter.dat`
formats can be used as they are, even with hundreds of thousands of lines.
The program never connects to these addresses, refuses their connections,
and ignores them in the DHT. When it starts it says how many ranges it
loaded and how many lines it did not understand, and while it runs it says
how many peers were refused.

**Bad data.** Every piece is checked when it arrives. A peer that sends
damaged data is disconnected and not used again, and the piece is fetched
again from someone else.

**Disk space.** When a download starts, the files are created at their full
size, so make sure there is room for the whole torrent.

**Sharing fairly.** While downloading, the program shares with the peers
that send to it the most, plus one other chosen at random every half
minute, so that newcomers get a chance.

## FILES

The program keeps a little data between runs, in `~/.torrentinfo` on Linux
and FreeBSD, in `~/Library/Application Support/torrentinfo` on macOS, and in
`%APPDATA%\torrentinfo` on Windows (made when first needed):

`config`
: Your settings, one `name = value` per line. The first time the program
  needs it, it writes this file with every setting listed but switched off
  (each line starts with `#`); remove the `#` to use one. Options given on
  the command line win over this file. Lines it does not understand are
  reported and skipped. The settings are:

  `blocklist = blocklist.txt`
  : The blocklist to use (a name on its own is looked for in this
    directory); `none` for no blocklist.

  `listen_port = 6881`
  : Like `-L`.

  `port_forwarding = yes`
  : `no` is like `-N`.

  `dht = yes`
  : `no` is like `-X`.

  `dht_bootstrap = host:port, host:port`
  : DHT nodes to start from instead of the usual public ones.

  `download_directory = ~/Downloads`
  : Where downloads go (and where `-V` looks) when `-O` is not given. `~`
    is your home directory.

  `max_peers = 50`
  : How many peers to be connected to at once (1 to 1000).

  `upload_slots = 4`
  : How many peers to upload to at once (1 to 100).

  `max_download_rate = 2M`, `max_upload_rate = 500k`
  : Like `-d` and `-u`.

  `encryption = prefer`
  : Like `-E`: `prefer`, `require` or `off`.

  `web_seeds = yes`
  : `no` is like `-W`.

`blocklist.txt`
: Addresses never to talk to (see "Blocking addresses" under NOTES). Used
  if it exists, unless `config` names another file or says `none`. If
  `config` names a file that cannot be read, the program stops rather than
  run without it.

`dht.dat`
: The DHT nodes it knew last time, so the next start finds peers sooner.

`resume/`
: One small file per torrent (named after its info hash) recording what
  has been downloaded, for fast resume. Deleting them is harmless: the
  next start just checks the data instead.

`torrents/`
: The torrent files fetched for magnet links, named after each link's info
  hash. Deleting them is harmless: they are fetched again when needed.

## ENVIRONMENT

`TORRENTINFO_CONFIG`
: A settings file to use instead of `config` in the directory above. It is
  not created if missing.

`TORRENTINFO_DHT_BOOTSTRAP`
: DHT nodes to start from, as `host:port` separated by commas, instead of
  the usual public ones, and `dht_bootstrap` in the settings. Rarely needed.

## EXAMPLES

Show what is in a torrent:

```
torrentinfo ubuntu.torrent
```

Download a torrent into your Downloads folder:

```
torrentinfo -D -O ~/Downloads ubuntu.torrent
```

On Windows:

```
torrentinfo.exe -D -O C:\Users\me\Downloads ubuntu.torrent
```

Carry on with a download that was stopped (the same command again):

```
torrentinfo -D -O ~/Downloads ubuntu.torrent
```

Download, then keep sharing with others until you press Ctrl-C:

```
torrentinfo -D -S -O ~/Downloads ubuntu.torrent
```

Share files you already have in full, using port 51413:

```
torrentinfo -D -S -L 51413 -O ~/Downloads ubuntu.torrent
```

Download only through your VPN connection:

```
torrentinfo -D -I tun0 -O ~/Downloads ubuntu.torrent
torrentinfo.exe -D -I "NordLynx" -O C:\Downloads ubuntu.torrent
```

Download from a magnet link (keep the quotes):

```
torrentinfo -D -O ~/Downloads "magnet:?xt=urn:btih:3b245504cf5f11bbdbe1201cea6a6bf45aee1bc0&dn=ubuntu&tr=https://torrent.ubuntu.com/announce"
```

Turn a torrent file into a magnet link to share:

```
torrentinfo -m holiday.torrent
```

See a torrent's files with their numbers, then download only the second
and third, or only the videos:

```
torrentinfo series.torrent
torrentinfo -D -f 2,3 series.torrent
torrentinfo -D -F '*.mkv' -F '*.srt' series.torrent
```

Check whether a download is complete, and which pieces are missing:

```
torrentinfo -V -O ~/Downloads ubuntu.torrent
```

Use the check in a script:

```
if torrentinfo -V -O ~/Downloads ubuntu.torrent > /dev/null; then
    echo "complete"
else
    echo "not complete yet"
fi
```

See how many people are sharing a torrent, and whether they answer:

```
torrentinfo -t ubuntu.torrent
torrentinfo -P ubuntu.torrent
```

Make a torrent of a holiday video to share, with one tracker:

```
torrentinfo -o holiday.torrent -a udp://tracker.example.org:6969/announce holiday.mp4
```

Make a torrent of several files, with a name, a comment and two trackers:

```
torrentinfo -o photos.torrent -n "Summer photos" -c "From the trip" \
    -a https://tracker.example.org/announce \
    -a udp://backup.example.net:6969/announce \
    beach.jpg harbour.jpg sunset.jpg
```

Block a range of addresses and download to your Downloads folder by
default (Linux and FreeBSD; on macOS the files are in
`~/Library/Application Support/torrentinfo`, on Windows in
`%APPDATA%\torrentinfo`):

```
echo "198.51.100.0/24" >> ~/.torrentinfo/blocklist.txt
echo "download_directory = ~/Downloads" >> ~/.torrentinfo/config
torrentinfo -D ubuntu.torrent
```

Download at no more than 1 MiB/s, then seed at no more than 200 KiB/s:

```
torrentinfo -D -S -d 1M -u 200k ubuntu.torrent
```

Then share it by seeding from the folder that holds the files:

```
torrentinfo -D -S -O . photos.torrent
```

Download two torrents and a magnet link at once, at no more than 2 MiB/s
in all:

```
torrentinfo -D -d 2M -O ~/Downloads ubuntu.torrent debian.torrent "magnet:?xt=urn:btih:..."
```

## SEE ALSO

*BUILDING.md*, which explains how to build the program on Linux, FreeBSD,
macOS (experimental) and Windows.
