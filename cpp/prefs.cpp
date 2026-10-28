// Preferences tab - Font, theme, dark/light/auto mode
// Allows customization of appearance

#include "torify.h"

static void apply_theme(std::shared_ptr<AppState>& state) {
    // Apply dark/light/auto theme
    if (!state->dark_mode_switch) return;
    
    gboolean dark_mode = gtk_switch_get_active(GTK_SWITCH(state->dark_mode_switch));
    // Simple theme application
}

GtkWidget* create_preferences_tab(std::shared_ptr<AppState>& state) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    
    // Appearance settings
    GtkWidget* appearance_frame = gtk_frame_new("Appearance");
    GtkWidget* appearance_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(appearance_box), 8);
    gtk_container_add(GTK_CONTAINER(appearance_frame), appearance_box);
    
    // Dark/Light mode
    GtkWidget* mode_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* mode_label = gtk_label_new("Dark Mode:");
    gtk_label_set_xalign(GTK_LABEL(mode_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(mode_box), mode_label, FALSE, FALSE, 0);
    
    state->dark_mode_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(state->dark_mode_switch), TRUE); // Default dark mode
    gtk_box_pack_end(GTK_BOX(mode_box), state->dark_mode_switch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(appearance_box), mode_box, FALSE, FALSE, 0);
    
    // Theme selection (auto/light/dark)
    GtkWidget* theme_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* theme_label = gtk_label_new("Theme:");
    gtk_label_set_xalign(GTK_LABEL(theme_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(theme_box), theme_label, FALSE, FALSE, 0);
    
    state->theme_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->theme_combo), "Auto");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->theme_combo), "Light");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->theme_combo), "Dark");
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->theme_combo), 0);
    gtk_box_pack_end(GTK_BOX(theme_box), state->theme_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(appearance_box), theme_box, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(vbox), appearance_frame, FALSE, FALSE, 0);
    
    // Font settings
    GtkWidget* font_frame = gtk_frame_new("Font");
    GtkWidget* font_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(font_box), 8);
    gtk_container_add(GTK_CONTAINER(font_frame), font_box);
    
    GtkWidget* font_size_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* font_label = gtk_label_new("Font Size:");
    gtk_label_set_xalign(GTK_LABEL(font_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(font_size_box), font_label, FALSE, FALSE, 0);
    
    state->font_size_spin = gtk_spin_button_new_with_range(8, 24, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->font_size_spin), 10);
    gtk_box_pack_end(GTK_BOX(font_size_box), state->font_size_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_box), font_size_box, FALSE, FALSE, 0);
    
    GtkWidget* apply_font_btn = gtk_button_new_with_label("Apply Font Settings");
    gtk_box_pack_start(GTK_BOX(font_box), apply_font_btn, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(vbox), font_frame, FALSE, FALSE, 0);
    
    // Apply button
    GtkWidget* apply_all_btn = gtk_button_new_with_label("Apply All Preferences");
    gtk_widget_set_margin_top(apply_all_btn, 12);
    gtk_box_pack_start(GTK_BOX(vbox), apply_all_btn, FALSE, FALSE, 0);
    
    return vbox;
}
