//! Dependency-light Rust CLI controller for Torify Prototype.
//! Build: cargo build --release
use std::env;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::process::{Command, Output};
use std::thread;
use std::time::Duration;

const SOCKS_PORT: u16 = 19500;
const TRANS_PORT: u16 = 19540;
const DNS_PORT: u16 = 19553;
const TABLE_FAMILY: &str = "inet";
const TABLE_NAME: &str = "torify_app";
const BOOTSTRAP_TIMEOUT: u64 = 180;

#[derive(Clone)]
struct Account { uid: u32, gid: u32, _name: String, dedicated: bool }

fn run_shell(command: &str) -> Output {
    Command::new("sh").arg("-c").arg(command).output().expect("failed to start shell")
}

fn find_binary(name: &str) -> Result<String, String> {
    // Search PATH directly instead of `command -v`, which may return shell built-ins such as `kill`.
    if name.contains('/') {
        let p = std::path::Path::new(name);
        if p.is_file() { return Ok(name.to_string()); }
    } else if let Some(path) = env::var_os("PATH") {
        for dir in env::split_paths(&path) {
            let candidate = dir.join(name);
            if let Ok(metadata) = fs::metadata(&candidate) {
                if metadata.is_file() && metadata.permissions().mode() & 0o111 != 0 {
                    return Ok(candidate.to_string_lossy().into_owned());
                }
            }
        }
    }
    Err(format!("Required command not found: {name}"))
}

fn quote(s: &str) -> String {
    format!("'{}'", s.replace('\'', "'\\''"))
}

