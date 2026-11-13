# torrentinfo

This code is written by Claude Opus 5.5 prompted from the chat web interface, deliverable we obtained as .tgz archives, compiled and tested on Linux and Windows 11.

The intent is a SMALL and FAST Torrent client library targeting Windows, Linux, FreeBSD and macOS with a CLI.

The Library is written entirely in C, has very few dependancies, Async IO provided by the libuv library and SSL through OpenSSL. 

To provide Async IO libuv uses epoll on Linux, kqueue on BSD/macOS and on Windows I/O Completion Ports (IOCP).

The original intent was accomplished within a week, and each iteration was fairly short, the current client is fairly comprehensive in its support, take a look at the usage documentation for examples.

Future direction may include a service/daemon, interaction with the service/daemon through shared memory and a UI.

See **[Usage Instructions](USAGE.md)**

See **[Build Instructions](BUILDING.md)**
