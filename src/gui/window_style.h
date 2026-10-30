#ifndef PINGKK_WINDOW_STYLE_H
#define PINGKK_WINDOW_STYLE_H

#include <QString>

namespace pingkk { namespace gui {
namespace colors {
constexpr const char* surface = "#ffffff";
constexpr const char* text = "#252a32";
constexpr const char* muted = "#727a86";
constexpr const char* border = "#dce0e5";
constexpr const char* accent = "#1769aa";
constexpr const char* accentPressed = "#12568c";
constexpr const char* hover = "#f5f7f9";
constexpr const char* disabled = "#f7f8fa";
constexpr const char* selection = "#e8f1f8";
constexpr const char* danger = "#b23b3b";
}

QString mainWindowStyle();
} }

#endif
