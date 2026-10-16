#ifndef PINGKK_SETTINGS_DIALOG_H
#define PINGKK_SETTINGS_DIALOG_H

#include <QDialog>
#include <functional>

class SettingsDialog : public QDialog {
public:
    SettingsDialog(bool english, const std::function<void()>& install, QWidget* parent);
};

#endif
