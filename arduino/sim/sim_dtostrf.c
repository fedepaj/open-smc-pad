/* dtostrf on the host (the core's goes through the device printf) */
#include <stdio.h>

char *dtostrf(double val, signed char width, unsigned char prec, char *sout)
{
    sprintf(sout, "%*.*f", width, prec, val);
    return sout;
}
