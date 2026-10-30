#include "timeout_spin_box.h"
#include "window_style.h"

#include <QPainter>
#include <QStyleOptionSpinBox>

TimeoutSpinBox::TimeoutSpinBox(QWidget* parent) : QSpinBox(parent) {
    // 保留 Qt 的步进、长按和键盘行为，避免无按钮模式下的编辑区布局异常。
    setButtonSymbols(QAbstractSpinBox::UpDownArrows);
}

void TimeoutSpinBox::paintEvent(QPaintEvent* event) {
    QSpinBox::paintEvent(event);
    QStyleOptionSpinBox option;
    initStyleOption(&option);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    for (int direction = 0; direction < 2; ++direction) {
        const bool up = direction == 0;
        const QRect button = style()->subControlRect(
            QStyle::CC_SpinBox, &option,
            up ? QStyle::SC_SpinBoxUp : QStyle::SC_SpinBoxDown, this);
        const bool available = isEnabled() &&
            (stepEnabled() & (up ? StepUpEnabled : StepDownEnabled));
        QPen pen{QColor(available ? pingkk::gui::colors::muted : pingkk::gui::colors::border)};
        pen.setWidthF(1.3);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        const QPointF center = QRectF(button).center();
        const qreal sign = up ? -1 : 1;
        QPolygonF arrow;
        arrow << center + QPointF(-3, -sign * 1.5)
              << center + QPointF(0, sign * 1.5)
              << center + QPointF(3, -sign * 1.5);
        painter.drawPolyline(arrow);
    }
}
