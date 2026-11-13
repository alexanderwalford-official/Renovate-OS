/*
    bi_graphics.c
    AerLang's built-in functions for graphics: taking the screen over (gopen, gclose), its size,
    colours (rgb), drawing (gclear, pixel, rect, line, text) and BMP pictures (image, imgsz).
    Then what games need: drawing out of sight and showing the finished frame (gbuf, gshow),
    keys held down (keydown) and the mouse.

    The drawing itself is done by drivers/video/gfx.c. aer->graphics says whether the program
    has the screen in graphics mode; aer_give_screen_back() in aer_builtins.c gives it back to
    the terminal when the program ends.

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../drivers/video/gfx.h"
#include "../drivers/input/IO.h"
#include "../drivers/input/mouse.h"

// stop the program with a helpful message if a drawing function is used before gopen(), rather
// than drawing into a screen that isn't in graphics mode. fn is the AerLang name, for the message
static void need_graphics (int line, const char* fn) {
    if (!aer->graphics) {
        aer_error(line, "%s() needs graphics, call gopen() first", fn);
    }
}

// gopen(): take the screen over for graphics. False if there is only the text screen
value_t bi_gfx_open (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    // calling it twice is harmless: the screen is only switched over the first time.
    // gfx_begin says whether it worked, and that becomes the program's graphics flag
    if (!aer->graphics) {
        aer->graphics = gfx_begin();
    }
    return aer_bool(aer->graphics);
}

// gclose(): give the screen back to the terminal (a program that ends does this by itself)
value_t bi_gfx_close (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    if (aer->graphics) {
        gfx_end();
        aer->graphics = false;
    }
    return aer_nil();
}

// gw(): the width of the graphics screen in pixels
value_t bi_gfx_width (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    need_graphics(line, "gw");
    return aer_int((int32_t)gfx_width());
}

// gh(): the height of the graphics screen in pixels
value_t bi_gfx_height (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    need_graphics(line, "gh");
    return aer_int((int32_t)gfx_height());
}

// rgb(r, g, b): a colour for the graphics functions, mixed from red, green and blue (each 0 to 255)
value_t bi_rgb (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t r = int_arg(argv, 0, line, "rgb") & 0xFF;
    int32_t g = int_arg(argv, 1, line, "rgb") & 0xFF;
    int32_t b = int_arg(argv, 2, line, "rgb") & 0xFF;
    // packed as 0xRRGGBB: red in bits 16 to 23, green in 8 to 15, blue in 0 to 7.
    // The & 0xFF above keeps each one to 8 bits, so one can't spill into its neighbour
    return aer_int((r << 16) | (g << 8) | b);
}

// gclear(colour): fill the whole screen with a colour (black without one)
value_t bi_gfx_clear (int argc, value_t* argv, int line) {
    need_graphics(line, "gclear");
    // 0 is rgb(0, 0, 0), black
    gfx_clear(argc > 0 ? (uint32_t)int_arg(argv, 0, line, "gclear") : 0);
    return aer_nil();
}

// pixel(x, y, colour): colour one pixel
value_t bi_pixel (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "pixel");
    gfx_pixel(int_arg(argv, 0, line, "pixel"), int_arg(argv, 1, line, "pixel"), (uint32_t)int_arg(argv, 2, line, "pixel"));
    return aer_nil();
}

// rect(x, y, w, h, colour): a filled rectangle, w by h pixels, with its top left corner at x, y
value_t bi_rect (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "rect");
    gfx_rect(int_arg(argv, 0, line, "rect"), int_arg(argv, 1, line, "rect"), int_arg(argv, 2, line, "rect"),
             int_arg(argv, 3, line, "rect"), (uint32_t)int_arg(argv, 4, line, "rect"));
    return aer_nil();
}

// line(x1, y1, x2, y2, colour): a straight line from one point to the other
value_t bi_line (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "line");
    gfx_line(int_arg(argv, 0, line, "line"), int_arg(argv, 1, line, "line"), int_arg(argv, 2, line, "line"),
             int_arg(argv, 3, line, "line"), (uint32_t)int_arg(argv, 4, line, "line"));
    return aer_nil();
}

// text(x, y, s, colour, scale): write s at x, y, every letter scale times its normal size (1 without scale)
value_t bi_text (int argc, value_t* argv, int line) {
    need_graphics(line, "text");
    // any value can be written, the way print() would show it (a new string, released below)
    value_t s = aer_to_string(argv[2]);
    int scale = argc > 4 ? int_arg(argv, 4, line, "text") : 1;
    gfx_text(int_arg(argv, 0, line, "text"), int_arg(argv, 1, line, "text"), s.as.obj->data.chars,
             (uint32_t)int_arg(argv, 3, line, "text"), scale);
    aer_release(s);
    return aer_nil();
}

// image(data, x, y, w, h): draw a BMP picture (bytes from rdb) at x, y, shrunk to fit w x h if given. False if it isn't one
value_t bi_image (int argc, value_t* argv, int line) {
    need_graphics(line, "image");
    aer_object_t* data = bytes_arg(argv, 0, line, "image");
    // without w and h (0) the picture is drawn at its own size, with them it is scaled to fit
    int max_w = argc > 3 ? int_arg(argv, 3, line, "image") : 0;
    int max_h = argc > 4 ? int_arg(argv, 4, line, "image") : 0;
    return aer_bool(gfx_image(data->data.bytes, data->length, int_arg(argv, 1, line, "image"),
                              int_arg(argv, 2, line, "image"), max_w, max_h));
}

// imgsz(data): a BMP picture's size as [width, height], nil if it isn't one
value_t bi_image_size (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* data = bytes_arg(argv, 0, line, "imgsz");
    int w, h;
    if (!gfx_image_size(data->data.bytes, data->length, &w, &h)) {
        return aer_nil();
    }
    value_t size = aer_list(2);
    aer_list_push(size.as.obj, aer_int(w));
    aer_list_push(size.as.obj, aer_int(h));
    return size;
}

// ---- for games: smooth drawing and held keys ---------------------------------------------------

// gbuf(on): true draws everything into a back buffer, out of sight, until gshow() puts the
// finished frame on the screen (no flicker); false draws straight onto the screen again.
// Returns false if there isn't enough memory for the back buffer
value_t bi_gbuf (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "gbuf");
    // aer_truthy, so any value works as on / off, not only true and false
    return aer_bool(gfx_buffered(aer_truthy(argv[0])));
}

// gshow(): put the frame drawn in the back buffer on the screen
value_t bi_gshow (int argc, value_t* argv, int line) {
    (void)argc;
    (void)argv;
    need_graphics(line, "gshow");
    gfx_show();
    return aer_nil();
}

// keydown(key): is this key held down right now? key is a character ("a", " ", or ord("a"))
// or one of the KEY_ constants (KEY_LEFT, KEY_SHIFT...). Unlike key(), it doesn't wait, and
// several keys can be down at once
value_t bi_keydown (int argc, value_t* argv, int line) {
    (void)argc;
    int key;
    // a string means its first character, read as unsigned (as in ord())
    if (argv[0].type == V_STR) {
        if (argv[0].as.obj->length == 0) {
            aer_error(line, "keydown() needs a key, not an empty string");
        }
        key = (uint8_t)argv[0].as.obj->data.chars[0];
    } else {
        // anything else must be a number: a character code or a KEY_ constant
        key = int_arg(argv, 0, line, "keydown");
    }
    return aer_bool(kb_is_down(key));
}

// ---- the mouse (only for programs: see drivers/input/mouse.h) ----------------------------------

// switch the mouse on if it isn't yet, the first time a program asks about it. The pointer moves
// over the screen in pixels, starting in the middle. False if there is no mouse.
// aer_builtins_finish switches it off again when the program ends
static bool mouse_on (void) {
    if (mouse_ready()) {
        return true;
    }
    if (!mouse_start()) {
        return false;
    }
    // the screen's size in pixels (VGA text mode has none, so a 640 x 480 area stands in)
    int width = (int)gfx_width();
    int height = (int)gfx_height();
    mouse_set_area(width > 0 ? width : 640, height > 0 ? height : 480);
    return true;
}

// mouse(): where the pointer is and which buttons are held, as [x, y, buttons]. x and y are
// pixels from the top left of the screen, and buttons adds up MOUSE_LEFT (1), MOUSE_RIGHT (2)
// and MOUSE_MIDDLE (4). nil if there is no mouse. The program draws its own pointer
value_t bi_mouse (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    if (!mouse_on()) {
        return aer_nil();
    }
    int x, y, buttons;
    mouse_position(&x, &y, &buttons);
    value_t result = aer_list(3);
    aer_list_push(result.as.obj, aer_int(x));
    aer_list_push(result.as.obj, aer_int(y));
    aer_list_push(result.as.obj, aer_int(buttons));
    return result;
}

// mousemove(): how far the mouse has moved since the last mousemove(), as [dx, dy] (right and
// down are positive). It never stops at the edge of the screen, so it is the one for turning a
// camera. [0, 0] if there is no mouse, so a game still works with the keys alone
value_t bi_mousemove (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    int dx = 0, dy = 0;
    if (mouse_on()) {
        mouse_motion(&dx, &dy);
    }
    value_t result = aer_list(2);
    aer_list_push(result.as.obj, aer_int(dx));
    aer_list_push(result.as.obj, aer_int(dy));
    return result;
}

// mousedown(button): is this button held right now? button is MOUSE_LEFT, MOUSE_RIGHT or
// MOUSE_MIDDLE (false if there is no mouse)
value_t bi_mousedown (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t button = int_arg(argv, 0, line, "mousedown");
    if (!mouse_on()) {
        return aer_bool(false);
    }
    int x, y, buttons;
    mouse_position(&x, &y, &buttons);
    return aer_bool((buttons & button) != 0);
}
