/*
    splash.h
    The AerOS boot screen: an orange page with the copyright box, the version number,
    "Press X for boot options." and the big "aerOS" wordmark, all drawn in VGA text mode.
    splash_screen() in lib/ren_comp.c shows it while it waits for the X key.
*/
#ifndef SPLASH_H
#define SPLASH_H

// draw the boot screen (repaints three palette colours into the AerOS oranges)
void splash_draw(void);

// blank the screen and give the borrowed palette colours back their normal colours
void splash_close(void);

#endif /* SPLASH_H */