fn safe_write(path: &str, contents: &str, mode: u32) -> Result<(), String> {
    match fs::symlink_metadata(path) {
        Ok(meta) => {
            if meta.file_type().is_symlink() || !meta.is_file() {
                return Err(format!("Refusing to overwrite a non-regular path: {path}"));
            }
            if meta.uid() != real_uid() && meta.uid() != 0 {
                return Err(format!("Refusing to overwrite a file not owned by this user: {path}"));
            }
            fs::remove_file(path).map_err(|e| format!("Could not remove old file {path}: {e}"))?;
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {},
        Err(e) => return Err(format!("Could not inspect path {path}: {e}")),
    }
    // create_new is O_EXCL on Linux, so a race that inserts a symlink cannot be followed.
    let mut file = OpenOptions::new().write(true).create_new(true).mode(mode).open(path)
        .map_err(|e| format!("Could not safely create {path}: {e}"))?;
    file.write_all(contents.as_bytes()).map_err(|e| format!("Could not write {path}: {e}"))?;
    file.set_permissions(fs::Permissions::from_mode(mode)).map_err(|e| format!("Could not set permissions on {path}: {e}"))?;
    Ok(())
}

fn verify_data_path(path: &str) -> Result<(), String> {
    match fs::symlink_metadata(path) {
        Ok(meta) if meta.file_type().is_symlink() || !meta.is_dir() => {
            Err(format!("Refusing to use a non-directory Tor data path: {path}"))
        }
        Ok(_) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(format!("Could not inspect Tor data path {path}: {e}")),
    }
}

fn real_uid() -> u32 {
    String::from_utf8_lossy(&run_shell("id -ru").stdout).trim().parse().unwrap_or(0)
}

fn effective_uid() -> u32 {
    String::from_utf8_lossy(&run_shell("id -u").stdout).trim().parse().unwrap_or(0)
}

fn paths() -> (String, String, String, String, String) {
    let uid = real_uid();
    let data = format!("/var/tmp/torify-prototype-{uid}");
    (data.clone(), format!("/tmp/torify-prototype-{uid}.torrc"),
     format!("/tmp/torify-prototype-{uid}.rules.nft"), format!("{data}/tor.pid"), format!("{data}/tor.log"))
}

fn account() -> Account {
    let choices = ["tor", "debian-tor", "_tor", "toranon", "torify-tor"];
    if let Ok(contents) = fs::read_to_string("/etc/passwd") {
        for wanted in choices {
            for line in contents.lines() {
                let fields: Vec<&str> = line.split(':').collect();
                if fields.len() >= 4 && fields[0] == wanted {
                    if let (Ok(uid), Ok(gid)) = (fields[2].parse::<u32>(), fields[3].parse::<u32>()) {
                        if uid != 0 { return Account { uid, gid, _name: wanted.to_string(), dedicated: true }; }
                    }
                }
            }
        }
    }
    Account { uid: 65534, gid: 65534, _name: "unavailable".into(), dedicated: false }
}

fn root_command(command: &str) -> String {
    if effective_uid() == 0 { command.to_string() }
    else { format!("{} -n {command}", find_binary("sudo").unwrap_or_else(|_| "sudo".into())) }
}

fn run(command: &str) -> bool { run_shell(command).status.success() }
fn capture(command: &str) -> String { String::from_utf8_lossy(&run_shell(command).stdout).trim().to_string() }

fn authenticate() -> Result<(), String> {
    if effective_uid() == 0 { return Ok(()); }
    let sudo = find_binary("sudo")?;
    let status = Command::new(sudo).arg("-v").status().map_err(|e| format!("Could not run sudo: {e}"))?;
    if status.success() { Ok(()) }
    else { Err("Could not obtain administrator permission. Run from a terminal so sudo can prompt.".into()) }
}

fn torrc_text(data: &str, pid: &str, log: &str) -> String {
    format!("# Generated by Torify Prototype. No secrets are stored here.\n\
DataDirectory {data}\nPidFile {pid}\nRunAsDaemon 1\n\
SocksPort 127.0.0.1:{SOCKS_PORT}\nSocksPort [::1]:{SOCKS_PORT}\n\
TransPort 127.0.0.1:{TRANS_PORT}\nTransPort [::1]:{TRANS_PORT}\n\
DNSPort 127.0.0.1:{DNS_PORT}\nDNSPort [::1]:{DNS_PORT}\n\
SafeSocks 1\nAutomapHostsOnResolve 1\nVirtualAddrNetworkIPv4 10.192.0.0/10\nLog notice file {log}\n")
}

fn rules_text(a: &Account) -> String {
    let ipv4 = "{ 0.0.0.0/8, 10.0.0.0/8, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.168.0.0/16, 224.0.0.0/4, 255.255.255.255 }";
    let ipv6 = "{ ::1, fc00::/7, fe80::/10, ff00::/8 }";
    format!("table {TABLE_FAMILY} {TABLE_NAME} {{\n\
 chain output_nat {{ type nat hook output priority -100; policy accept;\n\
  meta skuid {} return\n  oifname \"lo\" return\n\
  ip daddr {ipv4} return\n  ip6 daddr {ipv6} return\n\
  udp dport 53 redirect to :{DNS_PORT}\n  tcp dport 53 redirect to :{DNS_PORT}\n\
  meta l4proto tcp redirect to :{TRANS_PORT}\n }}\n\
 chain output_guard {{ type filter hook output priority 0; policy drop;\n\
  meta skuid {} accept\n  oifname \"lo\" accept\n\
  ip daddr {ipv4} accept\n  ip6 daddr {ipv6} accept\n\
  udp sport 68 udp dport 67 accept\n  udp sport 546 udp dport 547 accept\n\
  meta l4proto ipv6-icmp icmpv6 type {{ nd-router-solicit, nd-neighbor-solicit, nd-neighbor-advert, nd-router-advert }} accept\n }}\n}}\n", a.uid, a.uid)
}

fn table_exists() -> bool {
    let nft = match find_binary("nft") { Ok(s) => s, Err(_) => return false };
    run(&root_command(&format!("{} list table {} {}", quote(&nft), TABLE_FAMILY, TABLE_NAME)))
}

fn read_pid(pid_file: &str) -> String {
    let cat = find_binary("cat").unwrap_or_else(|_| "cat".into());
    let value = capture(&root_command(&format!("{} {}", quote(&cat), quote(pid_file))));
    if value.is_empty() || !value.chars().all(|c| c.is_ascii_digit()) || value == "0" || value == "1" { String::new() } else { value }
}

fn process_matches(pid: &str) -> bool {
    if pid.is_empty() || pid == "0" || pid == "1" || !pid.chars().all(|c| c.is_ascii_digit()) { return false; }
    let stat_bin = match find_binary("stat") { Ok(s) => s, Err(_) => return false };
    let readlink_bin = match find_binary("readlink") { Ok(s) => s, Err(_) => return false };
    let owner = capture(&root_command(&format!("{} -c %u /proc/{}", quote(&stat_bin), pid)));
    let mut exe = capture(&root_command(&format!("{} {}", quote(&readlink_bin), quote(&format!("/proc/{pid}/exe")))));
    if let Some(stripped) = exe.strip_suffix(" (deleted)") { exe = stripped.to_string(); }
    let basename = std::path::Path::new(&exe).file_name().and_then(|s| s.to_str()).unwrap_or("");
    owner == account().uid.to_string() && basename == "tor"
}

fn bootstrapped(log: &str) -> bool {
    let grep = find_binary("grep").unwrap_or_else(|_| "grep".into());
    run(&root_command(&format!("{} -Fq -- 'Bootstrapped 100%' {}", quote(&grep), quote(log))))
}

fn log_tail(log: &str) -> String {
    let tail = find_binary("tail").unwrap_or_else(|_| "tail".into());
    capture(&root_command(&format!("{} -n 20 {}", quote(&tail), quote(log))))
}

fn prepare_tor() -> Result<(), String> {
    let account = account();
    let (data, torrc, _rules, pid_file, log) = paths();
    let install = find_binary("install")?;
    let chown = find_binary("chown")?;
    let chmod = find_binary("chmod")?;
    let rm = find_binary("rm")?;
    safe_write(&torrc, &torrc_text(&data, &pid_file, &log), 0o644)?;
    verify_data_path(&data)?;
    if !run(&root_command(&format!("{} -d -o {} -g {} -m 700 {}", quote(&install), account.uid, account.gid, quote(&data)))) {
        return Err("Could not prepare Tor's private data directory.".into());
    }
    verify_data_path(&data)?;
    if !run(&root_command(&format!("{} -R {}:{} {}", quote(&chown), account.uid, account.gid, quote(&data)))) ||
       !run(&root_command(&format!("{} 700 {}", quote(&chmod), quote(&data)))) {
        return Err("Could not set Tor data-directory permissions.".into());
    }
    run(&root_command(&format!("{} -f {}", quote(&rm), quote(&pid_file))));
    Ok(())
}

fn launch_tor() -> Result<(), String> {
    let account = account();
    let (_, torrc, _, pid_file, _) = paths();
    let tor = find_binary("tor")?;
    let launch = if effective_uid() == 0 {
        if let Ok(setpriv) = find_binary("setpriv") {
            format!("{} --reuid={} --regid={} --clear-groups {} -f {}", quote(&setpriv), account.uid, account.gid, quote(&tor), quote(&torrc))
        } else {
            let sudo = find_binary("sudo")?;
            format!("{} -u '#{}' -g '#{}' {} -f {}", quote(&sudo), account.uid, account.gid, quote(&tor), quote(&torrc))
        }
    } else {
        let sudo = find_binary("sudo")?;
        format!("{} -n -u '#{}' -g '#{}' {} -f {}", quote(&sudo), account.uid, account.gid, quote(&tor), quote(&torrc))
    };
    if !run(&launch) {
        return Err("Tor failed to start. The firewall kill-switch remains active; click Disconnect to restore ordinary networking.".into());
    }
    for _ in 0..30 {
        let pid = read_pid(&pid_file);
        if process_matches(&pid) { return Ok(()); }
        thread::sleep(Duration::from_millis(500));
    }
    Err("Tor did not create a verifiable process. The firewall kill-switch remains active; click Disconnect to restore ordinary networking.".into())
}

fn status_text() -> String {
    if effective_uid() != 0 {
        let sudo = find_binary("sudo").unwrap_or_else(|_| "sudo".into());
        if !run(&format!("{} -n true", quote(&sudo))) {
            return "Status cannot be verified until sudo credentials are available. Run sudo -v in the terminal, then check again.".into();
        }
    }
    let (_, _, _, pid_file, log) = paths();
    if !table_exists() { return "Disconnected: no torify_app firewall table is installed.".into(); }
    let pid = read_pid(&pid_file);
    if process_matches(&pid) {
        if bootstrapped(&log) { format!("Connected (Tor PID {pid}); firewall kill-switch table is active.") }
        else { format!("Tor is running (PID {pid}) but bootstrap is incomplete; the kill-switch is active.") }
    } else { "Kill-switch table is active, but Tor was not verified. Use Disconnect or the README recovery command.".into() }
}

fn connect() -> Result<String, String> {
    let _ = find_binary("tor")?;
    let nft = find_binary("nft")?;
    if !account().dedicated {
        return Err("No dedicated Tor service account was found (expected tor, debian-tor, _tor, toranon, or torify-tor). This prototype refuses to use the shared nobody account because its other processes could bypass torification.".into());
    }
    authenticate()?;
    if table_exists() {
        let (_, _, _, pid_file, log) = paths();
        let pid = read_pid(&pid_file);
        if process_matches(&pid) {
            return Ok(if bootstrapped(&log) { "Already connected; firewall table is active.".into() }
                      else { "Firewall is active; Tor has not finished bootstrapping.".into() });
        }
        return Err("inet torify_app already exists but Tor was not verified. Use Disconnect before reconnecting.".into());
    }
    prepare_tor()?;
    let tor_account = account();
    let (_, _, rules, _, log) = paths();
    safe_write(&rules, &rules_text(&tor_account), 0o644)?;
    if !run(&root_command(&format!("{} -f {}", quote(&nft), quote(&rules)))) {
        let rm = find_binary("rm").unwrap_or_else(|_| "rm".into());
        run(&format!("{} -f {}", quote(&rm), quote(&rules)));
        return Err("nftables rejected the rules. No torify_app table was installed and Tor was not started.".into());
    }
    let rm = find_binary("rm").unwrap_or_else(|_| "rm".into());
    run(&format!("{} -f {}", quote(&rm), quote(&rules)));

    // Install firewall rules before Tor starts so start/bootstrap failures remain fail-closed.
    launch_tor()?;
    for _ in 0..(BOOTSTRAP_TIMEOUT / 2) {
        if bootstrapped(&log) {
            return Ok("Connected to Tor. TCP and DNS use Tor; unsupported public UDP and other protocols are blocked.".into());
        }
        thread::sleep(Duration::from_secs(2));
    }
    Err(format!("Tor did not bootstrap within 3 minutes. The firewall kill-switch remains active and public traffic is blocked rather than sent directly. Click Disconnect to restore normal networking.\n{}", log_tail(&log)))
}

fn disconnect() -> Result<String, String> {
    let nft = find_binary("nft")?;
    authenticate()?;
    let (_, torrc, rules, pid_file, _) = paths();
    let pid = read_pid(&pid_file);
    if process_matches(&pid) {
        let kill = find_binary("kill")?;
        run(&root_command(&format!("{} -TERM {}", quote(&kill), pid)));
        let mut alive = true;
        for _ in 0..25 {
            if !run(&root_command(&format!("{} -0 {}", quote(&kill), pid))) { alive = false; break; }
            thread::sleep(Duration::from_millis(200));
        }
        if alive && process_matches(&pid) {
            run(&root_command(&format!("{} -KILL {}", quote(&kill), pid)));
        }
    }
    if table_exists() && !run(&root_command(&format!("{} delete table {} {}", quote(&nft), TABLE_FAMILY, TABLE_NAME))) {
        return Err("Could not remove inet torify_app. Network may remain blocked; see README recovery instructions.".into());
    }
    let rm = find_binary("rm").unwrap_or_else(|_| "rm".into());
    run(&root_command(&format!("{} -f {} {} {}", quote(&rm), quote(&pid_file), quote(&torrc), quote(&rules))));
    Ok("Disconnected. This app's firewall table was removed; normal network routing is restored.".into())
}

fn main() {
    let action = env::args().nth(1).unwrap_or_else(|| "status".into());
    let result = match action.as_str() {
        "connect" => connect(),
        "disconnect" => disconnect(),
        "status" => Ok(status_text()),
        _ => Err("Usage: torify-rs [connect|disconnect|status]".into()),
    };
    match result {
        Ok(message) => println!("{message}"),
        Err(message) => { eprintln!("Error: {message}"); std::process::exit(1); }
    }
}
