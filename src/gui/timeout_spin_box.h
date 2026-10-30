#ifndef PINGKK_TIMEOUT_SPIN_BOX_H
#define PINGKK_TIMEOUT_SPIN_BOX_H

#include <QSpinBox>

class TimeoutSpinBox : public QSpinBox {
public:
    explicit TimeoutSpinBox(QWidget* parent = 0);

protected:
    void paintEvent(QPaintEvent* event) override;
};

#endif
