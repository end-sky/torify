// Dependency-light C++ CLI controller for Torify Prototype.
// Build: c++ -std=c++17 -O2 -Wall -Wextra -pedantic torify.cpp -o torify-cpp
#include <chrono>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {
constexpr int SOCKS_PORT = 19500;
constexpr int TRANS_PORT = 19540;
constexpr int DNS_PORT = 19553;
constexpr const char* TABLE_FAMILY = "inet";
constexpr const char* TABLE_NAME = "torify_app";
constexpr int BOOTSTRAP_TIMEOUT = 180;

std::string uid_text() { return std::to_string(static_cast<unsigned long>(getuid())); }
std::string torrc_path() { return "/tmp/torify-prototype-" + uid_text() + ".torrc"; }
std::string rules_path() { return "/tmp/torify-prototype-" + uid_text() + ".rules.nft"; }
std::string data_dir() { return "/var/tmp/torify-prototype-" + uid_text(); }
std::string pid_path() { return data_dir() + "/tor.pid"; }
std::string log_path() { return data_dir() + "/tor.log"; }

std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

std::string find_binary(const std::string& name, bool required = true) {
    FILE* pipe = popen(("command -v " + name + " 2>/dev/null").c_str(), "r");
    if (!pipe) return required ? name : "";
    char buf[4096];
    std::string result;
    if (fgets(buf, sizeof(buf), pipe)) result = buf;
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    if (result.empty() && required) throw std::runtime_error("Required command not found: " + name);
    return result;
}

std::string root_prefix() { return geteuid() == 0 ? "" : find_binary("sudo") + " -n "; }

int run(const std::string& command) {
    int status = std::system(command.c_str());
    if (status == -1) return 127;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128;
}

std::string capture(const std::string& command) {
    FILE* pipe = popen((command + " 2>/dev/null").c_str(), "r");
    if (!pipe) return "";
    char buf[4096];
    std::string out;
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    return out;
}

void safe_write_file(const std::string& path, const std::string& contents, mode_t mode = 0644) {
    struct stat st{};
    if (lstat(path.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode))
            throw std::runtime_error("Refusing to overwrite a non-regular path: " + path);
        if (st.st_uid != getuid() && st.st_uid != 0)
            throw std::runtime_error("Refusing to overwrite a file not owned by this user: " + path);
        if (unlink(path.c_str()) != 0) throw std::runtime_error("Could not remove old file: " + path);
    } else if (errno != ENOENT) {
        throw std::runtime_error("Could not inspect path: " + path);
    }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (fd < 0) throw std::runtime_error("Could not safely create file: " + path + ": " + std::strerror(errno));
    size_t offset = 0;
    while (offset < contents.size()) {
        ssize_t written = write(fd, contents.data() + offset, contents.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { close(fd); unlink(path.c_str()); throw std::runtime_error("Could not write file: " + path); }
        offset += static_cast<size_t>(written);
    }
    if (fchmod(fd, mode) != 0) { close(fd); throw std::runtime_error("Could not set file mode: " + path); }
    close(fd);
}

