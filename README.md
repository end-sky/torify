# Torify Prototype

A deliberately small Linux prototype inspired by Orbot's **Connect / Disconnect** flow. The Python implementation has a small Tkinter GUI. The C++ and Rust versions provide equivalent dependency-light command-line controllers.

> **Important limitation:** Tor is a proxy, not a general-purpose VPN interface. Linux needs kernel firewall rules to redirect system traffic into Tor. This prototype uses the `nft` command from **nftables**, so a literally Tor-only installation is not sufficient. The Tor manual documents that `TransPort` requires operating-system transparent-proxy support; Tor handles TCP streams, not arbitrary UDP/ICMP traffic. See the [Tor manual](https://manpages.debian.org/unstable/tor/tor.1.en.html) and [ArchWiki's transparent torification notes](https://wiki.archlinux.org/title/tor).

## What this prototype does

- Starts its own Tor client on local ports `19500` (SOCKS), `19540` (transparent TCP), and `19553` (DNS), avoiding Tor's usual default ports.
- Installs one isolated nftables table, `inet torify_app`. It does **not** flush or replace the system's other firewall rules.
- Redirects outbound TCP and DNS requests to Tor. Unsupported public UDP and other protocols are dropped while connected to avoid silently sending them outside Tor.
- Keeps the firewall active if Tor fails to start or bootstrap. This is intentionally fail-closed: use **Disconnect** to restore normal routing.
- Leaves loopback and common private/link-local/multicast destinations available for local network operation. Traffic to those destinations is **not** sent through Tor.
- Starts Tor under a dedicated service account so unrelated processes belonging to your desktop user do not inherit the firewall exception. It searches for `tor`, `debian-tor`, `_tor`, `toranon`, or `torify-tor`; if no dedicated account is present, Connect refuses to run rather than using the shared `nobody` account.

## Limitations and cautions

This is a prototype, not a hardened anonymity product. There are no bridges, no pluggable transports, and no per-app split-tunnelling controls. It does not provide a real TUN/VPN device and only handles locally generated host traffic, not forwarded/container traffic. Public UDP, ICMP, QUIC/HTTP/3 over UDP, and other non-TCP protocols are blocked while the table is active; apps that rely on them may fail. TCP DNS is redirected to Tor's DNSPort but resolver features outside Tor DNSPort's supported record types may not work. The private/LAN address exceptions are intentional bypasses.

A system administrator or root process can change the firewall, and transparent torification is not a replacement for a purpose-built isolation environment such as Whonix or Tails. Do not rely on this prototype for high-risk anonymity.

## Dependencies

- `tor`
- `nftables` (`nft` command) — required to install/remove kernel firewall rules
- Root permission through `sudo`, or launch the application as root. When run as root, the app uses `setpriv` from `util-linux` (or `sudo` as a fallback) to drop Tor to its service account.
- Python 3 + Tkinter for the GUI, or a C++17 compiler / Rust toolchain for the CLI variants
- Standard system utilities: coreutils, `grep`, `kill` from `procps`, and `setpriv` from `util-linux`

No Python pip packages or Rust crates are required. **nftables is an unavoidable additional dependency in this prototype**; using only the Tor package would require replacing the nft command integration with a bundled kernel-netlink implementation (not included here).

## Run the Python GUI

Launch from a terminal so `sudo` can prompt when you press Connect:

```sh
python3 python/torify.py
```

Press **Connect** to authenticate, install the kill-switch, start Tor, and wait for Tor to bootstrap. Press **Disconnect** to stop the prototype Tor process and remove its table. The GUI intentionally keeps authentication prompts in the terminal that launched it.

The Python implementation also has a CLI:

```sh
python3 python/torify.py status
python3 python/torify.py connect
python3 python/torify.py disconnect
```

## C++ CLI

```sh
c++ -std=c++17 -O2 -Wall -Wextra -pedantic cpp/torify.cpp -o torify-cpp
./torify-cpp status
./torify-cpp connect
./torify-cpp disconnect
```

## Rust CLI

Rust standard library only; there are no Cargo dependencies:

```sh
cd rust
cargo build --release
./target/release/torify-rs status
./target/release/torify-rs connect
./target/release/torify-rs disconnect
```

## NixOS / Nix flake

The flake's default package is the Python GUI. It also exposes `packages.cpp` and `packages.rust` for the other variants:

```sh
nix build
nix run
nix build .#cpp
nix build .#rust
```

The application still needs a dedicated Tor service account at runtime. The flake supplies `tor`, `nftables`, and the small set of helper utilities used by the implementation; this avoids extra Python pip packages or Rust crates, but it cannot eliminate the kernel firewall requirement. If the `tor` package on your system does not create one, add a dedicated account to your NixOS configuration (the app recognizes `torify-tor`):

```nix
{
  users.groups.torify-tor = {};
  users.users.torify-tor = {
    isSystemUser = true;
    group = "torify-tor";
    home = "/var/empty";
    createHome = false;
  };

  environment.systemPackages = with pkgs; [
    tor
    nftables
    python3
    python3Packages.tkinter
  ];
}
```

If using the GUI, run it from a terminal via `nix run .` so the privilege prompt has a terminal available. The app uses the account's UID in nftables to exempt only Tor's own outbound connections.

## Recovery if the app exits unexpectedly

The firewall table is designed to remain active if Tor dies; this prevents ordinary traffic from silently reverting to direct connections. To deliberately restore ordinary network routing even if the app is no longer running, remove **only this prototype's table**:

```sh
sudo nft delete table inet torify_app
```

This command removes the prototype's routing guard and restores the system's existing routing/firewall behavior. It does not remove unrelated nftables tables. To inspect the active rules first:

```sh
sudo nft list table inet torify_app
```

## Project layout

```text
python/torify.py       Tkinter GUI + Python controller
cpp/torify.cpp         C++17 CLI controller
rust/src/main.rs       Rust CLI controller
flake.nix              Nix package and development shell
```
