#ifndef TORIFY_H
#define TORIFY_H

#include <gtk/gtk.h>
#include <atomic>
#include <memory>
#include <string>

struct AppState {
    std::atomic<bool> alive{true};
    bool busy{false};
    GtkWidget* window{nullptr};
    GtkWidget* notebook{nullptr};
    
    // Home tab widgets
    GtkWidget* status_label{nullptr};
    GtkWidget* connect_button{nullptr};
    GtkWidget* disconnect_button{nullptr};
    GtkWidget* status_button{nullptr};
    
    // Settings tab widgets
    GtkWidget* bridge_entry{nullptr};
    GtkWidget* bridge_list{nullptr};
    GtkWidget* transport_combo{nullptr};
    
    // Preferences tab widgets
    GtkWidget* font_size_spin{nullptr};
    GtkWidget* theme_combo{nullptr};
    GtkWidget* dark_mode_switch{nullptr};
    
    std::string backend;
};

struct ProcessResult {
    int exit_code{1};
    std::string output;
};

struct ActionBinding {
    std::shared_ptr<AppState> state;
    std::string action;
};

struct Completion {
    std::shared_ptr<AppState> state;
    std::string action;
    ProcessResult result;
};

GtkWidget* create_settings_tab(std::shared_ptr<AppState>& state);
GtkWidget* create_preferences_tab(std::shared_ptr<AppState>& state);
GtkWidget* create_home_tab(std::shared_ptr<AppState>& state);

std::string trim(const std::string& value);
ProcessResult run_backend(const std::string& backend, const std::string& action);
void start_action(const std::shared_ptr<AppState>& state, const std::string& action);
void connect_action(GtkWidget* button, const std::shared_ptr<AppState>& state, const char* action);

#endif // TORIFY_H