void verify_data_path() {
    struct stat st{};
    if (lstat(data_dir().c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
            throw std::runtime_error("Refusing to use a non-directory Tor data path: " + data_dir());
    } else if (errno != ENOENT) {
        throw std::runtime_error("Could not inspect Tor data directory path.");
    }
}

struct Account { unsigned long uid = 65534; unsigned long gid = 65534; std::string name = "nobody"; bool dedicated = false; };
Account find_tor_account() {
    const std::vector<std::string> names = {"tor", "debian-tor", "_tor", "toranon", "torify-tor"};
    std::ifstream in("/etc/passwd");
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    for (const auto& wanted : names) {
        for (const auto& row : lines) {
            std::stringstream ss(row);
            std::string name, password, uid, gid, rest;
            if (!std::getline(ss, name, ':') || !std::getline(ss, password, ':') ||
                !std::getline(ss, uid, ':') || !std::getline(ss, gid, ':')) continue;
            if (name != wanted) continue;
            try {
                Account a{std::stoul(uid), std::stoul(gid), name, true};
                if (a.uid != 0) return a;
            } catch (...) { }
        }
    }
    return {};
}

Account ACCOUNT = find_tor_account();

std::string torrc_text() {
    std::ostringstream s;
    s << "# Generated by Torify Prototype. No secrets are stored here.\n"
      << "DataDirectory " << data_dir() << "\n"
      << "PidFile " << pid_path() << "\n"
      << "RunAsDaemon 1\n"
      << "SocksPort 127.0.0.1:" << SOCKS_PORT << "\n"
      << "SocksPort [::1]:" << SOCKS_PORT << "\n"
      << "TransPort 127.0.0.1:" << TRANS_PORT << "\n"
      << "TransPort [::1]:" << TRANS_PORT << "\n"
      << "DNSPort 127.0.0.1:" << DNS_PORT << "\n"
      << "DNSPort [::1]:" << DNS_PORT << "\n"
      << "SafeSocks 1\nAutomapHostsOnResolve 1\n"
      << "VirtualAddrNetworkIPv4 10.192.0.0/10\n"
      << "Log notice file " << log_path() << "\n";
    return s.str();
}

std::string nft_rules() {
    std::ostringstream s;
    const auto ipv4 = "{ 0.0.0.0/8, 10.0.0.0/8, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.168.0.0/16, 224.0.0.0/4, 255.255.255.255 }";
    const auto ipv6 = "{ ::1, fc00::/7, fe80::/10, ff00::/8 }";
    s << "table " << TABLE_FAMILY << " " << TABLE_NAME << " {\n"
      << " chain output_nat { type nat hook output priority -100; policy accept;\n"
      << "  meta skuid " << ACCOUNT.uid << " return\n  oifname \"lo\" return\n"
      << "  ip daddr " << ipv4 << " return\n  ip6 daddr " << ipv6 << " return\n"
      << "  udp dport 53 redirect to :" << DNS_PORT << "\n"
      << "  tcp dport 53 redirect to :" << DNS_PORT << "\n"
      << "  meta l4proto tcp redirect to :" << TRANS_PORT << "\n }\n"
      << " chain output_guard { type filter hook output priority 0; policy drop;\n"
      << "  meta skuid " << ACCOUNT.uid << " accept\n  oifname \"lo\" accept\n"
      << "  ip daddr " << ipv4 << " accept\n  ip6 daddr " << ipv6 << " accept\n"
      << "  udp sport 68 udp dport 67 accept\n  udp sport 546 udp dport 547 accept\n"
      << "  meta l4proto ipv6-icmp icmpv6 type { nd-router-solicit, nd-neighbor-solicit, nd-neighbor-advert, nd-router-advert } accept\n"
      << " }\n}\n";
    return s.str();
}

bool table_exists() {
    std::string cmd = root_prefix() + find_binary("nft") + " list table " + TABLE_FAMILY + " " + TABLE_NAME;
    return run(cmd) == 0;
}

std::string read_pid() {
    std::string cmd = root_prefix() + find_binary("cat") + " " + quote(pid_path());
    std::string s = capture(cmd);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s == "0" || s == "1") return "";
    return s;
}

bool process_matches(const std::string& pid) {
    if (pid.empty() || pid == "0" || pid == "1" || pid.find_first_not_of("0123456789") != std::string::npos) return false;
    const std::string stat_bin = find_binary("stat", false);
    const std::string readlink_bin = find_binary("readlink", false);
    if (stat_bin.empty() || readlink_bin.empty()) return false;
    std::string owner = capture(root_prefix() + quote(stat_bin) + " -c %u /proc/" + pid);
    while (!owner.empty() && (owner.back() == '\n' || owner.back() == '\r')) owner.pop_back();
    std::string exe = capture(root_prefix() + quote(readlink_bin) + " " + quote("/proc/" + pid + "/exe"));
    while (!exe.empty() && (exe.back() == '\n' || exe.back() == '\r')) exe.pop_back();
    constexpr const char* deleted = " (deleted)";
    if (exe.size() >= std::strlen(deleted) && exe.compare(exe.size() - std::strlen(deleted), std::strlen(deleted), deleted) == 0)
        exe.erase(exe.size() - std::strlen(deleted));
    const auto slash = exe.find_last_of('/');
    const std::string base = slash == std::string::npos ? exe : exe.substr(slash + 1);
    return owner == std::to_string(ACCOUNT.uid) && base == "tor";
}

bool bootstrapped() {
    std::string cmd = root_prefix() + find_binary("grep") + " -Fq -- 'Bootstrapped 100%' " + quote(log_path());
    return run(cmd) == 0;
}

std::string log_tail() {
    return capture(root_prefix() + find_binary("tail") + " -n 20 " + quote(log_path()));
}

void authenticate() {
    if (geteuid() == 0) return;
    std::string sudo = find_binary("sudo");
    if (run(sudo + " -v") != 0) throw std::runtime_error("Could not obtain administrator permission. Run from a terminal so sudo can prompt.");
}

void prepare_tor() {
    auto install = find_binary("install");
    auto chown = find_binary("chown");
    auto chmod_bin = find_binary("chmod");
    auto rm = find_binary("rm");
    safe_write_file(torrc_path(), torrc_text(), 0644);
    verify_data_path();
    if (run(root_prefix() + install + " -d -o " + std::to_string(ACCOUNT.uid) + " -g " + std::to_string(ACCOUNT.gid) + " -m 700 " + quote(data_dir())) != 0)
        throw std::runtime_error("Could not prepare Tor's private data directory.");
    verify_data_path();
    if (run(root_prefix() + chown + " -R " + std::to_string(ACCOUNT.uid) + ":" + std::to_string(ACCOUNT.gid) + " " + quote(data_dir())) != 0 ||
        run(root_prefix() + chmod_bin + " 700 " + quote(data_dir())) != 0)
        throw std::runtime_error("Could not set Tor data-directory permissions.");
    run(root_prefix() + rm + " -f " + quote(pid_path()));
}

