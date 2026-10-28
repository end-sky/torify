// Torify's modern GTK GUI with tabs
// Inspired by Orbot - Home, Settings, Preferences tabs
// Build: c++ -std=c++17 -O2 -Wall -Wextra -pedantic $(pkg-config --cflags --libs gtk+-3.0) *.cpp -o torify

#include "torify.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

bool is_file(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) && !error;
}

std::string find_backend() {
    if (const char* explicit_path = std::getenv("TORIFY_BACKEND")) {
        if (is_file(explicit_path)) return explicit_path;
    }

    std::error_code error;
    const fs::path executable = fs::read_symlink("/proc/self/exe", error);
    std::vector<fs::path> candidates;
    if (!error) {
        const fs::path bin_dir = executable.parent_path();
        candidates.emplace_back(bin_dir / ".." / "libexec" / "torify.py");
        candidates.emplace_back(bin_dir / ".." / "python" / "torify.py");
        candidates.emplace_back(bin_dir / "torify.py");
    }
    candidates.emplace_back(fs::current_path() / "python" / "torify.py");
    candidates.emplace_back(fs::current_path() / ".." / "python" / "torify.py");

    for (const auto& candidate : candidates) {
        if (is_file(candidate)) {
            std::error_code canonical_error;
            const auto canonical = fs::weakly_canonical(candidate, canonical_error);
            return (canonical_error ? candidate : canonical).string();
        }
    }
    return {};
}

