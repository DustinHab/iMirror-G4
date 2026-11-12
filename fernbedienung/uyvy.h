#ifndef UYVY_H
#define UYVY_H
void zu_uyvy(const unsigned char *y, int py, const unsigned char *u, int pu, const unsigned char *v, int pv,
             unsigned char *aus, int pa, int w, int h, int voll);
#endif