void launch_tor() {
    const std::string tor = find_binary("tor");
    std::string launch;
    if (geteuid() == 0) {
        const std::string setpriv = find_binary("setpriv", false);
        if (!setpriv.empty()) {
            launch = quote(setpriv) + " --reuid=" + std::to_string(ACCOUNT.uid) + " --regid=" + std::to_string(ACCOUNT.gid) + " --clear-groups " + quote(tor) + " -f " + quote(torrc_path());
        } else {
            const std::string sudo = find_binary("sudo");
            launch = sudo + " -u '#" + std::to_string(ACCOUNT.uid) + "' -g '#" + std::to_string(ACCOUNT.gid) + "' " + quote(tor) + " -f " + quote(torrc_path());
        }
    } else {
        const std::string sudo = find_binary("sudo");
        launch = sudo + " -n -u '#" + std::to_string(ACCOUNT.uid) + "' -g '#" + std::to_string(ACCOUNT.gid) + "' " + quote(tor) + " -f " + quote(torrc_path());
    }
    if (run(launch) != 0)
        throw std::runtime_error("Tor failed to start. The firewall kill-switch remains active; click Disconnect to restore normal networking.");
    for (int i = 0; i < 30; ++i) {
        std::string pid = read_pid();
        if (process_matches(pid)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    throw std::runtime_error("Tor did not create a verifiable process. The firewall kill-switch remains active; click Disconnect to restore normal networking.");
}

std::string status_text() {
    if (geteuid() != 0) {
        const std::string sudo = find_binary("sudo");
        if (run(sudo + " -n true") != 0) return "Status cannot be verified until sudo credentials are available. Run sudo -v in the terminal, then check again.";
    }
    if (!table_exists()) return "Disconnected: no torify_app firewall table is installed.";
    std::string pid = read_pid();
    if (process_matches(pid)) {
        if (bootstrapped()) return "Connected (Tor PID " + pid + "); firewall kill-switch table is active.";
        return "Tor is running (PID " + pid + ") but bootstrap is incomplete; the kill-switch is active.";
    }
    return "Kill-switch table is active, but the Tor process was not verified. Use Disconnect or the recovery command in README.md.";
}

void connect_tor() {
    (void)find_binary("tor");
    const std::string nft = find_binary("nft");
    if (!ACCOUNT.dedicated) throw std::runtime_error("No dedicated Tor service account was found (expected tor, debian-tor, _tor, toranon, or torify-tor). This prototype refuses to use the shared nobody account because its other processes could bypass torification.");
    authenticate();
    if (table_exists()) {
        std::string pid = read_pid();
        if (process_matches(pid)) {
            std::cout << (bootstrapped() ? "Already connected.\n" : "Firewall is active; Tor has not finished bootstrapping.\n");
            return;
        }
        throw std::runtime_error("inet torify_app already exists but Tor was not verified. Use Disconnect before reconnecting.");
    }
    prepare_tor();
    safe_write_file(rules_path(), nft_rules(), 0644);
    if (run(root_prefix() + nft + " -f " + quote(rules_path())) != 0) {
        run(find_binary("rm") + " -f " + quote(rules_path()));
        throw std::runtime_error("nftables rejected the rules. No torify_app table was installed and Tor was not started.");
    }
    run(find_binary("rm") + " -f " + quote(rules_path()));

    // Firewall goes on before Tor starts, so a startup failure remains fail-closed.
    launch_tor();
    for (int i = 0; i < BOOTSTRAP_TIMEOUT / 2; ++i) {
        if (bootstrapped()) {
            std::cout << "Connected to Tor. TCP and DNS are routed through Tor; unsupported public UDP and other protocols are blocked.\n";
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    throw std::runtime_error("Tor did not bootstrap within 3 minutes. The firewall kill-switch remains active and public traffic is blocked rather than sent directly. Click Disconnect to restore normal networking.\n" + log_tail());
}

void disconnect_tor() {
    const std::string nft = find_binary("nft");
    authenticate();
    std::string pid = read_pid();
    if (process_matches(pid)) {
        run(root_prefix() + find_binary("kill") + " -TERM " + pid);
        bool alive = true;
        for (int i = 0; i < 25; ++i) {
            if (run(root_prefix() + find_binary("kill") + " -0 " + pid) != 0) { alive = false; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (alive && process_matches(pid))
            run(root_prefix() + find_binary("kill") + " -KILL " + pid);
    }
    if (table_exists()) {
        if (run(root_prefix() + nft + " delete table " + TABLE_FAMILY + " " + TABLE_NAME) != 0)
            throw std::runtime_error("Could not remove inet torify_app. Network may remain blocked; see README recovery instructions.");
    }
    run(root_prefix() + find_binary("rm") + " -f " + quote(pid_path()) + " " + quote(torrc_path()) + " " + quote(rules_path()));
    std::cout << "Disconnected. This app's firewall table was removed; normal network routing is restored.\n";
}
}

int main(int argc, char** argv) {
    try {
        const std::string action = argc > 1 ? argv[1] : "status";
        if (action == "connect") connect_tor();
        else if (action == "disconnect") disconnect_tor();
        else if (action == "status") std::cout << status_text() << "\n";
        else {
            std::cerr << "Usage: torify-cpp [connect|disconnect|status]\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
