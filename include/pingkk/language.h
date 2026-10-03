#ifndef PINGKK_LANGUAGE_H
#define PINGKK_LANGUAGE_H

namespace pingkk {

// Language is selected before running diagnostics; Chinese remains the default.
void setEnglish(bool enabled);
bool isEnglish();
const char* text(const char* chinese);

}  // namespace pingkk

#endif