ProcessResult run_backend(const std::string& backend, const std::string& action) {
    ProcessResult result;
    if (backend.empty()) {
        result.output = "Could not find python/torify.py. Set TORIFY_BACKEND to the backend script path.";
        return result;
    }

    int pipes[2];
    if (pipe(pipes) != 0) {
        result.output = std::string("Could not create backend pipe: ") + std::strerror(errno);
        return result;
    }

    const pid_t child = fork();
    if (child < 0) {
        result.output = std::string("Could not start Python backend: ") + std::strerror(errno);
        close(pipes[0]);
        close(pipes[1]);
        return result;
    }

    if (child == 0) {
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0 || dup2(pipes[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(pipes[1]);
        setenv("PYTHONUNBUFFERED", "1", 1);
        char* const argv[] = {
            const_cast<char*>("python3"),
            const_cast<char*>(backend.c_str()),
            const_cast<char*>(action.c_str()),
            nullptr
        };
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "Could not start python3: %s\n", std::strerror(errno));
        _exit(127);
    }

    close(pipes[1]);
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(pipes[0], buffer, sizeof(buffer));
        if (count > 0) {
            result.output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    close(pipes[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        result.output += std::string("\nCould not wait for backend: ") + std::strerror(errno);
        result.exit_code = 1;
        return result;
    }
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    else result.exit_code = 1;
    return result;
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

void set_buttons_sensitive(const std::shared_ptr<AppState>& state, bool sensitive) {
    gtk_widget_set_sensitive(state->connect_button, sensitive);
    gtk_widget_set_sensitive(state->disconnect_button, sensitive);
    gtk_widget_set_sensitive(state->status_button, sensitive);
}

gboolean finish_action(gpointer user_data) {
    std::unique_ptr<Completion> completion(static_cast<Completion*>(user_data));
    const auto state = completion->state;
    if (!state->alive.load()) return G_SOURCE_REMOVE;

    state->busy = false;
    set_buttons_sensitive(state, true);
    const std::string output = trim(completion->result.output);
    if (completion->result.exit_code == 0) {
        const std::string message = output.empty() ? "Operation completed." : output;
        gtk_label_set_text(GTK_LABEL(state->status_label), message.c_str());
    } else {
        gtk_label_set_text(GTK_LABEL(state->status_label), "Operation failed. See the error details.");
        const std::string details = output.empty() ? "The Python backend exited with an error and returned no details." : output;
        GtkWidget* dialog = gtk_message_dialog_new(
            GTK_WINDOW(state->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE, "%s", details.c_str());
        gtk_window_set_title(GTK_WINDOW(dialog), "Torify");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
    return G_SOURCE_REMOVE;
}

void start_action(const std::shared_ptr<AppState>& state, const std::string& action) {
    if (!state->alive.load() || state->busy) return;
    state->busy = true;
    set_buttons_sensitive(state, false);
    std::string label = action;
    if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    const std::string progress = action == "status"
        ? "Checking status…"
        : label + " in progress… (sudo may prompt in the terminal)";
    gtk_label_set_text(GTK_LABEL(state->status_label), progress.c_str());

    std::thread([state, action]() {
        ProcessResult result = run_backend(state->backend, action);
        if (!state->alive.load()) return;
        auto* completion = new Completion{state, action, std::move(result)};
        g_idle_add_full(G_PRIORITY_DEFAULT, finish_action, completion, nullptr);
    }).detach();
}

void action_button_clicked(GtkButton*, gpointer user_data) {
    const auto* binding = static_cast<ActionBinding*>(user_data);
    start_action(binding->state, binding->action);
}

void destroy_action_binding(gpointer user_data, GClosure*) {
    delete static_cast<ActionBinding*>(user_data);
}

void connect_action(GtkWidget* button, const std::shared_ptr<AppState>& state, const char* action) {
    auto* binding = new ActionBinding{state, action};
    g_signal_connect_data(button, "clicked", G_CALLBACK(action_button_clicked), binding,
                          destroy_action_binding, static_cast<GConnectFlags>(0));
}

void window_destroyed(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    state->alive.store(false);
    gtk_main_quit();
}

GtkWidget* create_home_tab(std::shared_ptr<AppState>& state) {
    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(main_box), 16);
    
    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<span size=\"xx-large\" weight=\"bold\">TORIFY</span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(main_box), title, FALSE, FALSE, 2);
    
    GtkWidget* subtitle = gtk_label_new("System-wide transparent Tor routing prototype");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
    gtk_widget_set_margin_bottom(subtitle, 12);
    gtk_box_pack_start(GTK_BOX(main_box), subtitle, FALSE, FALSE, 0);
    
    state->status_label = gtk_label_new("Checking status…");
    gtk_label_set_xalign(GTK_LABEL(state->status_label), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(state->status_label), 0.0f);
    gtk_label_set_justify(GTK_LABEL(state->status_label), GTK_JUSTIFY_LEFT);
    gtk_label_set_line_wrap(GTK_LABEL(state->status_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(state->status_label), 53);
    gtk_widget_set_size_request(state->status_label, -1, 80);
    gtk_widget_set_margin_bottom(state->status_label, 10);
    gtk_box_pack_start(GTK_BOX(main_box), state->status_label, TRUE, TRUE, 0);
    
    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(main_box), buttons, FALSE, FALSE, 0);
    
    state->connect_button = gtk_button_new_with_label("Connect");
    gtk_widget_set_size_request(state->connect_button, 100, 32);
    gtk_box_pack_start(GTK_BOX(buttons), state->connect_button, FALSE, FALSE, 0);
    connect_action(state->connect_button, state, "connect");
    
    state->disconnect_button = gtk_button_new_with_label("Disconnect");
    gtk_widget_set_size_request(state->disconnect_button, 100, 32);
    gtk_box_pack_start(GTK_BOX(buttons), state->disconnect_button, FALSE, FALSE, 0);
    connect_action(state->disconnect_button, state, "disconnect");
    
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(buttons), spacer, TRUE, TRUE, 0);
    
    state->status_button = gtk_button_new_with_label("Status");
    gtk_widget_set_size_request(state->status_button, 76, 32);
    gtk_box_pack_start(GTK_BOX(buttons), state->status_button, FALSE, FALSE, 0);
    connect_action(state->status_button, state, "status");
    
    GtkWidget* note = gtk_label_new("TCP + DNS only. Public UDP and unsupported traffic are blocked.");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 53);
    gtk_widget_set_margin_top(note, 12);
    gtk_box_pack_start(GTK_BOX(main_box), note, FALSE, FALSE, 0);
    
    return main_box;
}

std::shared_ptr<AppState> create_window() {
    auto state = std::make_shared<AppState>();
    state->backend = find_backend();
    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state->window), "Torify");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 450, 380);
    gtk_window_set_resizable(GTK_WINDOW(state->window), TRUE);
    g_signal_connect(state->window, "destroy", G_CALLBACK(window_destroyed), state.get());
    
    state->notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(state->notebook), GTK_POS_TOP);
    gtk_container_add(GTK_CONTAINER(state->window), state->notebook);
    
    // Home tab
    GtkWidget* home_tab = create_home_tab(state);
    GtkWidget* home_label = gtk_label_new("Home");
    gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), home_tab, home_label);
    
    // Settings tab
    GtkWidget* settings_tab = create_settings_tab(state);
    GtkWidget* settings_label = gtk_label_new("Settings");
    gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), settings_tab, settings_label);
    
    // Preferences tab
    GtkWidget* prefs_tab = create_preferences_tab(state);
    GtkWidget* prefs_label = gtk_label_new("Preferences");
    gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), prefs_tab, prefs_label);
    
    if (state->backend.empty()) {
        if (state->status_label) {
            gtk_label_set_text(GTK_LABEL(state->status_label),
                "Python backend not found. Set TORIFY_BACKEND or run from the installed Torify package.");
        }
    }
    
    return state;
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    auto state = create_window();
    gtk_widget_show_all(state->window);
    if (!state->backend.empty() && state->status_label) {
        start_action(state, "status");
    }
    gtk_main();
    state->alive.store(false);
    return 0;
}
