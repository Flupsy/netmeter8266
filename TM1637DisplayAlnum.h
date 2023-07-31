#include <TM1637Display.h>

class TM1637DisplayAlnum : public TM1637Display
{
  public:
    using TM1637Display::TM1637Display;
    
    void showAlnum(char c, uint8_t dots = 0);
    void showAlnum(const char s[], uint8_t dots = 0);
};