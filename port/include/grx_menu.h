/* The port's own menu (port/src/menu.c): Esc, render resolution, frame
   rate and aspect, while the game waits */
#ifndef GRX_MENU_H
#define GRX_MENU_H

int grx_menu_open(void);
void grx_menu_toggle(void);

/* A key (SDL keycode and scancode) or a gamepad button (SDL_GamepadButton)
   pressed: 1 when the open menu took it */
int grx_menu_key(int key, int scancode);
int grx_menu_pad(int code);

/* The presenter (GL context current, default framebuffer bound): the menu
   over the picture at (x, y, w, h) in a ww x wh window */
void grx_menu_draw_gl(int x, int y, int w, int h, int ww, int wh);

#endif
