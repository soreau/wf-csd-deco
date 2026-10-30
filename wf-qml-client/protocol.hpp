#pragma once

#include <QObject>
// #include <QWidget>
#include <QMap>
#include <QSharedPointer>
#include <QString>
#include <QList>
// #include <QScrollArea>

// Forward declarations
// class DecorationWindow;

#include "DecorationWindow.h"


// Global data
extern QMap<uint32_t, QWindow*> view_to_decor;
// extern QMap<uint32_t, QWidget*> view_to_decor;

// Protocol functions
void setup_protocol(void *display);
void close_request(uint32_t wf_id);
void update_borders(uint32_t id, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right,
    uint32_t border);
void group_windows(uint32_t parent_id, uint32_t child_id);
void select_window(uint32_t id);
void ungroup_window(uint32_t id);

// Window management
DecorationWindow *create_deco_window(uint32_t wf_id);
void destroy_deco_window(uint32_t wf_id);
// void set_title(QWidget *window, const char *title);
// void set_app_id(QWidget *window, const char *app_id);
void update_theme(std::string theme);

void set_title(QWindow *window, const char *title);
void set_app_id(QWindow *window, const char *app_id);
