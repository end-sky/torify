// Settings tab - Bridge configuration
// Allows user to put in Tor bridges

#include "torify.h"
#include <vector>

GtkWidget* create_settings_tab(std::shared_ptr<AppState>& state) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    
    // Transport selection
    GtkWidget* transport_frame = gtk_frame_new("Transport");
    GtkWidget* transport_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(transport_box), 8);
    gtk_container_add(GTK_CONTAINER(transport_frame), transport_box);
    
    GtkWidget* transport_label = gtk_label_new("Select transport type:");
    gtk_label_set_xalign(GTK_LABEL(transport_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(transport_box), transport_label, FALSE, FALSE, 0);
    
    state->transport_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->transport_combo), "direct");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->transport_combo), "obfs4");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->transport_combo), "snowflake");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->transport_combo), "webtunnel");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->transport_combo), "meek_lite");
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->transport_combo), 0);
    gtk_box_pack_start(GTK_BOX(transport_box), state->transport_combo, FALSE, FALSE, 0);
    
    GtkWidget* transport_apply = gtk_button_new_with_label("Apply Transport");
    gtk_box_pack_start(GTK_BOX(transport_box), transport_apply, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(vbox), transport_frame, FALSE, FALSE, 0);
    
    // Bridge configuration
    GtkWidget* bridge_frame = gtk_frame_new("Tor Bridges");
    GtkWidget* bridge_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bridge_box), 8);
    gtk_container_add(GTK_CONTAINER(bridge_frame), bridge_box);
    
    GtkWidget* bridge_info = gtk_label_new("Add bridge lines from bridges.torproject.org");
    gtk_label_set_xalign(GTK_LABEL(bridge_info), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(bridge_info), TRUE);
    gtk_box_pack_start(GTK_BOX(bridge_box), bridge_info, FALSE, FALSE, 0);
    
    // Bridge entry
    GtkWidget* entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    state->bridge_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->bridge_entry), "Enter bridge line (e.g., obfs4 ...)");
    gtk_box_pack_start(GTK_BOX(entry_box), state->bridge_entry, TRUE, TRUE, 0);
    
    GtkWidget* add_bridge_btn = gtk_button_new_with_label("Add Bridge");
    gtk_box_pack_start(GTK_BOX(entry_box), add_bridge_btn, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(bridge_box), entry_box, FALSE, FALSE, 0);
    
    // Bridge list
    GtkWidget* list_label = gtk_label_new("Saved Bridges:");
    gtk_label_set_xalign(GTK_LABEL(list_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(bridge_box), list_label, FALSE, FALSE, 0);
    
    GtkWidget* scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled, -1, 80);
    
    state->bridge_list = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->bridge_list), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(state->bridge_list), FALSE);
    gtk_container_add(GTK_CONTAINER(scrolled), state->bridge_list);
    gtk_box_pack_start(GTK_BOX(bridge_box), scrolled, TRUE, TRUE, 0);
    
    GtkWidget* bridge_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* clear_bridges_btn = gtk_button_new_with_label("Clear All");
    gtk_box_pack_start(GTK_BOX(bridge_actions), clear_bridges_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bridge_actions), gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bridge_box), bridge_actions, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(vbox), bridge_frame, TRUE, TRUE, 0);
    
    return vbox;
}
